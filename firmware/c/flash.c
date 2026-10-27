#include "pico/flash.h"
#define LOG_MODULE LOG_MOD_FLASH
#include "debug.h"
#include "flash.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/m0plus.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/time.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ------------------------------
// CRC32 implementation
// ------------------------------
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table(void) {
    if (crc32_table_initialized)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_initialized = true;
}

uint32_t calc_crc32(const void *data, size_t len) {
    init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

bool get_firmware_slot_info(uint8_t slot, firmware_slot_info_t *info) {
    uint32_t offset = (slot == 0)   ? FIRMWARE_SLOT0_FLASH_OFFSET
                      : (slot == 1) ? FIRMWARE_SLOT1_FLASH_OFFSET
                                    : 0;

    if (offset == 0)
        return false;

    const firmware_header_t *header = (const firmware_header_t *)FLASH_PTR(offset);

    if (memcmp(header->magic, FIRMWARE_MAGIC, sizeof(header->magic)) != 0 ||
        header->valid_flag != 1)
        return false;

    strncpy(info->build_date, header->build_date, sizeof(info->build_date) - 1);
    info->build_date[sizeof(info->build_date) - 1] = '\0';
    strncpy(info->git_version, header->git_version, sizeof(info->git_version) - 1);
    info->git_version[sizeof(info->git_version) - 1] = '\0';
    info->size = header->firmware_size;
    info->crc32 = header->crc32;
    info->slot_index = header->slot;
    info->valid_flag = header->valid_flag;

    return true;
}

bool get_flash_logo_info(int *width, int *height, int *datalen) {
    const logo_header_t *header = (const logo_header_t *)FLASH_PTR(LOGO_FLASH_OFFSET);

    if (memcmp(header->magic, "LOGO", 4) != 0) {
        return false;
    }

    if (width)
        *width = header->width;
    if (height)
        *height = header->height;
    if (datalen) {
        if (header->datalen > (uint32_t)INT_MAX) {
            return false;
        }
        *datalen = (int)header->datalen;
    }

    return true;
}

const char *get_active_firmware_slot_info(void) {
    // Read current VTOR (Vector Table Offset Register) address
    uintptr_t vtor = *((volatile uint32_t *)(PPB_BASE + M0PLUS_VTOR_OFFSET));

    // Read reset handler address from vector table (entry 1 = reset handler)
    uintptr_t reset_handler = ((uintptr_t *)vtor)[1];

    const char *slot_name;

    if (reset_handler >= (uintptr_t)FLASH_PTR(BOOTLOADER_FLASH_OFFSET) &&
        reset_handler < (uintptr_t)FLASH_PTR(FIRMWARE_SLOT0_FLASH_OFFSET)) {
        slot_name = "SLOT_DIRECT";
    } else if (reset_handler >= (uintptr_t)FLASH_PTR(FIRMWARE_SLOT0_FLASH_OFFSET) &&
               reset_handler <
                   (uintptr_t)FLASH_PTR(FIRMWARE_SLOT0_FLASH_OFFSET + FIRMWARE_FLASH_SIZE)) {
        slot_name = "SLOT_0";
    } else if (reset_handler >= (uintptr_t)FLASH_PTR(FIRMWARE_SLOT1_FLASH_OFFSET) &&
               reset_handler <
                   (uintptr_t)FLASH_PTR(FIRMWARE_SLOT1_FLASH_OFFSET + FIRMWARE_FLASH_SIZE)) {
        slot_name = "SLOT_1";
    } else {
        slot_name = "SLOT_UNKNOWN";
    }

    // Static buffer for formatted return string
    static char info[64];
    snprintf(info, sizeof(info), "%s (Reset @ 0x%08lX)", slot_name, (unsigned long)reset_handler);
    return info;
}

void flash_log_status(void) {
    const char *active = get_active_firmware_slot_info();
    debug_status("OK", "Running from: %s\n", active);

    for (uint8_t slot = 0; slot < 2; slot++) {
        firmware_slot_info_t info = {0};
        bool has = get_firmware_slot_info(slot, &info);
        if (has) {
            debug_status("OK", "Slot %d: %s, Build %s, %u bytes\n", slot, info.git_version,
                         info.build_date, info.size);
        } else {
            debug_status("WARN", "Slot %d: no valid firmware\n", slot);
        }
    }
}

bool save_uploaded_logo_to_flash(const uint8_t *data, size_t len) {
    if (len < 18) {
        dlog("Logo upload failed: data too short (%d bytes)\n", len);
        return false;
    }

    if (memcmp(data, "LOGO", 4) != 0) {
        dlog("Logo upload failed: invalid magic header\n");
        return false;
    }

    uint16_t width = data[4] | (data[5] << 8);
    uint16_t height = data[6] | (data[7] << 8);
    uint32_t datalen = data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24);
    size_t expected = datalen + 18;

    if (expected != len) {
        dlog("Logo upload failed: datalen mismatch (%d + 18 != %d)\n", datalen, len);
        return false;
    }

    if (len > LOGO_FLASH_SIZE) {
        dlog("Logo upload failed: file too large (%d > %d bytes)\n", len, LOGO_FLASH_SIZE);
        return false;
    }

    dlog("Logo upload OK: %dx%d px, %d bytes total\n", width, height, len);

    static uint8_t padded[LOGO_FLASH_SIZE];
    memset(padded, 0xFF, sizeof(padded));
    memcpy(padded, data, len);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(LOGO_FLASH_OFFSET, LOGO_FLASH_SIZE);
    flash_range_program(LOGO_FLASH_OFFSET, padded, LOGO_FLASH_SIZE);
    restore_interrupts(ints);

    dlog("Logo written to Flash at offset 0x%X\n", LOGO_FLASH_OFFSET);
    return true;
}

