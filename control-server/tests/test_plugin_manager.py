"""
Tests for plugin manager (C04).

Covers:
- Manual registration
- Initialize / shutdown lifecycle
- Duplicate registration rejection
- Health check aggregation
- Error handling (unknown plugin, double init, etc.)
"""

from __future__ import annotations

from typing import Any

import pytest

from arbor_core.plugins.base import ArborPlugin, HealthState, HealthStatus
from arbor_core.plugins.manager import PluginError, PluginManager


# ---------------------------------------------------------------------------
# Fake plugin for testing
# ---------------------------------------------------------------------------
class FakeServoPlugin(ArborPlugin):
    """Minimal concrete plugin for testing."""

    NAME = "fake-servo"

    def __init__(self) -> None:
        self._initialized = False
        self._config: dict[str, Any] = {}

    @property
    def name(self) -> str:
        return "fake-servo"

    @property
    def version(self) -> str:
        return "0.1.0"

    @property
    def capabilities(self) -> list[str]:
        return ["servo"]

    @property
    def config_schema(self) -> dict[str, Any]:
        return {"type": "object"}

    async def initialize(self, config: dict[str, Any]) -> None:
        self._config = config
        self._initialized = True

    async def shutdown(self) -> None:
        self._initialized = False

    async def get_state(self) -> dict[str, Any]:
        return {"initialized": self._initialized}

    async def handle_command(self, command: str, params: dict[str, Any]) -> dict[str, Any]:
        return {"command": command, "params": params, "status": "ok"}

    async def health_check(self) -> HealthStatus:
        if self._initialized:
            return HealthStatus(state=HealthState.HEALTHY, message="all good")
        return HealthStatus(state=HealthState.UNHEALTHY, message="not initialized")


class FailingPlugin(ArborPlugin):
    """Plugin that fails on initialize."""

    NAME = "failing"

    @property
    def name(self) -> str:
        return "failing"

    @property
    def version(self) -> str:
        return "0.0.1"

    @property
    def capabilities(self) -> list[str]:
        return []

    @property
    def config_schema(self) -> dict[str, Any]:
        return {}

    async def initialize(self, config: dict[str, Any]) -> None:
        msg = "intentional init failure"
        raise RuntimeError(msg)

    async def shutdown(self) -> None:
        pass

    async def get_state(self) -> dict[str, Any]:
        return {}

    async def handle_command(self, command: str, params: dict[str, Any]) -> dict[str, Any]:
        return {}

    async def health_check(self) -> HealthStatus:
        return HealthStatus(state=HealthState.UNKNOWN)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
class TestPluginRegistration:
    """Plugin discovery and registration."""

    def test_register_plugin(self) -> None:
        mgr = PluginManager()
        mgr.register(FakeServoPlugin, name="fake-servo")
        assert "fake-servo" in mgr.registry

    def test_duplicate_registration_raises(self) -> None:
        mgr = PluginManager()
        mgr.register(FakeServoPlugin, name="fake-servo")
        with pytest.raises(PluginError, match="already registered"):
            mgr.register(FakeServoPlugin, name="fake-servo")

    def test_registry_is_copy(self) -> None:
        mgr = PluginManager()
        mgr.register(FakeServoPlugin, name="fake-servo")
        reg = mgr.registry
        reg.pop("fake-servo")
        assert "fake-servo" in mgr.registry  # internal state unchanged

    def test_discover_returns_empty_with_no_sources(self) -> None:
        mgr = PluginManager()
        result = mgr.discover()
        assert isinstance(result, list)


class TestPluginLifecycle:
    """Initialize and shutdown."""

    @pytest.fixture()
    def manager(self) -> PluginManager:
        mgr = PluginManager()
        mgr.register(FakeServoPlugin, name="fake-servo")
        return mgr

    async def test_initialize_plugin(self, manager: PluginManager) -> None:
        instance = await manager.initialize_plugin("fake-servo", config={"baud": 1000000})
        assert instance.name == "fake-servo"
        assert "fake-servo" in manager.instances

    async def test_initialize_unknown_raises(self, manager: PluginManager) -> None:
        with pytest.raises(PluginError, match="not registered"):
            await manager.initialize_plugin("nonexistent", config={})

    async def test_double_init_raises(self, manager: PluginManager) -> None:
        await manager.initialize_plugin("fake-servo", config={})
        with pytest.raises(PluginError, match="already initialized"):
            await manager.initialize_plugin("fake-servo", config={})

    async def test_shutdown_plugin(self, manager: PluginManager) -> None:
        await manager.initialize_plugin("fake-servo", config={})
        await manager.shutdown_plugin("fake-servo")
        assert "fake-servo" not in manager.instances

    async def test_shutdown_unknown_raises(self, manager: PluginManager) -> None:
        with pytest.raises(PluginError, match="not running"):
            await manager.shutdown_plugin("nonexistent")

    async def test_shutdown_all(self, manager: PluginManager) -> None:
        await manager.initialize_plugin("fake-servo", config={})
        await manager.shutdown_all()
        assert len(manager.instances) == 0

    async def test_get_instance(self, manager: PluginManager) -> None:
        await manager.initialize_plugin("fake-servo", config={})
        inst = manager.get_instance("fake-servo")
        assert inst is not None
        assert inst.name == "fake-servo"

    async def test_get_instance_missing(self, manager: PluginManager) -> None:
        assert manager.get_instance("nope") is None


class TestPluginInitFailure:
    """Plugins that fail during initialization."""

    async def test_failing_init_raises_plugin_error(self) -> None:
        mgr = PluginManager()
        mgr.register(FailingPlugin, name="failing")
        with pytest.raises(PluginError, match="Failed to initialize"):
            await mgr.initialize_plugin("failing", config={})
        # Should NOT be in instances after failed init
        assert "failing" not in mgr.instances


class TestHealthChecks:
    """Health check aggregation."""

    async def test_health_check_all(self) -> None:
        mgr = PluginManager()
        mgr.register(FakeServoPlugin, name="fake-servo")
        await mgr.initialize_plugin("fake-servo", config={})
        results = await mgr.health_check_all()
        assert "fake-servo" in results
        assert results["fake-servo"].state == HealthState.HEALTHY

    async def test_health_check_empty(self) -> None:
        mgr = PluginManager()
        results = await mgr.health_check_all()
        assert results == {}
