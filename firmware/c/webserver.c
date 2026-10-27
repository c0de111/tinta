/*
 * ==============================================================================
 * HTTP Processing Overview – inki Webserver
 * ==============================================================================
 *
 * recv_cb() — protocol layer:
 *   ├─ Collect header into upload_session.header_buffer until \r\n\r\n
 *   ├─ Parse method + path from first request line
 *   ├─ find_route() → route table lookup (static + dynamic use-case)
 *   └─ Dispatch by route type:
 *        ROUTE_PAGE/ACTION → handler(tpcb), cleanup
 *        ROUTE_FORM        → pre-parse Content-Length + body, generic setup,
 *                            multi-packet via process_form_upload_chunk()
 *        ROUTE_BINARY      → pre-parse, handler(tpcb, cl, body, body_len),
 *                            multi-packet via ota_stage_data()
 *
 * send_response(tpcb, body) → send_next_chunk() → tcp_write() in 1024-byte chunks
 *
 * Binary uploads (firmware, logo) stream to flash via OTA state machine
 * (firmware_update.c) — no full buffering in RAM.
 *
 * ==============================================================================
 */

#include "webserver.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "boot_input.h"
#include "captive_portal.h"
#include "debug.h"
#include "epaper.h"
#include "epaper_pages_shared.h"
#include "epaper_render.h"
#include "flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "led.h"
#include "lwip/tcp.h"
#include "morse.h"
#include "nfc_ndef.h"
#include "ota.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/time.h"
#include "power.h"
#include "rtc.h"
#include "sensors.h"
#include "use_case.h"
#include "version.h"
#include "web_clock.h"
#include "web_device_settings.h"
#include "web_export_settings.h"
#include "web_firmware.h"
#include "web_landing.h"
#include "web_logo.h"
#include "web_status.h"
#include "web_wifi.h"
#include "webserver_utils.h"
#include "wifi.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCP_CHUNK_SIZE 1024

// Global upload session instance
upload_session_t upload_session = {0};

bool webserver_upload_in_progress(void) { return upload_session.active; }

bool webserver_firmware_upload_active(void) {
    return upload_session.active && upload_session.type == UPLOAD_FIRMWARE;
}

// Shared page buffer — safe: lwIP callbacks are single-threaded
static char s_page_buf[16384];

// =============================================================================
// TRANSPORT
// =============================================================================

typedef struct {
    const char *ptr;
    size_t remaining;
    int chunk_index;
    char *body_copy;
} response_state_t;

static err_t send_next_chunk(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    response_state_t *state = (response_state_t *)arg;

    if (state->remaining == 0) {
        dlog("send_next_chunk: transfer completed.\n");
        tcp_output(tpcb);
        tcp_sent(tpcb, NULL);
        tcp_arg(tpcb, NULL);
        free(state->body_copy);
        free(state);
        tcp_close(tpcb);
        return ERR_OK;
    }

    u16_t chunk =
        (state->remaining > (size_t)TCP_CHUNK_SIZE) ? TCP_CHUNK_SIZE : (u16_t)state->remaining;
    err_t err = tcp_write(tpcb, state->ptr, chunk, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        dlog("send_next_chunk: tcp_write chunk %d failed at %zu bytes remaining: err=%d\n",
             state->chunk_index, state->remaining, err);
        free(state->body_copy);
        free(state);
        return err;
    }

    dlog("send_next_chunk: chunk %d (%u bytes) written, %zu remaining\n", state->chunk_index, chunk,
         state->remaining - chunk);

    state->ptr += chunk;
    state->remaining -= chunk;
    state->chunk_index++;

    tcp_output(tpcb);
    return ERR_OK;
}

static void send_response_with_content_type_and_disposition(struct tcp_pcb *tpcb, const char *body,
                                                            const char *content_type,
                                                            const char *content_disposition) {
    size_t body_len = strlen(body);

    char *body_copy = malloc(body_len + 1);
    if (!body_copy)
        return;
    memcpy(body_copy, body, body_len + 1);

    char header[512];
    int header_len = 0;
    if (content_disposition && content_disposition[0] != '\0') {
        header_len = snprintf(
            header, sizeof(header),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Disposition: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            (content_type && content_type[0] != '\0') ? content_type : "text/html; charset=UTF-8",
            content_disposition, body_len);
    } else {
        header_len = snprintf(
            header, sizeof(header),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            (content_type && content_type[0] != '\0') ? content_type : "text/html; charset=UTF-8",
            body_len);
    }

    err_t err = tcp_write(tpcb, header, header_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        dlog("send_response: tcp_write(header) failed with code %d\n", err);
        free(body_copy);
        return;
    }

    response_state_t *state = malloc(sizeof(response_state_t));
    if (!state) {
        dlog("send_response: malloc failed for state\n");
        free(body_copy);
        return;
    }

    state->ptr = body_copy;
    state->remaining = body_len;
    state->chunk_index = 0;
    state->body_copy = body_copy;

    tcp_arg(tpcb, state);
    tcp_sent(tpcb, send_next_chunk);
    send_next_chunk(state, tpcb, 0);
}