void flash_erase_logo(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(LOGO_FLASH_OFFSET, LOGO_FLASH_SIZE);
    restore_interrupts(ints);
    dlog("FLASH: logo sector erased\n");
}

void flash_zero_settings_sector(void) {
    const uint32_t sector_offset = UC_CONFIG_FLASH_OFFSET & ~(FLASH_SECTOR_SIZE - 1);
    dlog("FLASH: erasing settings sector at 0x%X\n", sector_offset);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(sector_offset, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

bool flash_demote_slot_header(uint32_t header_flash_offset) {
    const uint32_t sector_base = header_flash_offset & ~(FLASH_SECTOR_SIZE - 1);
    const size_t hdr_off = (size_t)(header_flash_offset - sector_base);

    if (hdr_off + sizeof(firmware_header_t) > FLASH_SECTOR_SIZE) {
        dlog("DEMOTE SLOT: header crosses sector boundary at 0x%08X\n", header_flash_offset);
        return false;
    }

    uint8_t sector_buffer[FLASH_SECTOR_SIZE];
    memcpy(sector_buffer, FLASH_PTR(sector_base), FLASH_SECTOR_SIZE);

    firmware_header_t *header = (firmware_header_t *)(sector_buffer + hdr_off);
    if (memcmp(header->magic, FIRMWARE_MAGIC, FIRMWARE_MAGIC_LEN) != 0 || header->valid_flag != 1) {
        dlog("DEMOTE SLOT: invalid header at 0x%08X\n", header_flash_offset);
        return false;
    }

    dlog("DEMOTE SLOT: before slot=%u version='%.*s' build='%.*s'\n", (unsigned)header->slot,
         (int)sizeof(header->git_version), header->git_version, (int)sizeof(header->build_date),
         header->build_date);

    memset(header->git_version, 0, sizeof(header->git_version));
    memset(header->build_date, 0, sizeof(header->build_date));
    snprintf(header->git_version, sizeof(header->git_version), "v0.0.0-0");
    snprintf(header->build_date, sizeof(header->build_date), "1970-01-01");

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(sector_base, FLASH_SECTOR_SIZE);
    flash_range_program(sector_base, sector_buffer, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    dlog("DEMOTE SLOT: after slot=%u version='%.*s' build='%.*s' (hdr=0x%08X sector=0x%08X)\n",
         (unsigned)header->slot, (int)sizeof(header->git_version), header->git_version,
         (int)sizeof(header->build_date), header->build_date, header_flash_offset, sector_base);
    return true;
}

// ------------------------------
// Wi-Fi config functions
// ------------------------------
static bool load_wifi_config(wifi_config_t *out_config) {
    const wifi_config_t *flash_config = (const wifi_config_t *)FLASH_PTR(WIFI_CONFIG_FLASH_OFFSET);
    memcpy(out_config, flash_config, sizeof(wifi_config_t));

    uint32_t expected_crc = calc_crc32(out_config, WIFI_CONFIG_CRC_SPAN);
    return (expected_crc == out_config->crc32);
}

bool save_wifi_config(const wifi_config_t *in_config) {
    wifi_config_t temp = *in_config;
    temp.region = (uint8_t)wifi_config_region(&temp);
    temp.region_check = temp.region ^ WIFI_REGION_CHECK;
    temp.crc32 = calc_crc32(&temp, WIFI_CONFIG_CRC_SPAN);

    const uint32_t sector_offset = WIFI_CONFIG_FLASH_OFFSET & ~(FLASH_SECTOR_SIZE - 1);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(sector_offset, FLASH_SECTOR_SIZE);
    flash_range_program(WIFI_CONFIG_FLASH_OFFSET, (const uint8_t *)&temp, sizeof(wifi_config_t));
    restore_interrupts(ints);

    return true;
}

// ------------------------------
// Append store
// ------------------------------
// For small records that change now and then, one sector each. A record is appended behind
// the last one, so a change costs one page program and no erase; the sector is erased only
// once it is full. The last record counts. A record of 0xFF bytes only cannot be stored: that
// is what an empty place looks like.
static const uint8_t *append_store_slot(uint32_t sector, size_t size, size_t index) {
    return (const uint8_t *)FLASH_PTR(sector) + index * size;
}

static bool append_store_slot_is(uint32_t sector, size_t size, size_t index, uint8_t value) {
    const uint8_t *bytes = append_store_slot(sector, size, index);
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != value)
            return false;
    }
    return true;
}

static size_t append_store_used(uint32_t sector, size_t size) {
    const size_t slots = FLASH_SECTOR_SIZE / size;
    size_t used = 0;
    while (used < slots && !append_store_slot_is(sector, size, used, 0xFF))
        used++;
    return used;
}

// The record that counts, or NULL if the sector is empty.
static const void *append_store_last(uint32_t sector, size_t size) {
    const size_t used = append_store_used(sector, size);
    return used ? append_store_slot(sector, size, used - 1) : NULL;
}

// size must divide FLASH_PAGE_SIZE, so that a record never straddles two pages.
static void append_store_add(uint32_t sector, const void *record, size_t size) {
    size_t index = append_store_used(sector, size);
    const bool full = index >= FLASH_SECTOR_SIZE / size;
    if (full)
        index = 0;

    const uint32_t record_offset = (uint32_t)(index * size);
    const uint32_t page_offset = record_offset & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    uint8_t page[FLASH_PAGE_SIZE];
    if (full)
        memset(page, 0xFF, sizeof(page));
    else
        memcpy(page, FLASH_PTR(sector + page_offset), sizeof(page));
    memcpy(page + (record_offset - page_offset), record, size);

    uint32_t ints = save_and_disable_interrupts();
    if (full)
        flash_range_erase(sector, FLASH_SECTOR_SIZE);
    flash_range_program(sector + page_offset, page, sizeof(page));
    restore_interrupts(ints);
}

// ------------------------------
// Wi-Fi join hint
// ------------------------------
// Kept in an append store; an all-zero record means "none".
typedef struct __attribute__((packed)) {
    uint8_t bssid[6];
    uint8_t channel; // 1-14; 0 = no access point, the record only carries the counter below
    uint8_t ip[4];   // address of the last lease, network order; 0 = none (INKI_EXP_DHCP_REBOOT)
    uint8_t wifi_failures; // failed joins in a row; 0 in records written before it existed
    uint8_t reserved[3];
    uint8_t check; // over the record and the stored SSID: another network invalidates the hint
} wifi_hint_t;
_Static_assert(FLASH_PAGE_SIZE % sizeof(wifi_hint_t) == 0, "hint record must divide a page");

static uint8_t wifi_hint_check(const wifi_hint_t *hint) {
    uint8_t keyed[offsetof(wifi_hint_t, check) + sizeof(wifi_config_flash.ssid)];
    memcpy(keyed, hint, offsetof(wifi_hint_t, check));
    memcpy(keyed + offsetof(wifi_hint_t, check), wifi_config_flash.ssid,
           sizeof(wifi_config_flash.ssid));
    return (uint8_t)calc_crc32(keyed, sizeof(keyed));
}

bool load_wifi_hint(uint8_t bssid[6], uint8_t *channel, uint8_t ip[4]) {
    const wifi_hint_t *hint = append_store_last(WIFI_HINT_FLASH_OFFSET, sizeof(wifi_hint_t));
    if (!hint)
        return false;
    if (hint->channel < 1 || hint->channel > 14 || hint->check != wifi_hint_check(hint))
        return false;
    memcpy(bssid, hint->bssid, 6);
    *channel = hint->channel;
    memcpy(ip, hint->ip, 4);
    return true;
}

// The last record whose check holds, hint or not; NULL if there is none.
static const wifi_hint_t *wifi_hint_record(void) {
    const wifi_hint_t *hint = append_store_last(WIFI_HINT_FLASH_OFFSET, sizeof(wifi_hint_t));
    return hint && hint->check == wifi_hint_check(hint) ? hint : NULL;
}

int load_wifi_failures(void) {
    const wifi_hint_t *hint = wifi_hint_record();
    return hint ? hint->wifi_failures : 0;
}

void save_wifi_failures(int n) {
    if (n < 0)
        n = 0;
    if (n > WIFI_FAILURES_MAX)
        n = WIFI_FAILURES_MAX;
    const wifi_hint_t *last = wifi_hint_record();
    if ((last ? last->wifi_failures : 0) == n)
        return;
    wifi_hint_t record = {0}; // no access point known: channel 0
    if (last)
        record = *last;
    record.wifi_failures = (uint8_t)n;
    record.check = wifi_hint_check(&record);
    append_store_add(WIFI_HINT_FLASH_OFFSET, &record, sizeof(record));
}

// A new hint comes from a successful join, so it also sets the failure count back to 0.
void save_wifi_hint(const uint8_t bssid[6], uint8_t channel, const uint8_t ip[4]) {
    uint8_t stored_bssid[6], stored_channel, stored_ip[4];
    if (load_wifi_hint(stored_bssid, &stored_channel, stored_ip) && stored_channel == channel &&
        memcmp(stored_bssid, bssid, 6) == 0 && memcmp(stored_ip, ip, 4) == 0 &&
        load_wifi_failures() == 0)
        return;

    wifi_hint_t record = {.channel = channel};
    memcpy(record.bssid, bssid, 6);
    memcpy(record.ip, ip, 4);
    record.check = wifi_hint_check(&record);
    append_store_add(WIFI_HINT_FLASH_OFFSET, &record, sizeof(record));
}

void clear_wifi_hint(void) {
    static const wifi_hint_t none = {0};
    const wifi_hint_t *last = append_store_last(WIFI_HINT_FLASH_OFFSET, sizeof(wifi_hint_t));
    if (!last || memcmp(last, &none, sizeof(none)) == 0)
        return;
    append_store_add(WIFI_HINT_FLASH_OFFSET, &none, sizeof(none));
}

// ------------------------------
// Resident page
// ------------------------------
// The page that stays on the panel between scheduled wakes, in an append store.
typedef struct __attribute__((packed)) {
    uint8_t page;
    uint8_t check; // page ^ RESIDENT_PAGE_CHECK; an erased or half-written record fails it
} resident_page_t;
#define RESIDENT_PAGE_CHECK 0x5Au
#define RESIDENT_PAGE_MAX 0x7F
_Static_assert(FLASH_PAGE_SIZE % sizeof(resident_page_t) == 0, "page record must divide a page");

int load_resident_page(void) {
    const resident_page_t *r =
        append_store_last(RESIDENT_PAGE_FLASH_OFFSET, sizeof(resident_page_t));
    if (!r || r->page > RESIDENT_PAGE_MAX || r->check != (uint8_t)(r->page ^ RESIDENT_PAGE_CHECK))
        return -1;
    return r->page;
}

void save_resident_page(int page) {
    if (page < 0 || page > RESIDENT_PAGE_MAX || load_resident_page() == page)
        return;
    const resident_page_t record = {(uint8_t)page, (uint8_t)(page ^ RESIDENT_PAGE_CHECK)};
    append_store_add(RESIDENT_PAGE_FLASH_OFFSET, &record, sizeof(record));
}

#if INKI_EXP_WIFI_PMK
// ------------------------------
// Stored WPA2 key
// ------------------------------
// One record at the start of its sector, rewritten only when the credentials change. A power
// cut while writing leaves an invalid record, which reads as "none" and is derived again.
#define WIFI_PMK_MAGIC 0x314B4D50u // "PMK1"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t source_crc; // over SSID and passphrase as stored: other credentials, other key
    uint32_t strikes;
    uint8_t pmk[32];
    uint32_t crc32;
} wifi_pmk_record_t;

