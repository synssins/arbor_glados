# Arbor — Modular Robotics Control Platform
## Master Project Plan v1.0

> **CANONICAL DOCUMENT** — All development decisions trace back to this plan.
> No architectural deviation is permitted without explicit human approval from the project owner.

---

## Table of Contents

1. [Vision & Goals](#1-vision--goals)
2. [Architecture Overview](#2-architecture-overview)
3. [Hardware Support Matrix](#3-hardware-support-matrix)
4. [Software Stack](#4-software-stack)
5. [Module Definitions](#5-module-definitions)
6. [API Design](#6-api-design)
7. [Security Architecture](#7-security-architecture)
8. [WebUI Specification](#8-webui-specification)
9. [Configuration System](#9-configuration-system)
10. [AI/Autonomy Integration](#10-aiautonomy-integration)
11. [Development Phases](#11-development-phases)
12. [Multi-Agent Development Rules](#12-multi-agent-development-rules)
13. [Technology Decisions (Locked)](#13-technology-decisions-locked)
14. [Open Questions (Require Human Input)](#14-open-questions-require-human-input)

---

## 1. Vision & Goals

**Arbor** is a fully modular, plugin-centric robotics control platform designed for:

- **AI-driven control** — Any system with a valid API key (e.g., GLaDOS) can attach and command hardware.
- **Human control** — A full WebUI allows direct manual operation, monitoring, and configuration.
- **Reference Design Stitching** — Each hardware node is independently functional. The control system stitches nodes together, extending capability without introducing hard dependencies.
- **Platform agnosticism** — Runs on ESP32 microcontrollers, Klipper-managed motion control boards (BigTreeTech SKR Pico/RP2040, Manta M8P/STM32, Octopus/STM32, and others), Raspberry Pi, Jetson Orin Nano Super, or any combination thereof.
- **Production security** — API key auth, TLS everywhere, rate limiting, input validation, zero hardcoded values.

### Design Principles

1. **ZERO hardcoded values.** Every configurable value lives in the central configuration system.
2. **Plugin-first.** All hardware drivers, protocols, and sensor types are plugins — nothing is baked in.
3. **API-first.** The WebUI is a consumer of the same API available to external systems.
4. **Reference Design Stitching.** Each node is independently useful; the control layer adds orchestration.
5. **Security is non-negotiable.** Follows VibeSec principles throughout. Security gates code review.
6. **ROS2-compatible.** Architecture exposes ROS2 topics/services as an optional transport layer.
7. **Klipper compatibility.** The stepper/motion control layer wraps or interfaces Klipper where applicable.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    CONTROL LAYER (Pi5 / Jetson)                 │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
│  │ Arbor│  │  Klipper │  │   ROS2   │  │  AI Gateway   │  │
│  │  Control │  │  Bridge  │  │  Bridge  │  │  (GLaDOS API) │  │
│  │  Server  │  │(Moonraker│  │          │  │               │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬────────┘  │
│       └─────────────┴─────────────┴────────────────┘          │
│                            ▼                                    │
│                    Plugin Manager + Config                       │
│                    WebUI Server (React SPA)                     │
└─────────────────────────────────────────────────────────────────┘
                             │
                    TRANSPORT LAYER
        (USB, UART, SPI, WiFi, Ethernet, CAN Bus)
                             │
     ┌───────────────────────┼────────────────────────┐
     ▼                       ▼                        ▼
┌─────────────────┐ ┌──────────────────────┐ ┌─────────────────┐
│   ESP32 Node    │ │  BTT Klipper Node    │ │   Pi Hat Node   │
│                 │ │  (SKR Pico / Manta   │ │                 │
│ Servo Bus      │ │   M8P / Octopus)     │ │ Audio + LED     │
│ PWM Servos     │ │                      │ │ Sensors         │
│ WS2812b LEDs   │ │ Stepper Motors       │ │ PWM Fans        │
│ Sensors        │ │ TMC2209/5160 Drivers │ │ WebUI + API     │
│ WebUI + API    │ │ Endstops / Probes    │ │                 │
│                 │ │ Fans / Heaters       │ │                 │
│                 │ │ Klipper Firmware     │ │                 │
│                 │ │ → Moonraker API     │ │                 │
└─────────────────┘ └──────────────────────┘ └─────────────────┘
```

### Control Flow

```
External AI / Human
        │
        ▼
Control Server API (HTTPS + API Key)
        │
        ├──→ Plugin Manager
        │         │
        │         ├──→ Servo Plugin → ESP32 Node (USB/UART/WiFi)
        │         ├──→ Stepper Plugin → Moonraker → BTT Klipper Board
        │         │                  → or direct step/dir (ESP32)
        │         ├──→ LED Plugin → WLED / native
        │         ├──→ Sensor Plugin → aggregate readings
        │         └──→ Audio Plugin → Pi audio / ESP32 audio
        │
        └──→ WebSocket → WebUI (live state updates)
```

---

## 3. Hardware Support Matrix

### Microcontroller Targets

| Platform | CPU | Flash | RAM | Notes |
|----------|-----|-------|-----|-------|
| ESP32 (original) | Xtensa dual-core 240MHz | 4MB+ | 520KB | Primary target |
| ESP32-S3 | Xtensa dual-core 240MHz | 8MB+ | 512KB | USB native, preferred |
| ESP32-C3 | RISC-V 160MHz | 4MB | 400KB | Low-cost option |
| ESP32-P4 | RISC-V dual-core 400MHz | Varies | 768KB | High-perf target |

### Klipper-Managed Motion Control Boards

These boards run Klipper firmware and are managed by the Control Server via Moonraker API. They are first-class node types — not secondary or optional. They excel at stepper motor control, endstop handling, and high-speed PWM (heaters, fans), making them ideal as dedicated motion controller nodes in a multi-node Arbor deployment.

The MCU firmware on these boards is **Klipper** — Arbor does not write custom firmware for them. The Control Server's `klipper-bridge` plugin communicates with Moonraker (the Klipper API layer) over HTTP/WebSocket.

| Board | MCU | Notes |
|-------|-----|-------|
| BigTreeTech SKR Pico | RP2040 (dual-core ARM Cortex-M0+ 133MHz) | Compact, 4x stepper, USB/UART to Pi |
| BigTreeTech Manta M8P | STM32G0B1 | 8x stepper, CAN bus, designed for CB1/Pi CM4 |
| BigTreeTech Octopus / Octopus Pro | STM32F446 / STM32H723 | 8x stepper, flagship motion board |
| BigTreeTech SKR 3 / SKR 3 EZ | STM32H743 | Mid-range, 5x stepper |
| BigTreeTech EBB CAN toolhead | STM32G0B1 | CAN bus toolhead/end-effector board |
| Generic LPC1768/LPC1769 | NXP ARM Cortex-M3 | Smoothieboard-era, Klipper supported |
| Generic STM32 (F1, F4, H7 series) | ARM Cortex-M3/M4/M7 | Wide range of Klipper-supported variants |
| Generic RP2040 boards | RP2040 | Any Klipper-supported RP2040 board |

**Stepper Drivers (on these boards):**

| Driver | Interface | Notes |
|--------|-----------|-------|
| TMC2209 | UART + Step/Dir | Most common, stall detection, quiet |
| TMC2226 | UART + Step/Dir | Higher current TMC2209 variant |
| TMC5160 | SPI + Step/Dir | High-current, external FETs |
| TMC2240 | SPI + Step/Dir | Next-gen, integrated |
| A4988 / DRV8825 | Step/Dir only | Basic, no UART |

### Single Board Computer Targets

| Platform | OS | Notes |
|----------|----|-------|
| Raspberry Pi 4/5 | Raspberry Pi OS / Ubuntu | Primary SBC, default control layer |
| Jetson Orin Nano Super | JetPack / Ubuntu | GPU-accelerated AI inference on-device |
| Generic x86 Linux | Ubuntu 22.04+ | Development / simulation |

### Hardware Peripherals (Plugin-Managed)

| Category | Supported Variants |
|----------|--------------------|
| Serial Bus Servos | Feetech SCS/STS series, Dynamixel AX/MX/X series, Herkulex |
| PWM Servos | Standard 50Hz, high-speed digital |
| Steppers (via Klipper) | TMC2209 (UART), TMC2226, TMC5160 (SPI), TMC2240 (SPI), A4988, DRV8825 — managed via Klipper/Moonraker on BTT boards |
| Steppers (direct/ESP32) | Step/Dir pulse interface for simple drivers without Klipper |
| LEDs Addressable | WS2812B, SK6812, APA102/SK9822, WS2801, LPD8806, TM1814 (WLED-compatible full list) |
| LEDs PWM | Single-channel, RGB, RGBW — dimmer control |
| Sensors — Temp | DS18B20, DHT22, BME280, AHT20, SHT31 |
| Sensors — Distance | HC-SR04 (sonar), VL53L0X (TOF), TF-Luna, LD06 (LIDAR) |
| Sensors — Vision | USB cameras, CSI cameras (Pi), MIPI (Jetson) |
| Sensors — Radar | HLK-LD303 (presence), mmWave modules |
| Endstops | NC/NO mechanical, optical, hall effect |
| Buttons | Momentary, toggle, capacitive |
| Fan Control | PWM tachometer-capable |
| Audio | I2S DAC/ADC, USB audio, HDMI audio (Pi/Jetson) |

---

## 4. Software Stack

### Firmware (ESP32 Nodes)

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| RTOS | FreeRTOS (via ESP-IDF) | Native ESP32 support |
| Framework | ESP-IDF 5.x | Full hardware access vs Arduino |
| HTTP Server | ESP-IDF httpd (native) | Lightweight, no deps |
| WebSocket | ESP-IDF WS | Real-time state |
| TLS | mbedTLS (built-in) | Required for API security |
| Config Store | NVS (Non-Volatile Storage) | Persistent, key-value |
| OTA Update | ESP-IDF OTA partition | Secure update capability |
| Serial Bus | Custom plugin (FreeRTOS task) | Servo protocol abstraction |
| Build System | CMake + idf.py | Standard ESP-IDF |

### Firmware (Klipper-Managed Motion Boards)

Arbor does **not** write or maintain firmware for BTT and similar boards. These run **Klipper** firmware, compiled and flashed using the standard Klipper toolchain (`make menuconfig`, `make flash`). The Control Server communicates with them exclusively through **Moonraker** (Klipper's HTTP/WebSocket API layer).

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| MCU Firmware | Klipper (community-maintained) | Industry-standard, battle-tested on these exact boards |
| API Layer | Moonraker (HTTP + WebSocket) | Full machine control, config management, OTA |
| Config | Klipper `printer.cfg` + Arbor wrapper | Klipper owns motion config, Arbor wraps the API |
| Flash Method | Klipper standard (`make`, USB DFU, SD card) | Board-specific, documented per board in `/docs/hardware/` |
| CAN Bus | CanBoot + Klipper CAN | For EBB and CAN-connected toolhead boards |

**Note on `printer.cfg`:** Klipper's config is robot/machine-specific. The `klipper-bridge` plugin in Arbor reads the Klipper config to discover available steppers, endstops, fans, and sensors — it does not generate or manage `printer.cfg` itself. A template `printer.cfg` for common BTT board + robot arm configurations will be maintained in `/docs/hardware/klipper-configs/`.

### Control Server (Pi / Jetson)

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| Runtime | Python 3.11+ | Wide library support, rapid dev |
| API Framework | FastAPI | Async, OpenAPI auto-docs, typed |
| WebSocket | FastAPI WebSocket | Same process, shared state |
| Config | Pydantic + YAML | Typed, validated, no hardcoding |
| Plugin System | Python importlib + entry_points | Dynamic discovery |
| Auth | JWT (RS256) + API Keys | Layered security |
| Database | SQLite (default) / PostgreSQL (optional) | Audit log, config persistence |
| Serial Comms | pyserial + asyncio | ESP32 and direct UART node bridge |
| Klipper Bridge | Moonraker API client (HTTP + WebSocket) | Full stepper/motion on BTT boards (SKR Pico, Manta M8P, Octopus, etc.) |
| CAN Bus Bridge | python-can (optional) | Direct CAN for EBB/toolhead boards outside Klipper context |
| ROS2 Bridge | rclpy (optional layer) | ROS2 ecosystem integration |
| Logging | structlog → JSON | Machine-parseable |
| Process Mgmt | systemd service | Production deployment |

### WebUI

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| Framework | React 18 + TypeScript | Industry standard |
| Build | Vite | Fast HMR, modern |
| State | Zustand | Lightweight, no boilerplate |
| Realtime | Native WebSocket client | Direct to API |
| UI | Tailwind CSS + shadcn/ui | Consistent, accessible |
| Charts | Recharts | Sensor data visualization |
| 3D View | Three.js (optional) | Robot arm visualization |

---

## 5. Module Definitions

Each module is independently deployable and follows the same internal interface.

### Module Interface Contract

Every plugin module MUST implement:

```python
class ArborPlugin:
    name: str              # unique, slug format
    version: str           # semver
    capabilities: list[str]  # e.g. ["servo", "pwm_output"]
    config_schema: dict    # Pydantic model schema

    async def initialize(self, config: dict) -> None: ...
    async def shutdown(self) -> None: ...
    async def get_state(self) -> dict: ...
    async def handle_command(self, command: str, params: dict) -> dict: ...
    async def health_check(self) -> HealthStatus: ...
```

### Core Modules (Phase 1)

| Module | Capabilities | Runs On |
|--------|-------------|---------|
| `servo-bus` | Serial bus servo read/write, multi-servo sync | ESP32, Pi |
| `servo-pwm` | PWM servo position, speed control | ESP32, Pi (via PCA9685) |
| `stepper` | Step/dir, Klipper bridge, homing | Pi, Jetson |
| `led-addressable` | WS2812b and all WLED-supported types | ESP32, Pi |
| `led-pwm` | Dimmable LED channels | ESP32, Pi |
| `fan-pwm` | PWM fan control with tachometer | ESP32, Pi |
| `sensor-temp` | Temperature sensor polling | ESP32, Pi |
| `sensor-endstop` | Endstop state, interrupt-driven | ESP32, Pi |
| `sensor-button` | Button events, debounce | ESP32, Pi |
| `sensor-distance` | TOF, sonar, LIDAR | ESP32, Pi |
| `audio` | Playback, capture, streaming | Pi, Jetson, ESP32-S3 |
| `camera` | Frame capture, stream, inference trigger | Pi, Jetson |

### Extension Modules (Phase 2+)

| Module | Capabilities |
|--------|-------------|
| `sensor-radar` | mmWave presence detection |
| `sensor-vision` | Object detection, pose estimation |
| `ros2-bridge` | Bidirectional ROS2 topic/service bridge |
| `klipper-bridge` | Full Klipper/Moonraker integration |
| `meshtastic` | Mesh networking I/O |
| `voice-pipeline` | STT → LLM → TTS pipeline |

---

## 6. API Design

### Principle: API Parity

The ESP32 node API and the Control Server API are **identical in structure**. The Control Server transparently proxies to nodes. This means an API consumer can target either layer with the same code.

Features not supported by a given platform are absent from that platform's OpenAPI spec — they are not error responses.

### Base URL Structure

```
# Node-level (ESP32 direct)
https://{node_ip}/api/v1/

# Control Server
https://{control_host}/api/v1/
https://{control_host}/api/v1/nodes/{node_id}/  # proxy to node
```

### Authentication

```
# API Key (machine-to-machine, AI agents)
Authorization: Bearer {api_key}

# JWT (WebUI sessions after login)
Authorization: Bearer {jwt_token}

# API Key scopes: read, write, admin, stream
```

### Core Endpoints

```yaml
# System
GET  /api/v1/system/info          # platform, firmware version, capabilities
GET  /api/v1/system/health        # health status of all modules
GET  /api/v1/system/config        # current config (no secrets)
PUT  /api/v1/system/config        # update config (admin scope)
POST /api/v1/system/restart       # restart node (admin scope)
GET  /api/v1/system/ota/status    # OTA update status
POST /api/v1/system/ota/trigger   # trigger OTA check (admin scope)

# Modules
GET  /api/v1/modules              # list loaded modules + status
GET  /api/v1/modules/{id}         # module info + current state
POST /api/v1/modules/{id}/reload  # hot reload module config (admin)

# Servos (serial bus)
GET  /api/v1/servo/{id}/state           # position, speed, load, temp, voltage
PUT  /api/v1/servo/{id}/position        # set target position
PUT  /api/v1/servo/{id}/speed           # set max speed
PUT  /api/v1/servo/{id}/torque          # enable/disable torque
POST /api/v1/servo/sync                 # multi-servo synchronized move
GET  /api/v1/servo/scan                 # scan bus for servo IDs

# Servos (PWM)
GET  /api/v1/pwm-servo/{channel}/state
PUT  /api/v1/pwm-servo/{channel}/position

# Steppers
GET  /api/v1/stepper/{id}/state
POST /api/v1/stepper/{id}/home
POST /api/v1/stepper/{id}/move          # { position, speed, acceleration }
POST /api/v1/stepper/{id}/stop
GET  /api/v1/stepper/{id}/position

# LEDs
GET  /api/v1/led/{id}/state
PUT  /api/v1/led/{id}/color             # { r, g, b, w, brightness }
PUT  /api/v1/led/{id}/effect            # { effect, speed, palette, ... }
PUT  /api/v1/led/{id}/segment/{seg}     # per-segment control
POST /api/v1/led/{id}/off

# PWM outputs (fans, non-addressable LEDs)
GET  /api/v1/pwm/{channel}/state
PUT  /api/v1/pwm/{channel}/duty         # 0-100%

# Sensors
GET  /api/v1/sensor/{id}/reading        # latest reading
GET  /api/v1/sensor/{id}/history        # time-series (last N readings)
GET  /api/v1/sensors                    # all sensor readings

# Digital I/O
GET  /api/v1/gpio/{pin}/state
PUT  /api/v1/gpio/{pin}/state           # set output state

# Audio
GET  /api/v1/audio/state
POST /api/v1/audio/play                 # { url_or_path, volume }
POST /api/v1/audio/stop
PUT  /api/v1/audio/volume               # { level: 0-100 }
GET  /api/v1/audio/devices
POST /api/v1/audio/capture/start
POST /api/v1/audio/capture/stop

# Events / Streaming
GET  /api/v1/events                     # SSE stream of all events
WS   /api/v1/ws                         # WebSocket — subscribe/publish

# Auth (Control Server only)
POST /api/v1/auth/login
POST /api/v1/auth/logout
POST /api/v1/auth/api-keys              # create API key
GET  /api/v1/auth/api-keys              # list keys (admin)
DELETE /api/v1/auth/api-keys/{id}       # revoke key

# Nodes (Control Server only)
GET  /api/v1/nodes                      # list registered nodes
POST /api/v1/nodes                      # register node
GET  /api/v1/nodes/{id}                 # node detail + status
DELETE /api/v1/nodes/{id}              # unregister node
```

### WebSocket Protocol

```json
// Subscribe to events
{"action": "subscribe", "topics": ["servo.*", "sensor.*", "system.health"]}

// Command
{"action": "command", "module": "servo", "id": "1", "cmd": "set_position", "params": {"position": 512}}

// Event from server
{"event": "servo.position_changed", "id": "1", "data": {"position": 512, "timestamp": "..."}}
```

---

## 7. Security Architecture

### Threat Model

| Threat | Mitigation |
|--------|-----------|
| Unauthorized API access | API key + JWT auth on all endpoints |
| Key compromise | Scoped keys, instant revocation, rotation policy |
| Network sniffing | TLS 1.2+ required everywhere, self-signed CA for local |
| Command injection via API | Input validation (Pydantic), param whitelisting |
| Physical hardware abuse | Per-command rate limiting, position/speed limits in config |
| OTA firmware tampering | Signed firmware bundles, checksum verification |
| Enumeration attacks | 404 for unauthorized resources (not 403) |
| Denial of service | Per-key rate limiting, connection limits |
| Config tampering | Config changes require admin scope + audit log entry |
| WebUI XSS | React JSX escaping + strict CSP header |

### API Key System

- Keys stored hashed (Argon2id) — original never stored
- Scopes: `read`, `write`, `admin`, `stream`
- Per-key rate limits configurable
- Per-key IP allowlist (optional)
- Full audit log: who did what, when, from where

### TLS on ESP32

- Self-signed CA generated on first boot of control server
- Nodes receive CA cert + node cert on provisioning
- Client cert auth optional (for highest-security deployments)
- Certificate rotation handled via OTA + config push

### Rate Limiting

All endpoints:
- Global: configurable requests/minute per API key
- Per servo/motor command: configurable commands/second
- Burst allowance: short burst permitted, then throttled
- 429 Too Many Requests with Retry-After header

---

## 8. WebUI Specification

### Pages / Views

| View | Description |
|------|-------------|
| Dashboard | Live overview — all node health, active alerts, recent events |
| Servo Control | Per-servo position sliders, sync move, bus scan |
| Motion Control | Stepper jog, homing, position display |
| LED Control | Per-strip color picker, effect selector, segment mapping |
| Sensors | Live gauges and history charts |
| I/O Monitor | GPIO states, endstop status, button events |
| Audio | Playback controls, volume, device selector |
| Nodes | Node registry, health, connect/disconnect |
| Config | YAML config editor with schema validation |
| API Keys | Create, list, revoke keys |
| Logs | Live log stream with filter |
| API Docs | Embedded OpenAPI/Swagger UI |

### Real-Time Updates

- WebSocket connection maintained — auto-reconnect
- Optimistic UI updates with server confirmation
- Live sensor data streams at configurable rate
- Connection status indicator always visible

### Safety Features

- **Command confirmation dialog** for potentially destructive operations (home, restart, OTA)
- **Hardware limits display** — shows configured position/speed limits
- **Emergency stop button** visible on all pages — sends stop-all command
- **Torque disable** one-click on servo panel

---

## 9. Configuration System

### Principles

- **Zero hardcoded values** — every tunable is in config
- **Hierarchical** — global defaults → node defaults → module config
- **Validated** — Pydantic schema validation on load and on PUT
- **Watched** — file watcher triggers hot-reload where possible
- **Versioned** — config version field, migration support

### Config File Structure

```yaml
# arbor.yaml — root config

version: "1.0"

server:
  host: "0.0.0.0"
  port: 8443
  tls:
    enabled: true
    cert_path: "/etc/arbor/tls/cert.pem"
    key_path: "/etc/arbor/tls/key.pem"
  
security:
  api_key_min_length: 32
  jwt_algorithm: "RS256"
  jwt_expiry_minutes: 60
  rate_limit_default_rpm: 300
  
nodes:
  - id: "arm-node-01"
    type: "esp32-s3"
    transport: "uart"
    transport_config:
      port: "/dev/ttyUSB0"
      baud: 921600
    tls:
      client_cert: "/etc/arbor/nodes/arm-node-01.pem"
    modules:
      - type: "servo-bus"
        config:
          protocol: "feetech-scs"
          baud: 1000000
          servos:
            - id: 1
              name: "shoulder-yaw"
              min_position: 0
              max_position: 4095
              max_speed: 2000
            - id: 2
              name: "shoulder-pitch"
              min_position: 100
              max_position: 3900
              max_speed: 1500

logging:
  level: "INFO"
  format: "json"
  output: "/var/log/arbor/control.log"

plugins:
  search_paths:
    - "/etc/arbor/plugins"
    - "/usr/local/lib/arbor/plugins"
```

### ESP32 Config (NVS + provisioning push)

The control server can push config updates to ESP32 nodes via the `/api/v1/system/config` endpoint. ESP32 stores all config in NVS. No defaults are compiled into firmware — all values must be provisioned.

---

## 10. AI/Autonomy Integration

### Design Intent

Arbor is designed to be the physical actuation layer for AI agents. Any AI system with a valid API key can:

- Read all sensor data
- Command any servo, motor, LED, audio output
- Subscribe to the event stream
- Request state of any component

### GLaDOS Integration Points

- **API Key**: GLaDOS holds a `write`-scoped API key
- **Event Stream**: GLaDOS subscribes to WebSocket for reactive control
- **Vision Integration**: Camera module → inference → servo command pipeline
- **Audio**: GLaDOS sends audio play commands, receives audio capture stream
- **Meshtastic**: Optional — GLaDOS can receive/send mesh messages via the meshtastic module

### AI Safety Constraints

All enforced in config — not in firmware or AI code:

```yaml
ai_constraints:
  max_servo_speed_pct: 60       # AI limited to 60% of configured max
  require_confirmation_for:     # These commands pause for human OK
    - "stepper.home"
    - "system.restart"
  blacklisted_endpoints:        # AI key cannot access these
    - "/api/v1/auth/*"
    - "/api/v1/system/ota/*"
  command_rate_limit_rpm: 120   # Separate limit for AI key scope
```

### ROS2 Bridge (Optional)

When the `ros2-bridge` module is loaded:

- Each module publishes state to a ROS2 topic: `/arbor/{module}/{id}/state`
- Commands can be sent via ROS2 service: `/arbor/{module}/{id}/command`
- Full ROS2 ecosystem compatibility — Nav2, MoveIt2, custom nodes

---

## 11. Development Phases

### Phase 1 — Foundation (MVP)

**Goal**: Single ESP32 node with servo bus, fully functional API and WebUI.

- [ ] ESP32 firmware project skeleton (ESP-IDF 5.x, CMake)
- [ ] NVS config system on ESP32
- [ ] TLS HTTPS server on ESP32
- [ ] API key auth on ESP32
- [ ] `servo-bus` plugin (Feetech SCS protocol)
- [ ] `servo-pwm` plugin
- [ ] `sensor-temp` plugin (DS18B20)
- [ ] `sensor-endstop` plugin
- [ ] WebSocket server on ESP32
- [ ] Control Server (FastAPI) project skeleton
- [ ] Plugin manager on control server
- [ ] UART/USB ESP32 bridge on control server
- [ ] Config system (Pydantic + YAML)
- [ ] API key management (create, list, revoke)
- [ ] JWT auth for WebUI sessions
- [ ] WebUI skeleton (React + Vite + Tailwind)
- [ ] WebUI: Dashboard, Servo Control, Sensor views
- [ ] OpenAPI docs auto-generated and served

### Phase 2 — Expansion

- [ ] Stepper control + Klipper bridge
- [ ] `led-addressable` plugin (WS2812b full WLED palette)
- [ ] `led-pwm` plugin
- [ ] `fan-pwm` plugin
- [ ] `sensor-distance` (TOF + sonar)
- [ ] `sensor-button` plugin
- [ ] Audio playback plugin (Pi)
- [ ] Multi-node support in control server
- [ ] Node auto-discovery (mDNS)
- [ ] OTA firmware update pipeline
- [ ] Audit log with SQLite backend
- [ ] WebUI: LED control, I/O monitor, node management, logs

### Phase 3 — AI & Advanced

- [ ] AI gateway module (rate limit, blacklist, constraint enforcement)
- [ ] Camera module (capture, MJPEG stream)
- [ ] Vision inference plugin (YOLOv8 on Jetson)
- [ ] ROS2 bridge module
- [ ] Voice pipeline module (STT → LLM → TTS)
- [ ] Meshtastic module
- [ ] WebUI: API key management, config editor, API docs
- [ ] Full Jetson-specific optimizations
- [ ] Docker compose deployment stack
- [ ] Home Assistant MQTT discovery integration

---

## 12. Multi-Agent Development Rules

> These rules govern how AI agents (Claude Code instances) develop this project.

### The Quorum Model

Major decisions require a **quorum** of at least 2 agent perspectives before proceeding, AND explicit human approval from the project owner.

**What constitutes a major decision:**

1. Adding or removing a technology from the stack
2. Changing the API structure or endpoint paths
3. Adding a new module type not defined in this plan
4. Changing the security architecture
5. Changing the config schema structure
6. Adding new external dependencies (libraries, services)
7. Changing the plugin interface contract
8. Any change that would break backward compatibility

**What can proceed without quorum (minor decisions):**

- Bug fixes within existing module implementations
- Adding documentation or comments
- Refactoring that doesn't change external interfaces
- Adding tests
- Implementing Phase tasks already specified in this plan

### Quorum Process

```
Agent A identifies decision needed
         │
         ▼
Agent A writes DECISION_REQUEST_{N}.md in /decisions/ folder
(includes: problem, proposed solution, alternatives considered, impact)
         │
         ▼
Agent B reviews, adds its perspective to the same file
         │
         ▼
STOP — Human Review Required
Present DECISION_REQUEST_{N}.md to user for approval
         │
         ├── Approved → proceed, update this plan, archive decision
         └── Rejected → archive decision with rejection reason, do not proceed
```

### Hard Rules for All Agents

1. **NO deviation from this plan without human approval.** If implementation reveals the plan is wrong, file a decision request — do not improvise.
2. **No new external dependencies** without human approval. This includes npm packages, pip packages, and system-level tools.
3. **All hardcoded values are bugs.** If you write a hardcoded value, stop, add it to config, and document it.
4. **Security gates all PRs.** Run the VibeSec checklist against every file before marking a task complete.
5. **API parity must be maintained.** Any endpoint added to one layer must be assessed for the other layer.
6. **Config schema changes require migration path.** Never break existing configs silently.
7. **All code must have tests.** Unit test coverage minimum 80% for core modules.
8. **Document as you go.** Every public function, class, and API endpoint must have docstrings/descriptions.

### Development Without Human Input

Agents may proceed autonomously for **minor decisions only**. The following scenarios require stopping and waiting for human input, even if it delays development:

- Any quorum-class decision (see above)
- A security vulnerability discovered in the design
- A hardware constraint discovered that makes a feature impossible as specified
- A dependency license conflict

**Agents must NOT:**

- Make assumptions about hardware setup and hardcode them
- Skip security reviews to move faster
- Implement features not in this plan as "nice to haves"
- Merge breaking changes without a migration path
- Create API keys, secrets, or credentials in any committed file

---

## 13. Technology Decisions (Locked)

These decisions are finalized. Agents may not change them without a quorum + human approval.

| Decision | Chosen | Locked Reason |
|----------|--------|---------------|
| ESP32 framework | ESP-IDF 5.x (not Arduino) | Full hardware access required |
| Control server language | Python 3.11+ | Ecosystem, LLM tooling |
| Control server framework | FastAPI | Async, typed, OpenAPI native |
| Config format | YAML + Pydantic | Human readable + type validated |
| Auth method | API Keys (Argon2id hash) + JWT RS256 | Layered, revocable, industry standard |
| WebUI framework | React 18 + TypeScript | Type safety, component ecosystem |
| TLS | Required everywhere | Security non-negotiable |
| LED protocol | WLED-compatible superset | Proven driver library |
| Servo bus | Plugin-abstracted (not hardcoded protocol) | Protocol agnosticism required |
| Logging | Structured JSON | Machine-parseable for AI consumption |

---

## 14. Open Questions (Require Human Input)

These items need owner decision before implementation. Agents must not proceed with them.

| # | Question | Context |
|---|----------|---------|
| OQ-1 | Primary servo protocol for Phase 1? | **ANSWERED:Feetech STS protocol, 1Mbps half-duplex UART 
| OQ-2 | Klipper integration: Moonraker API (assumed) confirmed? | **ANSWERED: Moonraker API, custom Klipper WebUI with Mainsail if necessary for camera view and control of system
| OQ-11 | Which BTT board is the Phase 1 stepper test target? | **ANSWERED: SKR Pico, Pi 5 and Waveshare "Servo Driver with ESP32"
| OQ-12 | CAN bus (EBB boards) support priority? | **ANSWERED: low for now, add framework for Klipper and normal CANBUS protocols
| OQ-3 | Target node count for Phase 1 testing? | **ANSWERED: 1
| OQ-4 | mDNS vs static config for node discovery? | **ANSWERED: support both
| OQ-5 | Database backend preference? |**ANSWERED: Fastest of the two, whatever runs on Pi or Jetson
| OQ-6 | Home Assistant integration priority? | **ANSWERED: MQTT support, yes. Custom integration, add framework
| OQ-7 | Should the ESP32 nodes support provisioning via BLE (ESP Provisioning) or web-based only? | **ANSWERED: Not certain. Flash firmware to ESP node, get WebUI and AP Hotspot, connect, join to WiFi (and allow serial, UART, etc interface from other systems) whatever works
| OQ-8 | Audio capture: should it stream to control server or be processed locally on ESP32? | **ANSWERED: what is faster? Framework for both, we can decide later
| OQ-9 | Should GLaDOS API key be provisioned at install time or through a UI flow? |**ANSWERED: later UI Flow
| OQ-10 | Target deployment: Docker Compose or systemd services (or both)? | **ANSWERED: what works best? Native on the system would likely work best.

---

*Document Version: 1.1*
*Last Updated: 2026-03-11 — Added BTT Klipper-managed board support (SKR Pico, Manta M8P, Octopus), expanded stepper driver matrix, updated architecture diagram and software stack.*
*Owner: Chris Kliewer*
*Status: APPROVED — CANONICAL*
