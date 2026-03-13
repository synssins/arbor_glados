"""
Arbor hardware bridge package.

Handles communication with hardware nodes:
- ESP32 nodes via UART/USB/WiFi
- Klipper nodes via Moonraker API
- Future: ROS2 bridge

Implementation tasks:
- C08: UART/USB bridge to ESP32 nodes  [DONE]
- C09: Proxy layer - control server API to node API  [DONE]
"""

from arbor_core.bridges.node_bridge import NodeBridge
from arbor_core.bridges.proxy import NodeProxy, ProxyError
from arbor_core.bridges.serial_transport import SerialTransport
from arbor_core.bridges.transport import NodeTransport, TransportError

__all__ = [
    "NodeBridge",
    "NodeProxy",
    "NodeTransport",
    "ProxyError",
    "SerialTransport",
    "TransportError",
]
