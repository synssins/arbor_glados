"""
Arbor Control Server.

A modular robotics control platform providing:
- REST API for servo, sensor, and hardware control
- WebSocket for real-time state updates
- Plugin system for extensible hardware support
- Multi-node orchestration

All configuration is loaded from YAML files or environment variables.
Zero hardcoded values.
"""

__version__ = "0.1.0"
__author__ = "Chris Kliewer"

from arbor_core.app import create_app

__all__ = ["__version__", "create_app"]
