"""
SCARA (Selective Compliance Articulated Robot Arm) IK/FK solver.

Analytical 2-link planar IK + Z prismatic joint.
Sub-millisecond solve time — no external dependencies.

SCARA geometry:
    Joint 1 (theta1): Revolute, base rotation
    Joint 2 (theta2): Revolute, elbow
    Joint 3 (z):      Prismatic, vertical
    Joint 4 (theta4): Revolute, wrist rotation (optional)

    Link 1 length: a1 (from DH param 0)
    Link 2 length: a2 (from DH param 1)

IK Math:
    cos_theta2 = (x² + y² - a1² - a2²) / (2 * a1 * a2)
    theta2 = acos(cos_theta2)
    theta1 = atan2(y, x) - atan2(a2 * sin(theta2), a1 + a2 * cos(theta2))
    z = target_z

Task: Robotics Phase 3
"""

from __future__ import annotations

import math

import structlog

from arbor_core.robotics.models import CartesianPose, DHParameter, JointConfig

logger = structlog.get_logger(__name__)


class SCARAError(Exception):
    """Raised when SCARA IK/FK fails."""


class SCARASolver:
    """
    Analytical IK/FK solver for SCARA robots.

    Expects 2 revolute links (planar) + 1 prismatic (Z) + optional wrist.
    Uses closed-form solution for sub-millisecond solve time.
    """

    def __init__(
        self,
        dh_parameters: list[DHParameter],
        joints: list[JointConfig],
        name: str = "scara",
    ) -> None:
        """
        Args:
            dh_parameters: DH parameters. First two define link lengths (a).
            joints: Joint configurations.
            name: Robot name for logging.

        Raises:
            SCARAError: If DH parameters are insufficient.
        """
        if len(dh_parameters) < 2:
            msg = "SCARA requires at least 2 DH parameters (2 link lengths)"
            raise SCARAError(msg)

        self._a1 = dh_parameters[0].a  # Link 1 length
        self._a2 = dh_parameters[1].a  # Link 2 length
        self._joints = joints
        self._joint_names = [j.name for j in joints]
        self._name = name

        if self._a1 <= 0 or self._a2 <= 0:
            msg = f"SCARA link lengths must be positive: a1={self._a1}, a2={self._a2}"
            raise SCARAError(msg)

        logger.info(
            "scara_created",
            name=name,
            a1=self._a1,
            a2=self._a2,
            joints=len(joints),
        )

    def forward_kinematics(
        self, joint_values: list[float]
    ) -> CartesianPose:
        """
        Compute end-effector pose from joint values.

        Args:
            joint_values: [theta1, theta2, z, ...] (radians, radians, meters, ...)

        Returns:
            End-effector pose (x, y, z + yaw from wrist if present).
        """
        if len(joint_values) < 3:
            msg = f"SCARA needs at least 3 joint values, got {len(joint_values)}"
            raise SCARAError(msg)

        theta1 = joint_values[0]
        theta2 = joint_values[1]
        z = joint_values[2]

        # Planar FK
        x = self._a1 * math.cos(theta1) + self._a2 * math.cos(theta1 + theta2)
        y = self._a1 * math.sin(theta1) + self._a2 * math.sin(theta1 + theta2)

        # Wrist rotation (4th joint, optional)
        yaw = 0.0
        if len(joint_values) >= 4:
            yaw = joint_values[3]

        return CartesianPose(x=x, y=y, z=z, yaw=yaw)

    def inverse_kinematics(
        self,
        target_pose: CartesianPose,
        elbow_up: bool = True,
    ) -> list[float]:
        """
        Compute joint angles for a target position.

        Analytical closed-form solution — sub-millisecond.

        Args:
            target_pose: Target position (x, y, z).
                         yaw is mapped to wrist joint if present.
            elbow_up: If True, use elbow-up solution (theta2 > 0).
                      If False, use elbow-down (theta2 < 0).

        Returns:
            Joint values [theta1, theta2, z, theta4?].

        Raises:
            SCARAError: If target is out of reach.
        """
        x = target_pose.x
        y = target_pose.y
        z = target_pose.z

        # Check reachability
        distance_sq = x * x + y * y
        max_reach = self._a1 + self._a2
        min_reach = abs(self._a1 - self._a2)

        distance = math.sqrt(distance_sq)
        if distance > max_reach + 1e-6:
            msg = (
                f"Target ({x:.3f}, {y:.3f}) is beyond reach "
                f"(distance={distance:.3f}, max={max_reach:.3f})"
            )
            raise SCARAError(msg)

        if distance < min_reach - 1e-6:
            msg = (
                f"Target ({x:.3f}, {y:.3f}) is inside minimum reach "
                f"(distance={distance:.3f}, min={min_reach:.3f})"
            )
            raise SCARAError(msg)

        # Clamp for numerical stability at workspace boundary
        cos_theta2 = (distance_sq - self._a1**2 - self._a2**2) / (
            2 * self._a1 * self._a2
        )
        cos_theta2 = max(-1.0, min(1.0, cos_theta2))

        # Theta2 (elbow angle)
        theta2 = math.acos(cos_theta2)
        if not elbow_up:
            theta2 = -theta2

        # Theta1 (base angle)
        k1 = self._a1 + self._a2 * cos_theta2
        k2 = self._a2 * math.sin(theta2)
        theta1 = math.atan2(y, x) - math.atan2(k2, k1)

        result = [theta1, theta2, z]

        # Wrist rotation (4th joint maps to yaw)
        if len(self._joints) >= 4:
            result.append(target_pose.yaw)

        logger.debug(
            "scara_ik_solved",
            name=self._name,
            target=[x, y, z],
            theta1=theta1,
            theta2=theta2,
        )

        return result

    def joint_names(self) -> list[str]:
        """Return ordered list of joint names."""
        return list(self._joint_names)
