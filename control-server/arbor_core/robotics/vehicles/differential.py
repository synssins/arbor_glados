"""
Differential drive kinematics solver.

Converts body-frame velocity commands into left/right wheel velocities
for tank-tread or 2-wheel differential drive robots.

Math:
    v_left  = (linear_x - angular_z * track_width / 2) / wheel_radius
    v_right = (linear_x + angular_z * track_width / 2) / wheel_radius

No external dependencies — pure stdlib math.

Task: Robotics Phase 2
"""

from __future__ import annotations

from dataclasses import dataclass

from arbor_core.robotics.models import VehicleGeometry, VelocityCommand


@dataclass(frozen=True)
class DifferentialWheelSpeeds:
    """Computed wheel speeds for a differential drive robot."""

    left_wheel: float   # rad/s
    right_wheel: float  # rad/s


class DifferentialDriveSolver:
    """
    Differential drive kinematics.

    Supports 2-wheel and tank-tread configurations.
    Assumes symmetric left/right wheel arrangement.
    """

    def __init__(self, geometry: VehicleGeometry) -> None:
        """
        Args:
            geometry: Vehicle geometry with track_width and wheel_radius.
        """
        self._track_width = geometry.track_width
        self._wheel_radius = geometry.wheel_radius

    def solve(self, cmd: VelocityCommand) -> DifferentialWheelSpeeds:
        """
        Convert velocity command to wheel speeds.

        Args:
            cmd: Body-frame velocity command.
                 linear_x = forward velocity (m/s)
                 angular_z = yaw rate (rad/s), positive = CCW
                 linear_y is ignored (differential drive cannot strafe)

        Returns:
            Left and right wheel angular velocities (rad/s).
        """
        half_track = self._track_width / 2.0

        # Linear velocity at each wheel
        v_left = cmd.linear_x - cmd.angular_z * half_track
        v_right = cmd.linear_x + cmd.angular_z * half_track

        # Convert to angular velocity (rad/s)
        omega_left = v_left / self._wheel_radius
        omega_right = v_right / self._wheel_radius

        return DifferentialWheelSpeeds(
            left_wheel=omega_left,
            right_wheel=omega_right,
        )

    def to_joint_values(self, cmd: VelocityCommand) -> dict[str, float]:
        """
        Convert velocity command to a joint_name -> value dict.

        Returns:
            {"left_wheel": omega_left, "right_wheel": omega_right}
        """
        speeds = self.solve(cmd)
        return {
            "left_wheel": speeds.left_wheel,
            "right_wheel": speeds.right_wheel,
        }

    @staticmethod
    def inverse(
        left_wheel: float,
        right_wheel: float,
        track_width: float,
        wheel_radius: float,
    ) -> VelocityCommand:
        """
        Inverse kinematics: wheel speeds → body velocity.

        Args:
            left_wheel: Left wheel angular velocity (rad/s).
            right_wheel: Right wheel angular velocity (rad/s).
            track_width: Distance between wheels (m).
            wheel_radius: Wheel radius (m).

        Returns:
            Body-frame velocity command.
        """
        v_left = left_wheel * wheel_radius
        v_right = right_wheel * wheel_radius

        linear_x = (v_left + v_right) / 2.0
        angular_z = (v_right - v_left) / track_width

        return VelocityCommand(linear_x=linear_x, angular_z=angular_z)
