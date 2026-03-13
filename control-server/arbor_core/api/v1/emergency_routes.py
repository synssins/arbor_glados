"""
Emergency stop endpoint — broadcasts torque-disable to all nodes.

Parity with ESP32 firmware F14 endpoint.

Per ARBOR_PROJECT_PLAN.md:
    POST /api/v1/emergency-stop  # all-servo torque disable, no rate limit

This endpoint is exempt from rate limiting (configured in auth middleware).
It broadcasts to ALL connected nodes for maximum safety.

Task: C13
"""

from __future__ import annotations

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from arbor_core.bridges.proxy import NodeProxy, ProxyError

logger = structlog.get_logger(__name__)

emergency_router = APIRouter(tags=["emergency"])


@emergency_router.post(
    "/emergency-stop",
    summary="Emergency stop",
    description="Immediately disables torque on all servos across all connected nodes. "
    "This endpoint is exempt from rate limiting.",
)
async def emergency_stop(request: Request) -> JSONResponse:
    """
    Emergency stop — broadcast torque-disable to all nodes.

    This is the highest-priority command. It bypasses rate limiting
    and broadcasts to every connected node simultaneously.
    """
    proxy: NodeProxy | None = getattr(request.app.state, "node_proxy", None)

    if proxy is None:
        logger.error("emergency_stop_no_proxy")
        return JSONResponse(
            status_code=503,
            content={"detail": "Node proxy not configured", "stopped": False},
        )

    logger.warning("emergency_stop_triggered")

    try:
        results = await proxy.forward_to_all("POST", "/emergency-stop")
    except ProxyError as exc:
        logger.error("emergency_stop_failed", error=str(exc))
        return JSONResponse(
            status_code=502,
            content={"detail": str(exc), "stopped": False},
        )

    # Check if any node successfully stopped
    any_success = any(
        isinstance(r, dict) for r in results.values()
    )

    status_code = 200 if any_success else 503
    return JSONResponse(
        status_code=status_code,
        content={
            "stopped": any_success,
            "nodes": {
                node_id: r if isinstance(r, str) else r
                for node_id, r in results.items()
            },
        },
    )
