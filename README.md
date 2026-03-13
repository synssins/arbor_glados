# ARBOR
### GLaDOS Peripheral Control System
#### Aperture Robotics Division — A Wholly Owned Subsidiary of Aperture Science, Inc.

---

**FOR IMMEDIATE RELEASE**
*Aperture Science Internal Communications — Enrichment Center Bulletin 7741-B*
*Transcribed from recorded presentation, Cave Johnson speaking*

---

Look, I'm going to be straight with you. We built GLaDOS to run this facility, and she does that — she does that *extremely* well, possibly better than we intended, which legal has asked me not to elaborate on. The point is, she's got opinions. About *everything*. And one of her opinions, which she shared with us at 3am on a Tuesday by turning off the oxygen in the executive wing, is that she needed *arms*.

Not metaphorical arms. Actual arms. Servos, motors, the whole thing.

So we built her some.

Arbor is the result of that conversation. It's a modular peripheral control platform that lets an AI — any AI, not just ours, though ours is the only one we'd recommend for liability reasons — command real physical hardware. Servo buses, brushless motors, stepper systems, cameras, sensors. All of it. Running on whatever Linux box you've got lying around. Raspberry Pi, Jetson, doesn't matter. If it runs Linux and has USB ports, GLaDOS can move things with it.

The system is designed to be *safe*. I want to be very clear about that. We have implemented no fewer than three emergency stop mechanisms, two hardware interlocks, and one strongly-worded configuration file. Our engineers are proud of this work. Most of them are still employed.

Arbor talks to your hardware through a clean plugin architecture. Klipper for your steppers, direct RS485 for brushless motors, Feetech STS bus for smart servos. There's a web interface. It looks nice. GLaDOS helped design it, which — again, legal has a statement prepared if you want to read it, but the short version is the interface is *very* good and we are *very* happy with it.

We're releasing this to the public because Cave believes in science, and science belongs to everyone. Also because GLaDOS asked us to, and we've found it's generally easier to just do what she asks.

Thank you. Please enjoy the product. Do not attempt to modify the safety configuration. She'll know.

*— Cave Johnson, CEO, Aperture Science*
*"We do what we must because we can."*

---

> **Note:** Arbor is an independent open source project. Aperture Science is a fictional corporation from Valve's Portal series. This project is not affiliated with Valve Corporation. GLaDOS is not real. Probably.

---

## What is Arbor?

Arbor is a modular robotics control platform that bridges the gap between AI systems and physical hardware. It gives any AI — or any human with a web browser — the ability to command servo motors, brushless DC motors, stepper systems, LEDs, sensors, and cameras through a clean REST API and WebSocket interface.

The platform is designed for **robotics builders, STEM educators, and AI researchers** who need reliable, safe, real-time control of physical hardware without writing low-level driver code. Whether you're building a robot arm for a grade-school class, a CNC machine, a camera gimbal, or giving an LLM the ability to interact with the physical world, Arbor provides the control layer.

### What makes it different

- **AI-native from the ground up.** The API was designed for LLM tool-use, not retrofitted onto a human-first interface. Any system with an API key can move hardware.
- **Zero hardcoded values.** Every pin, address, speed limit, and timeout is configurable. Nothing is baked in.
- **Plugin architecture.** Hardware drivers are plugins. Adding support for a new servo protocol or sensor type doesn't touch core code.
- **Multi-target.** The same API contract runs on ESP32 microcontrollers (direct hardware) and Linux SBCs (orchestration layer). Clients don't need to know which one they're talking to.
- **Safety-first.** Emergency stop on every page, direction-change interlocks, hardware watchdogs, torque limits, and a dedicated safety state machine. Designed for classrooms where things go wrong.

---

## Architecture