static void send_response_with_content_type(struct tcp_pcb *tpcb, const char *body,
                                            const char *content_type) {
    send_response_with_content_type_and_disposition(tpcb, body, content_type, NULL);
}

void send_response(struct tcp_pcb *tpcb, const char *body) {
    send_response_with_content_type(tpcb, body, "text/html; charset=UTF-8");
}

typedef struct {
    const uint8_t *ptr;
    size_t remaining;
} bin_response_state_t;

static err_t send_next_binary_chunk(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    bin_response_state_t *st = (bin_response_state_t *)arg;
    if (st->remaining == 0) {
        free(st);
        return ERR_OK;
    }
    u16_t chunk = st->remaining > TCP_CHUNK_SIZE ? TCP_CHUNK_SIZE : (u16_t)st->remaining;
    err_t err = tcp_write(tpcb, st->ptr, chunk, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
        return err;
    st->ptr += chunk;
    st->remaining -= chunk;
    tcp_output(tpcb);
    return ERR_OK;
}

void send_binary_response(struct tcp_pcb *tpcb, const char *content_type, const uint8_t *data,
                          size_t len) {
    char header[160];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.0 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %u\r\n"
                              "Connection: close\r\n\r\n",
                              content_type, (unsigned)len);
    if (tcp_write(tpcb, header, header_len, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return;
    bin_response_state_t *st = malloc(sizeof(bin_response_state_t));
    if (!st)
        return;
    st->ptr = data;
    st->remaining = len;
    tcp_arg(tpcb, st);
    tcp_sent(tpcb, send_next_binary_chunk);
    send_next_binary_chunk(st, tpcb, 0);
}

// =============================================================================
// REQUEST PARSING
// =============================================================================

static int parse_content_length(void) {
    const char *cl = strstr(upload_session.header_buffer, "Content-Length:");
    if (!cl)
        return -1;
    return atoi(cl + 15);
}

static const char *find_http_body(size_t *out_len) {
    const char *body = strstr(upload_session.header_buffer, "\r\n\r\n");
    if (!body)
        return NULL;
    body += 4;
    *out_len = upload_session.header_length - (size_t)(body - upload_session.header_buffer);
    return body;
}

static void cleanup_and_return(struct tcp_pcb *tpcb, struct pbuf *p, int copied) {
    tcp_recved(tpcb, copied);
    upload_session.header_complete = false;
    upload_session.header_length = 0;
    pbuf_free(p);
}

// =============================================================================
// FORM HANDLING
// =============================================================================

static const char *upload_type_label(upload_type_t type) {
    switch (type) {
    case UPLOAD_FORM_WIFI:
        return "WIFI";
    case UPLOAD_FORM_CLOCK:
        return "CLOCK";
    case UPLOAD_FORM_DEVICE:
        return "DEVICE";
    case UPLOAD_FORM_USECASE:
        return use_case.name;
    case UPLOAD_FORM_SETTINGS_IMPORT:
        return "SETTINGS";
    case UPLOAD_LOGO:
        return "LOGO";
    case UPLOAD_FIRMWARE:
        return "FIRMWARE";
    default:
        return "UNKNOWN";
    }
}

static void send_form_error(struct tcp_pcb *tpcb, upload_type_t type, const char *msg) {
    switch (type) {
    case UPLOAD_FORM_WIFI:
        build_wifi_config_page(s_page_buf, sizeof(s_page_buf), msg);
        break;
    case UPLOAD_FORM_CLOCK:
        build_clock_page(s_page_buf, sizeof(s_page_buf), msg);
        break;
    case UPLOAD_FORM_DEVICE:
        build_device_config_page(s_page_buf, sizeof(s_page_buf), msg);
        break;
    case UPLOAD_FORM_USECASE:
        if (use_case.render_config_page)
            use_case.render_config_page(s_page_buf, sizeof(s_page_buf), msg);
        break;
    case UPLOAD_FORM_SETTINGS_IMPORT:
        build_settings_transfer_page(s_page_buf, sizeof(s_page_buf), msg);
        break;
    default:
        return;
    }
    send_response(tpcb, s_page_buf);
}

// Dispatch a completed form to its handler and reset the session
static void dispatch_completed_form(struct tcp_pcb *tpcb) {
    upload_session.form_buffer[upload_session.expected_length] = '\0';

    switch (upload_session.type) {
    case UPLOAD_FORM_WIFI:
        handle_form_wifi(upload_session.form_buffer, upload_session.expected_length, s_page_buf,
                         sizeof(s_page_buf));
        send_response(tpcb, s_page_buf);
        break;
    case UPLOAD_FORM_USECASE: {
        const char *flash_msg = NULL;
        if (use_case.handle_config_form)
            flash_msg = use_case.handle_config_form(upload_session.form_buffer,
                                                    upload_session.expected_length);
        if (use_case.render_config_page) {
            use_case.render_config_page(s_page_buf, sizeof(s_page_buf), flash_msg ? flash_msg : "");
            send_response(tpcb, s_page_buf);
        }
        break;
    }
    case UPLOAD_FORM_DEVICE:
        handle_form_device_config(upload_session.form_buffer, upload_session.expected_length,
                                  s_page_buf, sizeof(s_page_buf));
        send_response(tpcb, s_page_buf);
        break;
    case UPLOAD_FORM_CLOCK:
        handle_form_clock(upload_session.form_buffer, upload_session.expected_length, s_page_buf,
                          sizeof(s_page_buf));
        send_response(tpcb, s_page_buf);
        break;
    case UPLOAD_FORM_SETTINGS_IMPORT:
        handle_form_settings_import(upload_session.form_buffer, upload_session.expected_length,
                                    s_page_buf, sizeof(s_page_buf));
        send_response(tpcb, s_page_buf);
        break;
    default:
        dlog("Unknown form type: %d\n", upload_session.type);
        break;
    }

    upload_session.active = false;
    upload_session.header_complete = false;
    upload_session.header_length = 0;
}

// Accumulate form chunks from subsequent recv_cb calls
static bool process_form_upload_chunk(const char *buffer, int copied, struct tcp_pcb *tpcb) {
    size_t to_copy = copied;
    if (upload_session.total_received + to_copy > upload_session.expected_length)
        to_copy = upload_session.expected_length - upload_session.total_received;

    memcpy(upload_session.form_buffer + upload_session.total_received, buffer, to_copy);
    upload_session.total_received += to_copy;
    tcp_recved(tpcb, copied);

    if (upload_session.total_received >= upload_session.expected_length) {
        dispatch_completed_form(tpcb);
        return true;
    }
    return false;
}

// =============================================================================
// ROUTE TABLE & HANDLERS
// =============================================================================

// --- Shutdown ---

static absolute_time_t shutdown_time = {0};

static int64_t shutdown_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;
    dlog("Shutdown callback invoked\n");
    power_shutdown_default();
    // watchdog_reboot(0, 0, 0);
    return 0;
}

void webserver_set_shutdown_time(absolute_time_t t) { shutdown_time = t; }

void add_timeout_info(char *buf, size_t buf_size) {
    uint64_t now_us = to_us_since_boot(get_absolute_time());
    uint64_t target_us = to_us_since_boot(shutdown_time);

    if (target_us > now_us) {
        uint64_t diff_us = target_us - now_us;
        int seconds = (int)(diff_us / 1000000ULL);
        int minutes = seconds / 60;
        seconds %= 60;
        snprintf(buf, buf_size, "<small>Remaining time before shutdown: %d:%02d minutes</small>",
                 minutes, seconds);
    } else {
        snprintf(buf, buf_size, "<small>Setup period expired</small>");
    }
}

static void handle_shutdown_route(struct tcp_pcb *tpcb) {
    static bool shutdown_triggered = false;
    if (shutdown_triggered) {
        dlog("Shutdown already in progress, ignoring\n");
        return;
    }

    shutdown_triggered = true;
    dlog("GET /shutdown called - redirecting and shutting down\n");

    send_response(tpcb, "<!DOCTYPE html><html><head>"
                        "<meta charset=\"UTF-8\">"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                        "<title>Rebooting</title>"
                        "<style>"
                        "body { font-family: sans-serif; text-align: center; padding: 2em; }"
                        "h1 { font-size: 1.5em; color: #333; }"
                        "p { font-size: 1em; color: green; }"
                        "</style></head><body>"
                        "<h1>✔️ Rebooting...</h1>"
                        "</body></html>");

    tcp_output(tpcb);
    add_alarm_in_ms(600, shutdown_callback, NULL, false);
}

// --- Page wrappers ---
// Adapt build_*(buf, sz, ...) to void (*)(tpcb) for the route table.
// To add a GET route: add a wrapper here, then one line in routes[] below.

static void send_landing_page_wrapper(struct tcp_pcb *tpcb) {
    build_landing_page(s_page_buf, sizeof(s_page_buf));
    send_response(tpcb, s_page_buf);
}

static void send_wifi_config_page_wrapper(struct tcp_pcb *tpcb) {
    build_wifi_config_page(s_page_buf, sizeof(s_page_buf), "");
    send_response(tpcb, s_page_buf);
}

static void send_usecase_config_page_wrapper(struct tcp_pcb *tpcb) {
    if (use_case.render_config_page) {
        use_case.render_config_page(s_page_buf, sizeof(s_page_buf), "");
        send_response(tpcb, s_page_buf);
    } else {
        send_response(tpcb, "<html><body><p>No config page for this use case.</p></body></html>");
    }
}

static void send_device_config_page_wrapper(struct tcp_pcb *tpcb) {
    build_device_config_page(s_page_buf, sizeof(s_page_buf), "");
    send_response(tpcb, s_page_buf);
}

static void send_clock_page_wrapper(struct tcp_pcb *tpcb) {
    build_clock_page(s_page_buf, sizeof(s_page_buf), "");
    send_response(tpcb, s_page_buf);
}

static void send_device_status_page_wrapper(struct tcp_pcb *tpcb) {
    build_device_status_page(s_page_buf, sizeof(s_page_buf));
    send_response(tpcb, s_page_buf);
}

static void send_upload_logo_page_wrapper(struct tcp_pcb *tpcb) {
    build_upload_logo_page(s_page_buf, sizeof(s_page_buf), "");
    send_response(tpcb, s_page_buf);
}

static void send_firmware_update_page_wrapper(struct tcp_pcb *tpcb) {
    build_firmware_update_page(s_page_buf, sizeof(s_page_buf), "");
    send_response(tpcb, s_page_buf);
}

static void send_settings_transfer_page_wrapper(struct tcp_pcb *tpcb) {
    build_settings_transfer_page(s_page_buf, sizeof(s_page_buf), "");
    send_response(tpcb, s_page_buf);
}

static void send_settings_export_txt_wrapper(struct tcp_pcb *tpcb) {
    char disposition[128];
    build_settings_export_txt(s_page_buf, sizeof(s_page_buf), disposition, sizeof(disposition));
    send_response_with_content_type_and_disposition(tpcb, s_page_buf, "text/plain; charset=UTF-8",
                                                    disposition);
}

// --- Route table ---

static const route_t routes[] = {
    // GET — page handlers
    {"/", HTTP_GET, ROUTE_PAGE, {.page = send_landing_page_wrapper}},
    {"/wifi", HTTP_GET, ROUTE_PAGE, {.page = send_wifi_config_page_wrapper}},
    {"/device_settings", HTTP_GET, ROUTE_PAGE, {.page = send_device_config_page_wrapper}},
    {"/clock", HTTP_GET, ROUTE_PAGE, {.page = send_clock_page_wrapper}},
    {"/device_status", HTTP_GET, ROUTE_PAGE, {.page = send_device_status_page_wrapper}},
    {"/upload_logo", HTTP_GET, ROUTE_PAGE, {.page = send_upload_logo_page_wrapper}},
    {"/firmware_update", HTTP_GET, ROUTE_PAGE, {.page = send_firmware_update_page_wrapper}},
    {"/settings_transfer", HTTP_GET, ROUTE_PAGE, {.page = send_settings_transfer_page_wrapper}},
    {"/settings_export.txt", HTTP_GET, ROUTE_PAGE, {.page = send_settings_export_txt_wrapper}},

    // GET — action handlers
    {"/logo", HTTP_GET, ROUTE_ACTION, {.page = web_serve_logo}},
    {"/shutdown", HTTP_GET, ROUTE_ACTION, {.page = handle_shutdown_route}},

    // POST — form uploads (dispatched generically by recv_cb)
    {"/wifi", HTTP_POST, ROUTE_FORM, {.form_type = UPLOAD_FORM_WIFI}},
    {"/device_config", HTTP_POST, ROUTE_FORM, {.form_type = UPLOAD_FORM_DEVICE}},
    {"/clock", HTTP_POST, ROUTE_FORM, {.form_type = UPLOAD_FORM_CLOCK}},
    {"/settings_import", HTTP_POST, ROUTE_FORM, {.form_type = UPLOAD_FORM_SETTINGS_IMPORT}},

    // POST — binary uploads (handler receives pre-parsed content_length + body)
    {"/upload_logo", HTTP_POST, ROUTE_BINARY, {.binary = web_logo_upload}},
    {"/firmware_update", HTTP_POST, ROUTE_BINARY, {.binary = web_firmware_upload}},

    // POST — actions
    {"/delete_logo", HTTP_POST, ROUTE_ACTION, {.page = web_delete_logo}},
    {"/firmware_demote_active", HTTP_POST, ROUTE_ACTION, {.page = web_firmware_demote_active}},
};

static const size_t num_routes = sizeof(routes) / sizeof(routes[0]);

// Route matching — static table first, then dynamic use-case route
static const route_t *find_route(const char *path, http_method_t method) {
    for (size_t i = 0; i < num_routes; i++) {
        if (routes[i].method == method && strcmp(routes[i].path, path) == 0)
            return &routes[i];
    }

    // Dynamic use-case route: "/<use_case.name>" for GET and POST
    static char uc_path[32] = "";
    static route_t uc_get_route;
    static route_t uc_post_route;
    if (!uc_path[0]) {
        snprintf(uc_path, sizeof(uc_path), "/%s", use_case.name);
        uc_get_route =
            (route_t){uc_path, HTTP_GET, ROUTE_PAGE, {.page = send_usecase_config_page_wrapper}};
        uc_post_route =
            (route_t){uc_path, HTTP_POST, ROUTE_FORM, {.form_type = UPLOAD_FORM_USECASE}};
    }
    if (strcmp(path, uc_path) == 0) {
        if (method == HTTP_GET)
            return &uc_get_route;
        if (method == HTTP_POST)
            return &uc_post_route;
    }

    // Use-case extra routes
    for (size_t i = 0; i < use_case.extra_route_count; i++) {
        if (use_case.extra_routes[i].method == method &&
            strcmp(use_case.extra_routes[i].path, path) == 0)
            return &use_case.extra_routes[i];
    }

    return NULL;
}

// =============================================================================
// MAIN HTTP REQUEST HANDLER
// =============================================================================

/*
 * recv_cb():
 * ├── Connection check
 * ├── Header collection phase
 * │   ├── Buffer header data until \r\n\r\n
 * │   ├── Parse HTTP request line (method + path)
 * │   └── Route dispatch:
 * │        ├── ROUTE_PAGE / ROUTE_ACTION → call handler, cleanup
 * │        ├── ROUTE_FORM → pre-parse Content-Length + body, generic form setup
 * │        └── ROUTE_BINARY → pre-parse, call handler with parsed data
 * ├── Continuation: binary upload chunks → ota_stage_data()
 * └── Continuation: form upload chunks → process_form_upload_chunk()
 */
// Setup mode leads through two steps on the panel (see setup_screen_tick). Two facts tie the
// panel to the web server:
// - Once a web request has been seen, the panel is not touched again until setup ends.
// - While the panel refreshes, which blocks the main loop for seconds, no upload may start.
// Together they keep a refresh and an upload from ever sharing the sign.
static volatile bool s_request_seen;
static volatile bool s_screen_busy;

static err_t recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    // There is one upload session for the whole server. While it is in use, a second
    // connection (a phone probing for internet access, a browser fetching an icon) must not
    // be read as more data of the upload: it is turned away.
    static const struct tcp_pcb *session_pcb;
    const bool session_in_use =
        upload_session.active || upload_session.header_complete || upload_session.header_length > 0;
    if (!p) {
        if (tpcb == session_pcb && !upload_session.active) {
            upload_session.header_complete = false;
            upload_session.header_length = 0;
        }
        tcp_close(tpcb);
        return ERR_OK;
    }
    if (!session_in_use) {
        session_pcb = tpcb;
    } else if (tpcb != session_pcb) {
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        tcp_close(tpcb);
        return ERR_OK;
    }

    char buffer[1501];
    int copied = pbuf_copy_partial(p, buffer, sizeof(buffer) - 1, 0);
    if (copied < 0)
        copied = 0;
    buffer[copied] = '\0';

    // --- Header collection phase ---
    if (!upload_session.header_complete) {
        if (upload_session.header_length + copied >= sizeof(upload_session.header_buffer)) {
            dlog("HEADER: buffer overflow\n");
            tcp_recved(tpcb, copied);
            pbuf_free(p);
            return ERR_OK;
        }

        memcpy(upload_session.header_buffer + upload_session.header_length, buffer, copied);
        upload_session.header_length += copied;
        upload_session.header_buffer[upload_session.header_length] = '\0';

        dlog("HEADER: collected %d bytes, total %d\n", copied, (int)upload_session.header_length);

        if (!strstr(upload_session.header_buffer, "\r\n\r\n")) {
            tcp_recved(tpcb, copied);
            pbuf_free(p);
            return ERR_OK;
        }

        upload_session.header_complete = true;

        // Parse first request line
        const char *eol = strstr(upload_session.header_buffer, "\r\n");
        if (!eol) {
            dlog("HEADER: malformed - no CRLF\n");
            cleanup_and_return(tpcb, p, copied);
            return ERR_OK;
        }

        size_t line_len = eol - upload_session.header_buffer;
        if (line_len >= 128)
            line_len = 127;
        char first_line[128];
        memcpy(first_line, upload_session.header_buffer, line_len);
        first_line[line_len] = '\0';

        dlog("HEADER LINE: %s\n", first_line);

        char method_str[8], path_str[64];
        if (sscanf(first_line, "%7s %63s", method_str, path_str) != 2) {
            dlog("Malformed request: %s\n", first_line);
            cleanup_and_return(tpcb, p, copied);
            return ERR_OK;
        }

        http_method_t method = (strcmp(method_str, "GET") == 0)    ? HTTP_GET
                               : (strcmp(method_str, "POST") == 0) ? HTTP_POST
                                                                   : -1;
        dlog("Web: %s %s\n", method_str, path_str);
        if (!s_request_seen) {
            s_request_seen = true;
            debug_status("OK", "Setup: first web request, the display stays as it is\n");
        }
        if (method == HTTP_POST && s_screen_busy) {
            // Nothing may be stored while the panel refreshes: turn the request away whole,
            // including the body that is still on its way.
            dlog("Web: display is refreshing, %s turned away\n", path_str);
            send_response(tpcb, "<p>Tinta is refreshing its display. Try again in a moment.</p>");
            cleanup_and_return(tpcb, p, copied);
            tcp_recv(tpcb, NULL);
            tcp_close(tpcb);
            return ERR_OK;
        }
        if (method == (http_method_t)-1) {
            dlog("Unsupported method: %s\n", method_str);
            cleanup_and_return(tpcb, p, copied);
            return ERR_OK;
        }

        const route_t *route = find_route(path_str, method);
        if (!route) {
            dlog("Route not found: %s %s\n", method_str, path_str);
            cleanup_and_return(tpcb, p, copied);
            return ERR_OK;
        }

        dlog("ROUTE: %s %s (type=%d)\n", method_str, path_str, route->type);

        // --- Dispatch by route type ---
        switch (route->type) {
        case ROUTE_PAGE:
        case ROUTE_ACTION:
            route->handler.page(tpcb);
            cleanup_and_return(tpcb, p, copied);
            return ERR_OK;

        case ROUTE_FORM: {
            // Generic form POST: pre-parse once, set up session, copy initial body
            upload_type_t form_type = route->handler.form_type;
            int cl = parse_content_length();
            if (cl < 0) {
                dlog("UPLOAD %s: Content-Length missing\n", upload_type_label(form_type));
                send_form_error(tpcb, form_type, "Content-Length missing");
                tcp_close(tpcb);
                cleanup_and_return(tpcb, p, copied);
                return ERR_OK;
            }
            if ((size_t)cl >= sizeof(upload_session.form_buffer)) {
                dlog("UPLOAD %s: form body too large (%d)\n", upload_type_label(form_type), cl);
                send_form_error(tpcb, form_type, "Form data too large");
                tcp_close(tpcb);
                cleanup_and_return(tpcb, p, copied);
                return ERR_OK;
            }

            upload_session.active = true;
            upload_session.total_received = 0;
            upload_session.expected_length = (size_t)cl;
            upload_session.type = form_type;

            size_t body_len = 0;
            const char *body = find_http_body(&body_len);
            if (body) {
                memcpy(upload_session.form_buffer, body, body_len);
                upload_session.total_received = body_len;
                tcp_recved(tpcb, copied);
                dlog("UPLOAD %s: first body chunk (%d bytes)\n", upload_type_label(form_type),
                     (int)body_len);
                if (upload_session.total_received >= upload_session.expected_length)
                    dispatch_completed_form(tpcb);
            } else {
                dlog("UPLOAD %s: body not found in header\n", upload_type_label(form_type));
                send_form_error(tpcb, form_type, "Form parse error");
                tcp_close(tpcb);
            }
            break;
        }

        case ROUTE_BINARY: {
            // Pre-parse Content-Length and body, pass to handler
            int cl = parse_content_length();
            size_t body_len = 0;
            const char *body = find_http_body(&body_len);
            route->handler.binary(tpcb, cl, body, body_len);
            break;
        }
        }

        // --- Continuation: subsequent chunks for active uploads ---
    } else if (upload_session.active &&
               (upload_session.type == UPLOAD_LOGO || upload_session.type == UPLOAD_FIRMWARE)) {
        ota_stage_data(buffer, (size_t)copied);
    } else if (upload_session.active) {
        process_form_upload_chunk(buffer, copied, tpcb);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, recv_cb);
    return ERR_OK;
}

