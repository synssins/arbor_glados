"""
Pydantic models for the robotics control system.

Defines robot profiles, joint configurations, actuator mappings,
and motion command schemas. Zero hardcoded values — all tunable
through configuration.

Task: Robotics Phase 1
"""

from __future__ import annotations

import time
from enum import Enum
from typing import Literal

from pydantic import BaseModel, Field, field_validator


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------
class RobotType(str, Enum):
    """Supported robot kinematic types."""

    SERIAL_ARM = "serial_arm"       # N-DOF serial chain (6DOF, 9DOF, etc.)
    SCARA = "scara"                 # 2-link planar + Z prismatic
    DIFFERENTIAL = "differential"   # Tank treads / 2-wheel differential drive
    ACKERMANN = "ackermann"         # Car-style steering (front/rear/both axle)
    MECANUM = "mecanum"             # 4-wheel omnidirectional


class ActuatorBackendType(str, Enum):
    """Supported actuator backend types."""

    ARBOR_SERVO = "arbor_servo"         # Feetech STS/SCS via ESP32 node
    KLIPPER_STEPPER = "klipper_stepper" # Klipper MANUAL_STEPPER via Moonraker
    KLIPPER_SERVO = "klipper_servo"     # Klipper SET_SERVO via Moonraker


# ---------------------------------------------------------------------------
# Actuator mapping
# ---------------------------------------------------------------------------
class ActuatorMapping(BaseModel):
    """
    Maps a logical joint to a physical actuator.

    The scale/offset transform converts joint-space values to hardware values:
        hw_value = joint_value * scale + offset
    """

    backend: ActuatorBackendType = Field(
        description="Which actuator backend drives this joint.",
    )
    # Arbor servo fields
    node_id: str | None = Field(
        default=None,
        description="Arbor node ID for arbor_servo backend.",
    )
    servo_id: int | None = Field(
        default=None,
        ge=0,
        le=253,
        description="Servo bus ID for arbor_servo backend.",
    )
    # Klipper fields
    klipper_name: str | None = Field(
        default=None,
        description="Klipper stepper/servo name (e.g. 'stepper_x', 'servo0').",
    )
    # Calibration
    scale: float = Field(
        default=1.0,
        description="Multiplier: hw_value = joint_value * scale + offset.",
    )
    offset: float = Field(
        default=0.0,
        description="Offset: hw_value = joint_value * scale + offset.",
    )

    @field_validator("node_id")
    @classmethod
    def arbor_servo_needs_node(cls, v: str | None, info: object) -> str | None:
        """Arbor servo backend requires node_id."""
        data = getattr(info, "data", {})
        backend = data.get("backend")
        if backend == ActuatorBackendType.ARBOR_SERVO and v is None:
            msg = "node_id is required for arbor_servo backend"
            raise ValueError(msg)
        return v

    @field_validator("servo_id")
    @classmethod
    def arbor_servo_needs_servo_id(cls, v: int | None, info: object) -> int | None:
        """Arbor servo backend requires servo_id."""
        data = getattr(info, "data", {})
        backend = data.get("backend")
        if backend == ActuatorBackendType.ARBOR_SERVO and v is None:
            msg = "servo_id is required for arbor_servo backend"
            raise ValueError(msg)
        return v

    @field_validator("klipper_name")
    @classmethod
    def klipper_needs_name(cls, v: str | None, info: object) -> str | None:
        """Klipper backends require klipper_name."""
        data = getattr(info, "data", {})
        backend = data.get("backend")
        if backend in (
            ActuatorBackendType.KLIPPER_STEPPER,
            ActuatorBackendType.KLIPPER_SERVO,
        ) and v is None:
            msg = "klipper_name is required for klipper backends"
            raise ValueError(msg)
        return v


# ---------------------------------------------------------------------------
# Joint configuration
# ---------------------------------------------------------------------------
class JointConfig(BaseModel):
    """Configuration for a single robot joint."""

    name: str = Field(
        min_length=1,
        max_length=64,
        description="Joint name. Must match solver output names.",
    )
    joint_type: Literal["revolute", "prismatic", "continuous", "wheel"] = Field(
        description=(
            "Joint type: revolute (limited rotation), prismatic (linear), "
            "continuous (unlimited rotation), wheel (velocity-controlled)."
        ),
    )
    min_value: float | None = Field(
        default=None,
        description="Minimum joint-space value (radians or meters). None = no limit.",
    )
    max_value: float | None = Field(
        default=None,
        description="Maximum joint-space value (radians or meters). None = no limit.",
    )
    home_value: float = Field(
        default=0.0,
        description="Home position value.",
    )
    actuator: ActuatorMapping = Field(
        description="Physical actuator this joint maps to.",
    )

    @field_validator("max_value")
    @classmethod
    def max_gte_min(cls, v: float | None, info: object) -> float | None:
        """Ensure max_value >= min_value when both are set."""
        data = getattr(info, "data", {})
        min_val = data.get("min_value")
        if v is not None and min_val is not None and v < min_val:
            msg = f"max_value ({v}) must be >= min_value ({min_val})"
            raise ValueError(msg)
        return v