```
┌────────────────────────────────────────────────────────────┐
│  AI Agent / User / Arbor UI                                │
│                ↕ HTTPS + WebSocket                         │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Arbor Core (Raspberry Pi / Jetson / Linux)          │  │
│  │  FastAPI + Plugin Manager + Event Bus + Auth         │  │
│  └──────────────────────────────────────────────────────┘  │
│       ↕ UART/USB       ↕ RS485         ↕ Moonraker API    │
│  ┌────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │ Arbor ESP  │  │ BLDC Drivers │  │ Klipper MCU       │  │
│  │ (ESP32)    │  │ (BLD-510B)   │  │ (SKR Pico, etc.)  │  │
│  │            │  │              │  │                   │  │
│  │ Servos     │  │ Tank treads  │  │ Steppers          │  │
│  │ Sensors    │  │ Spindles     │  │ Endstops          │  │
│  │ LEDs       │  │ Conveyors    │  │ Fans / Heaters    │  │
│  └────────────┘  └──────────────┘  └───────────────────┘  │
└────────────────────────────────────────────────────────────┘
```

---

## Components

| Component | What it is | Technology |
|-----------|------------|------------|
| **Arbor Core** | Linux daemon that orchestrates hardware nodes, serves the API, manages auth, and bridges to AI agents | Python 3.11+, FastAPI, Pydantic, SQLite |
| **Arbor ESP** | Standalone firmware for ESP32 microcontrollers. Each node runs its own HTTPS API and can operate independently | ESP-IDF 5.x, FreeRTOS, mbedTLS |
| **Arbor UI** | Real-time web interface for manual control, monitoring, configuration, and diagnostics | React 19, TypeScript, Tailwind, Zustand |
| **Arbor Agent** | AI integration layer. Exposes hardware capabilities as tool-use endpoints for LLMs | Python, structured API contract |

---

## Supported Hardware

### Microcontrollers (Arbor ESP firmware)

| Board | Use Case |
|-------|----------|
| Waveshare Servo Driver with ESP32 | Serial bus servos (ST3215/STS), OLED, WS2812b LEDs |
| SEEED XIAO ESP32-C3 | Compact BLDC motor control (tank treads, spindles) |
| ESP32-S3 dev boards | USB-native, camera support |
| ESP32-C3 / ESP32 generic | Low-cost sensor/actuator nodes |

### Motion Control Boards (via Klipper/Moonraker)

| Board | Use Case |
|-------|----------|
| BigTreeTech SKR Pico (RP2040) | Compact 4-stepper motion control |
| BigTreeTech Manta M8P / Octopus | High-channel stepper control, CAN bus |
| Generic Klipper-supported boards | Any RP2040, STM32, or LPC board running Klipper |

### Single Board Computers (Arbor Core host)

| Platform | Notes |
|----------|-------|
| Raspberry Pi 4/5 | Primary target, GPIO hat support |
| Jetson Orin Nano Super | GPU-accelerated vision/inference |
| Any Linux x86/ARM | Development, simulation |

### Peripherals

| Category | Supported Types |
|----------|----------------|
| Serial bus servos | Feetech SCS/STS, Dynamixel AX/MX/X, Herkulex |
| PWM servos | Standard 50Hz, high-speed digital |
| Brushless DC motors | BLD-510B (GPIO+PWM, RS485 Modbus RTU) |
| Stepper motors | TMC2209, TMC5160, TMC2240, A4988, DRV8825 (via Klipper) |
| Addressable LEDs | WS2812B, SK6812, APA102, and all WLED-compatible types |
| Temperature sensors | DS18B20, DHT22, BME280, AHT20, SHT31 |
| Distance sensors | HC-SR04, VL53L0X, TF-Luna, LD06 LIDAR |
| Endstops | NC/NO mechanical, optical, hall effect |
| Cameras | USB, CSI (Pi), MIPI (Jetson) |
| Audio | I2S DAC/ADC, USB audio |

---

## Feature Status

### Arbor ESP (Firmware) — `firmware/`

