"""
Integration tests — ESP32 bridge with mock node (C22).

End-to-end tests exercising the full stack:
    ServoRoute → NodeProxy → NodeBridge → SerialTransport → MockSerialPort

Uses a mock serial port to simulate a real ESP32 node responding to
API requests through the full proxy chain.
"""

from __future__ import annotations

import json
from typing import Any

import pytest
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app
from arbor_core.bridges.node_bridge import NodeBridge
from arbor_core.bridges.proxy import NodeProxy
from arbor_core.bridges.serial_transport import SerialTransport
from arbor_core.events import EventBus


class MockSerialPort:
    """Mock serial port simulating an ESP32 node."""

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
async def full_stack(mock_serial: MockSerialPort) -> dict[str, Any]:
    """Set up the full stack: app + bridge + proxy + transport."""
    bridge = NodeBridge()
    transport = SerialTransport(
        node_id="arm-01",
        port="/dev/ttyUSB0",
        serial_factory=lambda p, baud: mock_serial,
    )
    bridge.register_node("arm-01", transport)
    await bridge.connect_all()

    event_bus = EventBus()
    proxy = NodeProxy(bridge=bridge, event_bus=event_bus)

    app = create_app()
    app.state.node_proxy = proxy
    app.state.event_bus = event_bus
    app.state.node_bridge = bridge

    client = AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    )

    return {
        "app": app,
        "client": client,
        "mock_serial": mock_serial,
        "bridge": bridge,
        "proxy": proxy,
        "event_bus": event_bus,
    }


class TestServoEndToEnd:
    """Full-stack servo operations."""

    async def test_get_servo_state(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"position": 512, "speed": 100, "load": 30})
        resp = await client.get("/api/v1/servo/1/state")
        assert resp.status_code == 200
        data = resp.json()
        assert data["position"] == 512
        assert data["speed"] == 100

    async def test_set_servo_position(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"ok": True, "position": 1024})
        resp = await client.put(
            "/api/v1/servo/1/position", json={"position": 1024}
        )
        assert resp.status_code == 200
        assert resp.json()["ok"] is True

    async def test_set_servo_torque(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"ok": True, "torque_enabled": True})
        resp = await client.put(
            "/api/v1/servo/1/torque", json={"enabled": True}
        )
        assert resp.status_code == 200

    async def test_servo_sync_move(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"ok": True, "servos_moved": 2})
        resp = await client.post(
            "/api/v1/servo/sync",
            json={"moves": [{"id": 1, "position": 512}, {"id": 2, "position": 256}]},
        )
        assert resp.status_code == 200
        assert resp.json()["servos_moved"] == 2

    async def test_servo_scan(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"servo_ids": [1, 2, 5]})
        resp = await client.get("/api/v1/servo/scan")
        assert resp.status_code == 200
        assert resp.json()["servo_ids"] == [1, 2, 5]


class TestSensorEndToEnd:
    """Full-stack sensor operations."""

    async def test_get_sensor_reading(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"value": 23.5, "unit": "celsius"})
        resp = await client.get("/api/v1/sensor/temp-01/reading")
        assert resp.status_code == 200
        assert resp.json()["value"] == 23.5

    async def test_get_all_sensors(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({
            "sensors": [
                {"id": "temp-01", "value": 23.5},
                {"id": "endstop-01", "value": True},
            ]
        })
        resp = await client.get("/api/v1/sensors")
        assert resp.status_code == 200
        assert len(resp.json()["sensors"]) == 2


class TestEmergencyStopEndToEnd:
    """Full-stack emergency stop."""

    async def test_emergency_stop_through_proxy(
        self, full_stack: dict[str, Any]
    ) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        mock.queue_response({"stopped": True})
        resp = await client.post("/api/v1/emergency-stop")
        assert resp.status_code == 200
        data = resp.json()
        assert data["stopped"] is True
        assert "arm-01" in data["nodes"]


class TestEventPublishingEndToEnd:
    """Events published through the full stack."""

    async def test_put_publishes_event(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]
        event_bus = full_stack["event_bus"]

        queue = await event_bus.subscribe(["servo.*"])
        mock.queue_response({"ok": True})
        await client.put(
            "/api/v1/servo/1/position", json={"position": 512}
        )
        assert not queue.empty()
        event = queue.get_nowait()
        assert "servo" in event.topic
        assert event.data["node_id"] == "arm-01"

    async def test_get_does_not_publish(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]
        event_bus = full_stack["event_bus"]

        queue = await event_bus.subscribe(["*"])
        mock.queue_response({"position": 512})
        await client.get("/api/v1/servo/1/state")
        assert queue.empty()


class TestNodeHealthEndToEnd:
    """Node health through the proxy chain."""

    async def test_node_health_via_system(self, full_stack: dict[str, Any]) -> None:
        """System health endpoint works without plugin manager."""
        client = full_stack["client"]
        resp = await client.get("/api/v1/system/health")
        assert resp.status_code == 200


class TestMultiRequestSequence:
    """Verify request ID sequencing across multiple requests."""

    async def test_sequential_requests(self, full_stack: dict[str, Any]) -> None:
        client = full_stack["client"]
        mock = full_stack["mock_serial"]

        # Queue 3 responses with incrementing IDs
        for i in range(1, 4):
            mock.queue_response({"position": i * 100}, req_id=i)

        for i in range(1, 4):
            resp = await client.get(f"/api/v1/servo/{i}/state")
            assert resp.status_code == 200
            assert resp.json()["position"] == i * 100
