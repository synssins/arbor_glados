"""
Pydantic configuration models for the Arbor Control Server.

Defines the complete configuration schema as specified in
ARBOR_PROJECT_PLAN.md Section 9.

Zero hardcoded values - every tunable is a config field with
validation. Default values exist only where the project plan
explicitly specifies them (e.g. RS256 for JWT).

Task: C02
"""

from __future__ import annotations

from pathlib import Path
from typing import Annotated, Literal

from pydantic import BaseModel, Field, field_validator


# ---------------------------------------------------------------------------
# TLS
# ---------------------------------------------------------------------------
class TLSConfig(BaseModel):
    """TLS settings for the HTTPS server."""

    enabled: bool = Field(
        default=True,
        description="Enable TLS. Must be True in production.",
    )
    cert_path: Path = Field(
        description="Path to the TLS certificate PEM file.",
    )
    key_path: Path = Field(
        description="Path to the TLS private key PEM file.",
    )


# ---------------------------------------------------------------------------
# CORS
# ---------------------------------------------------------------------------
class CORSConfig(BaseModel):
    """Cross-Origin Resource Sharing settings."""

    allowed_origins: list[str] = Field(
        default_factory=list,
        description="Allowed CORS origins. Empty list blocks all cross-origin requests.",
    )


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------
class ServerConfig(BaseModel):
    """HTTP(S) server settings."""

    host: str = Field(
        default="0.0.0.0",
        description="Bind address for the server.",
    )
    port: int = Field(
        default=8443,
        ge=1,
        le=65535,
        description="Listen port.",
    )
    tls: TLSConfig | None = Field(
        default=None,
        description="TLS configuration. Required for production.",
    )
    cors: CORSConfig = Field(
        default_factory=CORSConfig,
        description="CORS configuration.",
    )


# ---------------------------------------------------------------------------
# Security
# ---------------------------------------------------------------------------
class SecurityConfig(BaseModel):
    """Authentication and authorization settings."""

    api_key_min_length: int = Field(
        default=32,
        ge=16,
        description="Minimum length for generated API keys.",
    )
    jwt_algorithm: Literal["RS256"] = Field(
        default="RS256",
        description="JWT signing algorithm. Locked to RS256 per project plan.",
    )
    jwt_expiry_minutes: int = Field(
        default=60,
        ge=1,
        description="JWT token lifetime in minutes.",
    )
    jwt_private_key_path: Path | None = Field(
        default=None,
        description="Path to RSA private key PEM for JWT signing.",
    )
    jwt_public_key_path: Path | None = Field(
        default=None,
        description="Path to RSA public key PEM for JWT verification.",
    )
    rate_limit_default_rpm: int = Field(
        default=300,
        ge=1,
        description="Default requests per minute per API key.",
    )


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
class LoggingConfig(BaseModel):
    """Structured logging settings."""

    level: Literal["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"] = Field(
        default="INFO",
        description="Minimum log level.",
    )
    format: Literal["json", "console"] = Field(
        default="json",
        description="Log output format. 'json' for production, 'console' for development.",
    )
    output: Path | None = Field(
        default=None,
        description="Log file path. None means stdout/stderr.",
    )


# ---------------------------------------------------------------------------
# Servo (bus-protocol serial servos)
# ---------------------------------------------------------------------------
class ServoConfig(BaseModel):
    """Configuration for a single serial-bus servo."""

    id: int = Field(ge=0, le=253, description="Servo bus ID.")
    name: str = Field(
        min_length=1,
        max_length=64,
        description="Human-readable name (e.g. 'shoulder-yaw').",
    )
    min_position: int = Field(
        default=0,
        ge=0,
        description="Minimum allowed position value.",
    )
    max_position: int = Field(
        default=4095,
        ge=0,
        description="Maximum allowed position value.",
    )
    max_speed: int = Field(
        default=2000,
        ge=0,
        description="Maximum speed value.",
    )

    @field_validator("max_position")
    @classmethod
    def max_gte_min(cls, v: int, info: object) -> int:
        """Ensure max_position >= min_position."""
        # info.data contains previously validated fields
        data = getattr(info, "data", {})
        min_pos = data.get("min_position", 0)
        if v < min_pos:
            msg = f"max_position ({v}) must be >= min_position ({min_pos})"
            raise ValueError(msg)
        return v