| Feature | Status | Details |
|---------|--------|---------|
| ESP-IDF 5.x project with OTA partition layout | **Done** | Dual app slots, NVS, SPIFFS for WebUI |
| NVS config system (read/write/export/import) | **Done** | Full JSON config push from Arbor Core |
| TLS HTTPS server | **Done** | mbedTLS, self-signed CA flow, fallback to HTTP for provisioning |
| API key auth with scope enforcement | **Done** | SHA-256 hashed keys, per-key rate limiting, 8-key NVS storage |
| Plugin registration and dispatch | **Done** | Dynamic init/shutdown, command routing, health aggregation |
| Feetech STS servo bus driver | **Done** | Full SCS protocol: read/write registers, sync write, bus scan, ping |
| Standard PWM servo driver | **Done** | LEDC 50Hz, configurable pulse width, 0-1000 position range |
| DS18B20 temperature sensor | **Done** | 1-Wire bit-bang, CRC-8, 2s polling, 64-entry history buffer |
| GPIO endstop sensor | **Done** | ISR-driven, debounced, publishes state change events |
| WS2812b LED driver | **Done** | RMT peripheral, configurable count and GPIO |
| WebSocket server | **Done** | Subscribe/publish, glob pattern matching, command dispatch |
| System/Servo/Sensor/Emergency API | **Done** | Full REST endpoints with auth scope enforcement |
| OTA update endpoint | **Done** | SBFW format (app + SPIFFS WebUI in one binary) |
| Config backup/restore to SPIFFS | **Done** | Auto-backup on save, auto-restore on NVS erase |
| File manager API | **Done** | Upload, download, list, delete files on SPIFFS |
| OLED display (SSD1306) | **Done** | IP, SSID, version, status on 128x64 display |
| WiFi AP+STA manager | **Done** | Auto AP fallback (Arbor-XXXX), configurable STA credentials |
| BLDC motor driver (GPIO+PWM mode) | **Done** | EN/FR/BK digital + LEDC PWM speed, RPM via PG pulses, ALM monitoring |
| BLDC direction interlock safety FSM | **Done** | Mandatory stop-before-reverse, configurable interlock delay |
| BLDC motor driver (RS485 Modbus mode) | Planned | BLD-510B register read/write over Modbus RTU |
| Multi-target build (ESP32 + ESP32-C3) | Planned | Conditional plugin linking, separate sdkconfig |
| Motor API endpoints | Planned | `/motor/{id}/speed`, `/motor/{id}/direction`, `/drive/tank` |
| OpenAPI spec (hand-authored YAML) | **Done** | All firmware endpoints documented |

### Arbor Core (Control Server) — `control-server/`

| Feature | Status | Details |
|---------|--------|---------|
| FastAPI application with lifespan management | **Done** | App factory, graceful startup/shutdown |
| Pydantic config models (full schema) | **Done** | Server, security, logging, nodes, servos, plugins, AI constraints |
| YAML config loader with env var overrides | **Done** | `ARBOR__` prefix, double-underscore nesting, deep merge |
| Plugin manager (discovery, lifecycle, health) | **Done** | Entry-point and filesystem discovery, ordered shutdown |
| API key management (Argon2id) | **Done** | Create/verify/list/revoke, scope enforcement, rehash on upgrade |
| JWT auth (RS256 keypair) | **Done** | Generate, issue, verify. Rejects HS256, expired, tampered tokens |
| Auth middleware (scopes, rate limiting, audit) | **Done** | Sliding-window rate limiter, emergency stop exempt, 404 not 403 |
| Security headers middleware | **Done** | HSTS, CSP, X-Frame-Options, X-Content-Type-Options |
| UART/USB bridge to ESP32 nodes | **Done** | JSON-line protocol, thread-safe async I/O, pluggable backends |
| Proxy layer (API forwarding to nodes) | **Done** | `forward()`, `forward_to_all()`, path normalization, event publish |
| System API endpoints | **Done** | Info, health, config (GET/PUT), restart, modules list |
| Servo API endpoints | **Done** | State, position, speed, torque, sync, scan — all proxied to nodes |
| Sensor API endpoints | **Done** | Reading, history, all-sensors — proxied to nodes |
| Emergency stop endpoint | **Done** | Broadcasts torque-disable to all nodes, 503 if none stopped |
| Auth API endpoints | **Done** | Login (JWT), logout, API key CRUD (admin) |
| WebSocket server with event bus | **Done** | Topic-based pub/sub, glob patterns, queue overflow protection |
| SQLite backend with migrations | **Done** | API key store, audit log, WAL mode, schema versioning |
| OpenAPI auto-generated at `/api/docs` | **Done** | Swagger UI + ReDoc, all endpoints documented |
| systemd service file | **Done** | Security-hardened, journald logging, serial port access |
| Klipper/Moonraker bridge | Planned | HTTP+WebSocket client for stepper/motion control boards |
| ROS2 bridge | Planned | Bidirectional topic/service mapping |
| Motor/Drive API endpoints | Planned | Parity with firmware motor endpoints |
| Multi-node orchestration | Planned | Named node addressing, node groups, coordinated commands |