static uint32_t wifi_pmk_source_crc(void) {
    return calc_crc32(&wifi_config_flash, WIFI_CONFIG_CRC_SPAN);
}

wifi_pmk_state_t load_wifi_pmk(uint8_t pmk[32], uint32_t *strikes) {
    *strikes = 0;
    const wifi_pmk_record_t *record = (const wifi_pmk_record_t *)FLASH_PTR(WIFI_PMK_FLASH_OFFSET);
    if (record->magic != WIFI_PMK_MAGIC ||
        record->crc32 != calc_crc32(record, sizeof(*record) - sizeof(uint32_t)) ||
        record->source_crc != wifi_pmk_source_crc())
        return WIFI_PMK_NONE;
    memcpy(pmk, record->pmk, 32);
    *strikes = record->strikes;
    return record->strikes >= WIFI_PMK_MAX_STRIKES ? WIFI_PMK_REJECTED : WIFI_PMK_VALID;
}

void save_wifi_pmk(const uint8_t pmk[32], uint32_t strikes) {
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    wifi_pmk_record_t record = {
        .magic = WIFI_PMK_MAGIC, .source_crc = wifi_pmk_source_crc(), .strikes = strikes};
    memcpy(record.pmk, pmk, 32);
    record.crc32 = calc_crc32(&record, sizeof(record) - sizeof(uint32_t));
    memcpy(page, &record, sizeof(record));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_PMK_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WIFI_PMK_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(ints);
}
#endif

