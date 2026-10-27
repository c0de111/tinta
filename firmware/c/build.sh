#!/bin/bash
set -e

# -----------------------------------------------------------------------------
# build.sh – Full build pipeline for inki firmware (Pico W)
#
# This script compiles the firmware, trims unused flash sections,
# generates a firmware metadata header (CRC32, size, version, timestamp),
# and creates OTA-compatible and SWD-flashable images.
#
# Key features:
# - Uses CMake and make to build from source
# - Trims the final binary to exclude .config_flash (for OTA or minimal flashing)
# - Extracts Git version and build timestamp from generated version.c
# - Compiles and uses gen_firmware_header to prepend metadata to OTA binaries
# - Produces:
#     - esign.bin            → full firmware binary
#     - esign_trimmed.bin    → firmware only (no config)
#     - esign_trimmed.hex    → SWD-compatible hex format
#     - esign_header.bin     → 74-byte metadata header
#     - esign_OTA.bin        → header + trimmed binary (for OTA updates)
#
# -----------------------------------------------------------------------------
export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico/pico-sdk}"

# Enable or disable USB <-> Serial support for debugging in the bootloader (1 = enabled, 0 = disabled)
# Default configuration
USB_BOOTLOADER_ENABLE=0
USE_CASE="SEATSURFING"  # Default use case
INKI_DEBUG_USB_WAIT=OFF
INKI_DEBUG_TLS_CB=OFF
INKI_DEBUG_TIMING=OFF
INKI_EXP_WIFI_PMK=OFF
INKI_LOG_UPLOAD=OFF
INKI_NET_UPDATE=OFF
INKI_EXP_DHCP_REBOOT=OFF
BUILD_ST25_TUNE=OFF

# -----------------------------------------------------------------------------
# Work around pico-sdk 2.1.0 + GCC 15 host build failure (pioasm lacks <cstdint>)
# This patches the SDK headers in place, idempotently, so fresh clones/build dirs work.
patch_pioasm_headers() {
  local sdk="$PICO_SDK_PATH"
  local files=(
    "$sdk/tools/pioasm/pio_types.h"
    "$sdk/tools/pioasm/output_format.h"
  )
  for f in "${files[@]}"; do
    [[ -f "$f" ]] || continue
    if ! grep -q '<cstdint>' "$f"; then
      echo "Patching $f to include <cstdint> (fixes GCC 15 build of pioasm)..."
      # Insert after the last existing std/SDK include to keep ordering stable
      perl -0777 -i -pe 's/(#include[^\n]*\n)(?=(\n|struct))/\1#include <cstdint>\n/ if ! /<cstdint>/' "$f"
    fi
  done
}

patch_pioasm_headers

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --use-case|-u)
      USE_CASE="$2"
      shift 2
      ;;
    --historian)
      USE_CASE="HISTORIAN"
      shift
      ;;
    --homematic)
      USE_CASE="HOMEMATIC"
      shift
      ;;
    --seatsurfing)
      USE_CASE="SEATSURFING"
      shift
      ;;
    --weathermap)
      USE_CASE="WEATHERMAP"
      shift
      ;;
    --nextcloud)
      USE_CASE="NEXTCLOUD"
      shift
      ;;
    --new-usecase)
      USE_CASE="NEW_USECASE"
      shift
      ;;
    --st25-tune)
      BUILD_ST25_TUNE=ON
      shift
      ;;
    --bootloader-usb)
      USB_BOOTLOADER_ENABLE=1
      shift
      ;;
    --debug-usb-wait)
      INKI_DEBUG_USB_WAIT=ON
      shift
      ;;
    --debug-tls-cb)
      INKI_DEBUG_TLS_CB=ON
      shift
      ;;
    --debug-timing)
      INKI_DEBUG_TIMING=ON
      shift
      ;;
    --exp-wifi-pmk)
      INKI_EXP_WIFI_PMK=ON
      shift
      ;;
    --log-upload)
      INKI_LOG_UPLOAD=ON
      shift
      ;;
    --exp-net-update)
      INKI_NET_UPDATE=ON
      shift
      ;;
    --exp-dhcp-reboot)
      INKI_EXP_DHCP_REBOOT=ON
      shift
      ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo "Options:"
      echo "  --use-case|-u <CASE>   Set use case: SEATSURFING (default), HISTORIAN, HOMEMATIC, WEATHERMAP, NEXTCLOUD, NEW_USECASE"
      echo "  --historian            Build for Historian use case"
      echo "  --homematic            Build for Homematic use case"
      echo "  --seatsurfing          Build for SeatSurfing use case (default)"
      echo "  --weathermap           Build for Weathermap use case"
      echo "  --nextcloud            Build for Nextcloud use case (day planner)"
      echo "  --new-usecase          Build for new use case template"
      echo "  --st25-tune            Build ST25 NFC antenna tuning helper (standalone)"
      echo "  --debug-usb-wait       Enable startup USB wait helper (CMake: INKI_DEBUG_USB_WAIT=ON)"
      echo "  --debug-tls-cb         Enable TLS debug callback helper (CMake: INKI_DEBUG_TLS_CB=ON)"
      echo "  --debug-timing         Log DNS, TLS and HTTP timing per wake (INKI_DEBUG_TIMING=ON)"
      echo "  --exp-wifi-pmk         Experiment: precomputed WPA2 key for the join, see wifi.c (CMake: INKI_EXP_WIFI_PMK=ON)"
      echo "  --log-upload           Nextcloud: upload the log of each wake to the tracker host (CMake: INKI_LOG_UPLOAD=ON)"
      echo "  --exp-net-update       Experiment: buttons 3 + 4 fetch new firmware from tinta-fw.wake-electronics.com (CMake: INKI_NET_UPDATE=ON)"
      echo "  --exp-dhcp-reboot      Experiment: ask DHCP for the last address again, see wifi.c (CMake: INKI_EXP_DHCP_REBOOT=ON)"
      echo "  --help|-h              Show this help"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      echo "Use --help for usage information"
      exit 1
      ;;
  esac
