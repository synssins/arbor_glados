"""
System API endpoints — info, health, config, restart.

Parity with ESP32 firmware F11 endpoints.

Per ARBOR_PROJECT_PLAN.md Section 6:
    GET  /api/v1/system/info       # platform, firmware version, capabilities
    GET  /api/v1/system/health     # health status of all modules
    GET  /api/v1/system/config     # current config (no secrets)
    PUT  /api/v1/system/config     # update config (admin scope)
    POST /api/v1/system/restart    # restart node (admin scope)

Task: C10
"""

from __future__ import annotations

import platform
import sys
from typing import Any

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from arbor_core import __version__
from arbor_core.plugins.base import HealthState

logger = structlog.get_logger(__name__)

system_router = APIRouter(prefix="/system", tags=["system"])


@system_router.get(
    "/info",
    summary="System information",
    description="Returns platform info, version, loaded modules, and capabilities.",
)
async def system_info(request: Request) -> JSONResponse:
    """
    System information endpoint.

    Returns platform details, Arbor version, Python version,
    loaded plugin list, and configured node count.
    """
    config = getattr(request.app.state, "config", None)
    plugin_mgr = getattr(request.app.state, "plugin_manager", None)

    loaded_plugins: list[dict[str, Any]] = []
    if plugin_mgr is not None:
        for name, instance in plugin_mgr.instances.items():
            loaded_plugins.append({
                "name": name,
                "version": instance.version,
                "capabilities": instance.capabilities,
            })

    node_count = len(config.nodes) if config else 0

    # Check if system is in provisioning mode (no API keys exist yet)
    api_key_mgr = getattr(request.app.state, "api_key_manager", None)
    provisioning_mode = api_key_mgr is not None and api_key_mgr.key_count == 0

    return JSONResponse(
        content={
            "platform": platform.system(),
            "architecture": platform.machine(),
            "python_version": sys.version,
            "arbor_version": __version__,
            "node_count": node_count,
            "loaded_plugins": loaded_plugins,
            "provisioning_mode": provisioning_mode,
        }
    )


@system_router.get(
    "/health",
    summary="System health",
    description="Health status of all loaded modules/plugins.",
)
async def system_health(request: Request) -> JSONResponse:
    """
    System health endpoint.

    Runs health checks on all loaded plugins and returns aggregated status.
    Overall status is 'healthy' only if all plugins are healthy.
    """
    plugin_mgr = getattr(request.app.state, "plugin_manager", None)

    if plugin_mgr is None:
        return JSONResponse(
            content={
                "status": "healthy",
                "modules": {},
                "detail": "No plugin manager configured",
            }
        )

    results = await plugin_mgr.health_check_all()

    module_statuses: dict[str, dict[str, Any]] = {}
    overall = HealthState.HEALTHY
    for name, health in results.items():
        module_statuses[name] = {
            "state": health.state.value,
            "message": health.message,
            "details": health.details,
        }
        if health.state == HealthState.UNHEALTHY:
            overall = HealthState.UNHEALTHY
        elif health.state == HealthState.DEGRADED and overall == HealthState.HEALTHY:
            overall = HealthState.DEGRADED

    return JSONResponse(
        content={
            "status": overall.value,
            "modules": module_statuses,
        }
    )


@system_router.get(
    "/config",
    summary="Current configuration (no secrets)",
    description="Returns the current running configuration with secrets redacted.",
)
async def system_config(request: Request) -> JSONResponse:
    """
    Return current config with secrets redacted.

    Strips TLS key paths, JWT key paths, and any field containing
    'secret', 'password', or 'key_path' from the output.
    """
    config = getattr(request.app.state, "config", None)
    if config is None:
        return JSONResponse(
            status_code=200,
            content={"detail": "No configuration loaded"},
        )

    # Serialize and redact secrets
    data = config.model_dump(mode="json")
    _redact_secrets(data)

    return JSONResponse(content=data)


@system_router.put(
    "/config",
    summary="Update configuration (admin only)",
    description="Update running configuration. Requires admin scope.",
)
async def update_config(request: Request) -> JSONResponse:
    """
    Update configuration endpoint.

    For Phase 1, this validates the incoming config but does not
    hot-reload all components. Full hot-reload is Phase 2.
    """
    auth = getattr(request.state, "auth", None)
    if not auth or not auth.authenticated or "admin" not in auth.scopes:
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    try:
        body = await request.json()
    except Exception:
        return JSONResponse(
            status_code=422,
            content={"detail": "Invalid JSON body"},
        )

    # Validate the incoming config
    from arbor_core.config.models import ArborConfig
    from pydantic import ValidationError

    try:
        new_config = ArborConfig(**body)
    except ValidationError as exc:
        return JSONResponse(
            status_code=422,
            content={"detail": "Validation failed", "errors": exc.errors()},
        )

    # Store the new config
    request.app.state.config = new_config
    logger.info("config_updated_via_api")

    return JSONResponse(
        status_code=200,
        content={"detail": "Configuration updated"},
    )


@system_router.post(
    "/restart",
    summary="Restart system (admin only)",
    description="Trigger a graceful restart. Requires admin scope.",
)
async def restart_system(request: Request) -> JSONResponse:
    """
    Restart endpoint.

    For Phase 1, this signals intent but does not actually restart
    the process (systemd handles restart on exit).
    """
    auth = getattr(request.state, "auth", None)
    if not auth or not auth.authenticated or "admin" not in auth.scopes:
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    logger.info("restart_requested", subject=auth.subject)

    # Phase 1: Signal restart intent. Actual restart via systemd.
    # A real implementation would set a flag checked by the main loop.
    request.app.state.restart_requested = True

    return JSONResponse(
        status_code=200,
        content={"detail": "Restart initiated"},
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
_SECRET_KEYS = frozenset({"key_path", "cert_path", "password", "secret", "key_hash"})


def _redact_secrets(data: Any, parent_key: str = "") -> None:
    """Recursively redact secret fields from a config dict."""
    if isinstance(data, dict):
        for key in list(data.keys()):
            if any(s in key.lower() for s in _SECRET_KEYS):
                data[key] = "***REDACTED***"
            else:
                _redact_secrets(data[key], key)
    elif isinstance(data, list):
        for item in data:
            _redact_secrets(item, parent_key)
