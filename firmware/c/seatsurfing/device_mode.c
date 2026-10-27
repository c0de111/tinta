#include "seatsurfing/device_mode.h"
#include "base64.h"
#define LOG_MODULE LOG_MOD_SEATSURFING
#include "debug.h"
#include "http_client.h"
#include "pico/rand.h"
#include "rtc.h"
#include "seatsurfing/seatsurfing_flash.h"
#include "sensors.h"
#include "third_party/cjson/cJSON.h"
#include "version.h"
#include "wifi.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DEVICES_PATH "/plus-features/devices"
#define MIN_SYNC_INTERVAL_S 60

// After a tap the sign syncs every few seconds (client.c). Each reply is logged only when it
// differs, so the 8 KB boot log keeps its last lines.
static int64_t s_logged_seconds = -1;
static bool s_logged_url;

typedef enum {
    REGISTER_UNKNOWN,
    REGISTER_PENDING,
    REGISTER_APPROVED,
    REGISTER_CONFIRMED
} register_status_t;

typedef struct {
    register_status_t status;
    char username[SS_DEVICE_USER_MAX_LEN];
    char password[SS_DEVICE_PWD_MAX_LEN];
} register_reply_t;

typedef struct {
    bool parsed;
    ss_device_sync_t *out;
} sync_reply_t;

// False if the item is no string or does not fit; dst is then left empty.
static bool copy_json_string(char *dst, size_t dst_len, const cJSON *item) {
    dst[0] = '\0';
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= dst_len)
        return false;
    strcpy(dst, item->valuestring);
    return true;
}

// Sent to the server only: the full MAC keeps signs apart in a large organisation, where the
// short name Tinta-XXXX on the panel could repeat.
#define SS_HARDWARE_ID_LEN 18 // "TINTA-" + 12 hex digits