// =============================================================================
// WEBSERVER SETUP & INITIALIZATION
// =============================================================================

static void start_setup_webserver(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
        return;

    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    tcp_accept(pcb, accept_cb);
}

// =============================================================================
// SETUP SCREEN
// =============================================================================

typedef struct {
    bool open_shown;        // the panel shows step 2 (open the settings page), not step 1 (join)
    absolute_time_t t_poll; // next look at who has joined
    absolute_time_t t_left; // since when nobody is joined, while step 2 is shown
} setup_screen_t;

#define SETUP_SCREEN_POLL_MS 1000
#define SETUP_SCREEN_BACK_MS 10000 // step 2 without anyone joined, before step 1 returns

// Draws a setup page unless the web server is in use. The check and the claim happen under
// the lwIP lock, so no request can slip in between them.
static bool setup_screen_show(bool open) {
    cyw43_arch_lwip_begin();
    const bool server_idle = !s_request_seen && ota_is_idle() && !upload_session.active &&
                             !upload_session.header_complete && upload_session.header_length == 0;
    if (server_idle)
        s_screen_busy = true;
    cyw43_arch_lwip_end();
    if (!server_idle)
        return false;

    uint8_t *image = epaper_init(false);
    if (image) {
        if (open)
            render_page_setup_open(image, captive_client_name());
        else
            render_page_setup_join(image);
        epaper_flush_and_sleep(image);
    }
    s_screen_busy = false;
    return true;
}

