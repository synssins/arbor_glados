"""
Transport abstraction for node communication.

Defines the abstract interface that all transport implementations
(UART/USB serial, WiFi HTTP, Ethernet) must implement.

Task: C08
"""

from __future__ import annotations

import abc
from typing import Any


class TransportError(Exception):
    """Raised when a transport operation fails."""


class NodeTransport(abc.ABC):
    """
    Abstract transport for communicating with a hardware node.

    Implementations handle the physical layer (serial, TCP, etc.)
    and provide a uniform request/response interface.
    """

    @property
    @abc.abstractmethod
    def node_id(self) -> str:
        """The node ID this transport is connected to."""

    @property
    @abc.abstractmethod
    def is_connected(self) -> bool:
        """Whether the transport is currently connected."""

    @abc.abstractmethod
    async def connect(self) -> None:
        """
        Open the transport connection.

        Raises:
            TransportError: If the connection fails.
        """

    @abc.abstractmethod
    async def disconnect(self) -> None:
        """Close the transport connection."""

    @abc.abstractmethod
    async def send_request(
        self, method: str, path: str, body: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        """
        Send an API request to the node and return the response.

        This maps to the node's REST API — the transport handles
        framing and delivery over whatever physical medium.

        Args:
            method: HTTP method (GET, PUT, POST, DELETE).
            path: API path (e.g. '/api/v1/servo/1/state').
            body: Optional JSON body for PUT/POST.

        Returns:
            Response body as a dictionary.

        Raises:
            TransportError: On communication failure or timeout.
        """

    @abc.abstractmethod
    async def health_check(self) -> bool:
        """
        Check if the node is reachable.

        Returns:
            True if the node responded to a health check.
        """
