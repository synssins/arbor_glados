"""
PWM servo management API — manual PWM servo configuration and control.

Supports both ESP32 nodes (via proxy) and Klipper (via SET_SERVO G-code).
PWM servo configs stored in-memory on app.state for Phase 1.

Task: PWM Servo Support
"""

from __future__ import annotations

from typing import Any

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from arbor_core.bridges.proxy import NodeProxy, ProxyError
from arbor_core.config.models import BoardCapabilities, PwmServoConfig
from arbor_core.robotics.backends.klipper_backend import KlipperBackend, KlipperError

logger = structlog.get_logger(__name__)

pwm_servo_router = APIRouter(prefix="/pwm-servo", tags=["pwm-servo"])

# Default board profiles (ESP32-WROOM-32)
DEFAULT_BOARD_PROFILES: dict[str, BoardCapabilities] = {
    "esp32-wroom-32": BoardCapabilities(
        pwm_capable_pins=[0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27],
        input_only_pins=[34, 35, 36, 39],
        reserved_pins=[6, 7, 8, 9, 10, 11],
    ),
    "esp32-s3": BoardCapabilities(
        pwm_capable_pins=list(range(0, 49)),
        input_only_pins=[],
        reserved_pins=[26, 27, 28, 29, 30, 31, 32],
    ),
}


def _get_pwm_configs(request: Request) -> list[dict[str, Any]]:
    """Get PWM servo configs from app state."""
    configs = getattr(request.app.state, "pwm_servo_configs", None)
    if configs is None:
        configs = []
        request.app.state.pwm_servo_configs = configs
    return configs


def _get_proxy(request: Request) -> NodeProxy | None:
    """Get the NodeProxy from app state."""
    return getattr(request.app.state, "node_proxy", None)


def _get_klipper(request: Request) -> KlipperBackend | None:
    """Get the KlipperBackend from app state."""
    return getattr(request.app.state, "klipper_backend", None)


@pwm_servo_router.get(
    "",
    summary="List PWM servos",
    description="List all configured PWM servos.",
)
async def list_pwm_servos(request: Request) -> JSONResponse:
    """Return all configured PWM servos."""
    configs = _get_pwm_configs(request)
    return JSONResponse(content={"servos": configs, "count": len(configs)})


@pwm_servo_router.post(
    "",
    summary="Add PWM servo",
    description="Add a new PWM servo configuration.",
)
async def add_pwm_servo(request: Request) -> JSONResponse:
    """Add a PWM servo configuration."""
    try:
        body = await request.json()
    except Exception:
        return JSONResponse(status_code=422, content={"detail": "Invalid JSON body"})

    # Validate with Pydantic model
    try:
        config = PwmServoConfig(**body)
    except Exception as exc:
        return JSONResponse(status_code=422, content={"detail": str(exc)})

    configs = _get_pwm_configs(request)

    # Check for duplicate channel
    for existing in configs:
        if existing["channel"] == config.channel and existing.get("node_id") == config.node_id:
            return JSONResponse(
                status_code=409,
                content={"detail": f"Channel {config.channel} already configured"},
            )

    config_dict = config.model_dump()
    configs.append(config_dict)

    logger.info(
        "pwm_servo_added",
        channel=config.channel,
        pin=config.pin,
        name=config.name,
        controller=config.controller_type,
    )

    return JSONResponse(
        status_code=201,
        content={"ok": True, "servo": config_dict},
    )


@pwm_servo_router.get(
    "/{channel}/state",
    summary="Get PWM servo state",
    description="Get current state of a PWM servo.",
)
async def get_pwm_servo_state(channel: int, request: Request) -> JSONResponse:
    """Get state of a PWM servo by channel."""
    configs = _get_pwm_configs(request)
    config = next((c for c in configs if c["channel"] == channel), None)
    if config is None:
        return JSONResponse(status_code=404, content={"detail": f"Channel {channel} not configured"})

    # For ESP32 nodes, proxy to the node
    if config.get("controller_type") == "esp32" and config.get("node_id"):
        proxy = _get_proxy(request)
        if proxy is not None:
            try:
                result = await proxy.forward(
                    config["node_id"], "GET", f"/pwm-servo/{channel}/state"
                )
                return JSONResponse(content=result)
            except ProxyError as exc:
                return JSONResponse(status_code=502, content={"detail": str(exc)})

    # For Klipper or when node is not available, return config-based state
    return JSONResponse(content={
        "channel": channel,
        "name": config.get("name", f"PWM {channel}"),
        "pin": config.get("pin"),
        "controller_type": config.get("controller_type"),
        "position": None,  # Position unknown without feedback
    })


