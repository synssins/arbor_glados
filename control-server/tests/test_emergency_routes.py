"""
Tests for emergency stop endpoint (C13).

Covers:
- POST /emergency-stop — broadcast to all nodes
- Partial success (some nodes connected, some not)
- No proxy configured
- All nodes disconnected
"""

from __future__ import annotations

from typing import Any
from unittest.mock import AsyncMock

import pytest
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app
from arbor_core.bridges.proxy import ProxyError


class MockProxy:
    """Mock NodeProxy for emergency stop testing."""

    def __init__(self) -> None:
        self.forward = AsyncMock()
        self.forward_to_all = AsyncMock(return_value={})

    @property
    def available_nodes(self) -> list[str]:
        return ["arm-01", "arm-02"]


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


class TestEmergencyStop:
    """POST /api/v1/emergency-stop."""

    async def test_all_nodes_stopped(
        self, client: AsyncClient, mock_proxy: MockProxy
    ) -> None:
        mock_proxy.forward_to_all.return_value = {
            "arm-01": {"stopped": True},
            "arm-02": {"stopped": True},
        }
        resp = await client.post("/api/v1/emergency-stop")
        assert resp.status_code == 200
        data = resp.json()
        assert data["stopped"] is True
        assert "arm-01" in data["nodes"]
        assert "arm-02" in data["nodes"]

    async def test_partial_success(
        self, client: AsyncClient, mock_proxy: MockProxy
    ) -> None:
        mock_proxy.forward_to_all.return_value = {
            "arm-01": {"stopped": True},
            "arm-02": "not connected",
        }
        resp = await client.post("/api/v1/emergency-stop")
        assert resp.status_code == 200
        data = resp.json()
        assert data["stopped"] is True
        assert data["nodes"]["arm-02"] == "not connected"

    async def test_all_disconnected(
        self, client: AsyncClient, mock_proxy: MockProxy
    ) -> None:
        mock_proxy.forward_to_all.return_value = {
            "arm-01": "not connected",
            "arm-02": "not connected",
        }
        resp = await client.post("/api/v1/emergency-stop")
        assert resp.status_code == 503
        assert resp.json()["stopped"] is False

    async def test_no_proxy_configured(self) -> None:
        app = create_app()
        # Don't set node_proxy
        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://test") as client:
            resp = await client.post("/api/v1/emergency-stop")
            assert resp.status_code == 503
            assert resp.json()["stopped"] is False

    async def test_broadcasts_to_all_nodes(
        self, client: AsyncClient, mock_proxy: MockProxy
    ) -> None:
        mock_proxy.forward_to_all.return_value = {"arm-01": {"stopped": True}}
        await client.post("/api/v1/emergency-stop")
        mock_proxy.forward_to_all.assert_called_once_with("POST", "/emergency-stop")
