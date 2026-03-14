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
    # If no config is provided, create a default so that in-memory
    # operations (node add/remove) work immediately.
    if config is None:
        from arbor_core.config.models import ArborConfig

        config = ArborConfig()
    app.state.config = config

    # Configure CORS - origins will come from config (C02/C03)
    # For now, using restrictive defaults
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],  # TODO(C02/C03): Populate from config.server.cors.allowed_origins
        allow_credentials=True,
        allow_methods=["GET", "POST", "PUT", "DELETE", "PATCH"],
        allow_headers=["Authorization", "Content-Type"],
    )

    # Security headers middleware will be added in C07
    # Rate limiting middleware will be added in C07
    # Auth middleware will be added in C07

    # Mount API routers
    app.include_router(api_v1_router, prefix="/api/v1")

    # Root-level health endpoint (nginx proxies /health directly)
    @app.get("/health")
    async def health():
        """Root-level health check for load balancers and monitoring."""
        return {"status": "ok", "version": app.version}

    logger.info("arbor_app_created")

    return app