// The LED keeps pace with the panel: the Morse message runs twice as fast once a device has
// joined, so the step can be seen from across the room too.
static void setup_led_step(bool open) {
#if LED_MORSE_ENABLED
    morse_set_unit_ms(open ? LED_MORSE_UNIT_MS / 2 : LED_MORSE_UNIT_MS);
#else
    (void)open;
#endif
}

// Step 1 until a device has an address, then step 2; back to step 1 if it leaves without ever
// asking for a page. From the first web request on the panel stays as it is. Joining alone is
// no proof: a phone with the wrong password is listed until its handshake fails, but only one
// with the right password can ask for an address.
static void setup_screen_tick(setup_screen_t *s) {
    if (s_request_seen || !time_reached(s->t_poll))
        return;
    s->t_poll = make_timeout_time_ms(SETUP_SCREEN_POLL_MS);

    const bool joined = wifi_ap_client_count() > 0;
    if (!s->open_shown) {
        if (!joined || !captive_client_name())
            return;
        if (!setup_screen_show(true))
            return;
        s->open_shown = true;
        s->t_left = at_the_end_of_time;
        setup_led_step(true);
        debug_status("OK", "Setup: device joined, showing how to open the settings page\n");
        // The tag now leads to the settings page too. The next normal run puts the address
        // of its page back (nfc_ndef_offer_url).
        if (nfc_ndef_set_url(SETUP_PAGE_URL) == NFC_NDEF_FAILED)
            debug_status("INFO", "Setup: settings address not written to the tag\n");
    } else if (joined) {
        s->t_left = at_the_end_of_time;
    } else if (is_at_the_end_of_time(s->t_left)) {
        s->t_left = make_timeout_time_ms(SETUP_SCREEN_BACK_MS);
    } else if (time_reached(s->t_left) && setup_screen_show(false)) {
        s->open_shown = false;
        setup_led_step(false);
        captive_forget_client();
        debug_status("INFO", "Setup: device left, showing how to join again\n");
    }
}

