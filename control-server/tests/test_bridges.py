"""
Tests for UART/USB bridge (C08).

Uses a mock serial port to test the full request/response cycle
without requiring actual hardware.

Covers:
- Serial transport connect/disconnect lifecycle
- JSON-line request/response protocol
- Health check via serial
- Node bridge manager (register, connect_all, send, health_check_all)
- Error handling (timeout, invalid response, not connected)
"""

from __future__ import annotations

import json
from typing import Any
from unittest.mock import MagicMock

import pytest

from arbor_core.bridges.node_bridge import NodeBridge
from arbor_core.bridges.serial_transport import SerialTransport
from arbor_core.bridges.transport import TransportError


class MockSerialPort:
    """Mock serial port for testing without hardware."""

    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.baud = baud
        self._is_open = True
        self._response_queue: list[bytes] = []

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
        if self._response_queue:
            return self._response_queue.pop(0)
        return b""

    def queue_response(self, response: dict[str, Any]) -> None:
        """Queue a JSON-line response for the next read."""
        self._response_queue.append(
            (json.dumps(response) + "\n").encode("utf-8")
        )


def _mock_factory(port: str, baud: int) -> MockSerialPort:
    """Factory that creates mock serial ports."""
    return MockSerialPort(port, baud)


@pytest.fixture()
def mock_serial() -> MockSerialPort:
    return MockSerialPort("/dev/ttyUSB0", 921600)


@pytest.fixture()
def transport(mock_serial: MockSerialPort) -> SerialTransport:
    return SerialTransport(
        node_id="test-node",
        port="/dev/ttyUSB0",
        baud=921600,
        serial_factory=lambda p, b: mock_serial,
    )


class TestSerialTransportLifecycle:
    """Connect and disconnect."""

    async def test_connect(self, transport: SerialTransport) -> None:
        await transport.connect()
        assert transport.is_connected
        assert transport.node_id == "test-node"

    async def test_disconnect(self, transport: SerialTransport) -> None:
        await transport.connect()
        await transport.disconnect()
        assert not transport.is_connected

    async def test_connect_idempotent(self, transport: SerialTransport) -> None:
        await transport.connect()
        await transport.connect()  # Should not raise
        assert transport.is_connected

    async def test_not_connected_initially(self) -> None:
        t = SerialTransport(
            node_id="x", port="/dev/null", serial_factory=_mock_factory
        )
        assert not t.is_connected


class TestSerialTransportRequests:
    """Request/response over serial."""

    async def test_send_request(
        self, transport: SerialTransport, mock_serial: MockSerialPort
    ) -> None:
        await transport.connect()

        # Queue a response
        mock_serial.queue_response({
            "id": 1,
            "status": 200,
            "body": {"position": 512},
        })

        result = await transport.send_request("GET", "/api/v1/servo/1/state")
        assert result == {"position": 512}

    async def test_send_request_with_body(
        self, transport: SerialTransport, mock_serial: MockSerialPort
    ) -> None:
        await transport.connect()
        mock_serial.queue_response({"id": 1, "status": 200, "body": {"ok": True}})

        result = await transport.send_request(
            "PUT", "/api/v1/servo/1/position", body={"position": 1024}
        )
        assert result == {"ok": True}

    async def test_send_not_connected_raises(self, transport: SerialTransport) -> None:
        with pytest.raises(TransportError, match="Not connected"):
            await transport.send_request("GET", "/api/v1/health")

    async def test_empty_response_raises(
        self, transport: SerialTransport, mock_serial: MockSerialPort
    ) -> None:
        await transport.connect()
        # No response queued — readline returns b""
        with pytest.raises(TransportError, match="Empty response"):
            await transport.send_request("GET", "/api/v1/health")


class TestSerialTransportHealthCheck:
    """Health check over serial."""

    async def test_healthy_node(
        self, transport: SerialTransport, mock_serial: MockSerialPort
    ) -> None:
        await transport.connect()
        mock_serial.queue_response({
            "id": 1,
            "status": 200,
            "body": {"status": "healthy"},
        })
        assert await transport.health_check() is True

    async def test_unhealthy_response(
        self, transport: SerialTransport, mock_serial: MockSerialPort
    ) -> None:
        await transport.connect()
        mock_serial.queue_response({
            "id": 1,
            "status": 200,
            "body": {"status": "error"},
        })
        assert await transport.health_check() is False

    async def test_disconnected_returns_false(self, transport: SerialTransport) -> None:
        assert await transport.health_check() is False


class TestNodeBridge:
    """Node bridge manager."""

    async def test_register_and_list(self) -> None:
        bridge = NodeBridge()
        t = SerialTransport(
            node_id="arm-01", port="/dev/ttyUSB0", serial_factory=_mock_factory
        )
        bridge.register_node("arm-01", t)
        assert "arm-01" in bridge.node_ids

    async def test_duplicate_register_raises(self) -> None:
        bridge = NodeBridge()
        t = SerialTransport(
            node_id="arm-01", port="/dev/ttyUSB0", serial_factory=_mock_factory
        )
        bridge.register_node("arm-01", t)
        with pytest.raises(TransportError, match="already registered"):
            bridge.register_node("arm-01", t)

    async def test_connect_all(self) -> None:
        bridge = NodeBridge()
        t = SerialTransport(
            node_id="arm-01", port="/dev/ttyUSB0", serial_factory=_mock_factory
        )
        bridge.register_node("arm-01", t)
        results = await bridge.connect_all()
        assert results["arm-01"] is True

    async def test_send_to_node(self) -> None:
        mock = MockSerialPort("/dev/ttyUSB0", 921600)
        bridge = NodeBridge()
        t = SerialTransport(
            node_id="arm-01",
            port="/dev/ttyUSB0",
            serial_factory=lambda p, b: mock,
        )
        bridge.register_node("arm-01", t)
        await bridge.connect_all()

        mock.queue_response({"id": 1, "status": 200, "body": {"ok": True}})
        result = await bridge.send("arm-01", "GET", "/api/v1/health")
        assert result == {"ok": True}

    async def test_send_to_unknown_node(self) -> None:
        bridge = NodeBridge()
        with pytest.raises(TransportError, match="Unknown node"):
            await bridge.send("nope", "GET", "/health")

    async def test_health_check_all(self) -> None:
        mock = MockSerialPort("/dev/ttyUSB0", 921600)
        bridge = NodeBridge()
        t = SerialTransport(
            node_id="arm-01",
            port="/dev/ttyUSB0",
            serial_factory=lambda p, b: mock,
        )
        bridge.register_node("arm-01", t)
        await bridge.connect_all()

        mock.queue_response({"id": 1, "status": 200, "body": {"status": "healthy"}})
        results = await bridge.health_check_all()
        assert results["arm-01"] is True

    async def test_disconnect_all(self) -> None:
        bridge = NodeBridge()
        t = SerialTransport(
            node_id="arm-01", port="/dev/ttyUSB0", serial_factory=_mock_factory
        )
        bridge.register_node("arm-01", t)
        await bridge.connect_all()
        await bridge.disconnect_all()
        assert not t.is_connected