@pwm_servo_router.put(
    "/{channel}/position",
    summary="Set PWM servo position",
    description="Set position (0-1000) for a PWM servo.",
)
async def set_pwm_servo_position(channel: int, request: Request) -> JSONResponse:
    """Set position for a PWM servo."""
    try:
        body = await request.json()
    except Exception:
        return JSONResponse(status_code=422, content={"detail": "Invalid JSON body"})

    position = body.get("position")
    if position is None or not isinstance(position, (int, float)):
        return JSONResponse(status_code=422, content={"detail": "'position' is required (number)"})

    configs = _get_pwm_configs(request)
    config = next((c for c in configs if c["channel"] == channel), None)
    if config is None:
        return JSONResponse(status_code=404, content={"detail": f"Channel {channel} not configured"})

    # For ESP32 nodes, proxy to the node
    if config.get("controller_type") == "esp32" and config.get("node_id"):
        proxy = _get_proxy(request)
        if proxy is not None:
            try:
                result = await proxy.forward(
                    config["node_id"], "PUT", f"/pwm-servo/{channel}/position", body
                )
                return JSONResponse(content=result)
            except ProxyError as exc:
                return JSONResponse(status_code=502, content={"detail": str(exc)})

    # For Klipper, use SET_SERVO command
    if config.get("controller_type") == "klipper":
        klipper = _get_klipper(request)
        if klipper is None:
            return JSONResponse(status_code=503, content={"detail": "Klipper backend not available"})
        klipper_name = config.get("klipper_name", f"servo{channel}")
        # Convert 0-1000 to angle (0-180)
        angle = position / 1000.0 * 180.0
        try:
            result = await klipper.send_position(klipper_name, angle, is_servo=True)
            return JSONResponse(content={"ok": True, "position": position, "angle": angle})
        except KlipperError as exc:
            return JSONResponse(content={"status": "error", "error": str(exc)})

    return JSONResponse(status_code=503, content={"detail": "No backend available for this servo"})


@pwm_servo_router.put(
    "/{channel}/config",
    summary="Update PWM servo config",
    description="Update configuration for a PWM servo.",
)
async def update_pwm_servo_config(channel: int, request: Request) -> JSONResponse:
    """Update a PWM servo configuration."""
    try:
        body = await request.json()
    except Exception:
        return JSONResponse(status_code=422, content={"detail": "Invalid JSON body"})

    configs = _get_pwm_configs(request)
    for i, config in enumerate(configs):
        if config["channel"] == channel:
            # Update fields
            for key in ("name", "pin", "min_pulse_us", "max_pulse_us", "invert", "pull_up", "pull_down"):
                if key in body:
                    config[key] = body[key]
            logger.info("pwm_servo_updated", channel=channel)
            return JSONResponse(content={"ok": True, "servo": config})

    return JSONResponse(status_code=404, content={"detail": f"Channel {channel} not configured"})


@pwm_servo_router.delete(
    "/{channel}",
    summary="Remove PWM servo",
    description="Remove a PWM servo configuration.",
)
async def remove_pwm_servo(channel: int, request: Request) -> JSONResponse:
    """Remove a PWM servo configuration."""
    configs = _get_pwm_configs(request)
    original_len = len(configs)
    configs[:] = [c for c in configs if c["channel"] != channel]

    if len(configs) == original_len:
        return JSONResponse(status_code=404, content={"detail": f"Channel {channel} not configured"})

    logger.info("pwm_servo_removed", channel=channel)
    return JSONResponse(content={"ok": True, "channel": channel})


@pwm_servo_router.get(
    "/board-profiles",
    summary="List board profiles",
    description="List available board GPIO profiles.",
)
async def list_board_profiles(request: Request) -> JSONResponse:
    """List all available board profiles."""
    # Merge defaults with config-provided profiles
    config = getattr(request.app.state, "config", None)
    profiles = dict(DEFAULT_BOARD_PROFILES)
    if config is not None:
        for name, caps in getattr(config, "board_profiles", {}).items():
            profiles[name] = caps

    result = {}
    for name, caps in profiles.items():
        if isinstance(caps, BoardCapabilities):
            result[name] = caps.model_dump()
        else:
            result[name] = dict(caps) if isinstance(caps, dict) else caps

    return JSONResponse(content={"profiles": result})


@pwm_servo_router.get(
    "/board-profiles/{board_type}",
    summary="Get board profile",
    description="Get GPIO capabilities for a specific board type.",
)
async def get_board_profile(board_type: str, request: Request) -> JSONResponse:
    """Get GPIO capabilities for a specific board type."""
    config = getattr(request.app.state, "config", None)
    profiles = dict(DEFAULT_BOARD_PROFILES)
    if config is not None:
        for name, caps in getattr(config, "board_profiles", {}).items():
            profiles[name] = caps

    caps = profiles.get(board_type)
    if caps is None:
        return JSONResponse(status_code=404, content={"detail": f"Board type '{board_type}' not found"})

    if isinstance(caps, BoardCapabilities):
        return JSONResponse(content=caps.model_dump())
    return JSONResponse(content=dict(caps) if isinstance(caps, dict) else caps)