# ---------------------------------------------------------------------------
# DH parameters (arm links)
# ---------------------------------------------------------------------------
class DHParameter(BaseModel):
    """
    Denavit-Hartenberg parameters for a single arm link.

    Standard DH convention: d (link offset), a (link length),
    alpha (link twist), theta_offset (joint angle offset).
    """

    d: float = Field(default=0.0, description="Link offset along Z (meters).")
    a: float = Field(default=0.0, description="Link length along X (meters).")
    alpha: float = Field(default=0.0, description="Link twist around X (radians).")
    theta_offset: float = Field(
        default=0.0,
        description="Joint angle offset (radians). Added to joint variable.",
    )


# ---------------------------------------------------------------------------
# Vehicle geometry
# ---------------------------------------------------------------------------
class VehicleGeometry(BaseModel):
    """Geometric parameters for a wheeled/tracked vehicle."""

    wheelbase: float = Field(
        gt=0,
        description="Distance between front and rear axles (meters).",
    )
    track_width: float = Field(
        gt=0,
        description="Distance between left and right wheels (meters).",
    )
    wheel_radius: float = Field(
        gt=0,
        description="Wheel radius (meters).",
    )
    max_steering_angle: float | None = Field(
        default=None,
        ge=0,
        description="Maximum steering angle (radians). For Ackermann only.",
    )
    steering_axles: Literal["front", "rear", "both"] = Field(
        default="front",
        description="Which axle(s) steer. For Ackermann only.",
    )


# ---------------------------------------------------------------------------
# Robot profile
# ---------------------------------------------------------------------------
class RobotProfile(BaseModel):
    """
    Complete robot definition including joints, kinematics, and actuator mappings.

    This is the primary configuration object for a robot. It defines all
    joints, their physical actuators, and kinematic parameters needed
    for motion computation.
    """

    id: str = Field(
        min_length=1,
        max_length=64,
        description="Unique robot identifier slug.",
    )
    name: str = Field(
        min_length=1,
        max_length=128,
        description="Human-readable robot name.",
    )
    robot_type: RobotType = Field(
        description="Kinematic type of this robot.",
    )
    joints: list[JointConfig] = Field(
        min_length=1,
        description="Joint definitions. Order matters for serial chains.",
    )
    dh_parameters: list[DHParameter] = Field(
        default_factory=list,
        description="DH parameters for arm links. Required for serial_arm type.",
    )
    vehicle_geometry: VehicleGeometry | None = Field(
        default=None,
        description="Vehicle geometry. Required for vehicle types.",
    )
    description: str = Field(
        default="",
        max_length=512,
        description="Optional description of this robot.",
    )

    @field_validator("dh_parameters")
    @classmethod
    def arms_need_dh(cls, v: list[DHParameter], info: object) -> list[DHParameter]:
        """Serial arm type requires DH parameters."""
        data = getattr(info, "data", {})
        robot_type = data.get("robot_type")
        if robot_type == RobotType.SERIAL_ARM and len(v) == 0:
            msg = "dh_parameters required for serial_arm robot type"
            raise ValueError(msg)
        return v

    @field_validator("vehicle_geometry")
    @classmethod
    def vehicles_need_geometry(
        cls, v: VehicleGeometry | None, info: object
    ) -> VehicleGeometry | None:
        """Vehicle types require vehicle_geometry."""
        data = getattr(info, "data", {})
        robot_type = data.get("robot_type")
        vehicle_types = {
            RobotType.DIFFERENTIAL,
            RobotType.ACKERMANN,
            RobotType.MECANUM,
        }
        if robot_type in vehicle_types and v is None:
            msg = f"vehicle_geometry required for {robot_type} robot type"
            raise ValueError(msg)
        return v


# ---------------------------------------------------------------------------
# Motion commands and state
# ---------------------------------------------------------------------------
class CartesianPose(BaseModel):
    """6DOF Cartesian pose (position + orientation)."""

    x: float = Field(default=0.0, description="X position (meters).")
    y: float = Field(default=0.0, description="Y position (meters).")
    z: float = Field(default=0.0, description="Z position (meters).")
    roll: float = Field(default=0.0, description="Roll angle (radians).")
    pitch: float = Field(default=0.0, description="Pitch angle (radians).")
    yaw: float = Field(default=0.0, description="Yaw angle (radians).")


class VelocityCommand(BaseModel):
    """Vehicle velocity command in body frame."""

    linear_x: float = Field(
        default=0.0,
        description="Forward velocity (m/s). Positive = forward.",
    )
    linear_y: float = Field(
        default=0.0,
        description="Lateral velocity (m/s). Positive = left. Mecanum only.",
    )
    angular_z: float = Field(
        default=0.0,
        description="Yaw rate (rad/s). Positive = counter-clockwise.",
    )


class JointMoveCommand(BaseModel):
    """Direct joint-space move command."""

    positions: dict[str, float] = Field(
        description="Joint name -> target value (radians or meters).",
    )
    speed: float | None = Field(
        default=None,
        ge=0,
        description="Move speed. Interpretation depends on backend.",
    )


class RobotState(BaseModel):
    """Current state of a robot."""

    robot_id: str = Field(description="Robot profile ID.")
    joint_positions: dict[str, float] = Field(
        default_factory=dict,
        description="Current joint positions (name -> value).",
    )
    end_effector_pose: CartesianPose | None = Field(
        default=None,
        description="End-effector pose. Only for arm types with FK.",
    )
    timestamp: float = Field(
        default_factory=time.time,
        description="Timestamp of this state reading.",
    )
