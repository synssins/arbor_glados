"""
UART/USB serial transport for ESP32 nodes.

Implements the NodeTransport interface over a serial port connection.
Uses a simple JSON-line protocol for request/response framing:

    Request:  {"method": "GET", "path": "/api/v1/health", "id": 1}\n
    Response: {"id": 1, "status": 200, "body": {...}}\n

The serial port is managed via pyserial (when available) or a pluggable
backend for testing. All I/O runs in a thread executor to avoid blocking
the asyncio event loop.

NOTE (Phase 1): The ESP32 firmware does not yet implement a JSON-line
protocol handler on UART/USB-CDC — the UART is dedicated to the Feetech
STS servo bus. Phase 1 uses WiFi/HTTP transport (HTTPTransport) for all
node communication. This serial transport is retained for Phase 2 when
a firmware-side JSON-line dispatcher may be added for wired operation.
See decisions/DECISION_REQUEST_3.md.

Task: C08
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Protocol

import structlog

from arbor_core.bridges.transport import NodeTransport, TransportError

logger = structlog.get_logger(__name__)


class SerialPort(Protocol):
    """Protocol for serial port objects (compatible with pyserial.Serial)."""

    def open(self) -> None: ...
    def close(self) -> None: ...
    def write(self, data: bytes) -> int | None: ...
    def readline(self) -> bytes: ...

    @property
    def is_open(self) -> bool: ...


class SerialTransport(NodeTransport):
    """
    UART/USB serial transport to ESP32 nodes.

    Uses a JSON-line protocol over serial. Each request is a JSON object
    terminated by newline, and the response is likewise a JSON line.

    Args:
        node_id: The node identifier.
        port: Serial port path (e.g. '/dev/ttyUSB0', 'COM3').
        baud: Baud rate. Default 921600 per project plan.
        timeout: Read timeout in seconds.
        serial_factory: Optional factory for creating serial port objects.
                        If None, uses pyserial. Inject a mock for testing.
    """

    def __init__(
        self,
        node_id: str,
        port: str,
        baud: int = 921600,
        timeout: float = 5.0,
        serial_factory: Any = None,
    ) -> None:
        self._node_id = node_id
        self._port_path = port
        self._baud = baud
        self._timeout = timeout
        self._serial_factory = serial_factory
        self._serial: SerialPort | None = None
        self._request_id = 0
        self._lock = asyncio.Lock()

    @property
    def node_id(self) -> str:
        return self._node_id

    @property
    def is_connected(self) -> bool:
        return self._serial is not None and self._serial.is_open

    async def connect(self) -> None:
        """Open the serial port."""
        if self.is_connected:
            return

        loop = asyncio.get_event_loop()
        try:
            if self._serial_factory is not None:
                self._serial = await loop.run_in_executor(
                    None, self._serial_factory, self._port_path, self._baud
                )
            else:
                # Import pyserial lazily to avoid hard dependency
                try:
                    import serial as pyserial
                except ImportError as exc:
                    msg = (
                        "pyserial is required for serial transport. "
                        "Install with: pip install pyserial"
                    )
                    raise TransportError(msg) from exc

                self._serial = await loop.run_in_executor(
                    None,
                    lambda: pyserial.Serial(
                        self._port_path,
                        self._baud,
                        timeout=self._timeout,
                    ),
                )

            logger.info(
                "serial_connected",
                node_id=self._node_id,
                port=self._port_path,
                baud=self._baud,
            )
        except TransportError:
            raise
        except Exception as exc:
            msg = f"Failed to open serial port {self._port_path}: {exc}"
            raise TransportError(msg) from exc

    async def disconnect(self) -> None:
        """Close the serial port."""
        if self._serial is not None:
            loop = asyncio.get_event_loop()
            try:
                await loop.run_in_executor(None, self._serial.close)
            except Exception:
                logger.exception("serial_close_error", node_id=self._node_id)
            self._serial = None
            logger.info("serial_disconnected", node_id=self._node_id)

    async def send_request(
        self, method: str, path: str, body: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        """
        Send a JSON-line request and read the response.

        Thread-safe via asyncio lock.
        """
        if not self.is_connected:
            msg = f"Not connected to node {self._node_id}"
            raise TransportError(msg)

        async with self._lock:
            self._request_id += 1
            req_id = self._request_id

            request = {
                "id": req_id,
                "method": method,
                "path": path,
            }
            if body is not None:
                request["body"] = body

            request_bytes = (json.dumps(request) + "\n").encode("utf-8")

            loop = asyncio.get_event_loop()

            # Write request
            try:
                await loop.run_in_executor(
                    None, self._serial.write, request_bytes  # type: ignore[union-attr]
                )
            except Exception as exc:
                msg = f"Serial write failed for node {self._node_id}: {exc}"
                raise TransportError(msg) from exc

            # Read response
            try:
                response_bytes = await asyncio.wait_for(
                    loop.run_in_executor(
                        None, self._serial.readline  # type: ignore[union-attr]
                    ),
                    timeout=self._timeout,
                )
            except TimeoutError as exc:
                msg = f"Timeout waiting for response from node {self._node_id}"
                raise TransportError(msg) from exc
            except Exception as exc:
                msg = f"Serial read failed for node {self._node_id}: {exc}"
                raise TransportError(msg) from exc

            # Parse response
            if not response_bytes:
                msg = f"Empty response from node {self._node_id}"
                raise TransportError(msg)

            try:
                response = json.loads(response_bytes.decode("utf-8").strip())
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                msg = f"Invalid response from node {self._node_id}: {exc}"
                raise TransportError(msg) from exc

            # Validate response ID matches request
            if response.get("id") != req_id:
                logger.warning(
                    "serial_response_id_mismatch",
                    expected=req_id,
                    got=response.get("id"),
                    node_id=self._node_id,
                )

            return response.get("body", response)

    async def health_check(self) -> bool:
        """Send a health check request to the node."""
        if not self.is_connected:
            return False

        try:
            result = await self.send_request("GET", "/api/v1/health")
            return result.get("status") == "healthy"
        except TransportError:
            return False
