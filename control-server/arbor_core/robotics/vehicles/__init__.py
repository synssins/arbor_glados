"""
Vehicle kinematics solvers.

Converts body-frame velocity commands into individual wheel
commands for different vehicle configurations.

Task: Robotics Phase 2
"""

from arbor_core.robotics.vehicles.ackermann import AckermannSolver
from arbor_core.robotics.vehicles.differential import DifferentialDriveSolver
from arbor_core.robotics.vehicles.mecanum import MecanumSolver

__all__ = [
    "AckermannSolver",
    "DifferentialDriveSolver",
    "MecanumSolver",
]