// =============================================================================
// LEAVING SETUP BY BUTTON
// =============================================================================

// Any button ends the setup, as the timeout does. The button that started it counts only
// once it has been let go, and nothing happens while an upload is being stored.
typedef struct {
    bool released; // all buttons have been up once since setup began
    int held;      // consecutive loop passes with a button down
} setup_exit_t;

#define SETUP_EXIT_HELD_PASSES 2 // about 100 ms at the loop's 50 ms pace

static bool setup_exit_requested(setup_exit_t *e, bool upload_running) {
    const bool down = any_button_down();
    if (!e->released) {
        e->released = !down;
        return false;
    }
    if (!down || upload_running) {
        e->held = 0;
        return false;
    }
    return ++e->held >= SETUP_EXIT_HELD_PASSES;
}

// =============================================================================
// webserver_run() — setup mode event loop
// Caller must have called wifi_ap_start() before this.
// Returns when the setup timeout expires; caller handles shutdown.
// =============================================================================

void webserver_run(void) {
    // Setup mode preamble (owns full setup mode lifecycle)
    debug_flush();
    set_debug_mode(DEBUG_REALTIME);
    led_set_enabled(true);
    rtc_disable_alarm();

    read_boot_battery_voltage(device_config_flash.data.conversion_factor);
    uint8_t *setup_image = epaper_init(false);
    if (setup_image != NULL) {
        render_page_setup_join(setup_image);
        epaper_flush_and_sleep(setup_image);
    }

    debug_info("WiFi setup mode: initializing...\n");

    if (!wifi_ap_start()) {
        debug_flush();
        power_shutdown_default();
    }

    absolute_time_t t_shutdown = make_timeout_time_ms(WIFI_SETUP_TIMEOUT_MS);
    webserver_set_shutdown_time(t_shutdown);

    start_setup_webserver();
    captive_start_dhcp();
    captive_start_dns();

    // Without a panel this line is the only place the password shows up.
    debug_status("OK", "AP active: SSID=%s  password=%s  IP=192.168.4.1\n", wifi_setup_ssid(),
                 wifi_setup_password());
    // The boot lines went out before a terminal could attach: repeat what is running.
    debug_status("OK", "Firmware: %s %s\n", program_name, version);
    flash_log_status();
    if (!wifi_has_network())
        debug_status("INFO", "Setup: no home network stored, every wake starts setup\n");

#if LED_MORSE_ENABLED
    morse_set_unit_ms(LED_MORSE_UNIT_MS);
    morse_set_message("TINTA");
    morse_set_enabled(true);
#else
#if LED_USE_EXT
    ext_led_on();
#endif
#endif

    absolute_time_t t_watchdog = get_absolute_time();
    bool led_upload_active = false;
    setup_screen_t screen = {.t_poll = get_absolute_time(), .t_left = at_the_end_of_time};
    captive_forget_client();
    s_request_seen = false;
    setup_exit_t exit_by_button = {0};

    while (true) {
        bool ota_active = !ota_is_idle();

        wifi_poll();

        webserver_ota_tick();
        setup_screen_tick(&screen);

        // LED: solid during OTA upload, Morse otherwise
#if LED_MORSE_ENABLED
        if (ota_active && !led_upload_active) {
            morse_set_enabled(false);
            board_led_on();
#if LED_USE_EXT
            ext_led_on();
#endif
            led_upload_active = true;
        } else if (!ota_active && led_upload_active) {
            board_led_off();
            morse_set_enabled(true);
            led_upload_active = false;
        }
        if (!ota_active)
            morse_tick();
#else
        if (ota_active && !led_upload_active) {
            board_led_on();
            led_upload_active = true;
        } else if (!ota_active && led_upload_active) {
            board_led_off();
            led_upload_active = false;
        }
#endif

        // Watchdog: feed every 2s during idle; ota_tick feeds it during flash ops
        if (absolute_time_diff_us(get_absolute_time(), t_watchdog) < -2000000) {
            watchdog_update();
            t_watchdog = get_absolute_time();
        }

        if (setup_exit_requested(&exit_by_button, ota_active || upload_session.active)) {
            debug_status("OK", "Setup: button pressed, leaving setup\n");
            wifi_deinit();
            return;
        }

        // Shutdown timeout
        if (absolute_time_diff_us(get_absolute_time(), t_shutdown) < 0) {
            debug_status("WARN", "Setup timeout — shutting down\n");
            wifi_deinit();
            return; // caller calls debug_flush() + power_shutdown_default()
        }

        if (!ota_active)
            sleep_ms(50); // yield to CYW43 background; skip during OTA (ota_tick uses sleep_ms)
    }
}
