"""
Transport factory — creates the appropriate NodeTransport for a NodeConfig.

Shared by app.py (lifespan startup) and node_routes.py (add-node auto-connect).

Task: Node Integration Step 2
"""

from __future__ import annotations

import structlog

from arbor_core.bridges.transport import NodeTransport
from arbor_core.config.models import NodeConfig

logger = structlog.get_logger(__name__)


def create_transport(node_cfg: NodeConfig) -> NodeTransport | None:
    """
    Create the appropriate transport for a node based on its config.

    Args:
        node_cfg: The node configuration.

    Returns:
        A NodeTransport instance, or None if the transport type is unsupported.
    """
    tc = node_cfg.transport_config

    if node_cfg.transport in ("wifi", "ethernet"):
        if not tc.host:
            logger.warning(
                "http_transport_no_host",
                node_id=node_cfg.id,
                transport=node_cfg.transport,
            )
            return None

        from arbor_core.bridges.http_transport import HTTPTransport

        return HTTPTransport(
            node_id=node_cfg.id,
            host=tc.host,
            port=tc.network_port or 80,
        )

    if node_cfg.transport in ("uart", "usb"):
        if not tc.port:
            logger.warning(
                "serial_transport_no_port",
                node_id=node_cfg.id,
                transport=node_cfg.transport,
            )
            return None

        from arbor_core.bridges.serial_transport import SerialTransport

        return SerialTransport(
            node_id=node_cfg.id,
            port=tc.port,
            baud=tc.baud,
        )

    logger.warning(
        "unsupported_transport_type",
        node_id=node_cfg.id,
        transport=node_cfg.transport,
    )
    return None
