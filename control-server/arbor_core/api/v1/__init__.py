"""
Arbor API v1.

This module contains all v1 API routes as defined in
ARBOR_PROJECT_PLAN.md Section 6.

Route modules:
- system: System info, health, config endpoints (C10)
- servo: Servo control endpoints (C11)
- sensor: Sensor reading endpoints (C12)
- auth: Authentication endpoints (C15)
"""

from arbor_core.api.v1.router import router

__all__ = ["router"]
