#include "net_update.h"

#include "config.h"
#include "debug.h"
#include "epaper.h"
#include "epaper_pages_shared.h"
#include "epaper_render.h"
#include "flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "http_client.h"
#include "pico/stdlib.h"
#include "power.h"
#include "st25_io.h"
#include "version.h"
#include "wifi.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_UPDATE_HOST "tinta-fw.wake-electronics.com"
#define NET_UPDATE_PORT 80
// Each piece is fetched whole into RAM and written while no connection is open, so the
// network never has to wait for the flash (the HTTP client cannot hold a sender back).
#define NET_UPDATE_PIECE (16 * 1024)
#define NET_UPDATE_TRIES 3
#define NET_UPDATE_IDLE_MS 5000

static uint8_t s_piece[NET_UPDATE_PIECE];

typedef struct {
    size_t received;
    int status;
    bool complete;
    bool success;
} piece_t;

static void on_header(const char *header, size_t header_len, int status_code, int content_length,
                      void *arg) {
    (void)header;
    (void)header_len;
    (void)content_length;
    ((piece_t *)arg)->status = status_code;
}

static void on_data(const uint8_t *data, size_t len, void *arg) {
    piece_t *p = (piece_t *)arg;
    if (p->received + len > sizeof(s_piece))
        len = sizeof(s_piece) - p->received;
    memcpy(s_piece + p->received, data, len);
    p->received += len;
}

static void on_complete(bool success, void *arg) {
    piece_t *p = (piece_t *)arg;
    p->success = success;
    p->complete = true;
}

// Bytes [from, from + len) of path into s_piece. True only if exactly len bytes came back
// as a partial answer (206).
static bool fetch_piece(const ip_addr_t *ip, const char *path, size_t from, size_t len) {
    static char request[320];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: " NET_UPDATE_HOST "\r\nRange: bytes=%u-%u\r\n"
             "User-Agent: Tinta/%s\r\nConnection: close\r\n\r\n",
             path, (unsigned)from, (unsigned)(from + len - 1), version);
    for (int attempt = 1; attempt <= NET_UPDATE_TRIES; attempt++) {
        piece_t p = {0};
        if (http_request_async_stream(ip, NET_UPDATE_PORT, request, on_header, on_data, on_complete,
                                      &p) == HTTP_SUCCESS) {
            absolute_time_t last = get_absolute_time();
            size_t seen = 0;
            while (!p.complete) {
                sleep_ms(10);
                watchdog_update();
                if (p.received != seen) {
                    seen = p.received;
                    last = get_absolute_time();
                } else if (absolute_time_diff_us(last, get_absolute_time()) / 1000 >
                           NET_UPDATE_IDLE_MS) {
                    break;
                }
            }
            if (p.complete && p.success && p.status == 206 && p.received == len)
                return true;
            debug_status("WARN", "Update: piece at %u, try %d: HTTP %d, %u of %u bytes\n",
                         (unsigned)from, attempt, p.status, (unsigned)p.received, (unsigned)len);
        }
        sleep_ms(200);
    }
    return false;
}

// "v0.17.0-583-gbcb5f" -> {0, 17, 0, 583}; the commit count orders builds of one tag.
static bool parse_version(const char *s, long out[4]) {
    if (*s == 'v')
        s++;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*s))
            return i == 3 ? (out[3] = 0, true) : false;
        char *end;
        out[i] = strtol(s, &end, 10);
        s = (*end == '.' || *end == '-') ? end + 1 : end;
    }
    return true;
}

static bool is_newer(const char *candidate, const char *running) {
    long a[4], b[4];
    if (!parse_version(candidate, a) || !parse_version(running, b))
        return false;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i])
            return a[i] > b[i];
    return false;
}

// "v0.17.0-595-g4faa9" -> "v0.17.0-595": the commit hash means nothing on the panel.
static void short_version(char *out, size_t len, const char *v) {
    snprintf(out, len, "%s", v);
    char *hash = strstr(out, "-g");
    if (hash)
        *hash = '\0';
}

// "2026-10-08 ..." -> "built 8 Oct 2026"
static void built_on(char *out, size_t len, const char *date) {
    static const char months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int y = atoi(date), m = atoi(date + 5), d = atoi(date + 8);
    if (y < 2000 || m < 1 || m > 12 || d < 1)
        snprintf(out, len, "%s", "");
    else
        snprintf(out, len, "built %d %s %d", d, months[m - 1], y);
}

static void show(const update_page_t *page) {
    uint8_t *image = epaper_init(false);
    if (image) {
        render_page_update(image, page);
        epaper_flush_and_sleep(image);
    }
}

// Shows the outcome and switches off; the new firmware, if any, starts with the next wake.
static void finish(bool st25_present, const update_page_t *page) {
    debug_status("INFO", "Update: %s: %s %s\n", page->title, page->state,
                 page->version ? page->version : "");
    show(page);
    if (st25_present)
        st25_power_off();
    debug_flush();
    power_shutdown_default();
}

// A failure with nothing written yet, or with the running firmware untouched.
static void fail(bool st25_present, const char *reason, bool erased) {
    const update_page_t page = {"Update failed",
                                reason,
                                NULL,
                                NULL,
                                erased ? "The running firmware stays." : "Nothing was changed.",
                                NET_UPDATE_HOST};
    finish(st25_present, &page);
}

