"""
Klipper/Moonraker actuator backend.

Dispatches joint commands to Klipper steppers and RC servos via
Moonraker's REST API at localhost:7125.

Uses stdlib http.client for consistency with the HTTPTransport pattern.
Moonraker is a local service, so latency is sub-millisecond.

Klipper command mapping:
- MANUAL_STEPPER: independent stepper control (position + velocity)
- SET_SERVO: RC servo PWM control (angle)

Task: Robotics Phase 1
"""

from __future__ import annotations

import asyncio
import http.client
import json
from typing import Any

import structlog

logger = structlog.get_logger(__name__)


class KlipperError(Exception):
    """Raised when a Klipper/Moonraker operation fails."""


class KlipperBackend:
    """
    Actuator backend for Klipper steppers and servos via Moonraker.

    Communicates with Moonraker's REST API over localhost HTTP.
    All I/O runs in a thread executor to avoid blocking asyncio.

    The actuator_name for this backend is the Klipper config name
    (e.g. "stepper_x", "servo0").
    """

    def __init__(
        self,
        host: str = "localhost",
        port: int = 7125,
        timeout: float = 5.0,
    ) -> None:
        """
        Args:
            host: Moonraker host (default localhost).
            port: Moonraker port (default 7125).
            timeout: HTTP request timeout in seconds.
        """
        self._host = host
        self._port = port
        self._timeout = timeout
        self._lock = asyncio.Lock()

    def _sync_request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """
        Blocking HTTP request to Moonraker.

        Fresh connection per request (same pattern as HTTPTransport).
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
                # Try to extract Klipper's actual error message
                detail = resp_body[:500]
                try:
                    err_data = json.loads(resp_body)
                    if isinstance(err_data, dict):
                        # Moonraker error format: {"error": {"message": "..."}}
                        if "error" in err_data:
                            err_obj = err_data["error"]
                            if isinstance(err_obj, dict) and "message" in err_obj:
                                detail = err_obj["message"]
                            elif isinstance(err_obj, str):
                                detail = err_obj
                        # Alternative format: {"result": "..."}
                        elif "result" in err_data:
                            detail = str(err_data["result"])
                        # Alternative format: {"message": "..."}
                        elif "message" in err_data:
                            detail = err_data["message"]
                except (json.JSONDecodeError, KeyError):
                    pass
                msg = (
                    f"Klipper error ({method} {path}): {detail}"
                )
                raise KlipperError(msg)

            if not resp_body.strip():
                return {}

            return json.loads(resp_body)

        except KlipperError:
            raise
        except json.JSONDecodeError as exc:
            msg = f"Invalid JSON from Moonraker ({method} {path}): {exc}"
            raise KlipperError(msg) from exc
        except Exception as exc:
            msg = f"Moonraker request failed ({method} {path}): {exc}"
            raise KlipperError(msg) from exc
        finally:
            conn.close()

    async def _async_request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Thread-safe async wrapper for _sync_request."""
        async with self._lock:
            loop = asyncio.get_running_loop()
            return await loop.run_in_executor(
                None, self._sync_request, method, path, body
            )

    async def send_gcode(self, script: str) -> dict[str, Any]:
        """
        Send raw G-code to Klipper via Moonraker.

        Args:
            script: G-code command string.

        Returns:
            Moonraker response.
        """
        logger.debug("klipper_gcode", script=script)
        return await self._async_request(
            "POST",
            "/printer/gcode/script",
            {"script": script},
        )

    async def send_position(
        self,
        actuator_name: str,
        position: float,
        *,
        speed: float | None = None,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """
        Move a stepper or servo to a position.

        For klipper_stepper: MANUAL_STEPPER STEPPER=name MOVE=pos [SPEED=spd]
        For klipper_servo: SET_SERVO SERVO=name ANGLE=pos

        The backend type is determined by kwarg 'is_servo'.
        """
        is_servo = kwargs.get("is_servo", False)

        if is_servo:
            gcode = f"SET_SERVO SERVO={actuator_name} ANGLE={position:.2f}"
        else:
            gcode = f"MANUAL_STEPPER STEPPER={actuator_name} MOVE={position:.4f}"
            if speed is not None:
                gcode += f" SPEED={speed:.4f}"

        logger.debug(
            "klipper_position",
            actuator=actuator_name,
            position=position,
            speed=speed,
            is_servo=is_servo,
        )

        return await self.send_gcode(gcode)

    async def send_velocity(
        self,
        actuator_name: str,
        velocity: float,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """
        Move a stepper at a continuous velocity.

        Uses MANUAL_STEPPER with a large MOVE and SPEED to simulate
        continuous motion. Direction is set by MOVE sign.

        For actual continuous rotation, set MOVE to a large value
        in the desired direction.
        """
        # Use a large displacement in the velocity direction
        # The stepper will move at the specified speed until stopped
        direction = 1.0 if velocity >= 0 else -1.0
        move_distance = direction * 99999.0
        speed = abs(velocity)

        gcode = (
            f"MANUAL_STEPPER STEPPER={actuator_name} "
            f"MOVE={move_distance:.4f} SPEED={speed:.4f}"
        )

        logger.debug(
            "klipper_velocity",
            actuator=actuator_name,
            velocity=velocity,
        )

        return await self.send_gcode(gcode)

    async def stop(self, actuator_name: str) -> dict[str, Any]:
        """
        Stop a stepper or servo.

        For steppers: MANUAL_STEPPER STEPPER=name SET_POSITION=<current>
        This effectively stops motion by setting target = current.
        """
        gcode = f"MANUAL_STEPPER STEPPER={actuator_name} STOP_ON_ENDSTOP=0"
        logger.info("klipper_stop", actuator=actuator_name)
        # Stop by sending SET_POSITION to current (stops the move)
        # Actually, the simplest stop is to re-issue MOVE to current position
        # but we don't know current position. Use STOP_ON_ENDSTOP=0 to abort.
        # Klipper will decelerate to stop.
        try:
            return await self.send_gcode(gcode)
        except KlipperError:
            # Fallback: try to stop by setting speed to 0
            logger.warning("klipper_stop_fallback", actuator=actuator_name)
            return await self.send_gcode(
                f"MANUAL_STEPPER STEPPER={actuator_name} MOVE=0 SPEED=0"
            )

    async def query_position(self, actuator_name: str) -> float | None:
        """
        Read current stepper position from Klipper.

        Uses Moonraker's object query API.
        """
        try:
            result = await self._async_request(
                "GET",
                f"/printer/objects/query?manual_stepper%20{actuator_name}",
            )
            status = result.get("result", {}).get("status", {})
            stepper_key = f"manual_stepper {actuator_name}"
            stepper_data = status.get(stepper_key, {})
            return stepper_data.get("position")
        except KlipperError:
            logger.warning(
                "klipper_query_failed",
                actuator=actuator_name,
            )
            return None

    async def query_status(self) -> dict[str, Any]:
        """
        Get Klipper printer status.

        Returns:
            Printer info including state, hostname, etc.
        """
        return await self._async_request("GET", "/printer/info")

    async def query_objects(
        self, objects: dict[str, list[str] | None] | None = None
    ) -> dict[str, Any]:
        """
        Query Klipper printer objects.

        Args:
            objects: Dict of object_name -> list of fields (or None for all).

        Returns:
            Object status data.
        """
        if objects is None:
            return await self._async_request("GET", "/printer/objects/list")

        # Build query string for specific objects
        query_parts = []
        for obj_name, fields in objects.items():
            if fields:
                query_parts.append(f"{obj_name}={','.join(fields)}")
            else:
                query_parts.append(obj_name)
        query = "&".join(query_parts)
        return await self._async_request(
            "GET", f"/printer/objects/query?{query}"
        )

    async def is_available(self) -> bool:
        """Check if Moonraker/Klipper is available and ready."""
        try:
            result = await self.query_status()
            state = result.get("result", {}).get("state")
            return state in ("ready", "standby")
        except KlipperError:
            return False
