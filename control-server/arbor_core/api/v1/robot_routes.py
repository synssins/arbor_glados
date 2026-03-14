"""
Robot management and motion control API routes.

Provides CRUD for robot profiles and motion endpoints for
joint-space, Cartesian, and velocity commands.

Task: Robotics Phase 1
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request

from arbor_core.robotics.engine import EngineError
from arbor_core.robotics.models import (
    CartesianPose,
    JointMoveCommand,
    RobotProfile,
    VelocityCommand,
)
from arbor_core.robotics.registry import RegistryError

robot_router = APIRouter(prefix="/robots", tags=["robots"])


def _get_registry(request: Request):
    """Get robot registry from app state."""
    registry = getattr(request.app.state, "robot_registry", None)
    if registry is None:
        raise HTTPException(
            status_code=503,
            detail="Robot registry not initialized",
        )
    return registry


def _get_engine(request: Request):
    """Get kinematics engine from app state."""
    engine = getattr(request.app.state, "kinematics_engine", None)
    if engine is None:
        raise HTTPException(
            status_code=503,
            detail="Kinematics engine not initialized",
        )
    return engine


# ---------------------------------------------------------------------------
# Profile CRUD
# ---------------------------------------------------------------------------
@robot_router.post(
    "",
    summary="Create robot profile",
    description="Register a new robot profile with joints and actuator mappings.",
    status_code=201,
)
async def create_robot(profile: RobotProfile, request: Request):
    """Create a new robot profile."""
    registry = _get_registry(request)
    try:
        registry.add(profile)
    except RegistryError as exc:
        raise HTTPException(status_code=409, detail=str(exc))
    return {"status": "created", "robot_id": profile.id}


@robot_router.get(
    "",
    summary="List robot profiles",
    description="Return all registered robot profiles.",
)
async def list_robots(request: Request):
    """List all robot profiles."""
    registry = _get_registry(request)
    profiles = registry.list_all()
    return {
        "robots": [p.model_dump() for p in profiles],
        "count": len(profiles),
    }


@robot_router.get(
    "/{robot_id}",
    summary="Get robot profile",
    description="Return a specific robot profile by ID.",
)
async def get_robot(robot_id: str, request: Request):
    """Get a robot profile by ID."""
    registry = _get_registry(request)
    try:
        profile = registry.get(robot_id)
    except RegistryError as exc:
        raise HTTPException(status_code=404, detail=str(exc))
    return profile.model_dump()


@robot_router.put(
    "/{robot_id}",
    summary="Update robot profile",
    description="Replace an existing robot profile.",
)
async def update_robot(robot_id: str, profile: RobotProfile, request: Request):
    """Update an existing robot profile."""
    if profile.id != robot_id:
        raise HTTPException(
            status_code=400,
            detail=f"Profile ID '{profile.id}' does not match URL '{robot_id}'",
        )
    registry = _get_registry(request)
    try:
        registry.update(profile)
    except RegistryError as exc:
        raise HTTPException(status_code=404, detail=str(exc))
    return {"status": "updated", "robot_id": robot_id}


@robot_router.delete(
    "/{robot_id}",
    summary="Delete robot profile",
    description="Remove a robot profile.",
)
async def delete_robot(robot_id: str, request: Request):
    """Delete a robot profile."""
    registry = _get_registry(request)
    try:
        registry.remove(robot_id)
    except RegistryError as exc:
        raise HTTPException(status_code=404, detail=str(exc))
    return {"status": "deleted", "robot_id": robot_id}


# ---------------------------------------------------------------------------
# Motion endpoints
# ---------------------------------------------------------------------------
@robot_router.post(
    "/{robot_id}/joints",
    summary="Joint-space move",
    description="Move specific joints to target positions (radians or meters).",
)
async def move_joints(robot_id: str, cmd: JointMoveCommand, request: Request):
    """
    Direct joint-space move.

    Validates limits, applies scale/offset, dispatches to backends.
    """
    engine = _get_engine(request)
    try:
        result = await engine.move_joints(robot_id, cmd.positions, speed=cmd.speed)
    except EngineError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    return result


@robot_router.post(
    "/{robot_id}/move",
    summary="Cartesian move (IK)",
    description="Move end-effector to a Cartesian pose using inverse kinematics.",
)
async def move_cartesian(robot_id: str, pose: CartesianPose, request: Request):
    """
    Cartesian move using IK solver.

    Phase 3: Currently returns not-implemented.
    """
    engine = _get_engine(request)
    try:
        result = await engine.move_cartesian(robot_id, pose)
    except EngineError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    return result


@robot_router.post(
    "/{robot_id}/drive",
    summary="Vehicle velocity command",
    description="Send a velocity command to a wheeled/tracked vehicle.",
)
async def drive_vehicle(robot_id: str, cmd: VelocityCommand, request: Request):
    """
    Vehicle velocity command.

    Phase 2: Currently returns not-implemented.
    """
    engine = _get_engine(request)
    try:
        result = await engine.drive(robot_id, cmd)
    except EngineError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    return result


@robot_router.get(
    "/{robot_id}/state",
    summary="Get robot state",
    description="Return current joint positions and end-effector pose.",
)
async def get_state(robot_id: str, request: Request):
    """Get current robot state."""
    engine = _get_engine(request)
    try:
        state = await engine.get_state(robot_id)
    except EngineError as exc:
        raise HTTPException(status_code=404, detail=str(exc))
    return state.model_dump()


@robot_router.post(
    "/{robot_id}/home",
    summary="Home robot",
    description="Move all joints to their configured home positions.",
)
async def home_robot(robot_id: str, request: Request):
    """Home all joints."""
    engine = _get_engine(request)
    try:
        result = await engine.home(robot_id)
    except EngineError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    return result


@robot_router.post(
    "/{robot_id}/stop",
    summary="Stop robot",
    description="Stop all motion on the robot immediately.",
)
async def stop_robot(robot_id: str, request: Request):
    """Emergency stop all joints."""
    engine = _get_engine(request)
    try:
        result = await engine.stop(robot_id)
    except EngineError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    return result
