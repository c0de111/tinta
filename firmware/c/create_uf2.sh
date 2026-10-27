#!/bin/bash
# -----------------------------------------------------------------------------
# create_uf2.sh – Create UF2 file for Tinta firmware USB flashing
#
# Creates a combined UF2 file containing bootloader, slot0 firmware, and
# default configuration for easy USB flashing without programmer/debugger.
# Users can simply drag-and-drop the generated UF2 file onto a Pico W in
# BOOTSEL mode.
#
# Memory Layout:
# - Bootloader: 0x10000000 (64KB)
# - Slot0:      0x10010000 (940KB)
# - Config:     0x101E7000 (12KB)
#
# Usage: ./create_uf2.sh
# Requires: ./build.sh to be run first to generate binaries
# -----------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
UF2_TOOL="$SCRIPT_DIR/uf2conv.py"

# Derive use-case suffix from CMake cache if available
USE_CASE_LINE=$(sed -n 's/^USE_CASE_DEFINE[^=]*=\(USE_CASE_.*\)$/\1/p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -n1)
CASE_SUFFIX="complete"
case "$USE_CASE_LINE" in
  USE_CASE_HISTORIAN)   CASE_SUFFIX="historian" ;;
  USE_CASE_SEATSURFING) CASE_SUFFIX="seatsurfing" ;;
  USE_CASE_HOMEMATIC)   CASE_SUFFIX="homematic" ;;
  USE_CASE_WEATHERMAP)  CASE_SUFFIX="weathermap" ;;
  USE_CASE_NEXTCLOUD)   CASE_SUFFIX="nextcloud" ;;
  USE_CASE_NEW_USECASE) CASE_SUFFIX="new_usecase" ;;
esac
OUTPUT_FILE="$BUILD_DIR/tinta_${CASE_SUFFIX}.uf2"

# Memory addresses from linker scripts
BOOTLOADER_ADDR=0x10000000

# Note: bootloader.bin already contains Boot2 stage at the beginning

echo "Creating UF2 file for Tinta firmware..."

# Download UF2 tools from Microsoft's repository if missing
if [ ! -f "$UF2_TOOL" ]; then
    echo "Downloading UF2 conversion tool from Microsoft's repository..."
    if ! curl -L -o "$UF2_TOOL" https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2conv.py; then
        echo "❌ Error: Failed to download UF2 conversion tool"
        exit 1
    fi
    chmod +x "$UF2_TOOL"
    echo "✅ Downloaded uf2conv.py"
fi

# Download families JSON file if missing
UF2_FAMILIES="$SCRIPT_DIR/uf2families.json"
if [ ! -f "$UF2_FAMILIES" ]; then
    echo "Downloading UF2 families database..."
    if ! curl -L -o "$UF2_FAMILIES" https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2families.json; then
        echo "❌ Error: Failed to download UF2 families database"
        exit 1
    fi
    echo "✅ Downloaded uf2families.json"
fi

# Ensure build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "❌ Error: Build directory not found. Run ./build.sh first."
    exit 1
fi

# Check that all required binaries exist
REQUIRED_FILES=("inki_bootloader.bin" "inki_slot0.bin" "inki_default_config.bin")
for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$BUILD_DIR/$file" ]; then
        echo "❌ Error: $BUILD_DIR/$file not found. Run ./build.sh first."
        exit 1
    fi
    SIZE=$(stat -c %s "$BUILD_DIR/$file")
    echo "   $file: $SIZE bytes"
done

echo ""
echo "Creating monolithic BIN image with all regions..."

# Patch slot0 binary to set valid_flag = 1 (same as flash.sh does)
# Note: Firmware is built with valid_flag = 0 by default for OTA safety.
# During OTA updates, the valid_flag is only set to 1 after successful CRC32 verification.
# This prevents corrupt/invalid firmware from breaking the system.
# For direct flashing, flash.sh and this script manually set valid_flag = 1 at offset 13.
echo "Patching slot0 binary to set valid_flag = 1..."
SLOT0_PATCHED="$BUILD_DIR/temp_slot0_patched.bin"
cp "$BUILD_DIR/inki_slot0.bin" "$SLOT0_PATCHED"
printf "\x01" | dd of="$SLOT0_PATCHED" bs=1 seek=13 count=1 conv=notrunc status=none

MONO_BIN="$BUILD_DIR/inki_complete.bin"

# Determine overall size: cover up to end of config blob
CONFIG_SIZE=$(stat -c %s "$BUILD_DIR/inki_default_config.bin")
MONO_SIZE=$((0x1E7000 + CONFIG_SIZE))

echo "Allocating monolithic BIN of $MONO_SIZE bytes filled with 0xFF..."
dd if=/dev/zero bs=1 count=0 seek=$MONO_SIZE of="$MONO_BIN" status=none
printf "\xFF" | dd of="$MONO_BIN" bs=1 count=$MONO_SIZE conv=notrunc status=none 2>/dev/null

echo "Placing bootloader at 0x000000..."
dd if="$BUILD_DIR/inki_bootloader.bin" of="$MONO_BIN" bs=1 seek=$((0x000000)) conv=notrunc status=none

echo "Placing slot0 (patched) at 0x010000..."
dd if="$SLOT0_PATCHED" of="$MONO_BIN" bs=1 seek=$((0x010000)) conv=notrunc status=none

echo "Placing default config at 0x1E7000..."
dd if="$BUILD_DIR/inki_default_config.bin" of="$MONO_BIN" bs=1 seek=$((0x1E7000)) conv=notrunc status=none

echo "Converting monolithic BIN to UF2 (Microsoft tool) with base 0x10000000..."
python3 "$UF2_TOOL" --base $BOOTLOADER_ADDR --family 0xe48bff56 --output "$OUTPUT_FILE" "$MONO_BIN" || { echo "❌ uf2conv.py failed"; exit 1; }

# Clean up temporary file
rm -f "$SLOT0_PATCHED" "$MONO_BIN"

UF2_SIZE=$(stat -c %s "$OUTPUT_FILE")
echo ""
echo "✅ Successfully created: $OUTPUT_FILE ($UF2_SIZE bytes)"
echo ""
echo "Usage Instructions:"
echo "1. Hold BOOTSEL button while connecting Pico W to USB"
echo "2. Pico W will appear as a USB mass storage device"
echo "3. Copy $OUTPUT_FILE to the USB drive"
echo "4. Pico W will write all regions and reboot"
