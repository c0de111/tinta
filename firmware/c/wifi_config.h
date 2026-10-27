#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    char ssid[32];
    char password[64];
    uint32_t crc32;
    // Radio rules, appended after the CRC (which covers SSID and password only). A sector
    // written by older firmware holds 0xFF here, which fails the check: worldwide.
    uint8_t region;       // wifi_region_t
    uint8_t region_check; // region ^ WIFI_REGION_CHECK
} wifi_config_t;

typedef enum {
    WIFI_REGION_WORLDWIDE, // channels 1-11, 12-13 listen only; the default
    WIFI_REGION_EUROPE,    // ETSI: channels 1-13
    WIFI_REGION_USA,       // FCC: channels 1-11
    WIFI_REGION_JAPAN,     // channels 1-13
    WIFI_REGION_COUNT
} wifi_region_t;
#define WIFI_REGION_CHECK 0x5Au

// The bytes the CRC covers: SSID and password.
#define WIFI_CONFIG_CRC_SPAN offsetof(wifi_config_t, crc32)

// The region of a stored record, worldwide if none is set or the record fails its check.
static inline wifi_region_t wifi_config_region(const wifi_config_t *c) {
    if (c->region >= WIFI_REGION_COUNT || c->region_check != (c->region ^ WIFI_REGION_CHECK))
        return WIFI_REGION_WORLDWIDE;
    return (wifi_region_t)c->region;
}

// What the SSID holds as long as nobody has entered a home network (factory default).
#define WIFI_SSID_UNSET "default_ssid"
