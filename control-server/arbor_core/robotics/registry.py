"""
Robot profile registry — in-memory CRUD for robot definitions.

Follows the same pattern as device_names storage in app.state.
Profiles persist only for the lifetime of the server process.
Future: persist to SQLite via DatabaseConfig.

Task: Robotics Phase 1
"""

from __future__ import annotations

import structlog

from arbor_core.robotics.models import RobotProfile

logger = structlog.get_logger(__name__)


class RegistryError(Exception):
    """Raised when a registry operation fails."""


class RobotRegistry:
    """
    In-memory store for robot profiles.

    Thread-safe for async access (Python dict operations are atomic
    for single key get/set). No lock needed for basic CRUD.
    """

    def __init__(self) -> None:
        self._profiles: dict[str, RobotProfile] = {}

    def add(self, profile: RobotProfile) -> None:
        """
        Register a new robot profile.

        Args:
            profile: The robot profile to add.

        Raises:
            RegistryError: If a profile with the same ID already exists.
        """
        if profile.id in self._profiles:
            msg = f"Robot profile '{profile.id}' already exists"
            raise RegistryError(msg)

        self._profiles[profile.id] = profile
        logger.info(
            "robot_registered",
            robot_id=profile.id,
            robot_type=profile.robot_type.value,
            joints=len(profile.joints),
        )

    def get(self, robot_id: str) -> RobotProfile:
        """
        Get a robot profile by ID.

        Args:
            robot_id: The robot identifier.

        Returns:
            The robot profile.

        Raises:
            RegistryError: If no profile with that ID exists.
        """
        profile = self._profiles.get(robot_id)
        if profile is None:
            msg = f"Robot profile '{robot_id}' not found"
            raise RegistryError(msg)
        return profile

    def update(self, profile: RobotProfile) -> None:
        """
        Update an existing robot profile.

        Args:
            profile: The updated profile. Must have an existing ID.

        Raises:
            RegistryError: If no profile with that ID exists.
        """
        if profile.id not in self._profiles:
            msg = f"Robot profile '{profile.id}' not found"
            raise RegistryError(msg)

        self._profiles[profile.id] = profile
        logger.info(
            "robot_updated",
            robot_id=profile.id,
            robot_type=profile.robot_type.value,
        )

    def remove(self, robot_id: str) -> RobotProfile:
        """
        Remove a robot profile.

        Args:
            robot_id: The robot identifier.

        Returns:
            The removed profile.

        Raises:
            RegistryError: If no profile with that ID exists.
        """
        profile = self._profiles.pop(robot_id, None)
        if profile is None:
            msg = f"Robot profile '{robot_id}' not found"
            raise RegistryError(msg)

        logger.info("robot_removed", robot_id=robot_id)
        return profile

    def list_all(self) -> list[RobotProfile]:
        """Return all registered robot profiles."""
        return list(self._profiles.values())

    @property
    def robot_ids(self) -> list[str]:
        """List of all registered robot IDs."""
        return list(self._profiles.keys())

    def __contains__(self, robot_id: str) -> bool:
        return robot_id in self._profiles

    def __len__(self) -> int:
        return len(self._profiles)
