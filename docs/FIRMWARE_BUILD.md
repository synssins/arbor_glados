# Arbor ESP Firmware Build Process

## Prerequisites

- **Docker** with the `espressif/idf:v5.3.2` image
- **Node.js** for building the WebUI
- WebUI must be built first and placed in `firmware/webui_files/`

Pull the Docker image if you don't have it:
```bash
docker pull espressif/idf:v5.3.2
```

## Version Management

The firmware version is defined in a single source of truth:
```
firmware/main/version.h
```

Bump `SB_VERSION_MAJOR`, `SB_VERSION_MINOR`, `SB_VERSION_PATCH` and the `SB_VERSION` string before each release. The CMake build system reads this file and injects the version into `esp_app_desc` (visible at runtime via `esp_app_get_description()`).

## Full Build (WebUI + Firmware)

```bash
# 1. Build WebUI
cd webui && npm run build && cd ..

# 2. Copy WebUI to firmware
cp -r control-server/arbor/webui/* firmware/webui_files/

# 3. Build firmware (Git Bash on Windows)
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$(pwd)/firmware:/project" \
  -w /project \
  espressif/idf:v5.3.2 \
  bash build.sh
```

> **Windows/MSYS note:** `MSYS_NO_PATHCONV=1` is required to prevent Git Bash from mangling the `-w /project` path.

## What the Build Does (build.sh)

1. **Extract version** from `version.h`
2. **Set target & compile** — `idf.py set-target esp32 && idf.py build`
3. **Generate SPIFFS image** — packs `webui_files/` into `build/spiffs.bin`
4. **Create OTA binary** — copies the app binary to `build/arbor-v{VERSION}-ota.bin`
5. **Create full-flash binary** — merges bootloader + partition table + OTA data + app + SPIFFS into `build/arbor-v{VERSION}.bin` via `esptool.py merge_bin`

## Build Outputs

| File | Size (typical) | Purpose |
|------|----------------|---------|
| `build/arbor-v{VERSION}.bin` | ~4 MB | **Full-flash** — flash a blank ESP32 in one command |
| `build/arbor-v{VERSION}-ota.bin` | ~1 MB | **OTA update** — upload via WebUI or API |

## Full Flash (Blank ESP32)

One command, one file:
```bash
esptool.py --chip esp32 --port COMx write_flash 0x0 build/arbor-v{VERSION}.bin
```

## OTA Update

Upload **`build/arbor-v{VERSION}-ota.bin`** through:
- The WebUI Settings page (OTA Firmware Update section)
- `POST /api/v1/system/ota/upload` with the binary as the request body

## Flash Addresses (from partitions.csv)

| Offset | Content |
|--------|---------|
| 0x1000 | Bootloader |
| 0x8000 | Partition table |
| 0x10000 | OTA data |
| 0x20000 | App (ota_0 partition) |
| 0x1E0000 | App (ota_1 partition) |
| 0x3A0000 | NVS keys |
| 0x3A1000 | SPIFFS |

## Settings Persistence

All settings are stored in NVS (non-volatile storage) and automatically backed up to `/spiffs/config_backup.json` on every save. If NVS is erased during a firmware update (rare, happens on ESP-IDF version changes), the backup is automatically restored on next boot.

Use the Backup & Restore feature in the WebUI Settings page or the API endpoints:
- `GET /api/v1/system/backup` — download full config as JSON
- `POST /api/v1/system/restore` — upload and apply config JSON

## Troubleshooting

- **Docker path error on Windows:** Make sure `MSYS_NO_PATHCONV=1` is set before the `docker run` command.
- **WebUI files missing:** Build the WebUI first (`cd webui && npm run build`) and copy output to `firmware/webui_files/`.
- **Image v5.3.2 vs v5.4:** The project uses `espressif/idf:v5.3.2`. Both work; v5.3.2 is tested and confirmed.
