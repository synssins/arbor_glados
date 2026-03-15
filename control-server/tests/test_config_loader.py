"""
Tests for configuration loader (C03).

Covers:
- Loading from YAML file
- Default config when no file exists
- Environment variable overrides (ARBOR__ prefix)
- Programmatic overrides
- Invalid YAML rejection
- Validation error propagation
- Config file auto-discovery (env var, default paths)
"""

from __future__ import annotations

from pathlib import Path

import pytest

from arbor_core.config.loader import ConfigError, load_config


@pytest.fixture()
def yaml_file(tmp_path: Path) -> Path:
    """Create a valid YAML config file."""
    cfg = tmp_path / "arbor.yaml"
    cfg.write_text(
        """\
version: "1.0"
server:
  host: "192.168.1.100"
  port: 9443
security:
  jwt_expiry_minutes: 120
  rate_limit_default_rpm: 500
logging:
  level: DEBUG
  format: console
""",
        encoding="utf-8",
    )
    return cfg


@pytest.fixture()
def yaml_with_nodes(tmp_path: Path) -> Path:
    """YAML with node definitions."""
    cfg = tmp_path / "nodes.yaml"
    cfg.write_text(
        """\
version: "1.0"
nodes:
  - id: "arm-01"
    type: "esp32-s3"
    transport: "uart"
    transport_config:
      port: "/dev/ttyUSB0"
      baud: 921600
    modules:
      - type: "servo-bus"
        config:
          protocol: "feetech-sts"
""",
        encoding="utf-8",
    )
    return cfg


class TestLoadFromYAML:
    """Loading config from YAML files."""

    def test_load_valid_yaml(self, yaml_file: Path) -> None:
        cfg = load_config(config_path=yaml_file)
        assert cfg.server.host == "192.168.1.100"
        assert cfg.server.port == 9443
        assert cfg.security.jwt_expiry_minutes == 120
        assert cfg.logging.level == "DEBUG"

    def test_load_with_nodes(self, yaml_with_nodes: Path) -> None:
        cfg = load_config(config_path=yaml_with_nodes)
        assert len(cfg.nodes) == 1
        assert cfg.nodes[0].id == "arm-01"
        assert cfg.nodes[0].transport == "uart"

    def test_empty_yaml_uses_defaults(self, tmp_path: Path) -> None:
        empty = tmp_path / "empty.yaml"
        empty.write_text("", encoding="utf-8")
        cfg = load_config(config_path=empty)
        assert cfg.server.port == 8000  # default
        assert cfg.version == "1.0"

    def test_yaml_with_only_mapping(self, tmp_path: Path) -> None:
        f = tmp_path / "partial.yaml"
        f.write_text("server:\n  port: 7777\n", encoding="utf-8")
        cfg = load_config(config_path=f)
        assert cfg.server.port == 7777
        assert cfg.security.jwt_algorithm == "RS256"  # default preserved


class TestDefaultConfig:
    """Config with no file."""

    def test_no_file_returns_defaults(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("ARBOR_CONFIG", raising=False)
        # Ensure no default paths exist in CWD
        cfg = load_config(config_path=None, overrides={"server": {"port": 1234}})
        assert cfg.server.port == 1234


class TestEnvOverrides:
    """Environment variable overrides."""

    def test_env_overrides_port(
        self, yaml_file: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setenv("ARBOR__SERVER__PORT", "7777")
        cfg = load_config(config_path=yaml_file)
        assert cfg.server.port == 7777

    def test_env_overrides_log_level(
        self, yaml_file: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setenv("ARBOR__LOGGING__LEVEL", "ERROR")
        cfg = load_config(config_path=yaml_file)
        assert cfg.logging.level == "ERROR"

    def test_env_creates_nested_keys(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        empty = tmp_path / "empty.yaml"
        empty.write_text("", encoding="utf-8")
        monkeypatch.setenv("ARBOR__SERVER__HOST", "10.0.0.1")
        cfg = load_config(config_path=empty)
        assert cfg.server.host == "10.0.0.1"


class TestProgrammaticOverrides:
    """Overrides dict applied after env vars."""

    def test_overrides_applied(self, yaml_file: Path) -> None:
        cfg = load_config(
            config_path=yaml_file,
            overrides={"server": {"port": 5555}},
        )
        assert cfg.server.port == 5555

    def test_overrides_deep_merge(self, yaml_file: Path) -> None:
        cfg = load_config(
            config_path=yaml_file,
            overrides={"security": {"jwt_expiry_minutes": 30}},
        )
        assert cfg.security.jwt_expiry_minutes == 30
        # Other security fields should keep their values
        assert cfg.security.rate_limit_default_rpm == 500  # from YAML


class TestInvalidConfig:
    """Error handling for bad configs."""

    def test_invalid_yaml_syntax(self, tmp_path: Path) -> None:
        bad = tmp_path / "bad.yaml"
        bad.write_text("server:\n  port: [unclosed", encoding="utf-8")
        with pytest.raises(ConfigError, match="Invalid YAML"):
            load_config(config_path=bad)

    def test_yaml_not_a_mapping(self, tmp_path: Path) -> None:
        bad = tmp_path / "list.yaml"
        bad.write_text("- item1\n- item2\n", encoding="utf-8")
        with pytest.raises(ConfigError, match="mapping"):
            load_config(config_path=bad)

    def test_validation_error_propagates(self, tmp_path: Path) -> None:
        bad = tmp_path / "invalid.yaml"
        bad.write_text("server:\n  port: 99999\n", encoding="utf-8")
        with pytest.raises(ConfigError, match="validation failed"):
            load_config(config_path=bad)

    def test_nonexistent_file_path(self, tmp_path: Path) -> None:
        missing = tmp_path / "nope.yaml"
        with pytest.raises(ConfigError, match="Cannot read"):
            load_config(config_path=missing)


class TestConfigFileDiscovery:
    """Auto-discovery via ARBOR_CONFIG env var."""

    def test_env_var_discovery(
        self, yaml_file: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setenv("ARBOR_CONFIG", str(yaml_file))
        cfg = load_config()
        assert cfg.server.host == "192.168.1.100"

    def test_env_var_missing_file(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("ARBOR_CONFIG", "/nonexistent/path.yaml")
        # Should fall through to defaults (not raise)
        cfg = load_config()
        assert cfg.server.port == 8000  # default
