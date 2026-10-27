#pragma once

#include "lwip/ip_addr.h"
#include "seatsurfing/client.h"
#include <stddef.h>
#include <stdint.h>

// Device mode of the Seatsurfing Plus plugin: the device enrolls itself with a claim code, an
// admin adopts it, and from then on one /sync per wake returns the occupancy of its spaces
// (plugin 1.5: up to 8; the first max_seats are shown).

typedef enum {
    SS_DEVICE_SYNCED,    // seats are filled
    SS_DEVICE_PENDING,   // waiting for an admin to enter the claim code
    SS_DEVICE_READOPT,   // server knows the device, but its credentials are gone
    SS_DEVICE_DISABLED,  // disabled by an admin
    SS_DEVICE_NO_PLUGIN, // server has no device endpoint
    SS_DEVICE_ERROR,     // transport or malformed reply
} ss_device_result_t;

#define SS_BOOKING_URL_LEN 448

// What the server sent for this device; valid on SS_DEVICE_SYNCED.
typedef struct {
    seat_info_t *seats; // caller-provided, max_seats entries
    uint8_t max_seats;
    uint8_t seat_count;   // spaces received, 1..max_seats
    char name[48];        // display name the admin gave the device (cut if longer)
    uint32_t next_sync_s; // when the server wants the next sync; 0 = it did not say
    // device.bookingUrl: one link for all spaces of the sign; "" if absent or too long, never
    // cut. One space is ~143 chars with our host, each further one adds 37, eight ~402.
    char booking_url[SS_BOOKING_URL_LEN];
} ss_device_sync_t;

// One wake cycle: enroll if needed, then sync. Requires WiFi; persists its own state.
ss_device_result_t ss_device_mode_run(const ip_addr_t *ip, uint16_t port, const char *host,
                                      int timeout_ms, ss_device_sync_t *out);

// Stored claim code, or "" if the device has never tried to enroll.
void ss_device_claim_code(char *out, size_t out_len);
