"""
Regression tests for file_routes.py — config file browser API.

Tests path traversal protection, extension filtering, read-only enforcement,
CRUD operations on filesystem sources, and async wrapping of blocking I/O.

Task: Regression tests for file_routes fixes
"""

from __future__ import annotations

import os
import textwrap
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest.mock import MagicMock

import pytest
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app

if TYPE_CHECKING:
    from fastapi import FastAPI


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def tmp_root(tmp_path: Path) -> Path:
    """Create a temporary file root with sample files."""
    cfg_dir = tmp_path / "config"
    cfg_dir.mkdir()
    (cfg_dir / "printer.cfg").write_text("[printer]\nkinematics = corexy\n", encoding="utf-8")
    (cfg_dir / "macros.cfg").write_text("[gcode_macro HOME]\ngcode: G28\n", encoding="utf-8")
    (cfg_dir / "secret.env").write_text("API_KEY=hunter2\n", encoding="utf-8")
    sub = cfg_dir / "includes"
    sub.mkdir()
    (sub / "steppers.cfg").write_text("[stepper_x]\nstep_pin: 1\n", encoding="utf-8")
    return cfg_dir


@pytest.fixture
def app_with_roots(tmp_root: Path) -> "FastAPI":
    """Create app with filesystem file roots configured."""
    app = create_app(config=None)

    # Inject file_roots into app config
    mock_config = MagicMock()
    mock_config.file_roots = [
        {
            "id": "test_config",
            "label": "Test Config",
            "base_path": str(tmp_root),
            "source": "filesystem",
            "readonly": False,
            "restart_command": None,
            "allowed_extensions": [".cfg", ".yaml", ".yml", ".conf"],
            "max_file_size": 1048576,
        },
        {
            "id": "readonly_root",
            "label": "Read Only",
            "base_path": str(tmp_root),
            "source": "filesystem",
            "readonly": True,
            "restart_command": None,
            "allowed_extensions": [".cfg"],
            "max_file_size": 1048576,
        },
    ]
    mock_config.robotics = None
    app.state.config = mock_config
    return app


@pytest.fixture
async def ac(app_with_roots: "FastAPI"):
    """Async HTTP client for testing."""
    async with AsyncClient(
        transport=ASGITransport(app=app_with_roots),
        base_url="http://test/api/v1",
    ) as client:
        yield client


# ---------------------------------------------------------------------------
# List roots
# ---------------------------------------------------------------------------

class TestListRoots:

    @pytest.mark.asyncio
    async def test_list_roots_returns_configured_roots(self, ac: AsyncClient):
        resp = await ac.get("/files/roots")
        assert resp.status_code == 200
        data = resp.json()
        assert data["count"] == 2
        ids = [r["id"] for r in data["roots"]]
        assert "test_config" in ids
        assert "readonly_root" in ids

    @pytest.mark.asyncio
    async def test_list_roots_includes_readonly_flag(self, ac: AsyncClient):
        resp = await ac.get("/files/roots")
        roots = {r["id"]: r for r in resp.json()["roots"]}
        assert roots["test_config"]["readonly"] is False
        assert roots["readonly_root"]["readonly"] is True


# ---------------------------------------------------------------------------
# List directory
# ---------------------------------------------------------------------------