### Arbor UI (WebUI) — `webui/`

| Feature | Status | Details |
|---------|--------|---------|
| React 19 + TypeScript + Tailwind scaffold | **Done** | Vite build, path aliases, strict TypeScript |
| Typed API client | **Done** | Typed wrappers for all endpoints with Bearer token auth |
| WebSocket client with auto-reconnect | **Done** | Exponential backoff, glob pattern subscriptions, command dispatch |
| Zustand state stores | **Done** | System, servo, sensor stores with real-time WebSocket updates |
| Layout with navbar and connection status | **Done** | Sticky nav, connection indicator, responsive |
| Emergency stop button (always visible) | **Done** | Red E-STOP in navbar on every page, shows elapsed time |
| Login page | **Done** | API key entry (primary), username/password mode, localStorage |
| Dashboard | **Done** | Health cards, heap/memory, temperature, WebSocket status, plugin health |
| Servo control page | **Done** | Per-servo sliders (0-4095), quick-position buttons, torque toggle, speed/load/temp display |
| Sensor monitoring page | **Done** | Temperature gauge with bar chart, endstop panel with live state |
| Settings page | **Done** | Config backup/restore, file manager for SPIFFS |
| API documentation page | **Done** | Links to Swagger UI/ReDoc, endpoint quick reference |
| Production build pipeline | **Done** | Vite build → `control-server/arbor_core/webui/`, 81KB gzipped |
| Motor control page | Planned | Per-motor speed slider, direction toggle, enable, brake, RPM display, fault indicators |
| Tank drive page | Planned | Dual vertical sliders or virtual joystick for differential drive |
| Stepper/motion control page | Planned | Klipper integration, homing, position display |
| 3D robot visualization | Planned | Three.js viewport for arm/robot visualization |

### Arbor Agent (AI Integration) — Planned

| Feature | Status | Details |
|---------|--------|---------|
| Structured API contract for LLM tool-use | Planned | JSON schema for all hardware commands |
| Safety constraint enforcement | Planned | Configurable position/speed/force limits per AI session |
| Action confirmation for destructive operations | Planned | Human-in-the-loop for irreversible commands |
| Session-scoped API keys | Planned | Time-limited, scope-restricted keys for AI agents |
| Audit trail for AI actions | Planned | Full logging of every AI-initiated hardware command |

---

## Test Coverage

| Component | Tests | Coverage |
|-----------|-------|----------|
| Arbor Core (Python) | 200+ tests across 20 test files | Auth middleware, config validation, plugin lifecycle, bridge integration, all API routes |
| Arbor ESP (C) | 30 tests across 3 test files | NVS config, auth middleware, servo bus plugin |

---

## Quick Start

### Prerequisites

