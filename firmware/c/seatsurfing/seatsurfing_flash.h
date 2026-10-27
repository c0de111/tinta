#pragma once
#include "flash.h"
#include "seatsurfing/config.h"
#include <stdbool.h>

#define seatsurfing_config_flash (*(const seatsurfing_config_t *)FLASH_PTR(UC_CONFIG_FLASH_OFFSET))

bool save_seatsurfing_config(const seatsurfing_config_t *in);

// Device-mode enrollment state (own sector, written by the firmware, not by the settings form)
#define SS_DEVICE_STATE_FLASH_OFFSET (CONFIG_FLASH_OFFSET + 0x5000) // 0x1EC000

#define SS_CLAIM_CODE_LEN 8
#define SS_DEVICE_USER_MAX_LEN 128 // "<org uuid>_device+<device uuid>@seatsurfing.local" = 98
#define SS_DEVICE_PWD_MAX_LEN 64

typedef struct {
    char magic[4]; // "SSDM"
    char claim_code[SS_CLAIM_CODE_LEN + 1];
    char username[SS_DEVICE_USER_MAX_LEN]; // empty until the server has issued credentials
    char password[SS_DEVICE_PWD_MAX_LEN];
    uint32_t crc32;
} ss_device_state_t;

// False if the sector holds no valid state (erased, foreign data, CRC mismatch).
bool load_ss_device_state(ss_device_state_t *out);
bool save_ss_device_state(const ss_device_state_t *in);