class ServoBusModuleConfig(BaseModel):
    """Configuration for the servo-bus plugin on a node."""

    protocol: str = Field(
        description="Servo bus protocol (e.g. 'feetech-scs', 'feetech-sts', 'dynamixel').",
    )
    baud: int = Field(
        default=1_000_000,
        ge=9600,
        description="Bus baud rate.",
    )
    tx_pin: int = Field(
        default=-1, ge=-1, le=39,
        description="UART TX GPIO pin (-1 = not assigned). Configurable via WebUI.",
    )
    rx_pin: int = Field(
        default=-1, ge=-1, le=39,
        description="UART RX GPIO pin (-1 = not assigned). Configurable via WebUI.",
    )
    dir_pin: int = Field(
        default=-1, ge=-1, le=39,
        description="Half-duplex direction GPIO pin (-1 = not used). Configurable via WebUI.",
    )
    servos: list[ServoConfig] = Field(
        default_factory=list,
        description="Servo definitions on this bus.",
    )


# ---------------------------------------------------------------------------
# Pin configuration (all pins configurable via WebUI)
# ---------------------------------------------------------------------------
class PinConfig(BaseModel):
    """
    GPIO pin assignments for a node.

    ALL pins are configurable — no hardcoded GPIO numbers in firmware.
    Pin assignments are pushed to nodes via NVS config and editable in WebUI.
    A value of -1 means the pin is not assigned.
    """

    i2c_sda: int = Field(default=-1, ge=-1, le=39, description="I2C SDA pin.")
    i2c_scl: int = Field(default=-1, ge=-1, le=39, description="I2C SCL pin.")
    spi_mosi: int = Field(default=-1, ge=-1, le=39, description="SPI MOSI pin.")
    spi_miso: int = Field(default=-1, ge=-1, le=39, description="SPI MISO pin.")
    spi_sclk: int = Field(default=-1, ge=-1, le=39, description="SPI SCLK pin.")
    oled_sda: int = Field(default=-1, ge=-1, le=39, description="OLED display SDA pin.")
    oled_scl: int = Field(default=-1, ge=-1, le=39, description="OLED display SCL pin.")
    ws2812_data: int = Field(default=-1, ge=-1, le=39, description="WS2812b LED data pin.")
    onewire: int = Field(default=-1, ge=-1, le=39, description="1-Wire bus pin (DS18B20).")
    endstop_pins: list[int] = Field(
        default_factory=list,
        description="Endstop GPIO pins (-1 = not assigned).",
    )
    button_pins: list[int] = Field(
        default_factory=list,
        description="Button GPIO pins (-1 = not assigned).",
    )
    pwm_pins: list[int] = Field(
        default_factory=list,
        description="PWM output GPIO pins (-1 = not assigned).",
    )


# ---------------------------------------------------------------------------
# Node module (generic wrapper)
# ---------------------------------------------------------------------------
class NodeModuleConfig(BaseModel):
    """A module instance attached to a node."""

    type: str = Field(
        description="Module type slug (e.g. 'servo-bus', 'sensor-temp').",
    )
    config: dict[str, object] = Field(
        default_factory=dict,
        description="Module-specific configuration. Validated by the plugin at load time.",
    )


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class TransportConfig(BaseModel):
    """Transport-specific settings for a node connection."""

    port: str | None = Field(
        default=None,
        description="Serial port path (e.g. '/dev/ttyUSB0').",
    )
    baud: int = Field(
        default=921600,
        ge=9600,
        description="Serial baud rate.",
    )
    host: str | None = Field(
        default=None,
        description="Network host for WiFi/Ethernet nodes.",
    )
    network_port: int | None = Field(
        default=None,
        ge=1,
        le=65535,
        description="Network port for WiFi/Ethernet nodes.",
    )


# ---------------------------------------------------------------------------
# Node TLS (client cert for node auth)
# ---------------------------------------------------------------------------
class NodeTLSConfig(BaseModel):
    """TLS client certificate for authenticating a node."""

    client_cert: Path | None = Field(
        default=None,
        description="Path to the node's client certificate PEM.",
    )


# ---------------------------------------------------------------------------
# Node
# ---------------------------------------------------------------------------
class NodeConfig(BaseModel):
    """Configuration for a single hardware node."""

    id: str = Field(
        min_length=1,
        max_length=64,
        description="Unique node identifier slug.",
    )
    type: str = Field(
        description="Node platform type (e.g. 'esp32', 'esp32-s3', 'klipper').",
    )
    transport: Literal["uart", "usb", "wifi", "ethernet"] = Field(
        description="Transport method to reach this node.",
    )
    transport_config: TransportConfig = Field(
        default_factory=TransportConfig,
        description="Transport-specific configuration.",
    )
    tls: NodeTLSConfig = Field(
        default_factory=NodeTLSConfig,
        description="Node TLS authentication.",
    )
    pins: PinConfig = Field(
        default_factory=PinConfig,
        description="GPIO pin assignments. All pins configurable via WebUI.",
    )
    modules: list[NodeModuleConfig] = Field(
        default_factory=list,
        description="Modules loaded on this node.",
    )


