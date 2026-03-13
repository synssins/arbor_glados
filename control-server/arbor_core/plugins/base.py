"""
Plugin base class and health status model.

Defines the ArborPlugin abstract interface that all plugins must
implement, as specified in ARBOR_PROJECT_PLAN.md Section 5.

Task: C04
"""

from __future__ import annotations

import abc
from enum import Enum
from typing import Any

from pydantic import BaseModel


class HealthState(str, Enum):
    """Health states a plugin can report."""

    HEALTHY = "healthy"
    DEGRADED = "degraded"
    UNHEALTHY = "unhealthy"
    UNKNOWN = "unknown"


class HealthStatus(BaseModel):
    """Health check result returned by a plugin."""

    state: HealthState
    message: str = ""
    details: dict[str, Any] = {}


class ArborPlugin(abc.ABC):
    """
    Abstract base class for all Arbor plugins.

    Every hardware driver, protocol handler, and sensor type must
    implement this interface.  The plugin manager discovers, loads,
    and manages the lifecycle of these plugins.

    Properties:
        name: Unique slug identifier (e.g. 'servo-bus').
        version: Semantic version string.
        capabilities: List of capability tags (e.g. ['servo', 'pwm_output']).
        config_schema: JSON Schema dict describing accepted configuration.
    """

    @property
    @abc.abstractmethod
    def name(self) -> str:
        """Unique plugin slug."""

    @property
    @abc.abstractmethod
    def version(self) -> str:
        """Semantic version."""

    @property
    @abc.abstractmethod
    def capabilities(self) -> list[str]:
        """Capability tags this plugin provides."""

    @property
    @abc.abstractmethod
    def config_schema(self) -> dict[str, Any]:
        """JSON Schema for the plugin's configuration dict."""

    @abc.abstractmethod
    async def initialize(self, config: dict[str, Any]) -> None:
        """
        Initialize the plugin with validated configuration.

        Called once during application startup.

        Args:
            config: Plugin-specific configuration dictionary.
        """

    @abc.abstractmethod
    async def shutdown(self) -> None:
        """
        Gracefully shut down the plugin.

        Release hardware resources, close connections, etc.
        Called once during application shutdown.
        """

    @abc.abstractmethod
    async def get_state(self) -> dict[str, Any]:
        """
        Return the current state of the plugin.

        Returns:
            Serializable state dictionary.
        """

    @abc.abstractmethod
    async def handle_command(self, command: str, params: dict[str, Any]) -> dict[str, Any]:
        """
        Handle a command dispatched to this plugin.

        Args:
            command: Command name (e.g. 'set_position').
            params: Command parameters.

        Returns:
            Command result dictionary.
        """

    @abc.abstractmethod
    async def health_check(self) -> HealthStatus:
        """
        Perform a health check.

        Returns:
            HealthStatus indicating plugin health.
        """
