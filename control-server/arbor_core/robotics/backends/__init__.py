"""
Actuator backends — dispatch joint commands to physical hardware.

Each backend translates abstract joint-space values into hardware-specific
commands for a particular actuator type.

Task: Robotics Phase 1
"""

from arbor_core.robotics.backends.base import ActuatorBackend

__all__ = ["ActuatorBackend"]