static void ss_device_hardware_id(char *out, size_t out_len) {
    const uint8_t *mac = wifi_mac();
    snprintf(out, out_len, "TINTA-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

void ss_device_claim_code(char *out, size_t out_len) {
    ss_device_state_t st;
    snprintf(out, out_len, "%s", load_ss_device_state(&st) ? st.claim_code : "");
}

static void new_claim_code(char code[SS_CLAIM_CODE_LEN + 1]) {
    // No 0/O, 1/I: the code is read off the display and typed in by hand.
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint64_t bits = get_rand_64();
    for (int i = 0; i < SS_CLAIM_CODE_LEN; i++) {
        code[i] = alphabet[bits & 0x1F];
        bits >>= 5;
    }
    code[SS_CLAIM_CODE_LEN] = '\0';
}

// --- /register ---

static void register_reply_received(const char *body, size_t length, void *arg) {
    (void)length;
    cJSON *root = cJSON_Parse(body);
    if (!root)
        return;

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (cJSON_IsString(status)) {
        register_reply_t *reply = arg;
        if (strcmp(status->valuestring, "pending") == 0) {
            reply->status = REGISTER_PENDING;
        } else if (strcmp(status->valuestring, "confirmed") == 0) {
            reply->status = REGISTER_CONFIRMED;
        } else if (strcmp(status->valuestring, "approved") == 0) {
            const cJSON *cred = cJSON_GetObjectItemCaseSensitive(root, "credentials");
            bool user_ok = copy_json_string(reply->username, sizeof(reply->username),
                                            cJSON_GetObjectItemCaseSensitive(cred, "username"));
            bool pwd_ok = copy_json_string(reply->password, sizeof(reply->password),
                                           cJSON_GetObjectItemCaseSensitive(cred, "password"));
            if (user_ok && pwd_ok)
                reply->status = REGISTER_APPROVED;
            else
                debug_status("ERROR", "Device mode: credentials missing or too long\n");
        }
    }
    cJSON_Delete(root);
}

// Returns the HTTP status (0 = no response).
static int request_register(const ip_addr_t *ip, uint16_t port, const char *host, int timeout_ms,
                            const char *claim_code, register_reply_t *reply) {
    char hardware_id[SS_HARDWARE_ID_LEN + 1];
    ss_device_hardware_id(hardware_id, sizeof(hardware_id));

    char body[192];
    int body_len =
        snprintf(body, sizeof(body),
                 "{\"hardwareId\":\"%s\",\"firmwareVersion\":\"%s\",\"claimToken\":\"%s\"}",
                 hardware_id, version, claim_code);
    if (body_len < 0 || (size_t)body_len >= sizeof(body))
        return 0;

    char req[HTTP_REQUEST_MAX];
    int len = snprintf(req, sizeof(req),
                       "POST " DEVICES_PATH "/register HTTP/1.0\r\n"
                       "Host: %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %d\r\n"
                       "\r\n"
                       "%s",
                       host, body_len, body);
    if (len < 0 || (size_t)len >= sizeof(req))
        return 0;

    http_request_sync(ip, port, req, timeout_ms, register_reply_received, reply);
    return http_get_last_status_code();
}

// --- /sync ---

// The server computes the next sync from the admin's refresh interval and office hours.
// serverTime is UTC. nextSyncAt carries the wall-clock time at the location, although it is
// labelled "Z" (plugin 1.3.0, kept so in 1.5 by design), so the local offset is taken off
// before the two are compared.
// Should the server switch to real UTC, the corrected value turns non-positive for short
// intervals and the plain difference is used.
static uint32_t next_sync_seconds(const cJSON *root, const cJSON *device) {
    const cJSON *server_time = cJSON_GetObjectItemCaseSensitive(root, "serverTime");
    const cJSON *next_sync = cJSON_GetObjectItemCaseSensitive(device, "nextSyncAt");
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(device, "refreshIntervalSeconds");

    int64_t now_s, next_s, seconds = 0;
    if (cJSON_IsString(server_time) && cJSON_IsString(next_sync) &&
        rtc_parse_iso8601(server_time->valuestring, &now_s) &&
        rtc_parse_iso8601(next_sync->valuestring, &next_s) && next_s > now_s) {
        int64_t as_sent = next_s - now_s;
        int64_t as_local = as_sent - rtc_local_utc_offset_s();
        seconds = as_local > 0 ? as_local : as_sent;
    } else if (cJSON_IsNumber(refresh) && refresh->valuedouble > 0) {
        seconds = (int64_t)refresh->valuedouble;
    }

    // Date and time to the second: "2026-10-03T08:08:02"
    if (seconds != s_logged_seconds)
        debug_status("INFO", "Device mode: serverTime=%.19s nextSyncAt=%.19s -> %ld s\n",
                     cJSON_IsString(server_time) ? server_time->valuestring : "?",
                     cJSON_IsString(next_sync) ? next_sync->valuestring : "?", (long)seconds);
    s_logged_seconds = seconds;

    if (seconds > 0 && seconds < MIN_SYNC_INTERVAL_S)
        seconds = MIN_SYNC_INTERVAL_S;
    return (uint32_t)seconds;
}

// One entry of "spaces" (plugin 1.5) or the single "space" of older plugins.
static bool parse_space(const cJSON *space, seat_info_t *seat) {
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(space, "status");
    if (!cJSON_IsString(status))
        return false;

    memset(seat, 0, sizeof(*seat));
    copy_json_string(seat->space_id, sizeof(seat->space_id),
                     cJSON_GetObjectItemCaseSensitive(space, "spaceId"));
    copy_json_string(seat->desk_name, sizeof(seat->desk_name),
                     cJSON_GetObjectItemCaseSensitive(space, "spaceName"));

    seat->is_available = strcmp(status->valuestring, "occupied") != 0;
    if (!seat->is_available) {
        const cJSON *booking = cJSON_GetObjectItemCaseSensitive(space, "currentBooking");
        bool owner_shown =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(booking, "ownerVisible")) &&
            copy_json_string(seat->user_email, sizeof(seat->user_email),
                             cJSON_GetObjectItemCaseSensitive(booking, "owner"));
        if (!owner_shown || !seat->user_email[0])
            strcpy(seat->user_email, "occupied");
    }
    return true;
}

static void sync_reply_received(const char *body, size_t length, void *arg) {
    (void)length;
    sync_reply_t *reply = arg;
    cJSON *root = cJSON_Parse(body);
    if (!root)
        return;

    ss_device_sync_t *out = reply->out;
    out->seat_count = 0;
    const cJSON *spaces = cJSON_GetObjectItemCaseSensitive(root, "spaces");
    if (cJSON_IsArray(spaces)) {
        const cJSON *space;
        cJSON_ArrayForEach(space, spaces) {
            if (out->seat_count == out->max_seats)
                break;
            if (parse_space(space, &out->seats[out->seat_count]))
                out->seat_count++;
        }
    } else if (parse_space(cJSON_GetObjectItemCaseSensitive(root, "space"), &out->seats[0])) {
        out->seat_count = 1;
    }

    if (out->seat_count > 0) {
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(device, "name");
        snprintf(reply->out->name, sizeof(reply->out->name), "%s",
                 cJSON_IsString(name) ? name->valuestring : "");
        reply->out->next_sync_s = next_sync_seconds(root, device);

        const cJSON *url = cJSON_GetObjectItemCaseSensitive(device, "bookingUrl");
        copy_json_string(reply->out->booking_url, sizeof(reply->out->booking_url), url);
        if (!s_logged_url) {
            debug_status("INFO", "Device mode: %d spaces, %u shown, bookingUrl (%u chars): %s\n",
                         cJSON_IsArray(spaces) ? cJSON_GetArraySize(spaces) : 1, out->seat_count,
                         cJSON_IsString(url) ? (unsigned)strlen(url->valuestring) : 0u,
                         reply->out->booking_url[0] ? reply->out->booking_url : "-");
            for (uint8_t i = 0; i < out->seat_count; i++)
                debug_status("INFO", "Device mode: space %u '%s' %s\n", i, out->seats[i].desk_name,
                             out->seats[i].is_available ? "free" : out->seats[i].user_email);
        }
        s_logged_url = true;
        reply->parsed = true;
    }
    cJSON_Delete(root);
}

static ss_device_result_t device_sync(const ip_addr_t *ip, uint16_t port, const char *host,
                                      int timeout_ms, const ss_device_state_t *st,
                                      ss_device_sync_t *out) {
    char userpass[SS_DEVICE_USER_MAX_LEN + SS_DEVICE_PWD_MAX_LEN + 2];
    snprintf(userpass, sizeof(userpass), "%s:%s", st->username, st->password);
    char auth_b64[4 * ((sizeof(userpass) + 2) / 3) + 1];
    base64_encode(userpass, strlen(userpass), auth_b64, sizeof(auth_b64));

    char telemetry[96];
    int tlen =
        snprintf(telemetry, sizeof(telemetry), "&batteryMillivolts=%d&cpuTemperatureCelsius=%.1f",
                 (int)(boot_battery_voltage() * 1000.0f + 0.5f), read_onchip_temperature_c());
    int32_t rssi_dbm;
    if (wifi_get_rssi(&rssi_dbm))
        snprintf(telemetry + tlen, sizeof(telemetry) - tlen, "&wifiRssiDbm=%ld", (long)rssi_dbm);

    char req[HTTP_REQUEST_MAX];
    int len = snprintf(req, sizeof(req),
                       "GET " DEVICES_PATH "/sync?firmwareVersion=%s%s HTTP/1.0\r\n"
                       "Host: %s\r\n"
                       "Authorization: Basic %s\r\n"
                       "\r\n",
                       version, telemetry, host, auth_b64);
    if (len < 0 || (size_t)len >= sizeof(req))
        return SS_DEVICE_ERROR;

    sync_reply_t reply = {.out = out};
    http_request_sync(ip, port, req, timeout_ms, sync_reply_received, &reply);
    int status = http_get_last_status_code();
    dlog("[SEATSURFING] device sync: HTTP %d\n", status);

    if (reply.parsed)
        return SS_DEVICE_SYNCED;
    if (status == 403)
        return SS_DEVICE_DISABLED;
    if (status == 401 || status == 404)
        return SS_DEVICE_READOPT;
    return SS_DEVICE_ERROR;
}

// --- Enrollment ---

// Registers with the stored claim code. Once the server hands out credentials, the first sync
// follows in the same wake: the server re-issues them on every /register until that sync.
static ss_device_result_t enroll(const ip_addr_t *ip, uint16_t port, const char *host,
                                 int timeout_ms, ss_device_state_t *st, bool *dirty,
                                 ss_device_sync_t *out) {
    register_reply_t reply = {0};
    int status = request_register(ip, port, host, timeout_ms, st->claim_code, &reply);
    if (status == 409) {
        // Another device holds this code: draw a new one and try once more.
        new_claim_code(st->claim_code);
        *dirty = true;
        reply = (register_reply_t){0};
        status = request_register(ip, port, host, timeout_ms, st->claim_code, &reply);
    }
    dlog("[SEATSURFING] device register: HTTP %d\n", status);

    if (status == 404)
        return SS_DEVICE_NO_PLUGIN;

    switch (reply.status) {
    case REGISTER_PENDING:
        return SS_DEVICE_PENDING;
    case REGISTER_CONFIRMED:
        return SS_DEVICE_READOPT;
    case REGISTER_APPROVED: {
        ss_device_state_t candidate = *st;
        strcpy(candidate.username, reply.username);
        strcpy(candidate.password, reply.password);
        ss_device_result_t result = device_sync(ip, port, host, timeout_ms, &candidate, out);
        if (result != SS_DEVICE_SYNCED)
            return SS_DEVICE_ERROR;
        *st = candidate;
        *dirty = true;
        return SS_DEVICE_SYNCED;
    }
    default:
        return SS_DEVICE_ERROR;
    }
}

ss_device_result_t ss_device_mode_run(const ip_addr_t *ip, uint16_t port, const char *host,
                                      int timeout_ms, ss_device_sync_t *out) {
    ss_device_state_t st;
    bool dirty = false;
    if (!load_ss_device_state(&st)) {
        memset(&st, 0, sizeof(st));
        new_claim_code(st.claim_code);
        dirty = true;
    }

    ss_device_result_t result = SS_DEVICE_READOPT;
    if (st.username[0])
        result = device_sync(ip, port, host, timeout_ms, &st, out);

    if (result == SS_DEVICE_READOPT) {
        if (st.username[0]) {
            // Revoked or deleted on the server: forget the credentials and enroll again.
            st.username[0] = '\0';
            st.password[0] = '\0';
            dirty = true;
        }
        result = enroll(ip, port, host, timeout_ms, &st, &dirty, out);
    }

    if (dirty)
        save_ss_device_state(&st);
    return result;
}
