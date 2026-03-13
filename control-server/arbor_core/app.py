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

if TYPE_CHECKING:
    from arbor_core.config.models import ArborConfig

logger = structlog.get_logger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    """
    Application lifespan context manager.

    Handles startup and shutdown of:
    - Plugin manager
    - Node connections
    - Background tasks
    """
    logger.info("arbor_starting", version=app.version)

    # Startup tasks will be added here as we implement:
    # - Plugin manager initialization (C04)
    # - Node bridge connections (C08)
    # - Database connections (C16)

    yield

    # Shutdown tasks
    logger.info("arbor_shutting_down")
    # Plugin shutdown, connection cleanup, etc.


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

    # Store config in app state for access by routes
    # Config loading will be implemented in C02/C03
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

    logger.info("arbor_app_created")

    return app
