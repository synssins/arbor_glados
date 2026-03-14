"""
Serial chain IK/FK solver using ikpy.

Constructs a kinematic chain from DH parameters and provides
forward/inverse kinematics for arbitrary N-DOF serial chain arms.

Performance: 7-50ms per IK solve on Pi 5 (numerical iterative).

Requires: ikpy>=3.3.4, numpy>=1.24
These are gated behind a decision request (DECISION_REQUEST_2.md).
If not installed, this module raises ImportError with instructions.

Task: Robotics Phase 3
"""

from __future__ import annotations

import math
from typing import TYPE_CHECKING

import structlog

from arbor_core.robotics.models import CartesianPose, DHParameter, JointConfig

if TYPE_CHECKING:
    pass

logger = structlog.get_logger(__name__)

# Lazy import ikpy — may not be installed yet (pending decision request)
_ikpy_available = False
try:
    import numpy as np
    from ikpy.chain import Chain
    from ikpy.link import URDFLink

    _ikpy_available = True
except ImportError:
    np = None  # type: ignore[assignment]
    Chain = None  # type: ignore[assignment, misc]
    URDFLink = None  # type: ignore[assignment, misc]


class SerialChainError(Exception):
    """Raised when serial chain IK/FK fails."""


class SerialChainSolver:
    """
    IK/FK solver for serial chain robot arms.

    Builds an ikpy Chain from DH parameters and joint configurations.
    Supports arbitrary N-DOF chains (6DOF, 9DOF, etc.).

    Usage::

        solver = SerialChainSolver(dh_params, joints)
        pose = solver.forward_kinematics([0, 0, 0, 0, 0, 0])
        angles = solver.inverse_kinematics(CartesianPose(x=0.15, z=0.25))
    """

    def __init__(
        self,
        dh_parameters: list[DHParameter],
        joints: list[JointConfig],
        name: str = "arm",
    ) -> None:
        """
        Args:
            dh_parameters: DH parameters for each link.
            joints: Joint configurations (for limits and names).
            name: Chain name for logging.

        Raises:
            SerialChainError: If ikpy is not installed.
        """
        if not _ikpy_available:
            msg = (
                "ikpy and numpy are required for serial chain IK. "
                "Install with: pip install ikpy numpy. "
                "See decisions/DECISION_REQUEST_2.md"
            )
            raise SerialChainError(msg)

        self._name = name
        self._joints = joints
        self._joint_names = [j.name for j in joints]

        # Build ikpy chain from DH parameters
        links = []

        # Base link (fixed, origin)
        links.append(URDFLink(
            name="base_link",
            origin_translation=[0, 0, 0],
            origin_orientation=[0, 0, 0],
            joint_type="fixed",
        ))

        # Active links from DH parameters
        for i, (dh, joint) in enumerate(zip(dh_parameters, joints)):
            bounds = None
            if joint.min_value is not None and joint.max_value is not None:
                bounds = (joint.min_value, joint.max_value)

            if joint.joint_type == "prismatic":
                jtype = "prismatic"
            else:
                jtype = "revolute"

            links.append(URDFLink(
                name=joint.name,
                origin_translation=[dh.a, 0, dh.d],
                origin_orientation=[dh.alpha, 0, dh.theta_offset],
                joint_type=jtype,
                bounds=bounds,
            ))

        # End-effector (fixed)
        links.append(URDFLink(
            name="end_effector",
            origin_translation=[0, 0, 0],
            origin_orientation=[0, 0, 0],
            joint_type="fixed",
        ))

        self._chain = Chain.from_links(links, name=name)
        self._n_joints = len(joints)

        logger.info(
            "serial_chain_created",
            name=name,
            dof=self._n_joints,
            links=len(links),
        )

    def forward_kinematics(
        self, joint_angles: list[float]
    ) -> CartesianPose:
        """
        Compute end-effector pose from joint angles.

        Args:
            joint_angles: List of joint values (radians/meters).
                          Must match number of active joints.

        Returns:
            6DOF end-effector pose.

        Raises:
            SerialChainError: If joint count is wrong.
        """
        if len(joint_angles) != self._n_joints:
            msg = (
                f"Expected {self._n_joints} joint values, "
                f"got {len(joint_angles)}"
            )
            raise SerialChainError(msg)

        # ikpy expects values for all links (including fixed base + end effector)
        # Active joints are indices 1..N, base is 0, end effector is N+1
        full_angles = [0.0] + list(joint_angles) + [0.0]

        # Compute forward kinematics (4x4 homogeneous transform)
        transform = self._chain.forward_kinematics(full_angles)

        # Extract position
        x, y, z = transform[0, 3], transform[1, 3], transform[2, 3]

        # Extract euler angles from rotation matrix
        roll, pitch, yaw = self._rotation_matrix_to_euler(transform[:3, :3])

        return CartesianPose(
            x=x, y=y, z=z,
            roll=roll, pitch=pitch, yaw=yaw,
        )

    def inverse_kinematics(
        self,
        target_pose: CartesianPose,
        initial_angles: list[float] | None = None,
    ) -> list[float]:
        """
        Compute joint angles for a target end-effector pose.

        Uses ikpy's numerical IK solver (iterative, 7-50ms on Pi 5).

        Args:
            target_pose: Target 6DOF pose.
            initial_angles: Initial guess for iterative solver.
                           If None, uses zeros.

        Returns:
            List of joint angles (radians/meters).

        Raises:
            SerialChainError: If IK fails to converge.
        """
        target_position = [target_pose.x, target_pose.y, target_pose.z]

        # Build target orientation matrix from euler angles
        target_orientation = self._euler_to_rotation_matrix(
            target_pose.roll, target_pose.pitch, target_pose.yaw
        )

        # Initial guess
        if initial_angles is not None:
            initial = [0.0] + list(initial_angles) + [0.0]
        else:
            initial = [0.0] * (self._n_joints + 2)

        try:
            result = self._chain.inverse_kinematics(
                target_position=target_position,
                target_orientation=target_orientation,
                orientation_mode="all",
                initial_position=initial,
            )

            # Extract active joint values (skip base and end effector)
            joint_angles = list(result[1 : self._n_joints + 1])

            logger.debug(
                "ik_solved",
                name=self._name,
                target=[target_pose.x, target_pose.y, target_pose.z],
                angles=joint_angles,
            )

            return joint_angles

        except Exception as exc:
            msg = f"IK solve failed for {self._name}: {exc}"
            raise SerialChainError(msg) from exc

    def joint_names(self) -> list[str]:
        """Return ordered list of joint names."""
        return list(self._joint_names)

    @staticmethod
    def _rotation_matrix_to_euler(R: "np.ndarray") -> tuple[float, float, float]:
        """
        Convert 3x3 rotation matrix to roll-pitch-yaw euler angles (XYZ).

        Returns:
            Tuple of (roll, pitch, yaw) in radians.
        """
        sy = math.sqrt(R[0, 0] ** 2 + R[1, 0] ** 2)

        if sy > 1e-6:
            roll = math.atan2(R[2, 1], R[2, 2])
            pitch = math.atan2(-R[2, 0], sy)
            yaw = math.atan2(R[1, 0], R[0, 0])
        else:
            roll = math.atan2(-R[1, 2], R[1, 1])
            pitch = math.atan2(-R[2, 0], sy)
            yaw = 0.0

        return roll, pitch, yaw

    @staticmethod
    def _euler_to_rotation_matrix(
        roll: float, pitch: float, yaw: float
    ) -> "np.ndarray":
        """
        Convert roll-pitch-yaw euler angles to 3x3 rotation matrix (XYZ).

        Returns:
            3x3 numpy rotation matrix.
        """
        cr, sr = math.cos(roll), math.sin(roll)
        cp, sp = math.cos(pitch), math.sin(pitch)
        cy, sy = math.cos(yaw), math.sin(yaw)

        return np.array([
            [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
            [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
            [-sp, cp * sr, cp * cr],
        ])
