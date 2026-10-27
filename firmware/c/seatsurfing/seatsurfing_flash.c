#include "seatsurfing/seatsurfing_flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>

bool save_seatsurfing_config(const seatsurfing_config_t *in) {
    seatsurfing_config_t temp = *in;
    temp.crc32 = calc_crc32(&temp.data, sizeof(seatsurfing_config_data_t));

    const uint32_t sector = UC_CONFIG_FLASH_OFFSET & ~(FLASH_SECTOR_SIZE - 1);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(sector, FLASH_SECTOR_SIZE);
    flash_range_program(UC_CONFIG_FLASH_OFFSET, (const uint8_t *)&temp,
                        sizeof(seatsurfing_config_t));
    restore_interrupts(ints);
    return true;
}

#define SS_DEVICE_STATE_MAGIC "SSDM"
_Static_assert(sizeof(ss_device_state_t) <= FLASH_PAGE_SIZE, "ss_device_state_t exceeds one page");

static uint32_t ss_device_state_crc(const ss_device_state_t *st) {
    return calc_crc32(st, offsetof(ss_device_state_t, crc32));
}

bool load_ss_device_state(ss_device_state_t *out) {
    memcpy(out, FLASH_PTR(SS_DEVICE_STATE_FLASH_OFFSET), sizeof(*out));
    return memcmp(out->magic, SS_DEVICE_STATE_MAGIC, sizeof(out->magic)) == 0 &&
           out->crc32 == ss_device_state_crc(out);
}

bool save_ss_device_state(const ss_device_state_t *in) {
    ss_device_state_t temp = *in;
    memcpy(temp.magic, SS_DEVICE_STATE_MAGIC, sizeof(temp.magic));
    temp.crc32 = ss_device_state_crc(&temp);

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &temp, sizeof(temp));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SS_DEVICE_STATE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SS_DEVICE_STATE_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(ints);
    return true;
}

// Linker retention — prevents the XIP-mapped default from being stripped.
const void *keep_seatsurfing_config_flash = &seatsurfing_config_flash;
