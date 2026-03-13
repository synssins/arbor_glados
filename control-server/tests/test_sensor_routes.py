"""
Tests for sensor API endpoints (C12).

Covers:
- GET /sensor/{id}/reading
- GET /sensor/{id}/history
- GET /sensors (all sensors)
- Error handling
"""

from __future__ import annotations

from typing import Any
from unittest.mock import AsyncMock

import pytest
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app
from arbor_core.bridges.proxy import ProxyError


class MockProxy:
    """Mock NodeProxy for testing."""

    def __init__(self) -> None:
        self._next_response: dict[str, Any] = {}
        self._fail: bool = False
        self.forward = AsyncMock(side_effect=self._forward)
        self.forward_to_all = AsyncMock(return_value={})

    @property
    def available_nodes(self) -> list[str]:
        return ["arm-01"]

    def set_response(self, response: dict[str, Any]) -> None:
        self._next_response = response

    def set_fail(self) -> None:
        self._fail = True

    async def _forward(
        self,
        node_id: str,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        if self._fail:
            msg = "Node unreachable"
            raise ProxyError(msg)
        return self._next_response


@pytest.fixture()
def mock_proxy() -> MockProxy:
    return MockProxy()


@pytest.fixture()
def app(mock_proxy: MockProxy) -> Any:
    a = create_app()
    a.state.node_proxy = mock_proxy
    return a


@pytest.fixture()
async def client(app: Any) -> AsyncClient:
    transport = ASGITransport(app=app)
    return AsyncClient(transport=transport, base_url="http://test")


class TestGetSensorReading:
    """GET /api/v1/sensor/{id}/reading."""

    async def test_returns_reading(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"value": 23.5, "unit": "celsius"})
        resp = await client.get("/api/v1/sensor/temp-01/reading")
        assert resp.status_code == 200
        assert resp.json()["value"] == 23.5

    async def test_proxy_failure(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_fail()
        resp = await client.get("/api/v1/sensor/temp-01/reading")
        assert resp.status_code == 502

    async def test_with_node_id(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"value": 42.0})
        resp = await client.get("/api/v1/sensor/temp-01/reading?node_id=arm-01")
        assert resp.status_code == 200
        mock_proxy.forward.assert_called_once_with(
            "arm-01", "GET", "/sensor/temp-01/reading"
        )


class TestGetSensorHistory:
    """GET /api/v1/sensor/{id}/history."""

    async def test_returns_history(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({
            "readings": [
                {"value": 23.5, "timestamp": "2026-03-11T10:00:00"},
                {"value": 23.6, "timestamp": "2026-03-11T10:01:00"},
            ]
        })
        resp = await client.get("/api/v1/sensor/temp-01/history")
        assert resp.status_code == 200
        assert len(resp.json()["readings"]) == 2

    async def test_with_limit(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"readings": []})
        resp = await client.get("/api/v1/sensor/temp-01/history?limit=50")
        assert resp.status_code == 200
        # Check the proxy was called with the limit in the path
        call_args = mock_proxy.forward.call_args
        assert "limit=50" in call_args[0][2]

    async def test_proxy_failure(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_fail()
        resp = await client.get("/api/v1/sensor/temp-01/history")
        assert resp.status_code == 502


class TestGetAllSensors:
    """GET /api/v1/sensors."""

    async def test_returns_all(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({
            "sensors": [
                {"id": "temp-01", "value": 23.5, "type": "temperature"},
                {"id": "endstop-01", "value": True, "type": "endstop"},
            ]
        })
        resp = await client.get("/api/v1/sensors")
        assert resp.status_code == 200
        assert len(resp.json()["sensors"]) == 2

    async def test_proxy_failure(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_fail()
        resp = await client.get("/api/v1/sensors")
        assert resp.status_code == 502
