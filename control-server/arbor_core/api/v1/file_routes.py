"""
Config file browser API — browse, read, write, and manage config files.

Supports two source types:
- filesystem: direct pathlib operations with path traversal protection
- moonraker: proxy to Moonraker's file management API

Task: Config File Browser
"""

from __future__ import annotations

import asyncio
import http.client
import json
import subprocess
from pathlib import Path
from typing import Any
from urllib.parse import quote

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

logger = structlog.get_logger(__name__)

file_router = APIRouter(prefix="/files", tags=["files"])


def _get_file_roots(request: Request) -> list[dict[str, Any]]:
    """Get configured file roots from app config."""
    config = getattr(request.app.state, "config", None)
    if config is None:
        return []
    roots = getattr(config, "file_roots", [])
    result = []
    for root in roots:
        if hasattr(root, "model_dump"):
            result.append(root.model_dump())
        elif isinstance(root, dict):
            result.append(root)
    return result


def _find_root(roots: list[dict[str, Any]], root_id: str) -> dict[str, Any] | None:
    """Find a file root by ID."""
    for root in roots:
        if root.get("id") == root_id:
            return root
    return None


def _validate_path(base_path: str, relative_path: str) -> Path | None:
    """
    Validate and resolve a path, preventing directory traversal.

    Returns resolved path if valid, None if traversal attempted.
    """
    base = Path(base_path).resolve()
    if not relative_path or relative_path == ".":
        return base
    target = (base / relative_path).resolve()
    try:
        target.relative_to(base)
        return target
    except ValueError:
        return None


def _validate_extension(filename: str, allowed: list[str]) -> bool:
    """Check if a file extension is in the allowed list."""
    if not allowed:
        return True
    ext = Path(filename).suffix.lower()
    return ext in allowed


def _moonraker_request(
    method: str,
    path: str,
    host: str = "localhost",
    port: int = 7125,
    body: bytes | None = None,
    content_type: str | None = None,
    timeout: float = 10.0,
) -> tuple[int, str]:
    """Blocking HTTP request to Moonraker. Returns (status, body_text)."""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        headers: dict[str, str] = {"Accept": "application/json"}
        if body is not None and content_type:
            headers["Content-Type"] = content_type
            headers["Content-Length"] = str(len(body))
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        resp_body = resp.read().decode("utf-8", errors="replace")
        return resp.status, resp_body
    except Exception as exc:
        return 500, str(exc)
    finally:
        conn.close()


