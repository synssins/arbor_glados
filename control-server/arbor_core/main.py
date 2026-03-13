"""
Arbor Control Server entry point.

This module provides both:
- A module-level `app` for uvicorn CLI: `uvicorn arbor_core.main:app`
- A `main()` CLI entry point for direct execution.

All runtime configuration comes from config files or environment variables.
"""

import logging
import sys
from typing import NoReturn

import structlog
import uvicorn

from arbor_core.app import create_app

# Configure structlog for JSON output as specified in ARBOR_PROJECT_PLAN.md
structlog.configure(
    processors=[
        structlog.contextvars.merge_contextvars,
        structlog.processors.add_log_level,
        structlog.processors.TimeStamper(fmt="iso"),
        structlog.dev.ConsoleRenderer()
        if sys.stderr.isatty()
        else structlog.processors.JSONRenderer(),
    ],
    wrapper_class=structlog.make_filtering_bound_logger(logging.INFO),
    context_class=dict,
    logger_factory=structlog.PrintLoggerFactory(),
    cache_logger_on_first_use=True,
)

logger = structlog.get_logger(__name__)

# Module-level app instance for uvicorn CLI usage:
#   uvicorn arbor_core.main:app --host 127.0.0.1 --port 8000
app = create_app()


def main() -> NoReturn:
    """
    Main entry point for the Arbor Control Server.

    Loads configuration and starts the uvicorn server.
    Configuration loading will be fully implemented in C02/C03.
    """
    logger.info("arbor_main_starting")

    # Server settings will come from config (C02/C03)
    # TODO(C02/C03): Load from config.server.host, config.server.port, config.server.tls
    uvicorn.run(
        app,
        host="127.0.0.1",  # Default to localhost for safety
        port=8000,  # Non-privileged port for development
        log_level="info",
    )
    sys.exit(0)


if __name__ == "__main__":
    main()
