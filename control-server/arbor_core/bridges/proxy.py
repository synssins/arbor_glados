"""
Proxy layer — transparently forwards API calls from the control server to nodes.

The control server exposes /api/v1/nodes/{node_id}/... which proxies to the
node's own /api/v1/... endpoints via the registered transport (serial, WiFi, etc.).

This enables the "API parity" design principle: an API consumer can target
either layer with the same code.

Per ARBOR_PROJECT_PLAN.md Section 6:
    https://{control_host}/api/v1/nodes/{node_id}/  # proxy to node

Task: C09
"""

from __future__ import annotations

from typing import Any

import structlog

from arbor_core.bridges.node_bridge import NodeBridge
from arbor_core.bridges.transport import TransportError
from arbor_core.events import Event, EventBus

logger = structlog.get_logger(__name__)


class ProxyError(Exception):
    """Raised when a proxy operation fails."""


class NodeProxy:
    """
    Proxies API requests from the control server to hardware nodes.

    Translates control-server-level paths to node-level paths and
    dispatches via the appropriate transport. Optionally publishes
    events for responses that indicate state changes.

    Usage::

        proxy = NodeProxy(bridge=node_bridge, event_bus=event_bus)
        result = await proxy.forward("arm-01", "GET", "/servo/1/state")
        result = await proxy.forward("arm-01", "PUT", "/servo/1/position", {"position": 512})
    """

    def __init__(
        self,
        bridge: NodeBridge,
        event_bus: EventBus | None = None,
    ) -> None:
        """
        Args:
            bridge: The node bridge manager with registered transports.
            event_bus: Optional event bus for publishing state change events.
        """
        self._bridge = bridge
        self._event_bus = event_bus

    @property
    def available_nodes(self) -> list[str]:
        """List of registered node IDs."""
        return self._bridge.node_ids

    async def forward(
        self,
        node_id: str,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """
        Forward an API request to a node.

        Args:
            node_id: Target node identifier.
            method: HTTP method (GET, PUT, POST, DELETE).
            path: API path relative to /api/v1/ (e.g. 'servo/1/state').
            body: Optional request body.

        Returns:
            Response from the node.

        Raises:
            ProxyError: If the node is unknown, disconnected, or request fails.
        """
        # Ensure the path is properly prefixed
        full_path = path if path.startswith("/api/v1/") else f"/api/v1/{path.lstrip('/')}"

        try:
            result = await self._bridge.send(node_id, method, full_path, body)
        except TransportError as exc:
            msg = f"Proxy to node '{node_id}' failed: {exc}"
            raise ProxyError(msg) from exc

        # Publish event for write operations
        if method in ("PUT", "POST", "DELETE") and self._event_bus is not None:
            topic = self._path_to_topic(path, method)
            self._event_bus.publish(Event(
                topic=topic,
                data={
                    "node_id": node_id,
                    "path": path,
                    "method": method,
                    "response": result,
                },
            ))

        logger.debug(
            "proxy_forwarded",
            node_id=node_id,
            method=method,
            path=full_path,
        )
        return result

    async def forward_to_all(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, dict[str, Any] | str]:
        """
        Forward a request to ALL connected nodes (e.g. emergency stop).

        Args:
            method: HTTP method.
            path: API path.
            body: Optional request body.

        Returns:
            Dict of node_id -> response (or error string on failure).
        """
        results: dict[str, dict[str, Any] | str] = {}
        for node_id in self._bridge.node_ids:
            transport = self._bridge.get_transport(node_id)
            if transport is None or not transport.is_connected:
                results[node_id] = "not connected"
                continue
            try:
                results[node_id] = await self.forward(node_id, method, path, body)
            except ProxyError as exc:
                results[node_id] = str(exc)
                logger.warning(
                    "proxy_broadcast_failed",
                    node_id=node_id,
                    error=str(exc),
                )
        return results

    async def node_health(self, node_id: str) -> dict[str, Any]:
        """
        Check a node's health via proxy.

        Returns:
            Health response from the node, or error dict.
        """
        try:
            return await self.forward(node_id, "GET", "/api/v1/health")
        except ProxyError as exc:
            return {"status": "unreachable", "error": str(exc)}

    @staticmethod
    def _path_to_topic(path: str, method: str) -> str:
        """
        Convert an API path + method to an event topic.

        Examples:
            /servo/1/position + PUT -> servo.position_changed
            /servo/1/torque + PUT -> servo.torque_changed
            /emergency-stop + POST -> system.emergency_stop
        """
        # Strip leading slash and /api/v1/ prefix
        clean = path.lstrip("/")
        if clean.startswith("api/v1/"):
            clean = clean[7:]

        parts = clean.split("/")
        if not parts:
            return "node.request"

        # Build topic from the resource type and last meaningful segment
        resource = parts[0]  # e.g. 'servo', 'sensor', 'system'

        if len(parts) >= 3:
            action = parts[-1]  # e.g. 'position', 'torque', 'state'
            return f"{resource}.{action}_changed"
        elif len(parts) == 2:
            return f"{resource}.state_changed"
        else:
            action = "command" if method == "POST" else "changed"
            return f"{resource}.{action}"
