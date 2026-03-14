"""
Arm kinematics solvers — IK/FK for serial chains and SCARA.

SerialChainSolver: wraps ikpy for arbitrary N-DOF chains (6DOF, 9DOF, etc.)
SCARASolver: analytical 2-link planar IK + Z prismatic

Task: Robotics Phase 3
"""

from arbor_core.robotics.arms.scara import SCARASolver
from arbor_core.robotics.arms.serial_chain import SerialChainSolver

__all__ = [
    "SCARASolver",
    "SerialChainSolver",
]