# ---------------------------------------------------------------------------
# Plugin search paths
# ---------------------------------------------------------------------------
class PluginConfig(BaseModel):
    """Plugin discovery configuration."""

    search_paths: list[Path] = Field(
        default_factory=list,
        description="Additional directories to search for plugins.",
    )


# ---------------------------------------------------------------------------
# AI constraints
# ---------------------------------------------------------------------------
class AIConstraintsConfig(BaseModel):
    """Safety constraints applied to AI-scoped API keys."""

    max_servo_speed_pct: int = Field(
        default=60,
        ge=0,
        le=100,
        description="AI key limited to this percentage of configured max speed.",
    )
    require_confirmation_for: list[str] = Field(
        default_factory=list,
        description="Commands that require human confirmation when issued by AI key.",
    )
    blacklisted_endpoints: list[str] = Field(
        default_factory=list,
        description="Endpoint patterns the AI key cannot access.",
    )
    command_rate_limit_rpm: int = Field(
        default=120,
        ge=1,
        description="Separate rate limit for AI-scoped keys.",
    )


# ---------------------------------------------------------------------------
# Database
# ---------------------------------------------------------------------------
class DatabaseConfig(BaseModel):
    """Database backend configuration."""

    backend: Literal["sqlite"] = Field(
        default="sqlite",
        description="Database backend. SQLite for Phase 1.",
    )
    path: Path = Field(
        default=Path("arbor.db"),
        description="Path to the SQLite database file.",
    )


# ---------------------------------------------------------------------------
# Moonraker / Klipper
# ---------------------------------------------------------------------------
class MoonrakerConfig(BaseModel):
    """Moonraker REST API connection settings."""

    host: str = Field(
        default="localhost",
        description="Moonraker host address.",
    )
    port: int = Field(
        default=7125,
        ge=1,
        le=65535,
        description="Moonraker port.",
    )
    timeout: float = Field(
        default=5.0,
        gt=0,
        description="HTTP request timeout in seconds.",
    )
    enabled: bool = Field(
        default=True,
        description="Enable Klipper/Moonraker integration.",
    )


# ---------------------------------------------------------------------------
# Robotics
# ---------------------------------------------------------------------------
class RoboticsConfig(BaseModel):
    """Robotics control system settings."""

    enabled: bool = Field(
        default=True,
        description="Enable the robotics control system.",
    )
    moonraker: MoonrakerConfig = Field(
        default_factory=MoonrakerConfig,
        description="Moonraker connection settings for Klipper backend.",
    )


# ---------------------------------------------------------------------------
# Root config
# ---------------------------------------------------------------------------
class ArborConfig(BaseModel):
    """
    Root configuration model for the Arbor Control Server.

    This is the single source of truth for all runtime configuration.
    Every configurable value in the system must be reachable from
    this model.  Zero hardcoded values.

    Schema defined in ARBOR_PROJECT_PLAN.md Section 9.
    """

    version: str = Field(
        default="1.0",
        description="Configuration schema version.",
    )
    server: ServerConfig = Field(
        default_factory=ServerConfig,
        description="HTTP(S) server settings.",
    )
    security: SecurityConfig = Field(
        default_factory=SecurityConfig,
        description="Authentication and authorization settings.",
    )
    logging: LoggingConfig = Field(
        default_factory=LoggingConfig,
        description="Logging settings.",
    )
    nodes: list[NodeConfig] = Field(
        default_factory=list,
        description="Hardware node definitions.",
    )
    plugins: PluginConfig = Field(
        default_factory=PluginConfig,
        description="Plugin discovery configuration.",
    )
    ai_constraints: AIConstraintsConfig = Field(
        default_factory=AIConstraintsConfig,
        description="Safety constraints for AI-scoped API keys.",
    )
    database: DatabaseConfig = Field(
        default_factory=DatabaseConfig,
        description="Database backend settings.",
    )
    robotics: RoboticsConfig = Field(
        default_factory=RoboticsConfig,
        description="Robotics control system settings.",
    )

    Annotated  # noqa: B018 — keeps import alive for future validators
