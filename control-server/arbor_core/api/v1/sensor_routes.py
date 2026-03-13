"""
Sensor API endpoints — proxy to node sensors.

Parity with ESP32 firmware F13 endpoints.

Per ARBOR_PROJECT_PLAN.md Section 6:
    GET  /api/v1/sensor/{id}/reading    # latest reading
    GET  /api/v1/sensor/{id}/history    # time-series (last N readings)
    GET  /api/v1/sensors                # all sensor readings at once

Task: C12
"""

from __future__ import annotations

import structlog
from fastapi import APIRouter, Query, Request
from fastapi.responses import JSONResponse

from arbor_core.bridges.proxy import NodeProxy, ProxyError

logger = structlog.get_logger(__name__)

sensor_router = APIRouter(prefix="/sensor", tags=["sensor"])
sensors_router = APIRouter(tags=["sensor"])


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
    proxy = _get_proxy(request)
    nodes = proxy.available_nodes
    if not nodes:
        msg = "No nodes available"
        raise ProxyError(msg)
    return nodes[0]


@sensor_router.get(
    "/{sensor_id}/reading",
    summary="Get sensor reading",
    description="Returns the latest reading from a specific sensor.",
)
async def get_sensor_reading(sensor_id: str, request: Request) -> JSONResponse:
    """Get latest reading from a sensor."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(
            node_id, "GET", f"/sensor/{sensor_id}/reading"
        )
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@sensor_router.get(
    "/{sensor_id}/history",
    summary="Get sensor history",
    description="Returns time-series data (last N readings) for a sensor.",
)
async def get_sensor_history(
    sensor_id: str,
    request: Request,
    limit: int = Query(default=100, ge=1, le=10000, description="Number of readings"),
) -> JSONResponse:
    """Get historical readings for a sensor."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(
            node_id, "GET", f"/sensor/{sensor_id}/history?limit={limit}"
        )
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})


@sensors_router.get(
    "/sensors",
    summary="Get all sensor readings",
    description="Returns latest readings from all sensors on the node.",
)
async def get_all_sensors(request: Request) -> JSONResponse:
    """Get all sensor readings at once."""
    try:
        proxy = _get_proxy(request)
        node_id = _get_node_id(request)
        result = await proxy.forward(node_id, "GET", "/sensors")
        return JSONResponse(content=result)
    except ProxyError as exc:
        return JSONResponse(status_code=502, content={"detail": str(exc)})
