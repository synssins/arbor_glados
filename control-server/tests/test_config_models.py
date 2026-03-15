"""
Tests for Pydantic configuration models (C02).

Covers:
- Default construction of root config
- Field validation (bounds, types, constraints)
- Servo min/max position cross-validation
- Nested model construction from dicts (simulating YAML load)
"""

import pytest
from pydantic import ValidationError

from arbor_core.config.models import (
    ArborConfig,
    NodeConfig,
    SecurityConfig,
    ServerConfig,
    ServoConfig,
    TLSConfig,
)


class TestArborConfigDefaults:
    """Root config builds with sensible defaults."""

    def test_default_construction(self) -> None:
        cfg = ArborConfig()
        assert cfg.version == "1.0"
        assert cfg.server.host == "0.0.0.0"
        assert cfg.server.port == 8000
        assert cfg.security.jwt_algorithm == "RS256"
        assert cfg.logging.level == "INFO"
        assert cfg.nodes == []

    def test_default_database_backend(self) -> None:
        cfg = ArborConfig()
        assert cfg.database.backend == "sqlite"

    def test_default_ai_constraints(self) -> None:
        cfg = ArborConfig()
        assert cfg.ai_constraints.max_servo_speed_pct == 60
        assert cfg.ai_constraints.command_rate_limit_rpm == 120


class TestServerConfig:
    """Server config validation."""

    def test_port_bounds_low(self) -> None:
        with pytest.raises(ValidationError, match="greater than or equal"):
            ServerConfig(port=0)

    def test_port_bounds_high(self) -> None:
        with pytest.raises(ValidationError, match="less than or equal"):
            ServerConfig(port=70000)

    def test_valid_port(self) -> None:
        cfg = ServerConfig(port=8080)
        assert cfg.port == 8080

    def test_tls_requires_paths(self) -> None:
        with pytest.raises(ValidationError):
            TLSConfig()  # cert_path and key_path are required


class TestSecurityConfig:
    """Security config validation."""

    def test_jwt_algorithm_locked(self) -> None:
        with pytest.raises(ValidationError):
            SecurityConfig(jwt_algorithm="HS256")  # type: ignore[arg-type]

    def test_api_key_min_length_floor(self) -> None:
        with pytest.raises(ValidationError, match="greater than or equal"):
            SecurityConfig(api_key_min_length=8)

    def test_rate_limit_positive(self) -> None:
        with pytest.raises(ValidationError, match="greater than or equal"):
            SecurityConfig(rate_limit_default_rpm=0)


class TestServoConfig:
    """Servo config validation."""

    def test_valid_servo(self) -> None:
        s = ServoConfig(id=1, name="shoulder-yaw")
        assert s.min_position == 0
        assert s.max_position == 4095

    def test_max_lt_min_rejected(self) -> None:
        with pytest.raises(ValidationError, match="max_position"):
            ServoConfig(id=1, name="bad", min_position=2000, max_position=100)

    def test_name_length_limit(self) -> None:
        with pytest.raises(ValidationError):
            ServoConfig(id=1, name="a" * 65)

    def test_id_bounds(self) -> None:
        with pytest.raises(ValidationError, match="less than or equal"):
            ServoConfig(id=254, name="over")

    def test_speed_non_negative(self) -> None:
        with pytest.raises(ValidationError, match="greater than or equal"):
            ServoConfig(id=1, name="neg-speed", max_speed=-1)


class TestNodeConfig:
    """Node config validation."""

    def test_valid_node(self) -> None:
        node = NodeConfig(
            id="arm-node-01",
            type="esp32-s3",
            transport="uart",
        )
        assert node.modules == []
        assert node.transport_config.baud == 921600

    def test_invalid_transport(self) -> None:
        with pytest.raises(ValidationError):
            NodeConfig(
                id="bad-node",
                type="esp32",
                transport="bluetooth",  # type: ignore[arg-type]
            )


class TestFullConfigFromDict:
    """Simulate loading from parsed YAML dict."""

    def test_from_yaml_like_dict(self) -> None:
        data = {
            "version": "1.0",
            "server": {
                "host": "0.0.0.0",
                "port": 8000,
            },
            "security": {
                "jwt_expiry_minutes": 120,
            },
            "nodes": [
                {
                    "id": "arm-node-01",
                    "type": "esp32-s3",
                    "transport": "uart",
                    "transport_config": {
                        "port": "/dev/ttyUSB0",
                        "baud": 921600,
                    },
                    "modules": [
                        {
                            "type": "servo-bus",
                            "config": {
                                "protocol": "feetech-sts",
                                "baud": 1000000,
                                "servos": [
                                    {
                                        "id": 1,
                                        "name": "shoulder-yaw",
                                        "min_position": 0,
                                        "max_position": 4095,
                                        "max_speed": 2000,
                                    }
                                ],
                            },
                        }
                    ],
                }
            ],
        }
        cfg = ArborConfig(**data)
        assert len(cfg.nodes) == 1
        assert cfg.nodes[0].id == "arm-node-01"
        assert cfg.nodes[0].modules[0].type == "servo-bus"
        assert cfg.security.jwt_expiry_minutes == 120
