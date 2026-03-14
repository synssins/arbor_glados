"""
Robotics control system — kinematics, backends, and motion dispatch.

Provides unified control of robot arms and vehicles through multiple
actuator backends (Arbor smart servos, Klipper steppers/servos).

Task: Robotics Phase 1
"""

from arbor_core.robotics.engine import KinematicsEngine
from arbor_core.robotics.models import (
    ActuatorBackendType,
    ActuatorMapping,
    CartesianPose,
    DHParameter,
    JointConfig,
    RobotProfile,
    RobotState,
    RobotType,
    VehicleGeometry,
    VelocityCommand,
)
from arbor_core.robotics.registry import RobotRegistry

__all__ = [
    "ActuatorBackendType",
    "ActuatorMapping",
    "CartesianPose",
    "DHParameter",
    "JointConfig",
    "KinematicsEngine",
    "RobotProfile",
    "RobotRegistry",
    "RobotState",
    "RobotType",
    "VehicleGeometry",
    "VelocityCommand",
]