void net_update_run(bool st25_present) {
    debug_status("INFO", "Update: buttons 3 + 4, looking for new firmware\n");
    // No page of its own for the search: the server answers within a second or two, and the
    // next page already says what came of it.

    const bool slot0_running = strncmp(get_active_firmware_slot_info(), "SLOT_0", 6) == 0;
    const int slot = slot0_running ? 1 : 0;
    const uint32_t offset = slot ? FIRMWARE_SLOT1_FLASH_OFFSET : FIRMWARE_SLOT0_FLASH_OFFSET;

    char folder[24];
    snprintf(folder, sizeof(folder), "%s", USE_CASE_NAME);
    for (char *c = folder; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    char path[64];
    snprintf(path, sizeof(path), "/%s/inki_slot%d.bin", folder, slot);

    ip_addr_t ip;
    if (wifi_connect() != WIFI_SUCCESS) {
        fail(st25_present, "No Wi-Fi.", false);
        return;
    }
    if (!wifi_resolve_hostname(NET_UPDATE_HOST, &ip, 5000)) {
        wifi_deinit();
        fail(st25_present, "Update server not found.", false);
        return;
    }

    // The header names version, use case, slot and size before anything is erased.
    if (!fetch_piece(&ip, path, 0, sizeof(firmware_header_t))) {
        wifi_deinit();
        fail(st25_present, "No firmware on the server.", false);
        return;
    }
    firmware_header_t h;
    memcpy(&h, s_piece, sizeof(h));
    h.git_version[sizeof(h.git_version) - 1] = '\0';
    h.use_case_name[sizeof(h.use_case_name) - 1] = '\0';
    debug_status("INFO", "Update: server has %s (%s, slot %u, %u bytes), running %s\n",
                 h.git_version, h.use_case_name, h.slot, (unsigned)h.firmware_size, version);

    const bool header_ok = memcmp(h.magic, FIRMWARE_MAGIC, FIRMWARE_MAGIC_LEN) == 0 &&
                           h.slot == slot && strcmp(h.use_case_name, USE_CASE_NAME) == 0 &&
                           h.firmware_size > sizeof(firmware_header_t) &&
                           h.firmware_size <= FIRMWARE_FLASH_SIZE;
    if (!header_ok) {
        wifi_deinit();
        fail(st25_present, "The file on the server is not for this sign.", false);
        return;
    }
    if (!is_newer(h.git_version, version)) {
        wifi_deinit();
        char running[24], built[24];
        short_version(running, sizeof(running), version);
        built_on(built, sizeof(built), build_date);
        const update_page_t current = {
            "Firmware update", "Tinta is up to date.", running, built, NULL, NET_UPDATE_HOST};
        finish(st25_present, &current);
        return;
    }

    char new_version[24], new_built[24];
    short_version(new_version, sizeof(new_version), h.git_version);
    h.build_date[sizeof(h.build_date) - 1] = '\0';
    built_on(new_built, sizeof(new_built), h.build_date);
    const update_page_t installing = {"Firmware update",
                                      "Installing",
                                      new_version,
                                      new_built,
                                      "Please wait, this takes a minute.",
                                      NET_UPDATE_HOST};
    show(&installing);

    const size_t size = h.firmware_size;
    const size_t erase_length = flash_ota_erase_length(size);
    if (!erase_length) {
        wifi_deinit();
        fail(st25_present, "The firmware is too large.", false);
        return;
    }
    flash_ota_begin(offset, erase_length);
    while (!flash_ota_erase_next_sector())
        sleep_ms(1);
    debug_status("OK", "Update: slot %d erased, loading %u bytes\n", slot, (unsigned)size);

    int reported = 0;
    for (size_t from = 0; from < size; from += NET_UPDATE_PIECE) {
        const size_t len = size - from < NET_UPDATE_PIECE ? size - from : NET_UPDATE_PIECE;
        if (!fetch_piece(&ip, path, from, len)) {
            wifi_deinit();
            fail(st25_present, "Download broke off.", true);
            return;
        }
        for (size_t page = 0; page < len; page += FLASH_PAGE_SIZE) {
            uint8_t buf[FLASH_PAGE_SIZE];
            const size_t n = len - page < FLASH_PAGE_SIZE ? len - page : FLASH_PAGE_SIZE;
            memset(buf, 0xFF, sizeof(buf));
            memcpy(buf, s_piece + page, n);
            flash_ota_write_page(buf);
            sleep_ms(1);
        }
        const int pct = (int)(100ULL * (from + len) / size);
        if (pct >= reported + 10) {
            reported = pct - pct % 10;
            debug_status("OK", "Update: %d%%\n", reported);
        }
    }
    wifi_deinit();

    const flash_ota_result_t r = flash_ota_finish();
    if (!(r.valid && r.slot_ok && r.crc_ok)) {
        debug_status("ERROR", "Update: check failed (magic %d, slot %d, CRC 0x%08X vs 0x%08X)\n",
                     r.valid, r.slot_ok, (unsigned)r.actual_crc, (unsigned)r.header.crc32);
        fail(st25_present, "The download did not check out.", true);
        return;
    }
    debug_status("OK", "Update: %s written to slot %d and marked valid\n", h.git_version, slot);
    const update_page_t installed = {"Firmware update",
                                     "Installed",
                                     new_version,
                                     new_built,
                                     "It starts with the next wake, or press a button.",
                                     NET_UPDATE_HOST};
    finish(st25_present, &installed);
}