done

# Validate use case
case $USE_CASE in
  SEATSURFING|HISTORIAN|HOMEMATIC|WEATHERMAP|NEXTCLOUD|NEW_USECASE)
    # Valid use case
    ;;
  *)
    echo "Error: Invalid use case '$USE_CASE'"
    echo "Valid options: SEATSURFING, HISTORIAN, HOMEMATIC, WEATHERMAP, NEXTCLOUD, NEW_USECASE"
    exit 1
    ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

VERSION_HEADER="$SCRIPT_DIR/version.h"
TEMPLATE_HEADER="$SCRIPT_DIR/version_template.h.in"

if [ ! -f "$VERSION_HEADER" ]; then
  echo "⚠️  Warning: '$VERSION_HEADER' not found."
  echo "Creating it from template: '$TEMPLATE_HEADER'"
  echo ""

  if [ -f "$TEMPLATE_HEADER" ]; then
    cp "$TEMPLATE_HEADER" "$VERSION_HEADER"
    echo "✅ '$VERSION_HEADER' created."
  else
    echo "❌ Error: '$TEMPLATE_HEADER' not found. Cannot proceed."
    exit 1
  fi
fi


# Print USB bootloader status in color
if [ "$USB_BOOTLOADER_ENABLE" -eq 1 ]; then
    echo -e "\033[1;31m[INFO] USB-UART Bootloader ENABLED -> careful, only for debugging! Jumping to the applications might not work.\033[0m"  # red
else
    echo -e "\033[1;32m[INFO] USB-UART Bootloader DISABLED\033[0m" # green
fi
echo "[INFO] Optional debug switches: --debug-usb-wait (INKI_DEBUG_USB_WAIT=${INKI_DEBUG_USB_WAIT}), --debug-tls-cb (INKI_DEBUG_TLS_CB=${INKI_DEBUG_TLS_CB}), --debug-timing (INKI_DEBUG_TIMING=${INKI_DEBUG_TIMING}), --exp-wifi-pmk (INKI_EXP_WIFI_PMK=${INKI_EXP_WIFI_PMK}), --log-upload (INKI_LOG_UPLOAD=${INKI_LOG_UPLOAD}), --exp-net-update (INKI_NET_UPDATE=${INKI_NET_UPDATE}), --exp-dhcp-reboot (INKI_EXP_DHCP_REBOOT=${INKI_EXP_DHCP_REBOOT})"
if [ "$BUILD_ST25_TUNE" = "ON" ]; then
    echo -e "\033[1;36m[INFO] ST25 tune helper: ENABLED\033[0m"
fi

# Define the build directory relative to the script's location
BUILD_DIR="$SCRIPT_DIR/build"

# Create the build directory if it doesn't exist
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

# Navigate to the build directory
cd "$BUILD_DIR" || exit 1

