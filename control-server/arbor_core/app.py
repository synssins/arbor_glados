"""
FastAPI application factory.

Creates and configures the Arbor Control Server application.
All configuration is injected - no hardcoded values.
"""

from collections.abc import AsyncGenerator
from contextlib import asynccontextmanager
from typing import TYPE_CHECKING

import structlog
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from arbor_core.api.v1.router import router as api_v1_router
from arbor_core.auth.api_keys import APIKeyManager
from arbor_core.auth.headers import SecurityHeadersMiddleware
from arbor_core.auth.middleware import AuthMiddleware
from arbor_core.bridges.factory import create_transport
from arbor_core.bridges.node_bridge import NodeBridge
from arbor_core.bridges.proxy import NodeProxy
from arbor_core.events import EventBus

if TYPE_CHECKING:
    from arbor_core.config.models import ArborConfig

logger = structlog.get_logger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    """
    Application lifespan context manager.

    Handles startup and shutdown of:
    - EventBus (in-process pub/sub)
    - NodeBridge (transport connection manager)
    - NodeProxy (API forwarding layer)
    - Transport connections to pre-configured nodes
    - RobotRegistry + KinematicsEngine + actuator backends
    """
    logger.info("arbor_starting", version=app.version)

    # 1. Create shared infrastructure
    event_bus = EventBus()
    bridge = NodeBridge()
    proxy = NodeProxy(bridge=bridge, event_bus=event_bus)

    # 2. Store on app.state for route access
    app.state.event_bus = event_bus
    app.state.node_bridge = bridge
    app.state.node_proxy = proxy

    # Initialize PWM servo config store (in-memory for Phase 1)
    app.state.pwm_servo_configs = []

    # 3. Create transports for pre-configured nodes
    config = getattr(app.state, "config", None)
    if config is not None and hasattr(config, "nodes"):
        for node_cfg in config.nodes:
            transport = create_transport(node_cfg)
            if transport is not None:
                try:
                    bridge.register_node(node_cfg.id, transport)
                except Exception:
                    logger.exception(
                        "node_register_failed_at_startup", node_id=node_cfg.id
                    )

    # 4. Connect all registered transports (non-blocking, logs failures)
    if bridge.node_ids:
        results = await bridge.connect_all()
        connected = sum(1 for v in results.values() if v)
        logger.info(
            "nodes_connected_at_startup",
            total=len(results),
            connected=connected,
            failed=len(results) - connected,
        )

    # 5. Initialize robotics control system
    from arbor_core.robotics.backends.klipper_backend import KlipperBackend
    from arbor_core.robotics.backends.servo_backend import ArborServoBackend
    from arbor_core.robotics.engine import KinematicsEngine
    from arbor_core.robotics.registry import RobotRegistry

    robotics_cfg = getattr(config, "robotics", None)
    robotics_enabled = robotics_cfg is None or robotics_cfg.enabled

    if robotics_enabled:
        registry = RobotRegistry()
        app.state.robot_registry = registry

        # Create actuator backends
        backends: dict[str, ArborServoBackend | KlipperBackend] = {
            "arbor_servo": ArborServoBackend(proxy=proxy),
        }

        # Create Klipper backend if Moonraker is configured
        moonraker_cfg = getattr(robotics_cfg, "moonraker", None) if robotics_cfg else None
        if moonraker_cfg is None or moonraker_cfg.enabled:
            klipper = KlipperBackend(
                host=getattr(moonraker_cfg, "host", "localhost"),
                port=getattr(moonraker_cfg, "port", 7125),
                timeout=getattr(moonraker_cfg, "timeout", 5.0),
            )
            backends["klipper"] = klipper
            app.state.klipper_backend = klipper
            logger.info(
                "klipper_backend_created",
                host=getattr(moonraker_cfg, "host", "localhost"),
                port=getattr(moonraker_cfg, "port", 7125),
            )

        # Create kinematics engine
        engine = KinematicsEngine(
            registry=registry,
            backends=backends,
            event_bus=event_bus,
        )
        app.state.kinematics_engine = engine
        logger.info(
            "robotics_initialized",
            backends=list(backends.keys()),
        )

    yield

    # Shutdown: disconnect all node transports
    logger.info("arbor_shutting_down")
    await bridge.disconnect_all()


