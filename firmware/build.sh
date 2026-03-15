#!/bin/bash
# Arbor ESP firmware build script — runs inside espressif/idf Docker container
# Usage: docker run --rm -v /path/to/firmware:/project -w /project espressif/idf:v5.3.2 bash build.sh
set -e

# Extract version from version.h (single source of truth)
VERSION=$(grep '#define SB_VERSION ' main/version.h | sed 's/.*"\(.*\)".*/\1/')
if [ -z "$VERSION" ]; then
    echo "ERROR: Could not extract version from main/version.h"
    exit 1
fi

echo "=== Arbor ESP Firmware Build v${VERSION} ==="
echo "IDF_PATH=$IDF_PATH"
echo "Target: ESP32"

# Clean previous build artifacts (optional, comment out for incremental)
# rm -rf build

# Step 1: Build firmware
echo ""
echo "=== Step 1: Building firmware ==="
idf.py set-target esp32
idf.py build

# Step 2: Generate SPIFFS image from WebUI files
echo ""
echo "=== Step 2: Generating SPIFFS image ==="

WEBUI_DIR="/project/webui_files"
SPIFFS_SIZE=$((0x5F000))  # Must match partitions.csv spiffs size
SPIFFS_IMG="build/spiffs.bin"
WEBUI_GZ_DIR="build/webui_gz"

if [ ! -d "$WEBUI_DIR" ]; then
    echo "ERROR: WebUI files not found at $WEBUI_DIR"
    echo "Mount the built WebUI directory as /project/webui_files"
    exit 1
fi

echo "Original WebUI files:"
find "$WEBUI_DIR" -type f -exec ls -la {} \;

# Pre-compress WebUI files with gzip for SPIFFS (saves ~70% space)
echo "Pre-compressing WebUI files..."
rm -rf "$WEBUI_GZ_DIR"
mkdir -p "$WEBUI_GZ_DIR"

# Copy directory structure and gzip all files
find "$WEBUI_DIR" -type d | while read dir; do
    rel="${dir#$WEBUI_DIR}"
    mkdir -p "$WEBUI_GZ_DIR$rel"
done
find "$WEBUI_DIR" -type f | while read file; do
    rel="${file#$WEBUI_DIR}"
    gzip -9 -c "$file" > "$WEBUI_GZ_DIR${rel}.gz"
done

echo "Compressed WebUI files:"
find "$WEBUI_GZ_DIR" -type f -exec ls -la {} \;

# Use spiffsgen.py from ESP-IDF (pack pre-compressed files)
python $IDF_PATH/components/spiffs/spiffsgen.py \
    $SPIFFS_SIZE \
    "$WEBUI_GZ_DIR" \
    "$SPIFFS_IMG" \
    --page-size 256 \
    --block-size 4096

echo "SPIFFS image created: $(ls -la $SPIFFS_IMG)"

# Step 3: Create release artifacts
echo ""
echo "=== Step 3: Creating release artifacts ==="

OTA_BIN="build/arbor-esp-v${VERSION}-ota.bin"
FULL_BIN="build/arbor-esp-v${VERSION}.bin"

# OTA binary (combined SBFW format: app + SPIFFS WebUI in one file)
APP_BIN="build/arbor-esp.bin"
APP_SIZE=$(stat -c%s "$APP_BIN")
SPIFFS_SIZE_BYTES=$(stat -c%s "$SPIFFS_IMG")

echo "Creating combined OTA binary (SBFW format)..."
echo "  App size:    $APP_SIZE bytes"
echo "  SPIFFS size: $SPIFFS_SIZE_BYTES bytes"

# Write 16-byte SBFW header: magic(4) + app_size(4) + spiffs_size(4) + reserved(4)
python3 -c "
import struct, sys
header = struct.pack('<4sIII', b'SBFW', $APP_SIZE, $SPIFFS_SIZE_BYTES, 0)
sys.stdout.buffer.write(header)
" > "$OTA_BIN"

# Append app binary + SPIFFS image
cat "$APP_BIN" >> "$OTA_BIN"
cat "$SPIFFS_IMG" >> "$OTA_BIN"

echo "  OTA binary:  $(stat -c%s "$OTA_BIN") bytes"

# Full-flash binary (bootloader + partition table + OTA data + app + SPIFFS — for blank ESP32)
esptool.py --chip esp32 merge_bin \
    --output "$FULL_BIN" \
    --flash_mode dio \
    --flash_freq 40m \
    --flash_size 4MB \
    0x1000  build/bootloader/bootloader.bin \
    0x8000  build/partition_table/partition-table.bin \
    0x10000 build/ota_data_initial.bin \
    0x20000 build/arbor-esp.bin \
    0x3A1000 build/spiffs.bin

echo ""
echo "=== Build Complete — v${VERSION} ==="
echo ""
echo "Release artifacts:"
ls -la "$FULL_BIN" "$OTA_BIN"
echo ""
echo "Full flash (blank ESP32):"
echo "  esptool.py --chip esp32 --port COMx write_flash 0x0 ${FULL_BIN}"
echo ""
echo "OTA update:"
echo "  Upload ${OTA_BIN} via WebUI or POST /api/v1/system/ota/upload"
