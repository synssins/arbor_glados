"""
Tests for proxy layer (C09).

Covers:
- Forward request to a single node
- Forward to all nodes (broadcast)
- Path normalization (/api/v1/ prefix)
- Event publishing on write operations
- Error handling (unknown node, disconnected, transport failure)
- Node health check via proxy
- Path-to-topic conversion
"""

from __future__ import annotations

import json
from typing import Any

import pytest

from arbor_core.bridges.node_bridge import NodeBridge
from arbor_core.bridges.proxy import NodeProxy, ProxyError
from arbor_core.bridges.serial_transport import SerialTransport
from arbor_core.events import EventBus


class MockSerialPort:
    """Mock serial port."""

    def __init__(self, port: str, baud: int) -> None:
        self._is_open = True
        self._responses: list[bytes] = []

    @property
    def is_open(self) -> bool:
        return self._is_open

    def open(self) -> None:
        self._is_open = True

    def close(self) -> None:
        self._is_open = False

    def write(self, data: bytes) -> int:
        return len(data)

    def readline(self) -> bytes:
        if self._responses:
            return self._responses.pop(0)
        return b""

    def queue_response(self, body: dict[str, Any], req_id: int = 1) -> None:
        resp = {"id": req_id, "status": 200, "body": body}
        self._responses.append((json.dumps(resp) + "\n").encode())


@pytest.fixture()
def mock_serial() -> MockSerialPort:
    return MockSerialPort("/dev/ttyUSB0", 921600)


@pytest.fixture()
def bridge(mock_serial: MockSerialPort) -> NodeBridge:
    b = NodeBridge()
    t = SerialTransport(
        node_id="arm-01",
        port="/dev/ttyUSB0",
        serial_factory=lambda p, baud: mock_serial,
    )
    b.register_node("arm-01", t)
    return b


@pytest.fixture()
def event_bus() -> EventBus:
    return EventBus()


@pytest.fixture()
def proxy(bridge: NodeBridge, event_bus: EventBus) -> NodeProxy:
    return NodeProxy(bridge=bridge, event_bus=event_bus)


class TestForward:
    """Forward requests to nodes."""

    async def test_forward_get(
        self, proxy: NodeProxy, bridge: NodeBridge, mock_serial: MockSerialPort
    ) -> None:
        await bridge.connect_all()
        mock_serial.queue_response({"position": 512})
        result = await proxy.forward("arm-01", "GET", "/servo/1/state")
        assert result == {"position": 512}

    async def test_forward_put(
        self, proxy: NodeProxy, bridge: NodeBridge, mock_serial: MockSerialPort
    ) -> None:
        await bridge.connect_all()
        mock_serial.queue_response({"ok": True})
        result = await proxy.forward(
            "arm-01", "PUT", "/servo/1/position", {"position": 1024}
        )
        assert result == {"ok": True}

    async def test_forward_unknown_node(self, proxy: NodeProxy) -> None:
        with pytest.raises(ProxyError, match="failed"):
            await proxy.forward("nonexistent", "GET", "/health")

    async def test_forward_disconnected_node(
        self, proxy: NodeProxy, bridge: NodeBridge
    ) -> None:
        # Don't connect — transport is not connected
        with pytest.raises(ProxyError, match="failed"):
            await proxy.forward("arm-01", "GET", "/health")

    async def test_path_normalization(
        self, proxy: NodeProxy, bridge: NodeBridge, mock_serial: MockSerialPort
    ) -> None:
        await bridge.connect_all()
        mock_serial.queue_response({"status": "healthy"})
        # Should add /api/v1/ prefix
        result = await proxy.forward("arm-01", "GET", "health")
        assert result == {"status": "healthy"}

    async def test_already_prefixed_path(
        self, proxy: NodeProxy, bridge: NodeBridge, mock_serial: MockSerialPort
    ) -> None:
        await bridge.connect_all()
        mock_serial.queue_response({"info": "test"})
        result = await proxy.forward("arm-01", "GET", "/api/v1/system/info")
        assert result == {"info": "test"}


class TestForwardToAll:
    """Broadcast to all nodes."""

    async def test_broadcast(
        self, proxy: NodeProxy, bridge: NodeBridge, mock_serial: MockSerialPort
    ) -> None:
        await bridge.connect_all()
        mock_serial.queue_response({"stopped": True})
        results = await proxy.forward_to_all("POST", "/emergency-stop")
        assert "arm-01" in results
        assert results["arm-01"] == {"stopped": True}

    async def test_broadcast_disconnected(self, proxy: NodeProxy) -> None:
        results = await proxy.forward_to_all("POST", "/emergency-stop")
        assert results["arm-01"] == "not connected"


class TestEventPublishing:
    """Events published on write operations."""

    async def test_put_publishes_event(
        self,
        proxy: NodeProxy,
        bridge: NodeBridge,
        mock_serial: MockSerialPort,
        event_bus: EventBus,
    ) -> None:
        await bridge.connect_all()
        queue = await event_bus.subscribe(["servo.*"])
        mock_serial.queue_response({"ok": True})
        await proxy.forward("arm-01", "PUT", "/servo/1/position", {"position": 512})
        assert not queue.empty()
        event = queue.get_nowait()
        assert "servo" in event.topic
        assert event.data["node_id"] == "arm-01"

    async def test_get_does_not_publish(
        self,
        proxy: NodeProxy,
        bridge: NodeBridge,
        mock_serial: MockSerialPort,
        event_bus: EventBus,
    ) -> None:
        await bridge.connect_all()
        queue = await event_bus.subscribe(["*"])
        mock_serial.queue_response({"position": 512})
        await proxy.forward("arm-01", "GET", "/servo/1/state")
        assert queue.empty()


class TestNodeHealth:
    """Node health via proxy."""

    async def test_healthy_node(
        self, proxy: NodeProxy, bridge: NodeBridge, mock_serial: MockSerialPort
    ) -> None:
        await bridge.connect_all()
        mock_serial.queue_response({"status": "healthy"})
        result = await proxy.node_health("arm-01")
        assert result.get("status") == "healthy"

    async def test_unreachable_node(self, proxy: NodeProxy) -> None:
        result = await proxy.node_health("arm-01")
        assert result["status"] == "unreachable"


class TestAvailableNodes:
    """Node listing."""

    def test_lists_registered_nodes(self, proxy: NodeProxy) -> None:
        assert proxy.available_nodes == ["arm-01"]


class TestPathToTopic:
    """Topic generation from paths."""

    def test_servo_position(self) -> None:
        assert NodeProxy._path_to_topic("/servo/1/position", "PUT") == "servo.position_changed"

    def test_servo_torque(self) -> None:
        assert NodeProxy._path_to_topic("/servo/1/torque", "PUT") == "servo.torque_changed"

    def test_servo_state(self) -> None:
        assert NodeProxy._path_to_topic("/servo/1", "GET") == "servo.state_changed"

    def test_emergency_stop(self) -> None:
        assert NodeProxy._path_to_topic("/emergency-stop", "POST") == "emergency-stop.command"

    def test_api_prefix_stripped(self) -> None:
        topic = NodeProxy._path_to_topic("/api/v1/servo/1/position", "PUT")
        assert topic == "servo.position_changed"
