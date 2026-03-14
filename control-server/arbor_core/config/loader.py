"""
Configuration loader — YAML file, environment variable overrides, validation.

Loading order (later overrides earlier):
1. Built-in defaults (from Pydantic model defaults)
2. YAML config file (path from ARBOR_CONFIG env var or default locations)
3. Environment variable overrides (ARBOR__ prefix, double-underscore nesting)

Task: C03
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import structlog
import yaml
from pydantic import ValidationError

from arbor_core.config.models import ArborConfig

logger = structlog.get_logger(__name__)

# Default config file search paths (checked in order)
_DEFAULT_CONFIG_PATHS: list[Path] = [
    Path("arbor.yaml"),
    Path("config/arbor.yaml"),
    Path("/etc/arbor/arbor.yaml"),
]

# Environment variable prefix for overrides
_ENV_PREFIX = "ARBOR__"


class ConfigError(Exception):
    """Raised when configuration loading or validation fails."""


def _find_config_file() -> Path | None:
    """
    Locate the configuration file.

    Checks ARBOR_CONFIG env var first, then default paths.

    Returns:
        Path to the config file, or None if not found.
    """
    env_path = os.environ.get("ARBOR_CONFIG")
    if env_path:
        path = Path(env_path)
        if path.is_file():
            return path
        logger.warning("config_env_path_not_found", path=env_path)
        return None

    for candidate in _DEFAULT_CONFIG_PATHS:
        if candidate.is_file():
            return candidate

    return None


def _load_yaml(path: Path) -> dict[str, Any]:
    """
    Load and parse a YAML config file.

    Args:
        path: Path to the YAML file.

    Returns:
        Parsed config dictionary.

    Raises:
        ConfigError: If the file cannot be read or parsed.
    """
    try:
        raw = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        msg = f"Cannot read config file: {path}: {exc}"
        raise ConfigError(msg) from exc

    try:
        data = yaml.safe_load(raw)
    except yaml.YAMLError as exc:
        msg = f"Invalid YAML in config file: {path}: {exc}"
        raise ConfigError(msg) from exc

    if data is None:
        return {}
    if not isinstance(data, dict):
        msg = f"Config file must contain a YAML mapping, got {type(data).__name__}"
        raise ConfigError(msg)
    return data


def _apply_env_overrides(data: dict[str, Any]) -> dict[str, Any]:
    """
    Apply environment variable overrides to config data.

    Environment variables are prefixed with ARBOR__ and use
    double-underscore for nesting. Examples:

        ARBOR__SERVER__PORT=9443     -> data["server"]["port"] = "9443"
        ARBOR__LOGGING__LEVEL=DEBUG  -> data["logging"]["level"] = "DEBUG"

    Values are strings; Pydantic handles type coercion.

    Args:
        data: Config dictionary to overlay onto.

    Returns:
        Updated config dictionary.
    """
    for key, value in os.environ.items():
        if not key.startswith(_ENV_PREFIX):
            continue
        # Strip prefix and split on double-underscore
        parts = key[len(_ENV_PREFIX) :].lower().split("__")
        if not parts or not all(parts):
            continue

        # Navigate into nested dict, creating intermediate dicts as needed
        target = data
        for part in parts[:-1]:
            if part not in target or not isinstance(target[part], dict):
                target[part] = {}
            target = target[part]
        target[parts[-1]] = value

    return data


def load_config(
    config_path: Path | None = None,
    overrides: dict[str, Any] | None = None,
) -> ArborConfig:
    """
    Load, merge, and validate the Arbor configuration.

    Args:
        config_path: Explicit path to a YAML config file.
                     If None, auto-discovers via env var or default paths.
        overrides: Optional dictionary of overrides (applied after env vars).

    Returns:
        Validated ArborConfig instance.

    Raises:
        ConfigError: If the config file is unreadable, invalid YAML, or
                     fails Pydantic validation.
    """
    # Step 1: Load YAML file (if any)
    path = config_path or _find_config_file()
    if path is not None:
        data = _load_yaml(path)
        logger.info("config_loaded_from_file", path=str(path))
    else:
        data = {}
        logger.info("config_using_defaults", reason="no config file found")

    # Step 2: Apply environment variable overrides
    data = _apply_env_overrides(data)

    # Step 3: Apply programmatic overrides
    if overrides:
        _deep_merge(data, overrides)

    # Step 4: Validate with Pydantic
    try:
        config = ArborConfig(**data)
    except ValidationError as exc:
        msg = f"Configuration validation failed:\n{exc}"
        raise ConfigError(msg) from exc

    logger.info(
        "config_validated",
        version=config.version,
        server_port=config.server.port,
        node_count=len(config.nodes),
    )
    return config


def _deep_merge(base: dict[str, Any], overlay: dict[str, Any]) -> None:
    """
    Recursively merge overlay into base (in-place).

    Args:
        base: The base dictionary (modified in-place).
        overlay: Values to merge on top.
    """
    for key, value in overlay.items():
        if key in base and isinstance(base[key], dict) and isinstance(value, dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value
