"""
API v1 main router.

Aggregates all v1 endpoint routers.
"""

from fastapi import APIRouter
from fastapi.responses import JSONResponse

from arbor_core.api.v1.auth_routes import auth_router
from arbor_core.api.v1.emergency_routes import emergency_router
from arbor_core.api.v1.sensor_routes import sensor_router, sensors_router
from arbor_core.api.v1.servo_routes import servo_router
from arbor_core.api.v1.system_routes import system_router
from arbor_core.api.v1.ws_routes import ws_router

router = APIRouter(tags=["v1"])
router.include_router(auth_router)
router.include_router(system_router)
router.include_router(servo_router)
router.include_router(sensor_router)
router.include_router(sensors_router)
router.include_router(emergency_router)
router.include_router(ws_router)


@router.get(
    "/health",
    summary="Health check",
    description="Basic health check endpoint. Returns server status. "
    "This endpoint is exempt from authentication as per SECURITY.md.",
    response_description="Health status",
)
async def health_check() -> JSONResponse:
    """
    Health check endpoint.

    Returns basic server health status. This is the only endpoint
    that does not require authentication per SECURITY.md.

    Returns:
        JSON with status and version.
    """
    return JSONResponse(
        content={
            "status": "healthy",
            "version": "0.1.0",
        }
    )


# All route modules included:
# - system_router (C10): /system/info, /system/health, /system/config
# - servo_router (C11): /servo/{id}/state, /servo/{id}/position, etc.
# - sensor_router (C12): /sensor/{id}/reading, /sensor/{id}/history
# - emergency_router (C13): /emergency-stop
# - auth_router (C15): /auth/login, /auth/logout, /auth/api-keys
# - ws_router (C14): /ws WebSocket endpoint
