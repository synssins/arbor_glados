"""
Servo API endpoints — proxy to node servo bus.

Parity with ESP32 firmware F12 endpoints.

Per ARBOR_PROJECT_PLAN.md Section 6:
    GET  /api/v1/servo/{id}/state       # position, speed, load, temp, voltage
    PUT  /api/v1/servo/{id}/position    # set target position
    PUT  /api/v1/servo/{id}/speed       # set max speed
    PUT  /api/v1/servo/{id}/torque      # enable/disable torque
    POST /api/v1/servo/sync             # multi-servo synchronized move
    GET  /api/v1/servo/scan             # scan bus for servo IDs

Task: C11
"""

from __future__ import annotations

import structlog
from fastapi import APIRouter, Path, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from arbor_core.bridges.proxy import NodeProxy, ProxyError


# ---------------------------------------------------------------------------
# Request body models (FastAPI auto-parses and validates these)
# ---------------------------------------------------------------------------
class SetPositionRequest(BaseModel):
    """Body for PUT /servo/{id}/position.

    ST3215: 12-bit position (0-4095 = 0-360°).
    Optional speed (steps/s) and time (ms) for profiled moves.
    """

    position: int = Field(ge=0, le=4095, description="Target position in steps (0-4095).")
    speed: int | None = Field(default=None, ge=0, le=4095, description="Speed limit in steps/s (0-4095). Optional.")
    time: int | None = Field(default=None, ge=0, le=30000, description="Arrival time in ms (0-30000). Optional.")


class SetSpeedRequest(BaseModel):
    """Body for PUT /servo/{id}/speed."""

    speed: int = Field(ge=0, le=4095, description="Maximum speed in steps/s (0-4095).")


class SetTorqueRequest(BaseModel):
    """Body for PUT /servo/{id}/torque."""

    enabled: bool = Field(description="True to enable torque, false to disable.")


class SyncMoveItem(BaseModel):
    """A single servo move within a sync command."""

    id: int = Field(ge=0, le=253, description="Servo ID (0-253).")
    position: int = Field(ge=0, le=4095, description="Target position in steps (0-4095).")
    speed: int | None = Field(default=None, ge=0, le=4095, description="Speed limit in steps/s. Optional.")
    time: int | None = Field(default=None, ge=0, le=30000, description="Arrival time in ms. Optional.")


class SyncMoveRequest(BaseModel):
    """Body for POST /servo/sync."""

    moves: list[SyncMoveItem] = Field(min_length=1, max_length=253, description="List of servo moves.")


logger = structlog.get_logger(__name__)

servo_router = APIRouter(prefix="/servo", tags=["servo"])


def _get_proxy(request: Request) -> NodeProxy:
    """Get the NodeProxy from app state."""
    proxy = getattr(request.app.state, "node_proxy", None)
    if proxy is None:
        msg = "Node proxy not configured"
        raise ProxyError(msg)
    return proxy


def _get_node_id(request: Request) -> str:
    """Get the target node ID from query param or default."""
    node_id = request.query_params.get("node_id")
    if node_id:
        return node_id
    # Default: use first available node (Phase 1 = single node)
    proxy = _get_proxy(request)
    nodes = proxy.available_nodes
    if not nodes:
        msg = "No nodes available"
        raise ProxyError(msg)
    return nodes[0]


@servo_router.get(
    "/{servo_id}/state",
    summary="Get servo state",
    description="Returns position, speed, load, temperature, and voltage for a servo.",
)
async def get_servo_state(
    request: Request,
    servo_id: int = Path(ge=0, le=253, description="Servo ID (0-253)."),
) -> JSONResponse:
    """Get full state of a servo by ID."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(
            node_id, "GET", f"/servo/{servo_id}/state"
        )
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@servo_router.put(
    "/{servo_id}/position",
    summary="Set servo position",
    description="Set the target position for a servo. Optionally include speed and/or time for profiled moves.",
)
async def set_servo_position(
    request: Request,
    body: SetPositionRequest,
    servo_id: int = Path(ge=0, le=253, description="Servo ID (0-253)."),
) -> JSONResponse:
    """Set target position for a servo."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        payload = body.model_dump(exclude_none=True)
        result = await proxy.forward(
            node_id, "PUT", f"/servo/{servo_id}/position", payload
        )
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@servo_router.put(
    "/{servo_id}/speed",
    summary="Set servo speed",
    description="Set the maximum speed for a servo.",
)
async def set_servo_speed(
    request: Request,
    body: SetSpeedRequest,
    servo_id: int = Path(ge=0, le=253, description="Servo ID (0-253)."),
) -> JSONResponse:
    """Set max speed for a servo."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(
            node_id, "PUT", f"/servo/{servo_id}/speed", body.model_dump()
        )
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@servo_router.put(
    "/{servo_id}/torque",
    summary="Set servo torque",
    description="Enable or disable torque for a servo.",
)
async def set_servo_torque(
    request: Request,
    body: SetTorqueRequest,
    servo_id: int = Path(ge=0, le=253, description="Servo ID (0-253)."),
) -> JSONResponse:
    """Enable or disable torque for a servo."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(
            node_id, "PUT", f"/servo/{servo_id}/torque", body.model_dump()
        )
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@servo_router.post(
    "/sync",
    summary="Synchronized multi-servo move",
    description="Move multiple servos to target positions simultaneously.",
)
async def servo_sync(body: SyncMoveRequest, request: Request) -> JSONResponse:
    """Synchronized move of multiple servos."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        payload = body.model_dump(exclude_none=True)
        result = await proxy.forward(node_id, "POST", "/servo/sync", payload)
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@servo_router.get(
    "/scan",
    summary="Scan servo bus",
    description="Scan the servo bus for connected servo IDs.",
)
async def servo_scan(request: Request) -> JSONResponse:
    """Scan the bus for servo IDs."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(node_id, "GET", "/servo/scan")
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})
