"""
Mecanum wheel kinematics solver.

Converts body-frame velocity commands into individual wheel velocities
for 4-wheel mecanum/omnidirectional drive robots.

Mecanum wheels have rollers at 45° that enable omnidirectional movement.
Standard wheel arrangement (rollers forming X pattern when viewed from top):

    FL (\\)  FR (/)
    RL (/)  RR (\\)

Math (standard mecanum kinematics):
    L = (wheelbase + track_width) / 2
    fl = (vx - vy - L * wz) / r
    fr = (vx + vy + L * wz) / r
    rl = (vx + vy - L * wz) / r
    rr = (vx - vy + L * wz) / r

No external dependencies — pure stdlib math.

Task: Robotics Phase 2
"""

from __future__ import annotations

from dataclasses import dataclass

from arbor_core.robotics.models import VehicleGeometry, VelocityCommand


@dataclass(frozen=True)
class MecanumWheelSpeeds:
    """Computed wheel speeds for a mecanum drive robot."""

    front_left: float   # rad/s
    front_right: float  # rad/s
    rear_left: float    # rad/s
    rear_right: float   # rad/s


class MecanumSolver:
    """
    Mecanum wheel kinematics.

    Computes individual wheel velocities for omnidirectional movement
    including forward/backward, strafing, and rotation simultaneously.
    """

    def __init__(self, geometry: VehicleGeometry) -> None:
        """
        Args:
            geometry: Vehicle geometry with wheelbase, track_width, wheel_radius.
        """
        self._wheelbase = geometry.wheelbase
        self._track_width = geometry.track_width
        self._wheel_radius = geometry.wheel_radius
        # Combined half-distance for rotation contribution
        self._L = (self._wheelbase + self._track_width) / 2.0

    def solve(self, cmd: VelocityCommand) -> MecanumWheelSpeeds:
        """
        Convert velocity command to wheel speeds.

        Args:
            cmd: Body-frame velocity command.
                 linear_x = forward velocity (m/s)
                 linear_y = lateral velocity (m/s), positive = left
                 angular_z = yaw rate (rad/s), positive = CCW

        Returns:
            Individual wheel angular velocities (rad/s).
        """
        vx = cmd.linear_x
        vy = cmd.linear_y
        wz = cmd.angular_z
        r = self._wheel_radius
        L = self._L

        # Standard mecanum inverse kinematics
        fl = (vx - vy - L * wz) / r
        fr = (vx + vy + L * wz) / r
        rl = (vx + vy - L * wz) / r
        rr = (vx - vy + L * wz) / r

        return MecanumWheelSpeeds(
            front_left=fl,
            front_right=fr,
            rear_left=rl,
            rear_right=rr,
        )

    def to_joint_values(self, cmd: VelocityCommand) -> dict[str, float]:
        """
        Convert velocity command to a joint_name -> value dict.

        Returns:
            {"front_left": fl, "front_right": fr,
             "rear_left": rl, "rear_right": rr}
        """
        speeds = self.solve(cmd)
        return {
            "front_left": speeds.front_left,
            "front_right": speeds.front_right,
            "rear_left": speeds.rear_left,
            "rear_right": speeds.rear_right,
        }

    @staticmethod
    def inverse(
        fl: float,
        fr: float,
        rl: float,
        rr: float,
        wheelbase: float,
        track_width: float,
        wheel_radius: float,
    ) -> VelocityCommand:
        """
        Forward kinematics: wheel speeds → body velocity.

        Args:
            fl, fr, rl, rr: Wheel angular velocities (rad/s).
            wheelbase: Distance between front and rear axles (m).
            track_width: Distance between left and right wheels (m).
            wheel_radius: Wheel radius (m).

        Returns:
            Body-frame velocity command.
        """
        r = wheel_radius
        L = (wheelbase + track_width) / 2.0

        linear_x = r * (fl + fr + rl + rr) / 4.0
        linear_y = r * (-fl + fr + rl - rr) / 4.0
        angular_z = r * (-fl + fr - rl + rr) / (4.0 * L)

        return VelocityCommand(
            linear_x=linear_x,
            linear_y=linear_y,
            angular_z=angular_z,
        )
