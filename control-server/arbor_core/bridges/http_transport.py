"""
HTTP transport for WiFi/Ethernet ESP32 nodes.

Implements the NodeTransport interface over HTTP using stdlib http.client.
Each request creates a fresh HTTPConnection — ESP32 nodes cannot hold
persistent connections, and http.client's minimal headers are the only
ones ESP-IDF's httpd reliably accepts.

Why not httpx?
    ESP-IDF's lightweight httpd silently drops requests that include
    Accept-Encoding, chunked Transfer-Encoding, or other headers that
    httpx/httpcore adds automatically. stdlib http.client sends clean,
    minimal HTTP/1.1 that ESP-IDF handles correctly. This is the same
    approach proven in node_routes._sync_get().

Task: Node Integration Step 1
"""

from __future__ import annotations

import asyncio
import http.client
import json
from typing import Any

import structlog

from arbor_core.bridges.transport import NodeTransport, TransportError

logger = structlog.get_logger(__name__)


class HTTPTransport(NodeTransport):
    """
    HTTP transport for WiFi/Ethernet ESP32 nodes.

    Uses stdlib http.client for maximum compatibility with ESP-IDF's httpd.
    All I/O runs in a thread executor to avoid blocking the asyncio loop.

    Args:
        node_id: The node identifier.
        host: IP address or hostname of the node.
        port: HTTP port (default 80).
        timeout: Request timeout in seconds.
    """

    def __init__(
        self,
        node_id: str,
        host: str,
        port: int = 80,
        timeout: float = 8.0,
    ) -> None:
        self._node_id = node_id
        self._host = host
        self._port = port
        self._timeout = timeout
        self._connected = False
        self._lock = asyncio.Lock()

    @property
    def node_id(self) -> str:
        return self._node_id

    @property
    def is_connected(self) -> bool:
        return self._connected

    def _sync_request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """
        Blocking HTTP request using stdlib http.client.

        Creates a fresh connection per request — ESP32 cannot hold
        persistent connections anyway.
        """
        conn = http.client.HTTPConnection(
            self._host, self._port, timeout=self._timeout
        )
        try:
            headers: dict[str, str] = {"Accept": "application/json"}
            body_bytes: bytes | None = None

            if body is not None:
                body_bytes = json.dumps(body).encode("utf-8")
                headers["Content-Type"] = "application/json"
                headers["Content-Length"] = str(len(body_bytes))

            conn.request(method, path, body=body_bytes, headers=headers)
            resp = conn.getresponse()
            resp_body = resp.read().decode("utf-8", errors="replace")

            if resp.status >= 400:
                msg = (
                    f"HTTP {resp.status} from node {self._node_id} "
                    f"({method} {path}): {resp_body[:200]}"
                )
                raise TransportError(msg)

            if not resp_body.strip():
                return {}

            return json.loads(resp_body)

        except TransportError:
            raise
        except json.JSONDecodeError as exc:
            msg = f"Invalid JSON from node {self._node_id} ({method} {path}): {exc}"
            raise TransportError(msg) from exc
        except Exception as exc:
            msg = f"HTTP request to node {self._node_id} failed ({method} {path}): {exc}"
            raise TransportError(msg) from exc
        finally:
            conn.close()

    async def connect(self) -> None:
        """
        Test connectivity with a health check.

        Sets _connected=True on success, raises TransportError on failure.
        """
        if self._connected:
            return

        loop = asyncio.get_running_loop()
        try:
            result = await loop.run_in_executor(
                None, self._sync_request, "GET", "/api/v1/health", None
            )
            self._connected = True
            logger.info(
                "http_connected",
                node_id=self._node_id,
                host=self._host,
                port=self._port,
                health=result,
            )
        except TransportError:
            raise
        except Exception as exc:
            msg = f"Failed to connect to node {self._node_id} at {self._host}:{self._port}: {exc}"
            raise TransportError(msg) from exc

    async def disconnect(self) -> None:
        """Mark as disconnected. HTTP is stateless — nothing to tear down."""
        if self._connected:
            self._connected = False
            logger.info("http_disconnected", node_id=self._node_id)

    async def send_request(
        self, method: str, path: str, body: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        """
        Send an API request to the node over HTTP.

        Thread-safe via asyncio lock. Runs blocking I/O in executor.
        """
        if not self._connected:
            msg = f"Not connected to node {self._node_id}"
            raise TransportError(msg)

        async with self._lock:
            loop = asyncio.get_running_loop()
            try:
                return await loop.run_in_executor(
                    None, self._sync_request, method, path, body
                )
            except TransportError:
                raise
            except Exception as exc:
                msg = f"Request to node {self._node_id} failed: {exc}"
                raise TransportError(msg) from exc

    async def health_check(self) -> bool:
        """Check if the node is reachable via health endpoint."""
        if not self._connected:
            return False

        try:
            result = await self.send_request("GET", "/api/v1/health")
            return result.get("status") in ("healthy", "ok")
        except TransportError:
            return False
