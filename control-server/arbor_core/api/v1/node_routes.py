"""
Node management API — list, add, remove, and probe hardware nodes.

Provides the control-plane endpoints for managing ESP32 and other
hardware nodes that Arbor Core communicates with.

Endpoints:
    GET    /api/v1/nodes          List registered nodes with status
    POST   /api/v1/nodes          Add a new node
    DELETE /api/v1/nodes/{id}     Remove a node
    POST   /api/v1/nodes/probe    Auto-detect a node's capabilities
"""

from __future__ import annotations

from typing import Any

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

logger = structlog.get_logger(__name__)

node_router = APIRouter(prefix="/nodes", tags=["nodes"])


@node_router.get(
    "",
    summary="List registered nodes",
    description="Returns all configured nodes with their connection status.",
)
async def list_nodes(request: Request) -> JSONResponse:
    """List all registered nodes, enriched with connection status."""
    config = getattr(request.app.state, "config", None)
    bridge = getattr(request.app.state, "node_bridge", None)

    if config is None or not hasattr(config, "nodes"):
        return JSONResponse(content={"nodes": [], "count": 0})

    nodes: list[dict[str, Any]] = []
    for node_cfg in config.nodes:
        status = "unknown"
        if bridge is not None:
            transport = bridge.get_transport(node_cfg.id)
            if transport is not None:
                status = "connected" if transport.is_connected else "disconnected"

        tc = node_cfg.transport_config or {}
        entry: dict[str, Any] = {
            "id": node_cfg.id,
            "type": node_cfg.type,
            "transport": node_cfg.transport,
            "status": status,
        }
        # Include connection details
        if "host" in tc:
            entry["host"] = tc["host"]
        if "network_port" in tc:
            entry["port"] = tc["network_port"]
        if "port" in tc:
            entry["serial_port"] = tc["port"]
        if "baud" in tc:
            entry["baud"] = tc["baud"]

        nodes.append(entry)

    return JSONResponse(content={"nodes": nodes, "count": len(nodes)})


@node_router.post(
    "",
    summary="Add a new node",
    description="Register a new hardware node. In-memory only for Phase 1.",
)
async def add_node(request: Request) -> JSONResponse:
    """Add a node to the running configuration."""
    config = getattr(request.app.state, "config", None)

    try:
        body = await request.json()
    except Exception:
        return JSONResponse(status_code=422, content={"detail": "Invalid JSON body"})

    node_id = body.get("id", "")
    if not node_id:
        return JSONResponse(status_code=422, content={"detail": "Node 'id' is required"})

    # Check for duplicate
    if config is not None and hasattr(config, "nodes"):
        for existing in config.nodes:
            if existing.id == node_id:
                return JSONResponse(
                    status_code=409,
                    content={"detail": f"Node '{node_id}' already exists"},
                )

    # Validate as NodeConfig
    from pydantic import ValidationError

    from arbor_core.config.models import NodeConfig

    try:
        node_cfg = NodeConfig(**body)
    except ValidationError as exc:
        return JSONResponse(
            status_code=422,
            content={"detail": "Validation failed", "errors": exc.errors()},
        )

    # Append to in-memory config
    if config is not None and hasattr(config, "nodes"):
        config.nodes.append(node_cfg)
    else:
        logger.error("no_config_loaded_cannot_persist_node")
        return JSONResponse(
            status_code=500,
            content={"detail": "Server configuration not loaded — cannot persist node"},
        )

    logger.info("node_added", node_id=node_id, transport=node_cfg.transport)

    return JSONResponse(
        status_code=201,
        content={"ok": True, "node_id": node_id, "detail": "Node registered"},
    )


