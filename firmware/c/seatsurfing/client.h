#ifndef SEATSURFING_CLIENT_H
#define SEATSURFING_CLIENT_H

#include "seatsurfing/config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool is_available;
    char user_email[64]; // empty if available
    char desk_name[32];  // "Desk 3", "Platz 1", etc.
    char space_id[64];   // UUID from server, used for booking
} seat_info_t;

extern seat_info_t seatsurfing_data[SEATSURFING_MAX_SEATS];

// What run() hands to render() for the SeatSurfing page.
typedef enum {
    SS_VIEW_SEATS,  // seatsurfing_data[]
    SS_VIEW_CLAIM,  // device mode: claim code waiting for adoption
    SS_VIEW_NOTICE, // device mode: state the admin has to resolve
} ss_view_kind_t;

typedef struct {
    ss_view_kind_t kind;
    char claim_code[16];
    char device_name[16]; // "Tinta-5929", as the setup network is called
    uint32_t next_wake_s; // device mode: interval the server asked for, 0 = device default
    const char *notice_title;
    const char *notice_detail;
    const char *notice_tip;
} ss_view_t;

void ss_format_seat(const seat_info_t *seat, char *out, size_t out_len, size_t max_chars);

#endif // SEATSURFING_CLIENT_H
