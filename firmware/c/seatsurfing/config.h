#pragma once
#include <stdint.h>

#define SEATSURFING_HOST_MAX_LEN 64
#define SEATSURFING_USER_MAX_LEN 64
#define SEATSURFING_PWD_MAX_LEN 64
#define SEATSURFING_SPACE_ID_LEN 64
#define SEATSURFING_LOCATION_ID_LEN 64
#define SEATSURFING_MAX_SEATS 4

typedef struct {
    char host[SEATSURFING_HOST_MAX_LEN];
    uint8_t ip[4];
    uint16_t port;
    char username[SEATSURFING_USER_MAX_LEN];
    char password[SEATSURFING_PWD_MAX_LEN];
    char location_id[SEATSURFING_LOCATION_ID_LEN];
    uint8_t seat_count; // number of entries in space_ids[] to use (1..SEATSURFING_MAX_SEATS)
    char space_ids[SEATSURFING_MAX_SEATS][SEATSURFING_SPACE_ID_LEN];
    // NFC booking (opcode 0x40) — SpaceAdmin user credentials for JWT login
    char booking_email[64];
    char booking_password[64];
    // 1 = enroll via claim code and use the Plus plugin's device endpoint. Occupies the former
    // trailing padding byte, so the stored layout is unchanged.
    uint8_t device_mode;
} seatsurfing_config_data_t;
_Static_assert(sizeof(seatsurfing_config_data_t) == 648,
               "seatsurfing_config_data_t layout changed");

// Duration for NFC-triggered bookings (hard-coded for now, adjustable via app later)
#define SEATSURFING_BOOKING_DURATION_S (8 * 3600)

typedef struct {
    seatsurfing_config_data_t data;
    uint32_t crc32;
} seatsurfing_config_t;
