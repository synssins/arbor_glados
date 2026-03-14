"""
Klipper/Moonraker direct access API routes.

Provides raw G-code execution, printer status, and object queries
for direct Klipper interaction outside the robotics abstraction.

Task: Robotics Phase 1
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel, Field

from arbor_core.robotics.backends.klipper_backend import KlipperBackend, KlipperError

klipper_router = APIRouter(prefix="/klipper", tags=["klipper"])


class GcodeRequest(BaseModel):
    """G-code execution request."""

    script: str = Field(
        min_length=1,
        description="G-code command string to execute.",
    )


class ObjectQueryRequest(BaseModel):
    """Klipper object query request."""

    objects: dict[str, list[str] | None] = Field(
        description=(
            "Dict of object_name -> list of fields (or null for all fields). "
            "Example: {'manual_stepper stepper_x': ['position'], 'heater_bed': null}"
        ),
    )


def _get_klipper(request: Request) -> KlipperBackend:
    """Get Klipper backend from app state."""
    klipper = getattr(request.app.state, "klipper_backend", None)
    if klipper is None:
        raise HTTPException(
            status_code=503,
            detail="Klipper backend not initialized. Is Moonraker running?",
        )
    return klipper


@klipper_router.post(
    "/gcode",
    summary="Send G-code",
    description="Execute raw G-code on Klipper via Moonraker.",
)
async def send_gcode(cmd: GcodeRequest, request: Request):
    """
    Send raw G-code to Klipper.

    This is the lowest-level Klipper interface. Use robot endpoints
    for coordinated motion control.
    """
    klipper = _get_klipper(request)
    try:
        result = await klipper.send_gcode(cmd.script)
    except KlipperError as exc:
        # Return error as a normal response so the console can display it
        return {"status": "error", "error": str(exc)}
    return {"status": "ok", "result": result}


@klipper_router.get(
    "/status",
    summary="Klipper printer status",
    description="Get current Klipper/Moonraker printer status.",
)
async def get_status(request: Request):
    """Get Klipper printer info (state, hostname, etc.)."""
    klipper = _get_klipper(request)
    try:
        result = await klipper.query_status()
    except KlipperError as exc:
        raise HTTPException(status_code=502, detail=str(exc))
    return result


@klipper_router.get(
    "/objects",
    summary="List Klipper objects",
    description="List available Klipper printer objects.",
)
async def list_objects(request: Request):
    """List all available Klipper printer objects."""
    klipper = _get_klipper(request)
    try:
        result = await klipper.query_objects(objects=None)
    except KlipperError as exc:
        raise HTTPException(status_code=502, detail=str(exc))
    return result


@klipper_router.post(
    "/objects/query",
    summary="Query Klipper objects",
    description="Query specific Klipper printer objects and their fields.",
)
async def query_objects(query: ObjectQueryRequest, request: Request):
    """
    Query specific Klipper objects.

    Example body:
    {
        "objects": {
            "manual_stepper stepper_x": ["position"],
            "heater_bed": null
        }
    }
    """
    klipper = _get_klipper(request)
    try:
        result = await klipper.query_objects(query.objects)
    except KlipperError as exc:
        raise HTTPException(status_code=502, detail=str(exc))
    return result
