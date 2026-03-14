"""
Abstract base class for actuator backends.

All backends must implement these methods to be usable by the
KinematicsEngine for dispatching joint commands.

Task: Robotics Phase 1
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any


class ActuatorBackend(ABC):
    """
    Abstract actuator backend.

    Translates joint-space commands into hardware-specific operations.
    Each backend type handles a different physical actuator:
    - ArborServoBackend → Feetech STS/SCS via ESP32 nodes
    - KlipperBackend → Steppers/servos via Moonraker API
    """

    @abstractmethod
    async def send_position(
        self,
        actuator_name: str,
        position: float,
        *,
        speed: float | None = None,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """
        Command an actuator to move to a position.

        Args:
            actuator_name: Backend-specific actuator identifier.
            position: Target position (already scaled/offset by engine).
            speed: Optional move speed.

        Returns:
            Response from the hardware.
        """

    @abstractmethod
    async def send_velocity(
        self,
        actuator_name: str,
        velocity: float,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """
        Command an actuator at a continuous velocity.

        Used for wheels and continuous-rotation joints.

        Args:
            actuator_name: Backend-specific actuator identifier.
            velocity: Target velocity (already scaled by engine).

        Returns:
            Response from the hardware.
        """

    @abstractmethod
    async def stop(self, actuator_name: str) -> dict[str, Any]:
        """
        Stop an actuator immediately.

        Args:
            actuator_name: Backend-specific actuator identifier.

        Returns:
            Response from the hardware.
        """

    @abstractmethod
    async def query_position(self, actuator_name: str) -> float | None:
        """
        Read the current position of an actuator.

        Args:
            actuator_name: Backend-specific actuator identifier.

        Returns:
            Current position value, or None if unavailable.
        """

    @abstractmethod
    async def is_available(self) -> bool:
        """Check if this backend is available and ready."""