async def _async_moonraker(
    method: str,
    path: str,
    host: str = "localhost",
    port: int = 7125,
    body: bytes | None = None,
    content_type: str | None = None,
) -> tuple[int, str]:
    """Async wrapper for moonraker requests."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(
        None, _moonraker_request, method, path, host, port, body, content_type
    )


# -- Roots --

@file_router.get(
    "/roots",
    summary="List file roots",
    description="List available file roots for browsing.",
)
async def list_roots(request: Request) -> JSONResponse:
    """List all configured file roots."""
    roots = _get_file_roots(request)
    return JSONResponse(content={
        "roots": [
            {
                "id": r.get("id"),
                "label": r.get("label"),
                "source": r.get("source"),
                "readonly": r.get("readonly", False),
                "has_restart": r.get("restart_command") is not None,
            }
            for r in roots
        ],
        "count": len(roots),
    })


# -- List --

@file_router.get(
    "/{root_id}/list",
    summary="List directory contents",
    description="List files and directories in a root.",
)
async def list_directory(root_id: str, request: Request) -> JSONResponse:
    """List directory contents."""
    roots = _get_file_roots(request)
    root = _find_root(roots, root_id)
    if root is None:
        return JSONResponse(status_code=404, content={"detail": f"Root '{root_id}' not found"})

    rel_path = request.query_params.get("path", "")
    source = root.get("source", "filesystem")

    if source == "moonraker":
        return await _list_moonraker(root, rel_path, request)
    else:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, _list_filesystem, root, rel_path)


def _list_filesystem(root: dict[str, Any], rel_path: str) -> JSONResponse:
    """List directory from filesystem."""
    target = _validate_path(root["base_path"], rel_path)
    if target is None:
        logger.warning("path_traversal_blocked", root=root.get("id"), path=rel_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    if not target.exists():
        return JSONResponse(status_code=404, content={"detail": "Path not found"})

    if not target.is_dir():
        return JSONResponse(status_code=400, content={"detail": "Path is not a directory"})

    items = []
    allowed_ext = root.get("allowed_extensions", [])
    try:
        for entry in sorted(target.iterdir(), key=lambda e: (not e.is_dir(), e.name.lower())):
            if entry.name.startswith("."):
                continue  # Skip hidden files
            if entry.is_dir():
                items.append({
                    "name": entry.name,
                    "type": "directory",
                    "size": 0,
                })
            elif not allowed_ext or entry.suffix.lower() in allowed_ext:
                items.append({
                    "name": entry.name,
                    "type": "file",
                    "size": entry.stat().st_size,
                    "modified": entry.stat().st_mtime,
                })
    except PermissionError:
        logger.warning("permission_denied", root=root.get("id"), path=rel_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    return JSONResponse(content={"path": rel_path, "items": items, "count": len(items)})


async def _list_moonraker(root: dict[str, Any], rel_path: str, request: Request) -> JSONResponse:
    """List directory via Moonraker API."""
    moonraker_cfg = _get_moonraker_config(request)
    host = moonraker_cfg.get("host", "localhost")
    port = moonraker_cfg.get("port", 7125)

    # Moonraker API: GET /server/files/directory?path={moonraker_root}/{subpath}
    # The path param includes the Moonraker root name (e.g. "config")
    moonraker_root = root.get("moonraker_root", "config")
    if rel_path:
        api_path = f"/server/files/directory?path={quote(moonraker_root)}/{quote(rel_path)}"
    else:
        api_path = f"/server/files/directory?path={quote(moonraker_root)}"

    status, body = await _async_moonraker("GET", api_path, host, port)
    if status >= 400:
        return JSONResponse(status_code=status, content={"detail": body[:500]})

    try:
        data = json.loads(body)
        result = data.get("result", {})
        dirs = result.get("dirs", [])
        files = result.get("files", [])

        items = []
        for d in sorted(dirs, key=lambda x: x.get("dirname", "").lower()):
            items.append({
                "name": d.get("dirname", ""),
                "type": "directory",
                "size": 0,
            })
        for f in sorted(files, key=lambda x: x.get("filename", "").lower()):
            items.append({
                "name": f.get("filename", ""),
                "type": "file",
                "size": f.get("size", 0),
                "modified": f.get("modified", 0),
            })

        return JSONResponse(content={"path": rel_path, "items": items, "count": len(items)})
    except (json.JSONDecodeError, KeyError) as exc:
        return JSONResponse(status_code=500, content={"detail": f"Failed to parse Moonraker response: {exc}"})


# -- Read --

@file_router.get(
    "/{root_id}/read",
    summary="Read file content",
    description="Read the text content of a file.",
)
async def read_file(root_id: str, request: Request) -> JSONResponse:
    """Read file content."""
    roots = _get_file_roots(request)
    root = _find_root(roots, root_id)
    if root is None:
        return JSONResponse(status_code=404, content={"detail": f"Root '{root_id}' not found"})

    file_path = request.query_params.get("path", "")
    if not file_path:
        return JSONResponse(status_code=422, content={"detail": "'path' query parameter required"})

    source = root.get("source", "filesystem")
    max_size = root.get("max_file_size", 1048576)

    if source == "moonraker":
        return await _read_moonraker(root, file_path, max_size, request)
    else:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, _read_filesystem, root, file_path, max_size)


def _read_filesystem(root: dict[str, Any], file_path: str, max_size: int) -> JSONResponse:
    """Read file from filesystem."""
    target = _validate_path(root["base_path"], file_path)
    if target is None:
        logger.warning("path_traversal_blocked", root=root.get("id"), path=file_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    if not target.exists():
        return JSONResponse(status_code=404, content={"detail": "File not found"})

    if not target.is_file():
        return JSONResponse(status_code=400, content={"detail": "Path is not a file"})

    if target.stat().st_size > max_size:
        return JSONResponse(status_code=413, content={"detail": f"File too large (max {max_size} bytes)"})

    allowed_ext = root.get("allowed_extensions", [])
    if not _validate_extension(target.name, allowed_ext):
        logger.warning("extension_blocked", root=root.get("id"), path=file_path, ext=target.suffix)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    try:
        content = target.read_text(encoding="utf-8", errors="replace")
    except PermissionError:
        logger.warning("permission_denied", root=root.get("id"), path=file_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    return JSONResponse(content={
        "path": file_path,
        "content": content,
        "size": len(content),
        "readonly": root.get("readonly", False),
    })


async def _read_moonraker(
    root: dict[str, Any], file_path: str, max_size: int, request: Request
) -> JSONResponse:
    """Read file via Moonraker API."""
    moonraker_cfg = _get_moonraker_config(request)
    host = moonraker_cfg.get("host", "localhost")
    port = moonraker_cfg.get("port", 7125)

    # Moonraker: GET /server/files/{root}/{path} returns raw file content
    moonraker_root = root.get("moonraker_root", "config")
    api_path = f"/server/files/{quote(moonraker_root)}/{quote(file_path)}"
    status, body = await _async_moonraker("GET", api_path, host, port)

    if status >= 400:
        return JSONResponse(status_code=status, content={"detail": body[:500]})

    if len(body) > max_size:
        return JSONResponse(status_code=413, content={"detail": f"File too large (max {max_size} bytes)"})

    return JSONResponse(content={
        "path": file_path,
        "content": body,
        "size": len(body),
        "readonly": root.get("readonly", False),
    })


# -- Write --

@file_router.put(
    "/{root_id}/write",
    summary="Write file content",
    description="Write text content to a file. Creates .bak backup for filesystem sources.",
)
async def write_file(root_id: str, request: Request) -> JSONResponse:
    """Write file content."""
    roots = _get_file_roots(request)
    root = _find_root(roots, root_id)
    if root is None:
        return JSONResponse(status_code=404, content={"detail": f"Root '{root_id}' not found"})

    if root.get("readonly", False):
        return JSONResponse(status_code=403, content={"detail": "This root is read-only"})

    try:
        body = await request.json()
    except Exception:
        return JSONResponse(status_code=422, content={"detail": "Invalid JSON body"})

    file_path = body.get("path", "")
    content = body.get("content", "")

    if not file_path:
        return JSONResponse(status_code=422, content={"detail": "'path' is required"})

    max_size = root.get("max_file_size", 1048576)
    if len(content) > max_size:
        return JSONResponse(status_code=413, content={"detail": f"Content too large (max {max_size} bytes)"})

    source = root.get("source", "filesystem")

    if source == "moonraker":
        return await _write_moonraker(root, file_path, content, request)
    else:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, _write_filesystem, root, file_path, content)


def _write_filesystem(root: dict[str, Any], file_path: str, content: str) -> JSONResponse:
    """Write file to filesystem with .bak backup."""
    target = _validate_path(root["base_path"], file_path)
    if target is None:
        logger.warning("path_traversal_blocked", root=root.get("id"), path=file_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    allowed_ext = root.get("allowed_extensions", [])
    if not _validate_extension(target.name, allowed_ext):
        logger.warning("extension_blocked", root=root.get("id"), path=file_path, ext=target.suffix)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    try:
        # Create backup if file exists
        if target.exists():
            backup = target.with_suffix(target.suffix + ".bak")
            backup.write_text(target.read_text(encoding="utf-8"), encoding="utf-8")
            logger.info("file_backup_created", path=str(backup))

        # Ensure parent directory exists
        target.parent.mkdir(parents=True, exist_ok=True)

        # Write the file
        target.write_text(content, encoding="utf-8")
        logger.info("file_written", path=str(target), size=len(content))

        return JSONResponse(content={
            "ok": True,
            "path": file_path,
            "size": len(content),
        })
    except PermissionError:
        logger.warning("permission_denied", root=root.get("id"), path=file_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})
    except Exception as exc:
        return JSONResponse(status_code=500, content={"detail": str(exc)})


async def _write_moonraker(
    root: dict[str, Any], file_path: str, content: str, request: Request
) -> JSONResponse:
    """Write file via Moonraker upload API."""
    moonraker_cfg = _get_moonraker_config(request)
    host = moonraker_cfg.get("host", "localhost")
    port = moonraker_cfg.get("port", 7125)

    # Moonraker upload: POST /server/files/upload (multipart)
    # Per docs: 'root' = target root, 'path' = subdirectory (optional),
    # 'file' filename = just the filename (not full path)
    boundary = "----ArborFileUpload"
    moonraker_root = root.get("moonraker_root", "config")

    # Split file_path into subdirectory and filename
    file_p = Path(file_path)
    upload_filename = file_p.name
    upload_subdir = str(file_p.parent) if file_p.parent != Path(".") else ""

    body_parts = []
    body_parts.append(f"--{boundary}")
    body_parts.append('Content-Disposition: form-data; name="root"')
    body_parts.append("")
    body_parts.append(moonraker_root)
    if upload_subdir:
        body_parts.append(f"--{boundary}")
        body_parts.append('Content-Disposition: form-data; name="path"')
        body_parts.append("")
        body_parts.append(upload_subdir)
    body_parts.append(f"--{boundary}")
    # Sanitize filename to prevent header injection via embedded quotes
    safe_filename = upload_filename.replace('"', "_").replace("\r", "").replace("\n", "")
    body_parts.append(f'Content-Disposition: form-data; name="file"; filename="{safe_filename}"')
    body_parts.append("Content-Type: text/plain")
    body_parts.append("")
    body_parts.append(content)
    body_parts.append(f"--{boundary}--")
    body_bytes = "\r\n".join(body_parts).encode("utf-8")

    status, resp_body = await _async_moonraker(
        "POST", "/server/files/upload", host, port,
        body=body_bytes,
        content_type=f"multipart/form-data; boundary={boundary}",
    )

    if status >= 400:
        return JSONResponse(status_code=status, content={"detail": resp_body[:500]})

    return JSONResponse(content={
        "ok": True,
        "path": file_path,
        "size": len(content),
    })


# -- Delete --

@file_router.delete(
    "/{root_id}/delete",
    summary="Delete a file",
    description="Delete a file from a root.",
)
async def delete_file(root_id: str, request: Request) -> JSONResponse:
    """Delete a file."""
    roots = _get_file_roots(request)
    root = _find_root(roots, root_id)
    if root is None:
        return JSONResponse(status_code=404, content={"detail": f"Root '{root_id}' not found"})

    if root.get("readonly", False):
        return JSONResponse(status_code=403, content={"detail": "This root is read-only"})

    file_path = request.query_params.get("path", "")
    if not file_path:
        return JSONResponse(status_code=422, content={"detail": "'path' query parameter required"})

    source = root.get("source", "filesystem")

    if source == "moonraker":
        return await _delete_moonraker(root, file_path, request)
    else:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, _delete_filesystem, root, file_path)


def _delete_filesystem(root: dict[str, Any], file_path: str) -> JSONResponse:
    """Delete file from filesystem."""
    target = _validate_path(root["base_path"], file_path)
    if target is None:
        logger.warning("path_traversal_blocked", root=root.get("id"), path=file_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    if not target.exists():
        return JSONResponse(status_code=404, content={"detail": "File not found"})

    if not target.is_file():
        return JSONResponse(status_code=400, content={"detail": "Can only delete files, not directories"})

    try:
        target.unlink()
        logger.info("file_deleted", path=str(target))
        return JSONResponse(content={"ok": True, "path": file_path})
    except PermissionError:
        logger.warning("permission_denied", root=root.get("id"), path=file_path)
        return JSONResponse(status_code=404, content={"detail": "Not found"})


async def _delete_moonraker(
    root: dict[str, Any], file_path: str, request: Request
) -> JSONResponse:
    """Delete file via Moonraker."""
    moonraker_cfg = _get_moonraker_config(request)
    host = moonraker_cfg.get("host", "localhost")
    port = moonraker_cfg.get("port", 7125)

    moonraker_root = root.get("moonraker_root", "config")
    api_path = f"/server/files/{quote(moonraker_root)}/{quote(file_path)}"
    status, body = await _async_moonraker("DELETE", api_path, host, port)

    if status >= 400:
        return JSONResponse(status_code=status, content={"detail": body[:500]})

    return JSONResponse(content={"ok": True, "path": file_path})


# -- Restart --

@file_router.post(
    "/{root_id}/restart",
    summary="Restart associated service",
    description="Restart the service associated with this file root.",
)
async def restart_service(root_id: str, request: Request) -> JSONResponse:
    """Restart the service associated with a file root."""
    roots = _get_file_roots(request)
    root = _find_root(roots, root_id)
    if root is None:
        return JSONResponse(status_code=404, content={"detail": f"Root '{root_id}' not found"})

    restart_cmd = root.get("restart_command")
    if not restart_cmd:
        return JSONResponse(status_code=400, content={"detail": "No restart command configured"})

    source = root.get("source", "filesystem")

    # For Moonraker source, send G-code restart
    if source == "moonraker":
        klipper = getattr(request.app.state, "klipper_backend", None)
        if klipper is not None:
            try:
                await klipper.send_gcode(restart_cmd)
                logger.info("service_restarted", root=root_id, method="gcode", command=restart_cmd)
                return JSONResponse(content={"ok": True, "command": restart_cmd})
            except Exception as exc:
                return JSONResponse(content={"status": "error", "error": str(exc)})
        return JSONResponse(status_code=503, content={"detail": "Klipper backend not available"})

    # For filesystem source, run system command
    try:
        loop = asyncio.get_running_loop()
        result = await loop.run_in_executor(
            None,
            lambda: subprocess.run(
                restart_cmd.split(),
                capture_output=True,
                text=True,
                timeout=30,
            ),
        )
        if result.returncode != 0:
            logger.warning(
                "restart_failed",
                root=root_id,
                command=restart_cmd,
                stderr=result.stderr[:500],
            )
            return JSONResponse(content={
                "ok": False,
                "command": restart_cmd,
                "error": result.stderr[:500],
            })

        logger.info("service_restarted", root=root_id, method="subprocess", command=restart_cmd)
        return JSONResponse(content={"ok": True, "command": restart_cmd})
    except Exception as exc:
        return JSONResponse(status_code=500, content={"detail": str(exc)})


# -- Helpers --

def _get_moonraker_config(request: Request) -> dict[str, Any]:
    """Get Moonraker connection config from app state."""
    config = getattr(request.app.state, "config", None)
    if config is not None:
        robotics = getattr(config, "robotics", None)
        if robotics is not None:
            moonraker = getattr(robotics, "moonraker", None)
            if moonraker is not None:
                return {
                    "host": getattr(moonraker, "host", "localhost"),
                    "port": getattr(moonraker, "port", 7125),
                }
    return {"host": "localhost", "port": 7125}