// ------------------------------
// Device config functions
// ------------------------------
static bool load_device_config(device_config_t *out_config) {
    const device_config_t *flash_config =
        (const device_config_t *)FLASH_PTR(DEVICE_CONFIG_FLASH_OFFSET);
    memcpy(out_config, flash_config, sizeof(device_config_t));

    uint32_t expected_crc = calc_crc32(&out_config->data, sizeof(device_config_data_t));
    return (expected_crc == out_config->crc32);
}

bool save_device_config(const device_config_t *in_config) {
    device_config_t temp = *in_config;
    temp.crc32 = calc_crc32(&temp.data, sizeof(device_config_data_t));

    const uint32_t sector_offset = DEVICE_CONFIG_FLASH_OFFSET & ~(FLASH_SECTOR_SIZE - 1);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(sector_offset, FLASH_SECTOR_SIZE);
    flash_range_program(DEVICE_CONFIG_FLASH_OFFSET, (const uint8_t *)&temp,
                        sizeof(device_config_t));
    restore_interrupts(ints);

    return true;
}

const void *keep_device_config_flash = &device_config_flash;
const void *keep_wifi_config_flash = &wifi_config_flash;

// =============================================================================
// OTA STREAMING API
// Must be called from Thread mode (main event loop), NOT from recv_cb.
// sleep_ms() after flash operations is effective only from Thread mode.
// =============================================================================