def create_app(config: "ArborConfig | None" = None) -> FastAPI:
    """
    Create and configure the FastAPI application.

    Args:
        config: Arbor configuration. If None, will be loaded from
                environment/config file (implemented in C02/C03).

    Returns:
        Configured FastAPI application instance.
    """
    # App metadata - these are static, not configuration
    app = FastAPI(
        title="Arbor Control Server",
        description="Modular robotics control platform API",
        version="0.1.0",
        docs_url="/api/docs",
        redoc_url="/api/redoc",
        openapi_url="/api/openapi.json",
        lifespan=lifespan,
    )

    # Store config in app state for access by routes.
    # If no config is provided, load from YAML / env vars / defaults.
    if config is None:
        from arbor_core.config.loader import ConfigError, load_config

        try:
            config = load_config()
        except ConfigError:
            logger.exception("config_load_failed_using_defaults")
            from arbor_core.config.models import ArborConfig

            config = ArborConfig()
    app.state.config = config

    # Create auth infrastructure from config
    security_cfg = getattr(config, "security", None)
    api_key_manager = APIKeyManager(
        min_key_length=security_cfg.api_key_min_length if security_cfg else 32,
    )
    app.state.api_key_manager = api_key_manager

    # Create JWT manager if RSA keys are configured
    jwt_manager = None
    if (
        security_cfg
        and security_cfg.jwt_private_key_path
        and security_cfg.jwt_public_key_path
    ):
        from arbor_core.auth.jwt import JWTManager

        try:
            jwt_manager = JWTManager.from_key_files(
                private_key_path=security_cfg.jwt_private_key_path,
                public_key_path=security_cfg.jwt_public_key_path,
                expiry_minutes=security_cfg.jwt_expiry_minutes,
            )
            logger.info("jwt_manager_created")
        except (FileNotFoundError, OSError):
            logger.warning(
                "jwt_keys_not_found",
                private=str(security_cfg.jwt_private_key_path),
                public=str(security_cfg.jwt_public_key_path),
            )
    app.state.jwt_manager = jwt_manager

    # ── Middleware stack ──────────────────────────────────────────────
    # Starlette executes middleware outermost-first. The LAST middleware
    # added wraps outermost. We want:
    #   Request → CORS → SecurityHeaders → Auth → route handler
    # So we add in innermost-first order:

    # 1. Auth middleware (innermost — runs last on request, first on response)
    app.add_middleware(
        AuthMiddleware,
        api_key_manager=api_key_manager,
        jwt_manager=jwt_manager,
        default_rate_limit_rpm=(
            security_cfg.rate_limit_default_rpm if security_cfg else 300
        ),
    )

    # 2. Security headers middleware (adds headers to all responses)
    app.add_middleware(SecurityHeadersMiddleware)

    # 3. CORS middleware (outermost — handles OPTIONS preflight before auth)
    cors_origins = config.server.cors.allowed_origins
    if not cors_origins:
        # No origins configured — allow all for first-boot / development.
        # Mirrors provisioning mode: works out of the box, warns to lock down.
        cors_origins = ["*"]
        logger.warning(
            "cors_wildcard_origins",
            hint="CORS allows all origins. Set server.cors.allowed_origins in config to restrict.",
        )
    else:
        logger.info("cors_configured", origins=cors_origins)

    # NOTE: allow_methods and allow_headers are structural constants tied to
    # the API design, not deployment tunables. They track which HTTP methods
    # and headers the API actually consumes. Promote to CORSConfig only if a
    # future phase needs configurable methods/headers (e.g. ROS2 bridge).
    app.add_middleware(
        CORSMiddleware,
        allow_origins=cors_origins,
        allow_credentials=True,
        allow_methods=["GET", "POST", "PUT", "DELETE", "PATCH"],
        allow_headers=["Authorization", "Content-Type"],
    )

    # Mount API routers
    app.include_router(api_v1_router, prefix="/api/v1")

    # Root-level health endpoint (nginx proxies /health directly)
    @app.get("/health")
    async def health():
        """Root-level health check for load balancers and monitoring."""
        return {"status": "ok", "version": app.version}

    logger.info("arbor_app_created")

    return app