# Run CMake with USB option and use case passed as variables
echo "Building for use case: $USE_CASE"
cmake -DUSB_BOOTLOADER_ENABLE=${USB_BOOTLOADER_ENABLE} \
      -DUSE_CASE_DEFINE="USE_CASE_${USE_CASE}" \
      -DINKI_DEBUG_USB_WAIT=${INKI_DEBUG_USB_WAIT} \
      -DINKI_DEBUG_TLS_CB=${INKI_DEBUG_TLS_CB} \
      -DINKI_DEBUG_TIMING=${INKI_DEBUG_TIMING} \
      -DINKI_EXP_WIFI_PMK=${INKI_EXP_WIFI_PMK} \
      -DINKI_LOG_UPLOAD=${INKI_LOG_UPLOAD} \
      -DINKI_NET_UPDATE=${INKI_NET_UPDATE} \
      -DINKI_EXP_DHCP_REBOOT=${INKI_EXP_DHCP_REBOOT} \
      -DBUILD_ST25_TUNE=${BUILD_ST25_TUNE} \
      "$SCRIPT_DIR"

# Force a rebuild
make -j4

# Generate binary files from ELF files
arm-none-eabi-objcopy -O binary inki_bootloader.elf inki_bootloader.bin
arm-none-eabi-objcopy -O binary inki_slot0.elf inki_slot0.bin
arm-none-eabi-objcopy -O binary inki_slot1.elf inki_slot1.bin
arm-none-eabi-objcopy -O binary inki_default_config.elf inki_default_config.bin

# patch binaries
BUILD_DIR="$SCRIPT_DIR/build"
CRC32SUM="$SCRIPT_DIR/crc32sum"
BIN_SLOT0="$BUILD_DIR/inki_slot0.bin"
BIN_SLOT1="$BUILD_DIR/inki_slot1.bin"
BIN_UPLOAD_DIR="$BUILD_DIR/binaries"

patch_firmware() {
    local BIN="$1"
    local SLOT="$2"

    echo "→ Patching $BIN (slot $SLOT)"

    if [ ! -f "$BIN" ]; then
        echo "ERROR: File not found: $BIN"
        exit 1
    fi

    SIZE=$(stat -c %s "$BIN")
    CRC=$("$CRC32SUM" "$BIN")

    if [ -z "$CRC" ]; then
        echo "❌ ERROR: CRC32 calculation failed – check path to crc32sum!"
        exit 1
    fi

    printf "   Size = %d bytes\n" "$SIZE"
    printf "   CRC32 = 0x%08X\n" "$CRC"

    # Patch firmware_size at offset 62 (4 bytes)
    printf "%08x" "$SIZE" | tac -rs .. | xxd -r -p | \
        dd of="$BIN" bs=1 seek=62 count=4 conv=notrunc status=none

    # Patch crc32 at offset 67 (4 bytes)
    printf "%08x" "$CRC" | tac -rs .. | xxd -r -p | \
        dd of="$BIN" bs=1 seek=67 count=4 conv=notrunc status=none
}

patch_firmware "$BIN_SLOT0" 0
patch_firmware "$BIN_SLOT1" 1

echo "✅ Firmware headers patched successfully."

# One file for every update: both slot images behind "TINTAFW1"; the firmware page cuts out
# the one for the slot that is not running (web/web_firmware.c). The single slot images stay
# in build/ for flash.sh and create_uf2.sh.
mkdir -p "$BIN_UPLOAD_DIR"
rm -f "$BIN_UPLOAD_DIR"/inki_slot0.bin "$BIN_UPLOAD_DIR"/inki_slot1.bin
header_text() { dd if="$1" bs=1 skip="$2" count="$3" status=none | tr -d '\0'; }
FW_VERSION=$(header_text "$BIN_SLOT0" 30 32)
FW_USE_CASE=$(header_text "$BIN_SLOT0" 73 24 | tr '[:upper:]' '[:lower:]')
BIN_CONTAINER="$BIN_UPLOAD_DIR/tinta-$FW_USE_CASE-$FW_VERSION.tfw"
rm -f "$BIN_UPLOAD_DIR"/*.tfw
{ printf 'TINTAFW1'; cat "$BIN_SLOT0" "$BIN_SLOT1"; } > "$BIN_CONTAINER"

echo "✅ Update file (either slot): $BIN_CONTAINER"

# ST25 tune helper binary
if [ "$BUILD_ST25_TUNE" = "ON" ] && [ -f "$BUILD_DIR/inki_st25_tune.elf" ]; then
    arm-none-eabi-objcopy -O binary "$BUILD_DIR/inki_st25_tune.elf" "$BUILD_DIR/inki_st25_tune.bin"
    echo "✅ ST25 tune binary: $BUILD_DIR/inki_st25_tune.bin ($(stat -c %s "$BUILD_DIR/inki_st25_tune.bin") bytes)"
fi
