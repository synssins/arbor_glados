"""
Node bridge manager — manages connections to all configured hardware nodes.

Creates appropriate transports based on node config and maintains
connection state for each node.

Task: C08
"""

from __future__ import annotations

from typing import Any

import structlog

from arbor_core.bridges.transport import NodeTransport, TransportError
from arbor_core.config.models import NodeConfig

logger = structlog.get_logger(__name__)


class NodeBridge:
    """
    Manages transport connections to all configured hardware nodes.

    Usage::

        bridge = NodeBridge()
        bridge.register_node(node_config, transport)
        await bridge.connect_all()
        result = await bridge.send("arm-node-01", "GET", "/api/v1/health")
        await bridge.disconnect_all()
    """

    def __init__(self) -> None:
        self._nodes: dict[str, NodeTransport] = {}

    def register_node(self, node_id: str, transport: NodeTransport) -> None:
        """
        Register a transport for a node.

        Args:
            node_id: Node identifier.
            transport: The transport instance.
        """
        if node_id in self._nodes:
            msg = f"Node '{node_id}' is already registered"
            raise TransportError(msg)
        self._nodes[node_id] = transport
        logger.info("node_registered", node_id=node_id)

    def get_transport(self, node_id: str) -> NodeTransport | None:
        """Get the transport for a node."""
        return self._nodes.get(node_id)

    @property
    def node_ids(self) -> list[str]:
        """List of registered node IDs."""
        return list(self._nodes.keys())

    async def connect_all(self) -> dict[str, bool]:
        """
        Connect to all registered nodes.

        Returns:
            Dict of node_id -> success boolean.
        """
        results: dict[str, bool] = {}
        for node_id, transport in self._nodes.items():
            try:
                await transport.connect()
                results[node_id] = True
                logger.info("node_connected", node_id=node_id)
            except TransportError:
                results[node_id] = False
                logger.exception("node_connect_failed", node_id=node_id)
        return results

    async def disconnect_all(self) -> None:
        """Disconnect from all nodes."""
        for node_id, transport in self._nodes.items():
            try:
                await transport.disconnect()
            except Exception:
                logger.exception("node_disconnect_error", node_id=node_id)
        logger.info("all_nodes_disconnected")

    async def send(
        self,
        node_id: str,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """
        Send a request to a specific node.

        Args:
            node_id: Target node identifier.
            method: HTTP method.
            path: API path.
            body: Optional request body.

        Returns:
            Response from the node.

        Raises:
            TransportError: If the node is unknown or not connected.
        """
        transport = self._nodes.get(node_id)
        if transport is None:
            msg = f"Unknown node: {node_id}"
            raise TransportError(msg)
        if not transport.is_connected:
            msg = f"Node '{node_id}' is not connected"
            raise TransportError(msg)
        return await transport.send_request(method, path, body)

    async def unregister_node(self, node_id: str) -> None:
        """
        Unregister and disconnect a node's transport.

        Args:
            node_id: Node identifier to remove.
        """
        transport = self._nodes.pop(node_id, None)
        if transport is not None:
            try:
                await transport.disconnect()
            except Exception:
                logger.exception("node_unregister_disconnect_error", node_id=node_id)
            logger.info("node_unregistered", node_id=node_id)

    async def health_check_all(self) -> dict[str, bool]:
        """
        Run health checks on all nodes.

        Returns:
            Dict of node_id -> healthy boolean.
        """
        results: dict[str, bool] = {}
        for node_id, transport in self._nodes.items():
            results[node_id] = await transport.health_check()
        return results
