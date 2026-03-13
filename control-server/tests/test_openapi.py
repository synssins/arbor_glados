"""
Tests for OpenAPI spec generation (C17).

Verifies that the auto-generated OpenAPI spec:
- Is served at /api/openapi.json
- Swagger UI is at /api/docs
- ReDoc is at /api/redoc
- Contains all registered endpoints from C10-C15
- Has correct metadata
"""

from __future__ import annotations

import pytest
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app


@pytest.fixture()
async def client() -> AsyncClient:
    app = create_app()
    transport = ASGITransport(app=app)
    return AsyncClient(transport=transport, base_url="http://test")


class TestOpenAPISpec:
    """OpenAPI spec generation and serving."""

    async def test_openapi_json_served(self, client: AsyncClient) -> None:
        resp = await client.get("/api/openapi.json")
        assert resp.status_code == 200
        spec = resp.json()
        assert spec["openapi"].startswith("3.")
        assert spec["info"]["title"] == "Arbor Control Server"

    async def test_swagger_ui_served(self, client: AsyncClient) -> None:
        resp = await client.get("/api/docs")
        assert resp.status_code == 200
        assert "text/html" in resp.headers["content-type"]

    async def test_redoc_served(self, client: AsyncClient) -> None:
        resp = await client.get("/api/redoc")
        assert resp.status_code == 200
        assert "text/html" in resp.headers["content-type"]

    async def test_spec_contains_health(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        assert "/api/v1/health" in spec["paths"]

    async def test_spec_contains_system_endpoints(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        paths = spec["paths"]
        assert "/api/v1/system/info" in paths
        assert "/api/v1/system/health" in paths
        assert "/api/v1/system/config" in paths
        assert "/api/v1/system/restart" in paths

    async def test_spec_contains_auth_endpoints(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        paths = spec["paths"]
        assert "/api/v1/auth/login" in paths
        assert "/api/v1/auth/api-keys" in paths

    async def test_spec_contains_servo_endpoints(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        paths = spec["paths"]
        assert "/api/v1/servo/{servo_id}/state" in paths
        assert "/api/v1/servo/{servo_id}/position" in paths
        assert "/api/v1/servo/{servo_id}/speed" in paths
        assert "/api/v1/servo/{servo_id}/torque" in paths
        assert "/api/v1/servo/sync" in paths
        assert "/api/v1/servo/scan" in paths

    async def test_spec_contains_sensor_endpoints(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        paths = spec["paths"]
        assert "/api/v1/sensor/{sensor_id}/reading" in paths
        assert "/api/v1/sensor/{sensor_id}/history" in paths
        assert "/api/v1/sensors" in paths

    async def test_spec_contains_emergency_stop(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        assert "/api/v1/emergency-stop" in spec["paths"]

    async def test_websocket_not_in_openapi(self, client: AsyncClient) -> None:
        """WebSocket routes are excluded from OpenAPI spec by design."""
        spec = (await client.get("/api/openapi.json")).json()
        # FastAPI does not include WebSocket endpoints in OpenAPI
        assert "/api/v1/ws" not in spec["paths"]

    async def test_version_in_spec(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        assert spec["info"]["version"] == "0.1.0"

    async def test_all_tags_present(self, client: AsyncClient) -> None:
        spec = (await client.get("/api/openapi.json")).json()
        # Collect all tags used across paths
        tags_used: set[str] = set()
        for path_methods in spec["paths"].values():
            for method_info in path_methods.values():
                if isinstance(method_info, dict) and "tags" in method_info:
                    tags_used.update(method_info["tags"])
        # Should have tags for our main resource groups
        assert "system" in tags_used
        assert "servo" in tags_used
        assert "sensor" in tags_used
        assert "emergency" in tags_used
