"""
Ackermann steering kinematics solver.

Converts body-frame velocity commands into steering angle and drive
velocity for car-style robots with front, rear, or both-axle steering.

Uses the bicycle model approximation:
    turning_radius = linear_x / angular_z
    steering_angle = atan(wheelbase / turning_radius)

Supports:
    - Front-axle steering (standard car)
    - Rear-axle steering (forklift style)
    - Both-axle steering (crab/counter steering)

No external dependencies — pure stdlib math.

Task: Robotics Phase 2
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Literal

from arbor_core.robotics.models import VehicleGeometry, VelocityCommand


@dataclass(frozen=True)
class AckermannOutput:
    """Computed outputs for an Ackermann-steered vehicle."""

    steering_angle: float    # radians (front axle angle, or avg of both)
    rear_steering_angle: float  # radians (rear axle, 0 if front-only)
    drive_velocity: float    # rad/s (drive wheel angular velocity)
    inner_angle: float       # radians (inner wheel, tighter turn)
    outer_angle: float       # radians (outer wheel, wider turn)


class AckermannSolver:
    """
    Ackermann steering kinematics.

    Computes proper inner/outer wheel steering angles using
    Ackermann geometry to eliminate tire scrub.
    """

    def __init__(self, geometry: VehicleGeometry) -> None:
        """
        Args:
            geometry: Vehicle geometry with wheelbase, track_width,
                      wheel_radius, max_steering_angle, steering_axles.
        """
        self._wheelbase = geometry.wheelbase
        self._track_width = geometry.track_width
        self._wheel_radius = geometry.wheel_radius
        self._max_steering = geometry.max_steering_angle or math.pi / 4
        self._steering_axles: Literal["front", "rear", "both"] = (
            geometry.steering_axles
        )

    def solve(self, cmd: VelocityCommand) -> AckermannOutput:
        """
        Convert velocity command to steering and drive outputs.

        Args:
            cmd: Body-frame velocity command.
                 linear_x = forward velocity (m/s)
                 angular_z = yaw rate (rad/s)
                 linear_y is ignored

        Returns:
            Steering angles and drive velocity.
        """
        drive_velocity = cmd.linear_x / self._wheel_radius

        # Pure straight line
        if abs(cmd.angular_z) < 1e-6:
            return AckermannOutput(
                steering_angle=0.0,
                rear_steering_angle=0.0,
                drive_velocity=drive_velocity,
                inner_angle=0.0,
                outer_angle=0.0,
            )

        # Turning radius from velocity command
        if abs(cmd.linear_x) < 1e-6:
            # Pivot turn — use minimum turning radius
            turning_radius = self._wheelbase / math.tan(self._max_steering)
            # Adjust sign based on angular_z
            if cmd.angular_z < 0:
                turning_radius = -turning_radius
        else:
            turning_radius = cmd.linear_x / cmd.angular_z

        # Bicycle model: steering angle
        if abs(turning_radius) < 1e-6:
            steering_angle = math.copysign(self._max_steering, cmd.angular_z)
        else:
            effective_wheelbase = self._wheelbase
            if self._steering_axles == "both":
                # Both axles steer — each takes half the angle
                effective_wheelbase = self._wheelbase / 2.0

            raw_angle = math.atan(effective_wheelbase / turning_radius)
            steering_angle = max(
                -self._max_steering, min(self._max_steering, raw_angle)
            )

        # Ackermann inner/outer angles
        half_track = self._track_width / 2.0
        if abs(turning_radius) > 1e-6:
            inner_angle = math.atan(
                self._wheelbase / (abs(turning_radius) - half_track)
            )
            outer_angle = math.atan(
                self._wheelbase / (abs(turning_radius) + half_track)
            )
            # Sign matches steering direction
            if turning_radius < 0:
                inner_angle, outer_angle = -outer_angle, -inner_angle
        else:
            inner_angle = steering_angle
            outer_angle = steering_angle

        # Rear steering for both-axle mode
        rear_steering = 0.0
        if self._steering_axles == "both":
            rear_steering = -steering_angle  # Counter-steer
        elif self._steering_axles == "rear":
            rear_steering = steering_angle
            steering_angle = 0.0  # Front doesn't steer

        return AckermannOutput(
            steering_angle=steering_angle,
            rear_steering_angle=rear_steering,
            drive_velocity=drive_velocity,
            inner_angle=inner_angle,
            outer_angle=outer_angle,
        )

    def to_joint_values(self, cmd: VelocityCommand) -> dict[str, float]:
        """
        Convert velocity command to a joint_name -> value dict.

        Joint names depend on steering_axles configuration:
        - front: steering, drive
        - rear: rear_steering, drive
        - both: steering, rear_steering, drive

        Returns:
            Joint name to value mapping.
        """
        output = self.solve(cmd)
        result: dict[str, float] = {"drive": output.drive_velocity}

        if self._steering_axles in ("front", "both"):
            result["steering"] = output.steering_angle
        if self._steering_axles in ("rear", "both"):
            result["rear_steering"] = output.rear_steering_angle

        return result
