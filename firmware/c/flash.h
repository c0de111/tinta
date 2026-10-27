#pragma once
#include "config.h"
#include "device_config.h"
#include "wifi_config.h"
#include <stdbool.h>
#include <stdint.h>

// #include "pico/flash.h"
#include <stddef.h>

#define LOGO_MAGIC "LOGO"

#define FIRMWARE_MAGIC "inki_firmware"
#define FIRMWARE_MAGIC_LEN (sizeof(FIRMWARE_MAGIC) - 1)
_Static_assert(FIRMWARE_MAGIC_LEN == 13, "FIRMWARE_MAGIC_LEN mismatch");

/*
 * RP2040 Flash Memory Map (2 MB = 0x200000)
 * ┌──────────────┬──────────────────────┬────────────┬──────────────────────────────────────┐
 * │   Address    │       Region         │   Size     │              Description             │
 * ├──────────────┼──────────────────────┼────────────┼──────────────────────────────────────┤
 * │ 0x000000     │ Bootloader           │  64 KB     │ Custom bootloader                    │
 * │ 0x010000     │ Firmware Slot 0      │ 940 KB     │ FIRMWARE_FLASH_SIZE = 0xEB800        │
 * │ 0x0FB800     │ Firmware Slot 1      │ 940 KB     │ FIRMWARE_FLASH_SIZE = 0xEB800        │
 * │ 0x1E7000     │ Config & Reserved    │ 100 KB     │ Configuration, logos, OTA buffers    │
 * │ 0x200000     │ Flash End            │            │ End of 2 MB QSPI flash               │
 * └──────────────┴──────────────────────┴────────────┴──────────────────────────────────────┘
 *
 * Firmware slots start at XIP_BASE + offset:
 *   - Slot 0: 0x10000000 + 0x010000 = 0x10010000
 *   - Slot 1: 0x10000000 + 0x0FB800 = 0x100FB800
 *
 * Notes:
 * - Bootloader include 256-byte boot2 at the start of the binary.
 * - Firmware binaries include a 256-byte firmware header at the start of the binary.
 * - "Config & Reserved" includes all flash-persistent settings and data.
 */

#define BOOTLOADER_FLASH_OFFSET                                                                    \
    0x000000 // bootloader (size: 64 kb) starts with boot2 (256 byte), directly after that
             // vectortable custom bootloader
#define FIRMWARE_SLOT0_FLASH_OFFSET                                                                \
    0x000000 + 0x010000 // = 0x010000 (size: 940 kb) has to be identical in application1_memmap.ld
                        // and flash.sh
#define FIRMWARE_SLOT1_FLASH_OFFSET                                                                \
    0x000000 + 0x010000 + 0xEB800    // = 0x0FB800 (size: 940 kb) has to be identical in
                                     // application1_memmap.ld and flash.sh
#define CONFIG_FLASH_OFFSET 0x1E7000 // Start of reserved flash region (top 100 KB)
#define VECTOR_TABLE_OFFSET                                                                        \
    0x100 // first 256 Byte of respective firmware store firmware_header_t, than vector_table; no
          // boot2

// Configuration blocks (each 4 KB = 1 flash sector)
#define WIFI_CONFIG_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x0000) // 0x1E7000 - Wi-Fi credentials
#define wifi_config_flash (*(const wifi_config_t *)FLASH_PTR(WIFI_CONFIG_FLASH_OFFSET))

// Use case config block — all use cases share this flash offset; typed accessor
// is in each use case's own *_flash.h
#define UC_CONFIG_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x1000) // 0x1E8000

#define DEVICE_CONFIG_FLASH_OFFSET                                                                 \
    (CONFIG_FLASH_OFFSET + 0x2000) // 0x1E9000 - Device and UI configuration
#define device_config_flash (*(const device_config_t *)FLASH_PTR(DEVICE_CONFIG_FLASH_OFFSET))

// Last access point that worked, as a hint for the next join. A sector of its own, so that
// rewriting it can never cost the credentials; above the weathermap image (ends 0x1FD000).
#define WIFI_HINT_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x16000) // 0x1FD000

// WPA2 key derived from SSID and passphrase, so that it is computed once and not on every
// wake. Only in builds with INKI_EXP_WIFI_PMK.
#define WIFI_PMK_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x17000) // 0x1FE000

// Page that stays on the panel between scheduled wakes. The last sector of a 2 MB flash.
#define RESIDENT_PAGE_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x18000) // 0x1FF000

// Logo storage block (8 KB = 2 flash sectors)
#define LOGO_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x3000) // 0x1EA000 - Uploadable logo binary
#define LOGO_FLASH_SIZE                                                                            \
    0x2000 // 8192 bytes reserved for 1-bit bitmap logo, has to be in multiples of FLASH_SECTOR_SIZE

#ifndef FLASH_PAGE_SIZE
#define FLASH_PAGE_SIZE 256 // 0x100 = 256, entspricht (1u << 8)
#endif

#define FIRMWARE_FLASH_SIZE 0xEB800 // 962,560 bytes, 235 flash sectors

// Flash access
#define FLASH_PTR(offset) ((const uint8_t *)(XIP_BASE + (offset)))
// #define FLASH_SECTOR_SIZE          0x1000  // already defined in pico SDK

// ┌───────────────────────────────────────────────┐
// │          Typical usage for LOGO_FLASH         │
// └───────────────────────────────────────────────┘
// - Custom user-uploaded logo via web interface
// - Format: raw 1-bit image with 18-byte header
// - Fits up to 200x200 pixels (5000 B) safely

