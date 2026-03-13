"""
Arbor plugin package.

Plugin-first architecture - all hardware drivers are plugins.

Plugin interface contract (from ARBOR_PROJECT_PLAN.md Section 5):
see base.py for ArborPlugin ABC.

Implementation tasks:
- C04: Plugin manager - discovery, load, init, shutdown  [DONE]
"""

from arbor_core.plugins.base import ArborPlugin, HealthState, HealthStatus
from arbor_core.plugins.manager import PluginError, PluginManager

__all__ = [
    "ArborPlugin",
    "HealthState",
    "HealthStatus",
    "PluginError",
    "PluginManager",
]