static bool flash_mark_firmware_valid(uint32_t slot_offset); // defined below

static struct {
    uint32_t slot_offset;  // base address of target firmware slot
    uint32_t write_cursor; // next flash address to program
    uint32_t erase_cursor; // next flash address to erase
    uint32_t erase_end;    // end of erase region
} s_ota;

// Slot 1 starts half way into a sector, so a length rounded to whole sectors would leave the
// last 2 KB unerased: one sector more.
size_t flash_ota_erase_length(size_t image_size) {
    const size_t length =
        ((image_size + FLASH_SECTOR_SIZE - 1) & ~(size_t)(FLASH_SECTOR_SIZE - 1)) +
        FLASH_SECTOR_SIZE;
    return length <= FIRMWARE_FLASH_SIZE ? length : 0;
}

void flash_ota_begin(uint32_t slot_offset, size_t erase_length) {
    s_ota.slot_offset = slot_offset;
    s_ota.write_cursor = slot_offset;
    s_ota.erase_cursor = slot_offset;
    s_ota.erase_end = slot_offset + erase_length;
}

// Returns true when the last sector has been erased.
bool flash_ota_erase_next_sector(void) {
    if (s_ota.erase_cursor >= s_ota.erase_end)
        return true;

    watchdog_update();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(s_ota.erase_cursor, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    s_ota.erase_cursor += FLASH_SECTOR_SIZE;

    return (s_ota.erase_cursor >= s_ota.erase_end);
}

// Write exactly FLASH_PAGE_SIZE bytes.  Caller must pad the last partial page with 0xFF.
void flash_ota_write_page(const uint8_t *data) {
    watchdog_update();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(s_ota.write_cursor, data, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    s_ota.write_cursor += FLASH_PAGE_SIZE;
}

// Validate header + CRC; mark slot valid if all checks pass.
// Call this after every page (including the last padded one) has been written.
flash_ota_result_t flash_ota_finish(void) {
    flash_ota_result_t r = {0};

    const firmware_header_t *hdr = (const firmware_header_t *)FLASH_PTR(s_ota.slot_offset);
    memcpy(&r.header, hdr, sizeof(r.header));

    r.valid = (memcmp(hdr->magic, FIRMWARE_MAGIC, FIRMWARE_MAGIC_LEN) == 0);
    uint8_t expected_slot = (s_ota.slot_offset == FIRMWARE_SLOT0_FLASH_OFFSET) ? 0 : 1;
    r.slot_ok = (hdr->slot == expected_slot);
    // CRC covers firmware_size bytes starting after the 256-byte header.
    if (r.header.firmware_size > sizeof(firmware_header_t)) {
        const uint8_t *payload = FLASH_PTR(s_ota.slot_offset) + sizeof(firmware_header_t);
        size_t payload_len = r.header.firmware_size - sizeof(firmware_header_t);
        r.actual_crc = calc_crc32(payload, payload_len);
    }
    r.crc_ok = (r.actual_crc == hdr->crc32);

    if (r.valid && r.crc_ok && r.slot_ok)
        flash_mark_firmware_valid(s_ota.slot_offset);

    return r;
}

// Rewrite the first sector of the slot with valid_flag = 1.
static bool flash_mark_firmware_valid(uint32_t slot_offset) {
    uint8_t sector_buf[FLASH_SECTOR_SIZE];
    memcpy(sector_buf, FLASH_PTR(slot_offset), FLASH_SECTOR_SIZE);

    firmware_header_t *hdr = (firmware_header_t *)sector_buf;
    if (memcmp(hdr->magic, FIRMWARE_MAGIC, FIRMWARE_MAGIC_LEN) != 0)
        return false;

    hdr->valid_flag = 1;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(slot_offset, FLASH_SECTOR_SIZE);
    flash_range_program(slot_offset, sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    dlog("[OTA] Slot at 0x%08X marked valid (v=%.*s)\n", slot_offset, (int)sizeof(hdr->git_version),
         hdr->git_version);
    return true;
}