typedef struct __attribute__((
    packed)) { // packed so that header is not padded for correct use of bitmap flash addressing
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint32_t datalen;
    uint8_t reserved[6];
} logo_header_t;

typedef struct __attribute__((packed)) {
    char magic[13];         // "inki_firmware"
    uint8_t valid_flag;     // 1 = gültig
    char build_date[16];    // z. B. "2025-07-16 13:30"
    char git_version[32];   // "v1.0.6-60-gcddef-dirty"
    uint32_t firmware_size; // Byte-Anzahl der Firmwaredaten
    uint8_t slot;           // 0 = slot0, 1 = slot1, 255 = direct
    uint32_t crc32;         // CRC über Firmwaredaten

    // Metadata v1 for use-case-aware OTA handling
    uint8_t meta_version;   // 0 = legacy/no metadata, 1 = metadata v1
    uint8_t use_case_id;    // See USE_CASE_ID_* in config.h
    char use_case_name[24]; // NUL-terminated use-case label

    uint8_t reserved[159]; // Reserve for future extension (header remains 256 B)
} firmware_header_t;
_Static_assert(sizeof(firmware_header_t) == 256, "firmware_header_t must be 256 bytes");
_Static_assert(sizeof(USE_CASE_NAME) <= sizeof(((firmware_header_t *)0)->use_case_name),
               "USE_CASE_NAME too long for firmware_header_t.use_case_name");
_Static_assert(offsetof(firmware_header_t, firmware_size) == 62, "firmware_size offset changed");
_Static_assert(offsetof(firmware_header_t, crc32) == 67, "crc32 offset changed");
_Static_assert(offsetof(firmware_header_t, meta_version) == 71, "meta_version offset changed");

// =============================================================================
// OTA streaming API  (must be called from Thread mode, NOT from recv_cb)
// =============================================================================

typedef struct {
    bool valid;               // magic check passed
    bool crc_ok;              // CRC32 matches header
    bool slot_ok;             // slot field matches target slot
    uint32_t actual_crc;      // computed CRC (for error messages)
    firmware_header_t header; // copy of header read back from flash
} flash_ota_result_t;

// Bytes to erase for an image of image_size; 0 if it does not fit a slot.
size_t flash_ota_erase_length(size_t image_size);

// Initialise state; does NOT erase flash.
void flash_ota_begin(uint32_t slot_offset, size_t erase_length);

// Erase one sector.  Call repeatedly from the event loop; returns true when done.
bool flash_ota_erase_next_sector(void);

// Write exactly FLASH_PAGE_SIZE bytes.  Caller pads the last partial page with 0xFF.
void flash_ota_write_page(const uint8_t *data);

// Validate header + CRC, mark slot valid if OK.  Call after all pages written.
flash_ota_result_t flash_ota_finish(void);

// flash_mark_firmware_valid() is internal to flash.c (called by flash_ota_finish).

/* CRC32 utility — shared with use-case flash modules */
uint32_t calc_crc32(const void *data, size_t len);

/* Wi-Fi config */
bool save_wifi_config(const wifi_config_t *in);

/* Wi-Fi join hint: BSSID and channel of the last access point, valid for the stored SSID only */
// The access point of the last join and the address of its lease (ip all zero if unknown).
bool load_wifi_hint(uint8_t bssid[6], uint8_t *channel, uint8_t ip[4]);
void save_wifi_hint(const uint8_t bssid[6], uint8_t channel, const uint8_t ip[4]);
void clear_wifi_hint(void);

// Failed Wi-Fi joins in a row, kept with the hint (learned state; written only when it
// changes). A new hint after a successful join sets it back to 0, as does clear_wifi_hint().
#define WIFI_FAILURES_MAX 250
int load_wifi_failures(void);
void save_wifi_failures(int n);

/* Resident page: the page a scheduled wake redraws (see page_def_t.stays), kept across
 * power-off. load returns -1 if none is stored. save writes only if the page differs. */
int load_resident_page(void);
void save_resident_page(int page);

#if INKI_EXP_WIFI_PMK
/* Stored WPA2 key (PMK), valid only for the SSID and passphrase it was derived from */
typedef enum {
    WIFI_PMK_NONE,     // nothing stored for the current credentials
    WIFI_PMK_VALID,    // pmk filled in
    WIFI_PMK_REJECTED, // this network refused the key on several wakes: use the passphrase
} wifi_pmk_state_t;
// A strike is a wake in which the key was refused and the passphrase then worked. One alone
// proves nothing (access points also refuse a correct key now and then), so the key counts
// as rejected only after WIFI_PMK_MAX_STRIKES of them in a row.
#define WIFI_PMK_MAX_STRIKES 3
wifi_pmk_state_t load_wifi_pmk(uint8_t pmk[32], uint32_t *strikes);
void save_wifi_pmk(const uint8_t pmk[32], uint32_t strikes);
#endif

/* Device config */
bool save_device_config(const device_config_t *in);

bool save_uploaded_logo_to_flash(const uint8_t *data, size_t len);
void flash_erase_logo(void);
void flash_zero_settings_sector(void);
bool flash_demote_slot_header(uint32_t header_flash_offset);

typedef struct {
    char build_date[16];
    char git_version[32];
    uint32_t size;
    uint32_t crc32;
    uint8_t slot_index;
    uint8_t valid_flag;
} firmware_slot_info_t;

const char *get_active_firmware_slot_info(void);
bool get_firmware_slot_info(uint8_t slot, firmware_slot_info_t *info);
bool get_flash_logo_info(int *width, int *height, int *datalen);
void flash_log_status(void);
