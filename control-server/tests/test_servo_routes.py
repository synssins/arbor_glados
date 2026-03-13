"""
Tests for servo API endpoints (C11).

Covers:
- GET /servo/{id}/state
- PUT /servo/{id}/position
- PUT /servo/{id}/speed
- PUT /servo/{id}/torque
- POST /servo/sync
- GET /servo/scan
- Error handling (no proxy, no nodes, proxy failure)
"""

from __future__ import annotations

from typing import Any
from unittest.mock import AsyncMock

import pytest
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app
from arbor_core.bridges.proxy import NodeProxy, ProxyError


class MockProxy:
    """Mock NodeProxy for testing."""

    def __init__(self) -> None:
        self._responses: dict[str, Any] = {}
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
        return getattr(self, "_next_response", {"ok": True})


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


class TestGetServoState:
    """GET /api/v1/servo/{id}/state."""

    async def test_returns_state(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"position": 512, "speed": 100, "load": 50})
        resp = await client.get("/api/v1/servo/1/state")
        assert resp.status_code == 200
        assert resp.json()["position"] == 512

    async def test_proxy_failure(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_fail()
        resp = await client.get("/api/v1/servo/1/state")
        assert resp.status_code == 502

    async def test_with_node_id(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"position": 256})
        resp = await client.get("/api/v1/servo/1/state?node_id=arm-01")
        assert resp.status_code == 200
        mock_proxy.forward.assert_called_once_with("arm-01", "GET", "/servo/1/state")


class TestSetServoPosition:
    """PUT /api/v1/servo/{id}/position."""

    async def test_set_position(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"ok": True})
        resp = await client.put(
            "/api/v1/servo/1/position", json={"position": 1024}
        )
        assert resp.status_code == 200
        assert resp.json()["ok"] is True

    async def test_proxy_failure(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_fail()
        resp = await client.put(
            "/api/v1/servo/1/position", json={"position": 1024}
        )
        assert resp.status_code == 502


class TestSetServoSpeed:
    """PUT /api/v1/servo/{id}/speed."""

    async def test_set_speed(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"ok": True})
        resp = await client.put(
            "/api/v1/servo/5/speed", json={"speed": 500}
        )
        assert resp.status_code == 200


class TestSetServoTorque:
    """PUT /api/v1/servo/{id}/torque."""

    async def test_set_torque(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"ok": True})
        resp = await client.put(
            "/api/v1/servo/1/torque", json={"enabled": True}
        )
        assert resp.status_code == 200


class TestServoSync:
    """POST /api/v1/servo/sync."""

    async def test_sync_move(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"ok": True, "servos_moved": 3})
        resp = await client.post(
            "/api/v1/servo/sync",
            json={"moves": [{"id": 1, "position": 512}, {"id": 2, "position": 256}]},
        )
        assert resp.status_code == 200


class TestServoScan:
    """GET /api/v1/servo/scan."""

    async def test_scan(self, client: AsyncClient, mock_proxy: MockProxy) -> None:
        mock_proxy.set_response({"servo_ids": [1, 2, 5, 7]})
        resp = await client.get("/api/v1/servo/scan")
        assert resp.status_code == 200
        assert resp.json()["servo_ids"] == [1, 2, 5, 7]


class TestNoProxy:
    """Endpoints when no proxy is configured."""

    async def test_no_proxy_returns_502(self) -> None:
        app = create_app()
        # Don't set node_proxy
        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://test") as client:
            resp = await client.get("/api/v1/servo/1/state")
            assert resp.status_code == 502
