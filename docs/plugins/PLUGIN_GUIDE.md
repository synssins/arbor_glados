# Plugin Development Guide

> Reference for implementing Arbor plugins on both ESP32 and the Control Server.

---

## Plugin Interface Contract

All plugins implement the same interface. This contract is **locked** — changes require quorum + human approval.

### Python (Control Server)

```python
from abc import ABC, abstractmethod
from enum import Enum
from pydantic import BaseModel
from typing import Any

class HealthStatus(Enum):
    OK = "ok"
    DEGRADED = "degraded"
    FAILED = "failed"

class PluginCapability(Enum):
    SERVO_BUS = "servo_bus"
    SERVO_PWM = "servo_pwm"
    STEPPER = "stepper"
    LED_ADDRESSABLE = "led_addressable"
    LED_PWM = "led_pwm"
    FAN_PWM = "fan_pwm"
    SENSOR_TEMP = "sensor_temp"
    SENSOR_DISTANCE = "sensor_distance"
    SENSOR_ENDSTOP = "sensor_endstop"
    SENSOR_BUTTON = "sensor_button"
    AUDIO = "audio"
    CAMERA = "camera"
    GPIO = "gpio"

class ArborPlugin(ABC):
    """Base class for all Arbor plugins."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Unique slug identifier, e.g. 'servo-bus-feetech'"""
        ...

    @property
    @abstractmethod
    def version(self) -> str:
        """SemVer string, e.g. '1.0.0'"""
        ...

    @property
    @abstractmethod
    def capabilities(self) -> list[PluginCapability]:
        """List of capabilities this plugin provides."""
        ...

    @property
    @abstractmethod
    def config_schema(self) -> type[BaseModel]:
        """Pydantic model class for this plugin's config."""
        ...

    @abstractmethod
    async def initialize(self, config: dict[str, Any]) -> None:
        """
        Called once at startup with validated config.
        Must complete initialization or raise PluginInitError.
        Must NOT block the event loop.
        """
        ...

    @abstractmethod
    async def shutdown(self) -> None:
        """
        Called on graceful shutdown. Release all resources.
        Must complete within 5 seconds.
        """
        ...

    @abstractmethod
    async def get_state(self) -> dict[str, Any]:
        """
        Return current state of all managed hardware.
        Must be non-blocking. Return cached state if hardware is slow.
        """
        ...

    @abstractmethod
    async def handle_command(self, command: str, params: dict[str, Any]) -> dict[str, Any]:
        """
        Handle a command. Return result dict.
        Raise PluginCommandError on invalid command or params.
        Raise PluginHardwareError on hardware failure.
        MUST validate all params against config limits before executing.
        """
        ...

    @abstractmethod
    async def health_check(self) -> HealthStatus:
        """
        Return current health. Must be fast (< 100ms).
        Should not perform hardware I/O — use cached state.
        """
        ...
```

### Plugin Errors

```python
class PluginInitError(Exception):
    """Plugin failed to initialize. System will not load the plugin."""

class PluginCommandError(ValueError):
    """Invalid command or params. Returns 400 to API caller."""

class PluginHardwareError(IOError):
    """Hardware communication failed. Returns 503 to API caller."""

class PluginLimitError(ValueError):
    """Command would exceed configured hardware limits. Returns 422."""
```

### Plugin Registration

Plugins are discovered via Python entry points in `pyproject.toml`:

```toml
[project.entry-points."arbor.plugins"]
servo-bus-feetech = "arbor_feetech.plugin:FeetechServoBusPlugin"
```

Or via the `plugins.search_paths` config (for user plugins loaded from directories).

---

## ESP32 Plugin Architecture

On ESP32, plugins are CMake components registered in `idf_component_manager`.

### ESP32 Plugin Interface (C)

```c
// arbor_plugin.h
typedef struct {
    const char *name;           // e.g. "servo-bus-feetech"
    const char *version;        // e.g. "1.0.0"
    const char *capabilities;   // comma-separated capability strings

    esp_err_t (*initialize)(const cJSON *config);
    esp_err_t (*shutdown)(void);
    esp_err_t (*get_state)(cJSON **state_out);
    esp_err_t (*handle_command)(const char *command, const cJSON *params, cJSON **result_out);
    esp_err_t (*health_check)(arbor_health_t *health_out);
} arbor_plugin_t;

// Register your plugin:
ESP_ERROR_CHECK(arbor_register_plugin(&my_plugin));
```

### ESP32 Component Structure

```
components/
└── plugin_servo_bus_feetech/
    ├── CMakeLists.txt
    ├── include/
    │   └── plugin_servo_bus_feetech.h
    ├── plugin_servo_bus_feetech.c
    └── idf_component.yml
```

---

## Config Schema Example

Every plugin defines a Pydantic config model:

```python
from pydantic import BaseModel, Field, field_validator

class ServoConfig(BaseModel):
    id: int = Field(ge=0, le=253)
    name: str = Field(min_length=1, max_length=64)
    min_position: int = Field(ge=0, le=4095)
    max_position: int = Field(ge=0, le=4095)
    max_speed: int = Field(ge=0, le=4000)
    max_acceleration: int = Field(ge=0, le=2000, default=500)

    @field_validator("max_position")
    @classmethod
    def max_must_exceed_min(cls, v: int, info) -> int:
        if "min_position" in info.data and v <= info.data["min_position"]:
            raise ValueError("max_position must be greater than min_position")
        return v

class FeetechServoBusConfig(BaseModel):
    protocol: str = Field(default="scs", pattern="^(scs|sts)$")
    baud: int = Field(default=1000000, ge=9600, le=4000000)
    uart_port: str  # No default — must be provisioned
    servos: list[ServoConfig] = Field(default_factory=list)
```

---

## Command Validation Pattern

All commands MUST validate against configured limits before executing hardware:

```python
async def handle_command(self, command: str, params: dict) -> dict:
    match command:
        case "set_position":
            servo_id = params["id"]
            position = params["position"]
            servo_cfg = self._get_servo_config(servo_id)
            
            # ALWAYS check limits from config — never trust caller
            if not (servo_cfg.min_position <= position <= servo_cfg.max_position):
                raise PluginLimitError(
                    f"Position {position} out of range "
                    f"[{servo_cfg.min_position}, {servo_cfg.max_position}]"
                )
            
            await self._write_position(servo_id, position)
            return {"servo_id": servo_id, "position": position, "status": "ok"}
        
        case _:
            raise PluginCommandError(f"Unknown command: {command!r}")
```

---

## State Publishing

Plugins should publish state changes to the event bus (not just return on poll):

```python
# In your plugin, after any state change:
from arbor.events import event_bus

await event_bus.publish(
    topic=f"servo.{servo_id}.position_changed",
    data={"position": new_position, "servo_id": servo_id}
)
```

---

## Testing Your Plugin

Every plugin must have:

1. Unit tests for config validation (valid + invalid configs)
2. Unit tests for command validation (valid + out-of-bounds)
3. Integration tests with a mock hardware interface
4. A `health_check()` test that exercises failure paths

```
tests/
└── plugins/
    └── test_servo_bus_feetech/
        ├── test_config.py
        ├── test_commands.py
        ├── test_state.py
        └── test_health.py
```

---

*This guide is authoritative for plugin development.*
*Interface changes require quorum + human approval.*
