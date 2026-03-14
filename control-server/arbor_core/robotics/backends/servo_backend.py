"""
Arbor smart servo backend.

Dispatches joint commands to Feetech STS/SCS servos on ESP32 nodes
via the existing NodeProxy → NodeBridge → HTTPTransport chain.

The actuator_name for this backend is formatted as "node_id:servo_id"
(e.g. "esp32-arm:1").

Task: Robotics Phase 1
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

import structlog

if TYPE_CHECKING:
    from arbor_core.bridges.proxy import NodeProxy

logger = structlog.get_logger(__name__)


class ArborServoBackend:
    """
    Actuator backend for Arbor smart servos via NodeProxy.

    Translates joint commands into Arbor servo API calls routed
    through the proxy/bridge/transport chain to ESP32 nodes.
    """

    def __init__(self, proxy: NodeProxy) -> None:
        """
        Args:
            proxy: NodeProxy instance for forwarding requests to nodes.
        """
        self._proxy = proxy

    @staticmethod
    def _parse_actuator_name(actuator_name: str) -> tuple[str, int]:
        """
        Parse 'node_id:servo_id' into components.

        Args:
            actuator_name: Format "node_id:servo_id" (e.g. "esp32-arm:1").

        Returns:
            Tuple of (node_id, servo_id).

        Raises:
            ValueError: If format is invalid.
        """
        parts = actuator_name.split(":", 1)
        if len(parts) != 2:
            msg = f"Invalid actuator name '{actuator_name}'. Expected 'node_id:servo_id'"
            raise ValueError(msg)
        try:
            return parts[0], int(parts[1])
        except ValueError:
            msg = f"Invalid servo_id in '{actuator_name}'. Must be integer."
            raise ValueError(msg) from None

    async def send_position(
        self,
        actuator_name: str,
        position: float,
        *,
        speed: float | None = None,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """
        Move a servo to a position.

        Maps to: PUT /api/v1/servo/{servo_id}/position
        On node: {node_id}

        Args:
            actuator_name: "node_id:servo_id"
            position: Target position (raw servo units after scale/offset).
            speed: Optional move speed.
        """
        node_id, servo_id = self._parse_actuator_name(actuator_name)

        body: dict[str, Any] = {"position": int(round(position))}
        if speed is not None:
            body["speed"] = int(round(speed))

        logger.debug(
            "arbor_servo_move",
            node_id=node_id,
            servo_id=servo_id,
            position=body["position"],
            speed=body.get("speed"),
        )

        return await self._proxy.forward(
            node_id, "PUT", f"servo/{servo_id}/position", body
        )

    async def send_velocity(
        self,
        actuator_name: str,
        velocity: float,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """
        Run a servo at continuous velocity (wheel mode).

        Maps to: PUT /api/v1/servo/{servo_id}/velocity
        On node: {node_id}

        Args:
            actuator_name: "node_id:servo_id"
            velocity: Target velocity (raw servo units after scale).
        """
        node_id, servo_id = self._parse_actuator_name(actuator_name)

        body: dict[str, Any] = {"speed": int(round(velocity))}

        logger.debug(
            "arbor_servo_velocity",
            node_id=node_id,
            servo_id=servo_id,
            velocity=body["speed"],
        )

        return await self._proxy.forward(
            node_id, "PUT", f"servo/{servo_id}/velocity", body
        )

    async def stop(self, actuator_name: str) -> dict[str, Any]:
        """
        Stop a servo.

        Maps to: PUT /api/v1/servo/{servo_id}/torque with enabled=false
        """
        node_id, servo_id = self._parse_actuator_name(actuator_name)

        logger.info("arbor_servo_stop", node_id=node_id, servo_id=servo_id)

        return await self._proxy.forward(
            node_id, "PUT", f"servo/{servo_id}/torque", {"enabled": False}
        )

    async def query_position(self, actuator_name: str) -> float | None:
        """
        Read current servo position.

        Maps to: GET /api/v1/servo/{servo_id}/state
        """
        node_id, servo_id = self._parse_actuator_name(actuator_name)

        try:
            result = await self._proxy.forward(
                node_id, "GET", f"servo/{servo_id}/state"
            )
            return result.get("position")
        except Exception:
            logger.warning(
                "arbor_servo_query_failed",
                node_id=node_id,
                servo_id=servo_id,
            )
            return None

    async def is_available(self) -> bool:
        """Check if any Arbor nodes are connected."""
        return len(self._proxy.available_nodes) > 0