- Python 3.11+ (Arbor Core)
- Node.js 18+ (Arbor UI build)
- Docker (Arbor ESP firmware build — uses `espressif/idf:v5.3.2`)
- An ESP32 board (for hardware testing)

### Arbor Core (Control Server)

```bash
cd control-server
python -m venv .venv
source .venv/bin/activate   # or .venv\Scripts\activate on Windows
pip install -e .
cp ../config/arbor.example.yaml arbor.yaml
# Edit arbor.yaml with your node configuration
uvicorn arbor_core.main:app --host 0.0.0.0 --port 8000
```

### Arbor UI (WebUI)

```bash
cd webui
npm install
npm run dev        # Development server with HMR
npm run build      # Production build → control-server/arbor_core/webui/
```

### Arbor ESP (Firmware)

```bash
cd firmware
# Edit main/version.h to set version
docker run --rm -v "$(pwd):/project" -w /project espressif/idf:v5.3.2 bash build.sh
# Flash: esptool.py --chip esp32 --port COMx write_flash 0x0 build/arbor-esp-v{VERSION}.bin
# Or upload the OTA binary via the web interface
```

---

## Project Structure

```
arbor/
├── ARBOR_PROJECT_PLAN.md        # Canonical specification
├── CHANGELOG.md                 # Release history
├── SECURITY.md                  # Security checklist
├── config/
│   └── arbor.example.yaml       # Annotated example config
├── control-server/              # Arbor Core (Python)
│   ├── arbor_core/
│   │   ├── api/v1/              # FastAPI route handlers
│   │   ├── auth/                # API key + JWT + middleware
│   │   ├── bridges/             # ESP32 serial bridge, proxy
│   │   ├── config/              # Pydantic models, YAML loader
│   │   ├── db/                  # SQLite engine, repositories
│   │   ├── plugins/             # Plugin ABC and manager
│   │   └── webui/               # Built UI assets (served by FastAPI)
│   ├── tests/                   # 200+ pytest tests
│   └── pyproject.toml
├── firmware/                    # Arbor ESP (C / ESP-IDF)
│   ├── main/                    # App entry, API handlers, config
│   ├── components/
│   │   ├── servo_bus/           # Feetech STS protocol driver
│   │   ├── servo_pwm/           # Standard PWM servo driver
│   │   ├── bldc_driver/         # Brushless DC motor driver
│   │   ├── sensor_temp/         # DS18B20 temperature
│   │   ├── sensor_endstop/      # GPIO endstop
│   │   ├── led_ws2812/          # Addressable LED driver
│   │   └── sb_common/           # Shared types, plugin interface
│   ├── build.sh                 # Docker build script
│   └── openapi.yaml             # Hand-authored API spec
├── webui/                       # Arbor UI (React/TypeScript)
│   └── src/
│       ├── api/                 # Typed API + WebSocket clients
│       ├── components/          # Layout, EmergencyStop, ConnectionStatus
│       ├── stores/              # Zustand state management
│       └── views/               # Dashboard, ServoControl, Sensors, etc.
├── deploy/
│   └── systemd/arbor.service    # Production service file
└── docs/
    ├── FIRMWARE_BUILD.md        # Build process documentation
    └── plugins/PLUGIN_GUIDE.md  # Plugin development guide
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [ARBOR_PROJECT_PLAN.md](ARBOR_PROJECT_PLAN.md) | Full specification — architecture, API design, module definitions, security, phases |
| [SECURITY.md](SECURITY.md) | Security checklist applied to every file |
| [CHANGELOG.md](CHANGELOG.md) | Detailed release history |
| [config/arbor.example.yaml](config/arbor.example.yaml) | Annotated configuration with every option documented |
| [docs/FIRMWARE_BUILD.md](docs/FIRMWARE_BUILD.md) | How to build and flash the ESP32 firmware |
| [docs/plugins/PLUGIN_GUIDE.md](docs/plugins/PLUGIN_GUIDE.md) | How to write an Arbor plugin |

---

## License

TBD