@node_router.delete(
    "/{node_id}",
    summary="Remove a node",
    description="Unregister a node and disconnect its transport.",
)
async def remove_node(node_id: str, request: Request) -> JSONResponse:
    """Remove a node from the running configuration."""
    config = getattr(request.app.state, "config", None)
    bridge = getattr(request.app.state, "node_bridge", None)

    if config is None or not hasattr(config, "nodes"):
        return JSONResponse(status_code=404, content={"detail": f"Node '{node_id}' not found"})

    # Find and remove
    original_len = len(config.nodes)
    config.nodes = [n for n in config.nodes if n.id != node_id]
    found = len(config.nodes) < original_len

    if not found:
        return JSONResponse(status_code=404, content={"detail": f"Node '{node_id}' not found"})

    # Disconnect transport if present
    if bridge is not None:
        await bridge.unregister_node(node_id)

    logger.info("node_removed", node_id=node_id)

    return JSONResponse(content={"ok": True, "detail": f"Node '{node_id}' removed"})


def _sync_get(host: str, port: int, path: str, timeout: float) -> tuple[int, str]:
    """
    Minimal HTTP GET using stdlib http.client.

    httpx/httpcore sends headers (Accept-Encoding, chunked TE, etc.) that
    ESP-IDF's lightweight httpd cannot parse, causing silent hangs.
    stdlib http.client sends clean, minimal HTTP/1.1 that ESP-IDF handles.
    """
    import http.client
    import json as _json

    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("GET", path, headers={"Accept": "application/json"})
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
        return resp.status, body
    finally:
        conn.close()


@node_router.post(
    "/probe",
    summary="Auto-detect a node",
    description="Probe a network address to discover node type and capabilities.",
)
async def probe_node(request: Request) -> JSONResponse:
    """
    Probe a node's HTTP API to auto-detect its capabilities.

    Makes outbound HTTP requests to the target node:
    1. GET /api/v1/health — reachability check
    2. GET /api/v1/system/info — platform and plugin info
    3. GET /api/v1/system/config — peripheral configuration
    4. GET /api/v1/servo/scan — servo bus discovery

    Returns partial results if later steps fail.

    Uses stdlib http.client instead of httpx because ESP-IDF's httpd
    does not respond to httpx/httpcore's request format.
    """
    import asyncio
    import json as _json

    try:
        body = await request.json()
    except Exception:
        return JSONResponse(status_code=422, content={"detail": "Invalid JSON body"})

    host = body.get("host", "")
    port = body.get("port", 80)
    timeout = body.get("timeout_seconds", 8)

    if not host:
        return JSONResponse(status_code=422, content={"detail": "'host' is required"})

    result: dict[str, Any] = {"reachable": False}
    loop = asyncio.get_running_loop()

    async def _get(path: str) -> tuple[int, dict[str, Any] | None]:
        """Run blocking http.client call in thread pool."""
        try:
            status, body_text = await loop.run_in_executor(
                None, _sync_get, host, port, path, timeout,
            )
            if status == 200:
                return status, _json.loads(body_text)
            return status, None
        except Exception as exc:
            logger.debug("probe_request_failed", path=path, error=str(exc))
            raise

    # Step 1: Health check
    try:
        status, data = await _get("/api/v1/health")
        if status == 200 and data is not None:
            result["reachable"] = True
            result["health"] = data
        else:
            result["health_error"] = f"HTTP {status}"
            return JSONResponse(content=result)
    except Exception as exc:
        result["health_error"] = str(exc)
        return JSONResponse(content=result)

    # Step 2: System info
    try:
        status, data = await _get("/api/v1/system/info")
        if status == 200 and data is not None:
            result["info"] = data
    except Exception as exc:
        result["info_error"] = str(exc)

    # Step 3: System config
    try:
        status, data = await _get("/api/v1/system/config")
        if status == 200 and data is not None:
            result["config"] = data
    except Exception as exc:
        result["config_error"] = str(exc)

    # Step 4: Servo scan
    try:
        status, data = await _get("/api/v1/servo/scan")
        if status == 200 and data is not None:
            result["servo_scan"] = data
    except Exception as exc:
        result["servo_scan_error"] = str(exc)

    logger.info("node_probed", host=host, port=port, reachable=result["reachable"])
    return JSONResponse(content=result)
