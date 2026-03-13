"""
Plugin manager — discovery, loading, initialization, and shutdown.

Discovers plugins via:
1. Python entry_points (group: 'arbor.plugins')
2. Configured search_paths directories

Manages the full lifecycle: discover -> load -> initialize -> shutdown.

Task: C04
"""

from __future__ import annotations

import importlib
import sys
from importlib.metadata import entry_points
from pathlib import Path
from typing import Any

import structlog

from arbor_core.plugins.base import ArborPlugin, HealthState, HealthStatus

logger = structlog.get_logger(__name__)


class PluginError(Exception):
    """Raised when a plugin operation fails."""


class PluginManager:
    """
    Discovers, loads, and manages Arbor plugin lifecycles.

    Usage::

        manager = PluginManager()
        manager.discover(search_paths=[Path("/etc/arbor/plugins")])
        await manager.initialize_plugin("servo-bus", config={...})
        ...
        await manager.shutdown_all()

    Attributes:
        registry: Mapping of plugin name -> plugin class (discovered but not instantiated).
        instances: Mapping of plugin name -> live plugin instance.
    """

    def __init__(self) -> None:
        self._registry: dict[str, type[ArborPlugin]] = {}
        self._instances: dict[str, ArborPlugin] = {}

    # -- Public properties ---------------------------------------------------

    @property
    def registry(self) -> dict[str, type[ArborPlugin]]:
        """All discovered plugin classes, keyed by name."""
        return dict(self._registry)

    @property
    def instances(self) -> dict[str, ArborPlugin]:
        """All live (initialized) plugin instances, keyed by name."""
        return dict(self._instances)

    # -- Discovery -----------------------------------------------------------

    def discover(self, search_paths: list[Path] | None = None) -> list[str]:
        """
        Discover available plugins from entry_points and filesystem paths.

        Args:
            search_paths: Additional directories containing plugin modules.

        Returns:
            List of newly discovered plugin names.
        """
        discovered: list[str] = []
        discovered.extend(self._discover_entry_points())
        if search_paths:
            discovered.extend(self._discover_paths(search_paths))
        logger.info(
            "plugin_discovery_complete",
            total_registered=len(self._registry),
            newly_discovered=discovered,
        )
        return discovered

    def _discover_entry_points(self) -> list[str]:
        """Discover plugins registered as Python entry_points."""
        discovered: list[str] = []
        plugin_eps = entry_points(group="arbor.plugins")
        for ep in plugin_eps:
            try:
                plugin_cls = ep.load()
                self._validate_and_register(plugin_cls, source=f"entry_point:{ep.name}")
                discovered.append(plugin_cls.name.fget(plugin_cls) if isinstance(plugin_cls.name, property) else ep.name)  # type: ignore[union-attr]
            except Exception:
                logger.exception("plugin_entry_point_load_failed", entry_point=ep.name)
        return discovered

    def _discover_paths(self, search_paths: list[Path]) -> list[str]:
        """Discover plugins from filesystem directories."""
        discovered: list[str] = []
        for search_dir in search_paths:
            if not search_dir.is_dir():
                logger.warning("plugin_search_path_not_found", path=str(search_dir))
                continue
            # Add to sys.path so importlib can find modules
            path_str = str(search_dir)
            if path_str not in sys.path:
                sys.path.insert(0, path_str)
            for py_file in search_dir.glob("*.py"):
                if py_file.name.startswith("_"):
                    continue
                module_name = py_file.stem
                try:
                    module = importlib.import_module(module_name)
                    for attr_name in dir(module):
                        attr = getattr(module, attr_name)
                        if (
                            isinstance(attr, type)
                            and issubclass(attr, ArborPlugin)
                            and attr is not ArborPlugin
                        ):
                            name = self._validate_and_register(
                                attr, source=f"path:{py_file}"
                            )
                            if name:
                                discovered.append(name)
                except Exception:
                    logger.exception(
                        "plugin_module_load_failed",
                        module=module_name,
                        path=str(py_file),
                    )
        return discovered

    def _validate_and_register(
        self, plugin_cls: type[ArborPlugin], source: str
    ) -> str | None:
        """
        Validate a plugin class and add it to the registry.

        Returns:
            The plugin name if registered, None if skipped.
        """
        if not (isinstance(plugin_cls, type) and issubclass(plugin_cls, ArborPlugin)):
            logger.warning(
                "plugin_invalid_class",
                source=source,
                cls=str(plugin_cls),
            )
            return None

        # Instantiate temporarily to read name property — or use class check
        # We need the name to register. For ABCs with @property, we need an instance.
        # Instead, we store the class and defer name resolution.
        # We'll use the entry_point name or discover name at init time.
        # For now, register by source and resolve at init.

        if plugin_cls in self._registry.values():
            return None

        # Try to get name from class (works if name is a class variable, not a property)
        # For properties, we store by class name and resolve later.
        try:
            # Create a temporary instance check is too expensive for discovery.
            # Convention: plugin classes should have a NAME class attribute or we use cls.__name__
            name = getattr(plugin_cls, "NAME", None) or plugin_cls.__name__
        except Exception:
            name = plugin_cls.__name__

        if name in self._registry:
            logger.warning(
                "plugin_name_conflict",
                name=name,
                existing=str(self._registry[name]),
                new=str(plugin_cls),
                source=source,
            )
            return None

        self._registry[name] = plugin_cls
        logger.info("plugin_registered", name=name, source=source)
        return name

    # -- Registration (manual) -----------------------------------------------

    def register(self, plugin_cls: type[ArborPlugin], name: str | None = None) -> None:
        """
        Manually register a plugin class.

        Args:
            plugin_cls: The plugin class to register.
            name: Override name. If None, uses class NAME attr or __name__.
        """
        resolved_name = name or getattr(plugin_cls, "NAME", None) or plugin_cls.__name__
        if resolved_name in self._registry:
            msg = f"Plugin '{resolved_name}' is already registered"
            raise PluginError(msg)
        self._registry[resolved_name] = plugin_cls
        logger.info("plugin_registered", name=resolved_name, source="manual")

    # -- Lifecycle -----------------------------------------------------------

    async def initialize_plugin(self, name: str, config: dict[str, Any]) -> ArborPlugin:
        """
        Instantiate and initialize a registered plugin.

        Args:
            name: Registered plugin name.
            config: Plugin-specific configuration dict.

        Returns:
            The initialized plugin instance.

        Raises:
            PluginError: If the plugin is not registered or init fails.
        """
        if name not in self._registry:
            msg = f"Plugin '{name}' is not registered. Discovered: {list(self._registry.keys())}"
            raise PluginError(msg)

        if name in self._instances:
            msg = f"Plugin '{name}' is already initialized"
            raise PluginError(msg)

        plugin_cls = self._registry[name]
        try:
            instance = plugin_cls()
            await instance.initialize(config)
        except Exception as exc:
            msg = f"Failed to initialize plugin '{name}': {exc}"
            raise PluginError(msg) from exc

        self._instances[name] = instance
        logger.info("plugin_initialized", name=name, version=instance.version)
        return instance

    async def shutdown_plugin(self, name: str) -> None:
        """
        Shut down a running plugin instance.

        Args:
            name: Plugin instance name.

        Raises:
            PluginError: If the plugin is not running.
        """
        if name not in self._instances:
            msg = f"Plugin '{name}' is not running"
            raise PluginError(msg)

        instance = self._instances[name]
        try:
            await instance.shutdown()
        except Exception:
            logger.exception("plugin_shutdown_error", name=name)
        finally:
            del self._instances[name]
            logger.info("plugin_shutdown", name=name)

    async def shutdown_all(self) -> None:
        """Shut down all running plugin instances."""
        names = list(self._instances.keys())
        for name in names:
            await self.shutdown_plugin(name)
        logger.info("all_plugins_shutdown")

    # -- Queries -------------------------------------------------------------

    def get_instance(self, name: str) -> ArborPlugin | None:
        """Get a running plugin instance by name."""
        return self._instances.get(name)

    async def health_check_all(self) -> dict[str, HealthStatus]:
        """
        Run health checks on all running plugins.

        Returns:
            Mapping of plugin name -> HealthStatus.
        """
        results: dict[str, HealthStatus] = {}
        for name, instance in self._instances.items():
            try:
                results[name] = await instance.health_check()
            except Exception as exc:
                results[name] = HealthStatus(
                    state=HealthState.UNHEALTHY,
                    message=f"Health check raised: {exc}",
                )
        return results
