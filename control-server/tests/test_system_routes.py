"""
Tests for system API endpoints (C10).

Covers:
- GET /system/info — returns platform, version, plugins
- GET /system/health — aggregates plugin health
- GET /system/config — returns config with secrets redacted
- PUT /system/config — admin-only config update with validation
- POST /system/restart — admin-only restart signal
"""

from __future__ import annotations

from typing import Any

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from arbor_core.api.v1.system_routes import system_router
from arbor_core.auth.api_keys import APIKeyManager, APIKeyScope
from arbor_core.auth.middleware import AuthMiddleware
from arbor_core.config.models import ArborConfig
from arbor_core.plugins.base import ArborPlugin, HealthState, HealthStatus
from arbor_core.plugins.manager import PluginManager


# Fake plugin for health tests
class _HealthyPlugin(ArborPlugin):
    NAME = "healthy-test"

    @property
    def name(self) -> str:
        return "healthy-test"

    @property
    def version(self) -> str:
        return "1.0.0"

    @property
    def capabilities(self) -> list[str]:
        return ["test"]

    @property
    def config_schema(self) -> dict[str, Any]:
        return {}

    async def initialize(self, config: dict[str, Any]) -> None:
        pass

    async def shutdown(self) -> None:
        pass

    async def get_state(self) -> dict[str, Any]:
        return {}

    async def handle_command(self, command: str, params: dict[str, Any]) -> dict[str, Any]:
        return {}

    async def health_check(self) -> HealthStatus:
        return HealthStatus(state=HealthState.HEALTHY, message="ok")


def _make_app(
    api_key_mgr: APIKeyManager,
    config: ArborConfig | None = None,
    plugin_mgr: PluginManager | None = None,
) -> FastAPI:
    """Build test app with system routes."""
    from fastapi import APIRouter

    app = FastAPI()
    v1 = APIRouter(prefix="/api/v1")
    v1.include_router(system_router)
    app.include_router(v1)

    app.state.config = config
    app.state.plugin_manager = plugin_mgr
    app.state.api_key_manager = api_key_mgr

    app.add_middleware(
        AuthMiddleware,
        api_key_manager=api_key_mgr,
        default_rate_limit_rpm=1000,
    )
    return app


@pytest.fixture()
def api_key_mgr() -> APIKeyManager:
    return APIKeyManager(min_key_length=32)


@pytest.fixture()
def admin_key(api_key_mgr: APIKeyManager) -> str:
    return api_key_mgr.create_key(name="admin", scopes=[APIKeyScope.ADMIN]).plaintext_key


@pytest.fixture()
def read_key(api_key_mgr: APIKeyManager) -> str:
    return api_key_mgr.create_key(name="reader", scopes=[APIKeyScope.READ]).plaintext_key


def _h(key: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {key}"}


class TestSystemInfo:
    """GET /system/info."""

    async def test_returns_version(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr, config=ArborConfig())
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/info", headers=_h(read_key))
            assert resp.status_code == 200
            data = resp.json()
            assert "arbor_version" in data
            assert "platform" in data
            assert data["node_count"] == 0

    async def test_includes_plugins(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        mgr = PluginManager()
        mgr.register(_HealthyPlugin, name="healthy-test")
        await mgr.initialize_plugin("healthy-test", config={})

        app = _make_app(api_key_mgr, config=ArborConfig(), plugin_mgr=mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/info", headers=_h(read_key))
            data = resp.json()
            assert len(data["loaded_plugins"]) == 1
            assert data["loaded_plugins"][0]["name"] == "healthy-test"

        await mgr.shutdown_all()


class TestSystemHealth:
    """GET /system/health."""

    async def test_healthy_with_no_plugins(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/health", headers=_h(read_key))
            assert resp.status_code == 200
            assert resp.json()["status"] == "healthy"

    async def test_healthy_with_plugins(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        mgr = PluginManager()
        mgr.register(_HealthyPlugin, name="healthy-test")
        await mgr.initialize_plugin("healthy-test", config={})

        app = _make_app(api_key_mgr, plugin_mgr=mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/health", headers=_h(read_key))
            data = resp.json()
            assert data["status"] == "healthy"
            assert "healthy-test" in data["modules"]

        await mgr.shutdown_all()


class TestSystemConfig:
    """GET /system/config."""

    async def test_returns_config(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        cfg = ArborConfig()
        app = _make_app(api_key_mgr, config=cfg)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/config", headers=_h(read_key))
            assert resp.status_code == 200
            data = resp.json()
            assert data["version"] == "1.0"
            assert data["server"]["port"] == 8443

    async def test_secrets_redacted(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        from pathlib import Path

        cfg = ArborConfig(
            security={
                "jwt_private_key_path": Path("/secret/private.pem"),
                "jwt_public_key_path": Path("/secret/public.pem"),
            }
        )
        app = _make_app(api_key_mgr, config=cfg)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/config", headers=_h(read_key))
            data = resp.json()
            assert data["security"]["jwt_private_key_path"] == "***REDACTED***"
            assert data["security"]["jwt_public_key_path"] == "***REDACTED***"

    async def test_no_config_loaded(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr, config=None)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/config", headers=_h(read_key))
            assert resp.status_code == 200


class TestUpdateConfig:
    """PUT /system/config."""

    async def test_admin_can_update(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _make_app(api_key_mgr, config=ArborConfig())
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.put(
                "/api/v1/system/config",
                json={"version": "1.0", "server": {"port": 9999}},
                headers=_h(admin_key),
            )
            assert resp.status_code == 200

    async def test_invalid_config_rejected(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _make_app(api_key_mgr, config=ArborConfig())
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.put(
                "/api/v1/system/config",
                json={"server": {"port": 99999}},  # Out of bounds
                headers=_h(admin_key),
            )
            assert resp.status_code == 422

    async def test_non_admin_rejected(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr, config=ArborConfig())
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.put(
                "/api/v1/system/config",
                json={"version": "1.0"},
                headers=_h(read_key),
            )
            assert resp.status_code == 404


class TestRestart:
    """POST /system/restart."""

    async def test_admin_can_restart(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/system/restart", headers=_h(admin_key)
            )
            assert resp.status_code == 200
            assert app.state.restart_requested is True

    async def test_non_admin_rejected(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/system/restart", headers=_h(read_key)
            )
            assert resp.status_code == 404