class TestListDirectory:

    @pytest.mark.asyncio
    async def test_list_root_directory(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/list")
        assert resp.status_code == 200
        data = resp.json()
        names = [i["name"] for i in data["items"]]
        assert "printer.cfg" in names
        assert "macros.cfg" in names
        assert "includes" in names

    @pytest.mark.asyncio
    async def test_list_subdirectory(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/list?path=includes")
        assert resp.status_code == 200
        data = resp.json()
        names = [i["name"] for i in data["items"]]
        assert "steppers.cfg" in names

    @pytest.mark.asyncio
    async def test_list_unknown_root_returns_404(self, ac: AsyncClient):
        resp = await ac.get("/files/nonexistent/list")
        assert resp.status_code == 404

    @pytest.mark.asyncio
    async def test_list_filters_by_allowed_extensions(self, ac: AsyncClient):
        """Files with disallowed extensions should not appear."""
        resp = await ac.get("/files/test_config/list")
        names = [i["name"] for i in resp.json()["items"]]
        # .env is not in allowed_extensions [.cfg, .yaml, .yml, .conf]
        assert "secret.env" not in names

    @pytest.mark.asyncio
    async def test_list_hides_dotfiles(self, ac: AsyncClient, tmp_root: Path):
        """Hidden files (starting with .) should not appear."""
        (tmp_root / ".hidden_file").write_text("secret", encoding="utf-8")
        resp = await ac.get("/files/test_config/list")
        names = [i["name"] for i in resp.json()["items"]]
        assert ".hidden_file" not in names

    @pytest.mark.asyncio
    async def test_directories_sorted_before_files(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/list")
        items = resp.json()["items"]
        dirs = [i for i in items if i["type"] == "directory"]
        files = [i for i in items if i["type"] == "file"]
        # All dirs should come before all files in the list
        if dirs and files:
            last_dir_idx = max(items.index(d) for d in dirs)
            first_file_idx = min(items.index(f) for f in files)
            assert last_dir_idx < first_file_idx


# ---------------------------------------------------------------------------
# Path traversal protection
# ---------------------------------------------------------------------------

class TestPathTraversal:

    @pytest.mark.asyncio
    async def test_list_path_traversal_returns_404(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/list?path=../../etc")
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"

    @pytest.mark.asyncio
    async def test_read_path_traversal_returns_404(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/read?path=../../etc/passwd")
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"

    @pytest.mark.asyncio
    async def test_write_path_traversal_returns_404(self, ac: AsyncClient):
        resp = await ac.put(
            "/files/test_config/write",
            json={"path": "../../etc/evil.cfg", "content": "pwned"},
        )
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"

    @pytest.mark.asyncio
    async def test_delete_path_traversal_returns_404(self, ac: AsyncClient):
        resp = await ac.delete("/files/test_config/delete?path=../../etc/passwd")
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"

    @pytest.mark.asyncio
    async def test_encoded_traversal_returns_404(self, ac: AsyncClient):
        """Percent-encoded traversal should also be blocked."""
        resp = await ac.get("/files/test_config/read?path=..%2F..%2Fetc%2Fpasswd")
        assert resp.status_code == 404


# ---------------------------------------------------------------------------
# Extension filtering
# ---------------------------------------------------------------------------

class TestExtensionFiltering:

    @pytest.mark.asyncio
    async def test_read_disallowed_extension_returns_404(self, ac: AsyncClient):
        """Reading a file with a disallowed extension returns 404."""
        resp = await ac.get("/files/test_config/read?path=secret.env")
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"

    @pytest.mark.asyncio
    async def test_write_disallowed_extension_returns_404(self, ac: AsyncClient):
        """Writing a file with a disallowed extension returns 404."""
        resp = await ac.put(
            "/files/test_config/write",
            json={"path": "evil.sh", "content": "#!/bin/bash\nrm -rf /"},
        )
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"


# ---------------------------------------------------------------------------
# Read file
# ---------------------------------------------------------------------------

class TestReadFile:

    @pytest.mark.asyncio
    async def test_read_existing_file(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/read?path=printer.cfg")
        assert resp.status_code == 200
        data = resp.json()
        assert "printer" in data["content"].lower()
        assert data["path"] == "printer.cfg"
        assert data["readonly"] is False

    @pytest.mark.asyncio
    async def test_read_file_in_subdirectory(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/read?path=includes/steppers.cfg")
        assert resp.status_code == 200
        assert "stepper_x" in resp.json()["content"]

    @pytest.mark.asyncio
    async def test_read_nonexistent_file_returns_404(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/read?path=nonexistent.cfg")
        assert resp.status_code == 404

    @pytest.mark.asyncio
    async def test_read_missing_path_returns_422(self, ac: AsyncClient):
        resp = await ac.get("/files/test_config/read")
        assert resp.status_code == 422

    @pytest.mark.asyncio
    async def test_read_oversized_file_returns_413(self, ac: AsyncClient, tmp_root: Path):
        """Files exceeding max_file_size should be rejected."""
        big_file = tmp_root / "huge.cfg"
        big_file.write_text("x" * (1048576 + 1), encoding="utf-8")
        resp = await ac.get("/files/test_config/read?path=huge.cfg")
        assert resp.status_code == 413

    @pytest.mark.asyncio
    async def test_read_readonly_root_sets_flag(self, ac: AsyncClient):
        resp = await ac.get("/files/readonly_root/read?path=printer.cfg")
        assert resp.status_code == 200
        assert resp.json()["readonly"] is True


# ---------------------------------------------------------------------------
# Write file
# ---------------------------------------------------------------------------

class TestWriteFile:

    @pytest.mark.asyncio
    async def test_write_new_file(self, ac: AsyncClient, tmp_root: Path):
        resp = await ac.put(
            "/files/test_config/write",
            json={"path": "new_config.cfg", "content": "[extruder]\ntemp: 200\n"},
        )
        assert resp.status_code == 200
        assert resp.json()["ok"] is True
        assert (tmp_root / "new_config.cfg").read_text(encoding="utf-8") == "[extruder]\ntemp: 200\n"

    @pytest.mark.asyncio
    async def test_write_creates_backup(self, ac: AsyncClient, tmp_root: Path):
        """Writing an existing file should create a .bak backup."""
        original = (tmp_root / "printer.cfg").read_text(encoding="utf-8")
        resp = await ac.put(
            "/files/test_config/write",
            json={"path": "printer.cfg", "content": "[printer]\nkinematics = delta\n"},
        )
        assert resp.status_code == 200
        backup = tmp_root / "printer.cfg.bak"
        assert backup.exists()
        assert backup.read_text(encoding="utf-8") == original

    @pytest.mark.asyncio
    async def test_write_readonly_root_returns_403(self, ac: AsyncClient):
        resp = await ac.put(
            "/files/readonly_root/write",
            json={"path": "printer.cfg", "content": "hacked"},
        )
        assert resp.status_code == 403
        assert "read-only" in resp.json()["detail"]

    @pytest.mark.asyncio
    async def test_write_missing_path_returns_422(self, ac: AsyncClient):
        resp = await ac.put(
            "/files/test_config/write",
            json={"content": "no path field"},
        )
        assert resp.status_code == 422

    @pytest.mark.asyncio
    async def test_write_oversized_content_returns_413(self, ac: AsyncClient):
        resp = await ac.put(
            "/files/test_config/write",
            json={"path": "big.cfg", "content": "x" * (1048576 + 1)},
        )
        assert resp.status_code == 413


# ---------------------------------------------------------------------------
# Delete file
# ---------------------------------------------------------------------------

class TestDeleteFile:

    @pytest.mark.asyncio
    async def test_delete_existing_file(self, ac: AsyncClient, tmp_root: Path):
        target = tmp_root / "macros.cfg"
        assert target.exists()
        resp = await ac.delete("/files/test_config/delete?path=macros.cfg")
        assert resp.status_code == 200
        assert resp.json()["ok"] is True
        assert not target.exists()

    @pytest.mark.asyncio
    async def test_delete_nonexistent_returns_404(self, ac: AsyncClient):
        resp = await ac.delete("/files/test_config/delete?path=nope.cfg")
        assert resp.status_code == 404

    @pytest.mark.asyncio
    async def test_delete_readonly_root_returns_403(self, ac: AsyncClient):
        resp = await ac.delete("/files/readonly_root/delete?path=printer.cfg")
        assert resp.status_code == 403

    @pytest.mark.asyncio
    async def test_delete_directory_returns_400(self, ac: AsyncClient):
        resp = await ac.delete("/files/test_config/delete?path=includes")
        assert resp.status_code == 400

    @pytest.mark.asyncio
    async def test_delete_missing_path_returns_422(self, ac: AsyncClient):
        resp = await ac.delete("/files/test_config/delete")
        assert resp.status_code == 422


# ---------------------------------------------------------------------------
# Anti-enumeration (404 vs 403)
# ---------------------------------------------------------------------------

class TestAntiEnumeration:
    """Security-sensitive denials must return 404 with generic message."""

    @pytest.mark.asyncio
    async def test_traversal_does_not_leak_403(self, ac: AsyncClient):
        """Path traversal must return 404 'Not found', never 403."""
        for path in ["../../etc/passwd", "../../../root/.ssh/id_rsa"]:
            resp = await ac.get(f"/files/test_config/read?path={path}")
            assert resp.status_code == 404, f"Expected 404 for traversal path: {path}"
            assert resp.json()["detail"] == "Not found"

    @pytest.mark.asyncio
    async def test_blocked_extension_does_not_leak_403(self, ac: AsyncClient):
        """Blocked extensions must return 404, not 403."""
        resp = await ac.get("/files/test_config/read?path=secret.env")
        assert resp.status_code == 404
        assert resp.json()["detail"] == "Not found"
