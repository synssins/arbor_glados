# CHANGELOG

All notable changes to Arbor are documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
Versioning: [Semantic Versioning](https://semver.org/)

---

## [0.3.1] — 2026-03-13

### Changed
- **Project renamed from ServoBoard to Arbor** — GLaDOS Peripheral Control System, by Aperture Robotics Division
  - Components: Arbor Core (Python daemon), Arbor ESP (firmware), Arbor UI (WebUI), Arbor Agent (AI layer)
  - Python package: `servoboard` → `arbor_core`
  - Config files: `servoboard.yaml` → `arbor.yaml`
  - Firmware artifacts: `servoboard-firmware` → `arbor-esp`
  - WiFi AP SSID: `ServoBoard-XXXX` → `Arbor-XXXX`
  - NVS namespace: `servoboard` → `arbor`
  - systemd service: `servoboard.service` → `arbor.service`
  - All class names: `ArborConfig` → `ArborConfig`, `ArborPlugin` → `ArborPlugin`
  - C code `sb_` prefix retained for binary compatibility

---

## [0.3.0] — 2026-03-13

### Added
- **BLDC motor driver plugin** (`bldc-driver`) — new component for controlling brushless DC motors via external drivers (BLD-510B as first target). Implements the sb_plugin_t interface with full command dispatch: `set_speed`, `set_direction`, `set_enable`, `brake`, `clear_fault`, `emergency_stop`, `tank_drive`.
- **GPIO+PWM sub-driver** (`bldc_gpio.c`) — direct digital control signals (EN, F/R, BK) + LEDC PWM speed output (SV). Configurable EN polarity for BLD-510B V2.0 vs V2.4. RPM measurement via PG pulse counting ISR. ALM alarm input monitoring.
- **Direction interlock safety FSM** (`bldc_safety.c`) — enforces mandatory stop-and-wait before direction reversal to protect motor drivers from back-EMF damage. States: STOPPED → RUNNING → BRAKING → REVERSING → STOPPED. Emergency stop transitions all motors to ESTOP in <100ms.
- **RS485 Modbus RTU sub-driver stub** (`bldc_modbus.c`) — placeholder returning ESP_ERR_NOT_SUPPORTED. Full implementation deferred to Phase B.
- **Multi-target plugin registration** — `main.c` conditionally registers servo/sensor plugins on ESP32-WROOM and BLDC plugin on all targets via `CONFIG_IDF_TARGET_ESP32C3` guards.
- **Tank drive command** — `tank_drive` command accepts `left_speed`/`right_speed` (negative = reverse) for differential drive with two motors.

---

## [0.2.7] — 2026-03-13

### Fixed
- **Register reads fail when servo has active alarm** — `read_registers` rejected any response with a non-zero ERROR byte, but the ERROR byte is the servo's alarm status bitmask (voltage, overload, overheating, etc.), not a read-failure flag. The data is still valid. Now only fails on actual protocol errors (bit 4: checksum, bit 6: instruction error). Alarm status is logged at DEBUG level.
- **Factory reset used hardcoded 7.4V-variant defaults** — the manual register-write approach wrote Max Voltage = 8.0V, which triggers overvoltage alarm on 12V-powered servos (Waveshare recommends 12V). Replaced with Feetech native RESET instruction (0x06), which lets the servo's own firmware restore the correct defaults for its specific variant. Servo ID is preserved across reset.

---

## [0.2.6] — 2026-03-12

### Fixed
- **Factory reset bus communication** — added `bus_recv` + `uart_flush_input` after every EEPROM write in the factory reset handler. Without consuming servo write responses, the half-duplex UART bus would desync, causing factory reset to silently fail. Now matches the proven write_reg handler pattern.

---

## [0.2.5] — 2026-03-12

### Fixed
- **Register read chunk size** — reduced from 50 to 25 bytes per read (3 chunks: 0–24, 25–49, 55–70). ST3215 servos fail to return 50+ bytes reliably. Now matches backup function which has always worked.
- **EEPROM Lock display** — Identity section showed `regs[48]` (Torque Limit) instead of `regs[55]` (Lock Mark).

### Added
- **Factory reset** — new "Reset to Factory Defaults" button in Backup/Restore section. Writes all EEPROM registers 6–39 to official ST3215 defaults (preserves servo ID). Firmware command, API endpoint (`POST /servo/{id}/factory-reset`), and WebUI button.

---

## [0.2.3] — 2026-03-12

### Fixed
- **Critical: EEPROM Lock register address** — was 48 (Torque Limit), corrected to 55 per ST3215 memory table v3.6. EEPROM writes now actually persist through power cycles.
- **Critical: Mode register address** — WebUI was reading/writing address 35 (Protection Time, default 200) instead of address 33 (Operating Mode). This caused "Mode 200" display and mode changes that didn't work.
- **Register read range** — expanded from 50 to 71 bytes to cover all registers through address 69 (Present Current).
- **Register name map** — corrected all register names/addresses to match official ST3215 datasheet.

### Added
- **Editable register table** — Raw Registers section now has inline edit fields for all writable registers, with EEPROM vs SRAM write handling.
- **Register help text** — every register shows description, min/max range, unit, default value, and storage type (EPROM/SRAM) from official ST3215 memory table.
- **`STS_REG_MODE` and `STS_REG_TORQUE_LIMIT`** — added missing register defines to firmware.

---

## [0.2.0] — 2026-03-12

### Added
- **Firmware versioning system** — `version.h` as single source of truth, injected into `esp_app_desc` via CMake `PROJECT_VER`
- **OLED version display** — firmware version shown on Row 3 of OLED (e.g. `v0.2.0`)
- **WebUI version display** — firmware version shown in navbar to the right of the last menu item
- **Config backup/restore API**
  - `GET /api/v1/system/backup` — download full config as JSON (admin, includes all fields)
  - `POST /api/v1/system/restore` — upload JSON config and apply (admin)
- **File manager API** — upload, download, list, delete config/data files on SPIFFS
  - `GET /api/v1/system/files` — list files with sizes and storage stats
  - `GET /api/v1/system/files/{name}` — download a file
  - `POST /api/v1/system/files/{name}` — upload a file (max 16KB)
  - `DELETE /api/v1/system/files/{name}` — delete a file (WebUI files protected)
- **WebUI: Backup & Restore section** in Settings — download/restore config JSON files
- **WebUI: File Manager section** in Settings — browse, upload, download, delete config files on device SPIFFS
- **SPIFFS auto-backup** — config automatically backed up to `/spiffs/config_backup.json` on every save, restored on boot if NVS is erased

### Changed
- **Build system** — single release artifact (`arbor-esp-v{VERSION}.bin`) for both OTA and serial flash; removed merged full-flash binary
- **Build script** — extracts version from `version.h`, produces versioned filename
- **SPIFFS mounted early** in boot sequence for config backup availability; `webui_server` gracefully handles already-mounted state

### Fixed
- **NVS erase recovery** — if NVS is erased (version mismatch on OTA update), config is automatically restored from SPIFFS backup instead of falling back to defaults

## [Unreleased]

### Added
- **W01-W12: WebUI — React + Vite + TypeScript + Tailwind**
  - `webui/` — Full single-page application for Arbor control
  - **W01**: Vite + React 19 + TypeScript + Tailwind CSS scaffold with path aliases
  - **W02**: Typed API client (`src/api/client.ts`) — typed wrappers for all endpoints (system, servo, sensor, emergency, modules, auth) with Bearer token auth
  - **W03**: WebSocket client (`src/api/ws.ts`) — auto-reconnect with exponential backoff, subscribe/unsubscribe, glob pattern matching, command dispatch
  - **W04**: Zustand stores (`src/stores/`) — system (info, health, connection), servo (positions, scan, sync), sensor (temperature, endstops) with real-time event updates
  - **W05**: Layout (`src/components/Layout.tsx`) — sticky navbar with nav links, connection status indicator, emergency stop always visible
  - **W06**: Emergency stop button — red E-STOP button in navbar, shows elapsed time and result
  - **W07**: Login page — API key entry (primary) and username/password modes, stored in localStorage
  - **W08**: Dashboard — health status cards, heap stats, temperature, WebSocket status, plugin list with health indicators
  - **W09**: Servo Control — per-servo cards with position slider (0-4095), quick-position buttons, torque toggle, speed/load/temp display, bus scan, manual ID entry
  - **W10**: Sensors — temperature gauge with bar visualization and history chart, endstop panel with live triggered state
  - **W11**: API Docs — links to Swagger UI/ReDoc, endpoint reference for Node API
  - **W12**: Build pipeline — Vite build outputs to `control-server/arbor_core/webui/`, dev proxy to localhost:8000
  - Production build: 260KB JS + 18KB CSS (81KB + 4KB gzipped)
- **F17-F19: Firmware unit tests**
  - `firmware/main/test/test_app_config.c` (F17) — 9 tests: init, load/save roundtrip, JSON import/export, provisioned flag, pin defaults, servo config persistence
  - `firmware/main/test/test_api_auth.c` (F18) — 11 tests: scope bitmask, ADMIN implies all, rate limiting (under/over limit, per-key isolation), NULL handling
  - `firmware/main/test/test_servo_bus.c` (F19) — 10 tests: plugin struct validation, health before init, unknown command, param validation for all commands, plugin manager integration
- **F16: OpenAPI spec — ESP32 subset**
  - `firmware/openapi.yaml` — Hand-authored OpenAPI 3.0.3 spec covering all firmware endpoints
  - System (info, health, config, restart, OTA), Servo (state, position, speed, torque, sync, scan), Sensor (reading, history, all), Emergency stop
  - Security scheme: Bearer auth with SHA-256 hashed API keys
  - Documents scope requirements, rate limit exemptions, provisioning mode behavior
- **F15: OTA partition layout and status endpoint**
  - GET `/api/v1/system/ota` — running partition, next update partition, boot partition, app description, partition table summary
  - Added `app_update` and `esp_partition` to CMake REQUIRES
- **F14: Emergency stop endpoint**
  - `firmware/main/api_emergency.c` — POST `/api/v1/emergency-stop` iterates ALL plugins, sends `emergency_stop` command
  - Measures execution time against 100ms deadline, publishes event with elapsed_us
  - Exempt from rate limiting and authentication
- **F13: Sensor API endpoints**
  - `firmware/main/api_sensor.c` — GET `/sensors` (all), GET `/sensor/{id}/reading`, GET `/sensor/{id}/history` with limit param
  - Sensor ID convention: "temp-0" → sensor-temp plugin, "endstop-2" → sensor-endstop plugin
  - Wildcard URI handler for `/api/v1/sensor/*`
- **F12: Servo API endpoints**
  - `firmware/main/api_servo.c` — Full implementation: GET state, PUT position/speed/torque, POST sync, GET scan
  - Wildcard URI handler extracts servo ID and action from path
  - Auth scope enforcement (WRITE for mutations, READ for scan), dispatches to servo-bus plugin
- **F11: System API endpoints**
  - `firmware/main/api_system.c` — Full implementation with app description, aggregated health, config export/import, modules list
  - GET `/system/config` uses `sb_config_to_json()` for full config with redacted secrets
  - PUT `/system/config` validates JSON, saves to NVS via `sb_config_load_json()`
  - GET `/modules` lists all plugins with health status
- **F10: WebSocket server**
  - `firmware/main/ws_server.c` — Full subscribe/publish protocol over WebSocket at `/api/v1/ws`
  - JSON text frame protocol: subscribe, unsubscribe, command types; event and result responses
  - Client tracking (up to 4 concurrent), glob pattern matching for topic subscriptions
  - Event bus integration: subscribes to all events, forwards to matching WebSocket clients
  - Command dispatch: clients can send commands directly to plugins via WebSocket
- **F09: sensor-endstop plugin**
  - `firmware/components/sensor_endstop/sensor_endstop.c` — GPIO interrupt-driven endstop sensor
  - Configurable pins with pull-up, ISR on any edge, 5ms debounce
  - FreeRTOS task for ISR event processing + periodic backup polling
  - Publishes `sensor.endstop` events on state change
- **F08: sensor-temp plugin**
  - `firmware/components/sensor_temp/sensor_temp.c` — DS18B20 1-Wire temperature sensor
  - Bit-bang 1-Wire protocol: reset, read/write byte, CRC-8 verification
  - 2-second polling task, 64-entry circular history buffer
  - Publishes `sensor.temperature` events, supports `get_reading` and `get_history` commands
- **F07: servo-pwm plugin**
  - `firmware/components/servo_pwm/servo_pwm.c` — Standard PWM servo driver using ESP32 LEDC
  - 50Hz PWM at 14-bit resolution, configurable pulse width (500-2500us)
  - Position range 0-1000 (0.1% resolution), per-channel configuration from NVS pins
- **F06: servo-bus plugin — Feetech STS protocol**
  - `firmware/components/servo_bus/servo_bus.c` — Full Feetech STS/SCS protocol implementation
  - Half-duplex UART with configurable TX/RX/DIR pins, direction pin control for bus arbitration
  - Packet framing: 0xFF 0xFF header, ID, length, instruction, params, checksum
  - Read registers: position, speed, load, temperature, voltage, torque state
  - Write: goal position, moving speed, torque enable/disable
  - Sync write for multi-servo coordinated moves
  - Bus scan (ping IDs 0-253), emergency stop (torque disable all)
  - Health check pings first servo, publishes events on position changes
- **F05: Plugin registration and dispatch system**
  - `firmware/main/plugin_manager.c` — Full implementation with per-plugin state tracking, reverse-order shutdown, command dispatch, health aggregation, JSON listing
  - `sb_plugin_init_all()` continues on individual failures, `sb_plugin_dispatch()` validates initialized state before routing
- **F04: API key auth middleware**
  - `firmware/main/api_auth.c` — SHA-256 with per-key salt (ESP32 compromise vs Argon2id on control server)
  - NVS-backed key storage (up to 8 keys as blobs), Bearer token extraction, scope bitmask enforcement
  - Sliding-window rate limiting with 512-entry circular buffer per key, emergency stop exempt
  - Public paths (health) bypass auth; provisioning mode (no keys) allows all; returns 404 per SECURITY.md
- **F03: TLS HTTPS server**
  - `firmware/main/http_server.c` — `esp_https_server` with `httpd_ssl_start()`, automatic fallback to plain HTTP for provisioning/development
  - Configurable port via `sb_config_t`, proper cleanup with `httpd_ssl_stop()` vs `httpd_stop()`
- **F02: NVS config system**
  - `firmware/main/app_config.c` — Full NVS read/write for all config fields (server, security, logging, pins, servo bus)
  - `sb_config_load_json()` parses cJSON from control server push, validates, saves to NVS, updates running config
  - `sb_config_to_json()` exports as cJSON (secrets redacted), `sb_config_is_provisioned()` checks NVS flag
  - Minimal boot config when not provisioned (plain HTTP, all scopes) for initial control server handshake
- **F01: ESP-IDF 5.x project skeleton**
  - `firmware/CMakeLists.txt` — ESP-IDF 5.x CMake project targeting ESP32-WROOM-32
  - `firmware/sdkconfig.defaults` — Default config (flash, FreeRTOS, mbedTLS, HTTPD, WebSocket, UART)
  - `firmware/partitions.csv` — OTA-ready partition layout (dual app slots, NVS, NVS keys)
  - `firmware/main/` — 14 source files: main entry point, config system, pin validator, plugin manager, HTTP server, WebSocket server, event bus, JSON utils, auth middleware, system/servo/sensor/emergency API routes
  - `firmware/components/` — 4 plugin component skeletons: servo_bus (Feetech STS), servo_pwm, sensor_temp (DS18B20), sensor_endstop
  - All pins fully configurable — no hardcoded GPIO numbers; `sb_pin_config_t` and `pin_validator.c` enforce valid assignments
  - Plugin interface contract (`sb_plugin_t`) mirrors Python `ArborPlugin` ABC for API parity
  - Stubs compile-ready with TODO markers for each downstream task (F02-F14)
- **Pin configurability requirement**
  - Added `PinConfig` model to control server config (I2C, SPI, OLED, WS2812, 1-Wire, endstops, buttons, PWM)
  - Added `tx_pin`, `rx_pin`, `dir_pin` to `ServoBusModuleConfig`
  - Added `pins` field to `NodeConfig` — all pin assignments configurable via WebUI
- **C22: Integration tests — ESP32 bridge with mock node**
  - `tests/test_integration.py` — 12 end-to-end tests exercising full stack: HTTP endpoint → NodeProxy → NodeBridge → SerialTransport → MockSerialPort
  - Covers servo CRUD, sensor reads, emergency stop, event publishing, request sequencing
- **C17: OpenAPI spec auto-generated and served**
  - Verified auto-generated spec at `/api/openapi.json` includes all endpoints from C10-C15
  - Swagger UI at `/api/docs`, ReDoc at `/api/redoc`
  - `tests/test_openapi.py` — 12 tests verifying spec completeness, endpoint coverage, tag presence
- **C13: Emergency stop endpoint**
  - `arbor_core/api/v1/emergency_routes.py` — POST `/emergency-stop` broadcasts torque-disable to all connected nodes via proxy
  - Returns per-node results, 503 if no nodes stopped, exempt from rate limiting
  - 5 tests covering full/partial/no success, no proxy, broadcast verification
- **C12: Sensor API endpoints**
  - `arbor_core/api/v1/sensor_routes.py` — GET `/sensor/{id}/reading`, GET `/sensor/{id}/history` (with limit param), GET `/sensors` (all at once)
  - All endpoints proxy to nodes via NodeProxy, support `node_id` query param
  - 8 tests covering reads, history with limit, all-sensors, error handling
- **C11: Servo API endpoints**
  - `arbor_core/api/v1/servo_routes.py` — GET `/servo/{id}/state`, PUT `/servo/{id}/position`, PUT `/servo/{id}/speed`, PUT `/servo/{id}/torque`, POST `/servo/sync`, GET `/servo/scan`
  - All endpoints proxy to nodes via NodeProxy, support `node_id` query param (defaults to first node for Phase 1)
  - 10 tests covering all endpoints, error handling, node selection
- **C09: Proxy layer**
  - `arbor_core/bridges/proxy.py` — `NodeProxy` with `forward()`, `forward_to_all()`, `node_health()`, path normalization, event publishing on write operations
  - `_path_to_topic()` converts API paths to event bus topics
  - 18 tests covering forwarding, broadcasting, events, health, path normalization, topic generation
- **C18: systemd service file**
  - `deploy/systemd/arbor.service` — production service with security hardening (NoNewPrivileges, ProtectSystem, PrivateTmp), serial port access via dialout group, journald logging
- **C08: UART/USB bridge**
  - `arbor_core/bridges/transport.py` — Abstract `NodeTransport` interface for all transport types
  - `arbor_core/bridges/serial_transport.py` — `SerialTransport` with JSON-line protocol over serial, thread-safe async I/O, pluggable serial backend for testing
  - `arbor_core/bridges/node_bridge.py` — `NodeBridge` manager for multi-node connection lifecycle
  - 18 tests with mock serial port covering full request/response cycle
- **C14: WebSocket server**
  - `arbor_core/events.py` — `EventBus` with async topic-based pub/sub, glob pattern matching, queue overflow protection
  - `arbor_core/api/v1/ws_routes.py` — WebSocket endpoint with subscribe/command protocol, Bearer token auth via query param
  - 17 tests covering event delivery, patterns, overflow, subscriber management
- **C10: System API endpoints**
  - `arbor_core/api/v1/system_routes.py` — GET /system/info (platform, version, plugins), GET /system/health (aggregated plugin health), GET /system/config (secrets redacted), PUT /system/config (admin, validated), POST /system/restart (admin)
  - 12 tests covering all endpoints, scope enforcement, secret redaction
- **C15: Auth API endpoints**
  - `arbor_core/api/v1/auth_routes.py` — POST /auth/login (JWT issuance), POST /auth/logout, POST /auth/api-keys (create, admin), GET /auth/api-keys (list, admin), DELETE /auth/api-keys/{id} (revoke, admin)
  - All admin-only endpoints return 404 (not 403) per SECURITY.md
  - 8 tests covering CRUD, scope enforcement, unauthenticated access
- **C16: SQLite backend**
  - `arbor_core/db/engine.py` — Database class with schema migration system, WAL mode, version tracking
  - `arbor_core/db/repositories.py` — `APIKeyRepository` (CRUD for persistent key storage) and `AuditLogRepository` (append-only log with time/key filters)
  - 20 tests covering schema creation, key persistence, audit queries
- **C07: Auth middleware**
  - `arbor_core/auth/middleware.py` — `AuthMiddleware` with Bearer token extraction, API key + JWT verification, scope enforcement, sliding-window rate limiting, audit logging
  - `arbor_core/auth/headers.py` — `SecurityHeadersMiddleware` with all SECURITY.md required headers (HSTS, CSP, X-Frame-Options, etc.)
  - Emergency stop exempt from rate limiting, 404 on unauthorized (not 403)
  - 17 tests covering auth flows, scopes, rate limiting, headers, audit logging
- **C03: Config loader**
  - `arbor_core/config/loader.py` — YAML file loading, env var overrides (ARBOR__ prefix with double-underscore nesting), programmatic overrides, deep merge, Pydantic validation
  - Auto-discovery via ARBOR_CONFIG env var or default paths
  - 16 tests covering YAML loading, env overrides, error handling, discovery
- **C06: JWT RS256 auth**
  - `arbor_core/auth/jwt.py` — RSA keypair generation, token issuance (RS256 only, server-enforced), verification with required claims
  - `JWTManager` with `from_key_files()` factory, configurable expiry and issuer
  - Rejects HS256, expired, tampered, and wrong-issuer tokens
  - 16 tests covering keygen, issuance, all rejection paths
- **C05: API key management**
  - `arbor_core/auth/api_keys.py` — Argon2id hashing, create/verify/list/revoke lifecycle
  - `APIKeyManager` with scope enforcement (admin implies all), automatic rehash on parameter upgrade
  - Plaintext key returned exactly once at creation, never stored
  - 16 tests covering CRUD, scope checks, revocation
- **C04: Plugin manager**
  - `arbor_core/plugins/base.py` — `ArborPlugin` ABC with full interface contract from project plan Section 5
  - `arbor_core/plugins/manager.py` — `PluginManager` with entry_point and filesystem discovery, register, init, shutdown, health check aggregation
  - 15 tests covering registration, lifecycle, failure handling, health checks
- **C02: Pydantic config models**
  - `arbor_core/config/models.py` — Full config schema: `ArborConfig` root with `ServerConfig`, `SecurityConfig`, `LoggingConfig`, `NodeConfig`, `ServoConfig`, `PluginConfig`, `AIConstraintsConfig`, `DatabaseConfig`
  - Cross-field validation (servo min/max position), bound checks on all numeric fields
  - JWT algorithm locked to RS256 via Literal type
  - 18 tests covering defaults, validation, rejection, and YAML-like dict construction
- **DECISION_REQUEST_1**: Approved `argon2-cffi`, `PyJWT`, `cryptography` as core dependencies
- **C01: Control Server FastAPI skeleton**
  - `pyproject.toml` with core dependencies (FastAPI, Pydantic, structlog, uvicorn)
  - Package structure: `arbor_core/` with `api/`, `auth/`, `bridges/`, `config/`, `plugins/` subpackages
  - FastAPI application factory in `app.py` with lifespan management
  - API v1 router with `/health` endpoint (unauthenticated per SECURITY.md)
  - OpenAPI documentation at `/api/docs`, `/api/redoc`, `/api/openapi.json`
  - Test suite with pytest fixtures and 8 passing smoke tests
  - Development tooling: ruff, black, mypy, pytest-cov configured
- Project initialization
- `ARBOR_PROJECT_PLAN.md` — canonical specification
- `AGENTS.md` — multi-agent development governance
- `SECURITY.md` — security checklist
- `decisions/` folder with decision request template
- Project directory structure defined

### Changed (v1.0 → v1.1)
- Added **Klipper-Managed Motion Control Boards** section to hardware matrix — BigTreeTech SKR Pico (RP2040), Manta M8P (STM32G0B1), Octopus (STM32F446/H723), plus generic LPC/STM32/RP2040 boards
- Added full stepper driver matrix: TMC2209, TMC2226, TMC5160, TMC2240, A4988, DRV8825
- Updated architecture diagram to show BTT Klipper node as a distinct node type alongside ESP32 and Pi Hat nodes
- Added CAN bus to transport layer (for EBB toolhead boards)
- Expanded firmware section — Klipper-managed boards get their own firmware subsection clarifying Arbor does not write their firmware
- Updated Arbor Core software stack — Moonraker bridge entry expanded, added python-can as optional dep
- Added `klipper` node type to config schema and example config with full SKR Pico example
- Added OQ-11 (which BTT board is Phase 1 stepper target) and OQ-12 (CAN bus priority)
- Updated OQ-2 to reference specific boards in scope

---

*Add entries here as tasks are completed. Use the format above.*
*Group changes under: Added, Changed, Fixed, Removed, Security*
