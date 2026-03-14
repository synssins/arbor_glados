"""
Device aggregation API — unified view of all devices across all nodes.

Queries connected nodes via the proxy layer and returns a merged list
of servos, sensors, and other devices tagged with their source node.
Also provides device renaming (in-memory for Phase 1).

Endpoints:
    GET  /api/v1/devices                              List all devices across all nodes
    PUT  /api/v1/devices/{node_id}/{type}/{device_id}/name  Rename a device

Task: Node Integration Step 4
"""

from __future__ import annotations

from typing import Any

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from arbor_core.bridges.proxy import NodeProxy, ProxyError

logger = structlog.get_logger(__name__)

device_router = APIRouter(prefix="/devices", tags=["devices"])


def _get_proxy(request: Request) -> NodeProxy | None:
    """Get the NodeProxy from app state, or None if not configured."""
    return getattr(request.app.state, "node_proxy", None)


def _get_device_names(request: Request) -> dict[str, str]:
    """
    Get the in-memory device name store.

    Keys are "{node_id}/{type}/{device_id}" → friendly name.
    Stored on app.state.device_names for Phase 1 (in-memory only).
    """
    names = getattr(request.app.state, "device_names", None)
    if names is None:
        names = {}
        request.app.state.device_names = names
    return names


@device_router.get(
    "",
    summary="List all devices across all nodes",
    description=(
        "Queries each connected node for its servos and sensors, "
        "returning a unified device list tagged with source node."
    ),
)
async def list_devices(request: Request) -> JSONResponse:
    """
    Aggregate devices from all connected nodes.

    Queries each node's servo scan endpoint and merges results
    into a flat list with node attribution.
    """
    proxy = _get_proxy(request)
    if proxy is None or not proxy.available_nodes:
        return JSONResponse(content={"devices": [], "count": 0})

    device_names = _get_device_names(request)
    bridge = getattr(request.app.state, "node_bridge", None)
    devices: list[dict[str, Any]] = []

    # Query each connected node for servo scan
    for node_id in proxy.available_nodes:
        # Check if node is actually connected
        if bridge is not None:
            transport = bridge.get_transport(node_id)
            if transport is None or not transport.is_connected:
                continue

        try:
            scan_result = await proxy.forward(node_id, "GET", "/servo/scan")
        except ProxyError as exc:
            logger.debug(
                "device_scan_failed",
                node_id=node_id,
                error=str(exc),
            )
            continue

        # Parse servo scan results
        # ESP32 returns {"found_ids": [1, 2], "count": 2}
        servo_ids = scan_result.get("found_ids", [])
        if isinstance(servo_ids, list):
            for servo_id in servo_ids:
                if servo_id is None:
                    continue

                name_key = f"{node_id}/servo/{servo_id}"
                friendly_name = device_names.get(
                    name_key, f"Servo {servo_id}"
                )

                devices.append({
                    "type": "servo",
                    "id": servo_id,
                    "node_id": node_id,
                    "name": friendly_name,
                    "status": "connected",
                })

    # Include PWM servos from config
    pwm_configs = getattr(request.app.state, "pwm_servo_configs", [])
    for pwm in pwm_configs:
        name_key = f"pwm/{pwm.get('channel', 0)}"
        friendly_name = device_names.get(
            name_key, pwm.get("name", f"PWM {pwm.get('channel', 0)}")
        )
        devices.append({
            "type": "pwm_servo",
            "id": pwm.get("channel", 0),
            "node_id": pwm.get("node_id", "klipper"),
            "name": friendly_name,
            "status": "configured",
            "controller_type": pwm.get("controller_type", "esp32"),
            "pin": pwm.get("pin"),
        })

    return JSONResponse(content={"devices": devices, "count": len(devices)})


@device_router.put(
    "/{node_id}/{device_type}/{device_id}/name",
    summary="Rename a device",
    description="Set a friendly name for a device. In-memory only for Phase 1.",
)
async def rename_device(
    node_id: str,
    device_type: str,
    device_id: str,
    request: Request,
) -> JSONResponse:
    """Set a friendly name for a device."""
    try:
        body = await request.json()
    except Exception:
        return JSONResponse(
            status_code=422, content={"detail": "Invalid JSON body"}
        )

    name = body.get("name", "")
    if not name or not isinstance(name, str):
        return JSONResponse(
            status_code=422, content={"detail": "'name' is required (string)"}
        )

    if len(name) > 64:
        return JSONResponse(
            status_code=422,
            content={"detail": "Name must be 64 characters or fewer"},
        )

    device_names = _get_device_names(request)
    name_key = f"{node_id}/{device_type}/{device_id}"
    device_names[name_key] = name.strip()

    logger.info(
        "device_renamed",
        node_id=node_id,
        device_type=device_type,
        device_id=device_id,
        name=name.strip(),
    )

    return JSONResponse(
        content={
            "ok": True,
            "key": name_key,
            "name": name.strip(),
        }
    )
