#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "historian/web_config.h"
#define LOG_MODULE LOG_MOD_HISTORIAN
#include "debug.h"
#include "epaper_pages_shared.h"
#include "hardware/watchdog.h"
#include "historian/client.h"
#include "historian/config.h"
#include "historian/epaper_pages.h"
#include "historian/historian_flash.h"
#include "http_client.h"
#include "lwip/ip_addr.h"
#include "st25_io.h"
#include "use_case.h"

static bool historian_parse_timeseries(const char *json, TimeSeries *result);

// Historian data storage (shared with historian/epaper_pages.c)
TimeSeries historian_data = {0};
static run_error_t s_last_run_error = RUN_OK;

// Callback for Historian HTTP response
static void historian_data_received(const char *json_data, size_t length, void *arg) {
    if (!json_data || length == 0) {
        s_last_run_error = RUN_ERROR_CONNECTION;
        debug_status("ERROR", "Historian: transfer failed or incomplete\n");
        return;
    }

    dlog("[HISTORIAN] Received %d bytes of JSON data\n", (int)length);

    // Parse JSON into TimeSeries
    if (historian_parse_timeseries(json_data, &historian_data)) {
        dlog("[HISTORIAN] Successfully parsed %d data points\n", historian_data.count);
        dlog("[HISTORIAN] Temperature range: %.2f - %.2f °C\n", historian_data.min_value,
             historian_data.max_value);

        for (int i = 0; i < historian_data.count && i < 10; i++) {
            dlog("[HISTORIAN] #%03d: timestamp=%llu ms UTC, "
                 "value=%.2f %s, state=%u\n",
                 i, (unsigned long long)historian_data.points[i].timestamp,
                 historian_data.points[i].value, historian_data.unit,
                 historian_data.points[i].state);
        }
    } else {
        s_last_run_error = RUN_ERROR_PARSE;
        debug_status("ERROR", "Historian: JSON parse failed\n");
    }
}

// Time conversions
static uint64_t historian_rtc_to_unix_ms(const rtc_time_t *rtc_time) {
    struct tm timeinfo = {0};
    timeinfo.tm_year = rtc_time->year + 100; // tm_year is years since 1900
    timeinfo.tm_mon = rtc_time->month - 1;   // tm_mon is 0-11
    timeinfo.tm_mday = rtc_time->date;
    timeinfo.tm_hour = rtc_time->hours;
    timeinfo.tm_min = rtc_time->minutes;
    timeinfo.tm_sec = rtc_time->seconds;
    timeinfo.tm_isdst = 0; // RTC has no DST info

    setenv("TZ", "CET-1", 1); // MEZ = UTC+1
    tzset();

    time_t unix_seconds = mktime(&timeinfo); // interprets as MEZ, returns UTC
    return (uint64_t)unix_seconds * 1000ULL;
}

static uint64_t historian_get_current_unix_ms(void) {
    rtc_time_t current_time;
    if (rtc_read_time(&current_time) != 0) {
        dlog("[HISTORIAN] RTC read failed, returning 0 timestamp\n");
        return 0;
    }
    return historian_rtc_to_unix_ms(&current_time);
}

static uint64_t historian_get_unix_ms_hours_ago(int hours) {
    uint64_t now_ms = historian_get_current_unix_ms();
    uint64_t hours_in_ms = (uint64_t)hours * 3600ULL * 1000ULL;
    return now_ms - hours_in_ms;
}

// HTTP request build
static int historian_build_http_request(char *buffer, size_t buffer_size, const char *host,
                                        int datapoint_id, uint64_t start_time_ms,
                                        uint64_t end_time_ms) {
    static char json_body[HTTP_JSON_BODY_MAX];
    int json_len =
        snprintf(json_body, sizeof(json_body),
                 "{"
                 "\"id\":%d,"
                 "\"method\":\"getTimeSeries\","
                 "\"params\":[%d,%llu,%llu]"
                 "}",
                 123, // Request-ID (could be dynamic)
                 datapoint_id, (unsigned long long)start_time_ms, (unsigned long long)end_time_ms);

    if (json_len >= (int)sizeof(json_body)) {
        dlog("[HISTORIAN] JSON body too large\n");
        return -1;
    }

    int request_len = snprintf(buffer, buffer_size,
                               "POST /query/jsonrpc.gy HTTP/1.0\r\n"
                               "Host: %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %d\r\n"
                               "\r\n"
                               "%s",
                               host, json_len, json_body);

    if (request_len >= (int)buffer_size) {
        dlog("[HISTORIAN] HTTP request too large\n");
        return -1;
    }

    return request_len;
}

// JSON parsing
static bool historian_parse_timeseries(const char *json, TimeSeries *result) {
    if (!json || !result)
        return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        dlog("[HISTORIAN] JSON parse failed\n");
        return false;
    }

    cJSON *res = cJSON_GetObjectItem(root, "result");
    if (!res) {
        dlog("[HISTORIAN] No 'result' in JSON\n");
        cJSON_Delete(root);
        return false;
    }

    cJSON *dataPoint = cJSON_GetObjectItem(res, "dataPoint");
    if (dataPoint) {
        cJSON *dp_id = cJSON_GetObjectItem(dataPoint, "id");
        if (dp_id) {
            cJSON *interfaceId = cJSON_GetObjectItem(dp_id, "interfaceId");
            if (interfaceId && cJSON_IsString(interfaceId)) {
                strncpy(result->name, interfaceId->valuestring, sizeof(result->name) - 1);
            }
        }

        cJSON *attributes = cJSON_GetObjectItem(dataPoint, "attributes");
        if (attributes) {
            cJSON *unit = cJSON_GetObjectItem(attributes, "unit");
            if (unit && cJSON_IsString(unit)) {
                strncpy(result->unit, unit->valuestring, sizeof(result->unit) - 1);
            }
        }
    }

    cJSON *timestamps = cJSON_GetObjectItem(res, "timestamps");
    cJSON *values = cJSON_GetObjectItem(res, "values");
    cJSON *states = cJSON_GetObjectItem(res, "states");

    if (!timestamps || !values) {
        dlog("[HISTORIAN] Missing timestamps or values\n");
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(timestamps);
    if (count > MAX_DATA_POINTS)
        count = MAX_DATA_POINTS;

    for (int i = 0; i < count; i++) {
        cJSON *ts = cJSON_GetArrayItem(timestamps, i);
        cJSON *val = cJSON_GetArrayItem(values, i);

        if (ts && val) {
            result->points[i].timestamp = (uint64_t)cJSON_GetNumberValue(ts);
            result->points[i].value = (float)cJSON_GetNumberValue(val);

            if (states) {
                cJSON *state = cJSON_GetArrayItem(states, i);
                if (state) {
                    result->points[i].state = (uint8_t)cJSON_GetNumberValue(state);
                } else {
                    result->points[i].state = 0;
                }
            } else {
                result->points[i].state = 0;
            }
        }
    }

    result->count = count;

    if (count > 0) {
        result->min_value = result->max_value = result->points[0].value;
        for (int i = 1; i < count; i++) {
            if (result->points[i].value < result->min_value)
                result->min_value = result->points[i].value;
            if (result->points[i].value > result->max_value)
                result->max_value = result->points[i].value;
        }
        result->last_value = result->points[count - 1].value;
    }

    cJSON_Delete(root);

    dlog("[HISTORIAN] Parsed %d data points (min=%.2f, max=%.2f)\n", count, result->min_value,
         result->max_value);

    return true;
}

// --- use_case_t: Historian page catalog, input map, and export ---

static const page_def_t page_historian_graph = {"historian", render_page_historian, true};
static const page_def_t page_historian_ph = {"placeholder", render_page_historian_placeholder,
                                             false};

enum {
    HIST_PAGE_GRAPH,
    HIST_PAGE_PLACEHOLDER,
    HIST_PAGE_DECISION_MAKER,
    HIST_PAGE_WIFI_SETUP,
    HIST_PAGE_NFC_TEXT,
    HIST_PAGE_NFC_IMAGE,
    HIST_PAGE_COUNT
};

static const page_def_t *hist_pages[] = {
    [HIST_PAGE_GRAPH] = &page_historian_graph,
    [HIST_PAGE_PLACEHOLDER] = &page_historian_ph,
    [HIST_PAGE_DECISION_MAKER] = &page_decision_maker,
    [HIST_PAGE_WIFI_SETUP] = &page_wifi_setup,
    [HIST_PAGE_NFC_TEXT] = &page_nfc_text,
    [HIST_PAGE_NFC_IMAGE] = &page_nfc_image,
    NULL,
};

static const input_map_entry_t hist_input_map[] = {
    {INPUT_BUTTON(0), HIST_PAGE_GRAPH},
    {INPUT_BUTTON(1), HIST_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(2), HIST_PAGE_DECISION_MAKER},
    {INPUT_BUTTON(3), HIST_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(4), PAGE_ACTION_SETUP},
    {INPUT_BUTTON(5), HIST_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(6), HIST_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(7), PAGE_ACTION_SETUP},
    {INPUT_NFC(ST25_OPCODE_PAGE_0), HIST_PAGE_GRAPH},
    {INPUT_NFC(ST25_OPCODE_REFRESH), HIST_PAGE_GRAPH},
    {INPUT_NFC(ST25_OPCODE_PAGE_2), HIST_PAGE_DECISION_MAKER},
    {INPUT_NFC(ST25_OPCODE_TEXT), HIST_PAGE_NFC_TEXT},
    {INPUT_NFC(ST25_OPCODE_DRAW_IMAGE), HIST_PAGE_NFC_IMAGE},
    {INPUT_BUTTON(16), HIST_PAGE_NFC_TEXT},
    {INPUT_MAP_END, 0},
};

// --- Lifecycle: run (fetch) and render ---

static bool historian_make_request(void) {
    static char http_request[HTTP_REQUEST_MAX];

    char historian_host[16];
    snprintf(historian_host, sizeof(historian_host), "%d.%d.%d.%d",
             historian_config_flash.data.ip[0], historian_config_flash.data.ip[1],
             historian_config_flash.data.ip[2], historian_config_flash.data.ip[3]);
    int datapoint_id = historian_config_flash.data.datapoint_id;
    int hours_back = historian_config_flash.data.hours_back;

    uint64_t end_time = historian_get_current_unix_ms();
    uint64_t start_time = historian_get_unix_ms_hours_ago(hours_back);

    int request_len = historian_build_http_request(
        http_request, sizeof(http_request), historian_host, datapoint_id, start_time, end_time);
    if (request_len < 0) {
        s_last_run_error = RUN_ERROR_CONNECTION;
        debug_status("ERROR", "Historian: request build failed\n");
        return false;
    }

    dlog("[HISTORIAN] Constructed HTTP request:\n%s\n", http_request);
    watchdog_update();

    ip_addr_t ip;
    IP4_ADDR(&ip, historian_config_flash.data.ip[0], historian_config_flash.data.ip[1],
             historian_config_flash.data.ip[2], historian_config_flash.data.ip[3]);

    if (!http_request_sync(&ip, historian_config_flash.data.port, http_request,
                           device_config_flash.data.max_wait_data_wifi * 50,
                           historian_data_received, NULL)) {
        if (s_last_run_error == RUN_OK)
            s_last_run_error = RUN_ERROR_CONNECTION;
        debug_status("ERROR", "Historian: fetch failed\n");
        return false;
    }
    return true;
}

static run_result_t hist_run(int page, int input) {
    (void)page;
    (void)input;
    s_last_run_error = RUN_OK;
    if (historian_make_request())
        return (run_result_t){.data = &historian_data};
    int ep = (s_last_run_error == RUN_ERROR_PARSE) ? PAGE_PARSE_ERROR : PAGE_HTTP_ERROR;
    return (run_result_t){.error_page = ep};
}

static void hist_render(uint8_t *image, device_caps_t caps, int page, const void *data) {
    (void)caps;
    if (page == PAGE_WIFI_ERROR) {
        debug_status("ERROR", "Historian: showing WiFi error page\n");
        render_page_error(image, "WiFi Error!", "Unable to connect to WiFi",
                          "Check WiFi settings.");
        return;
    }
    if (page == PAGE_HTTP_ERROR) {
        debug_status("ERROR", "Historian: showing server error page\n");
        render_page_error(image, "Server Error!", "Cannot reach Historian",
                          "Check IP and port in settings.");
        return;
    }
    if (page == PAGE_PARSE_ERROR) {
        debug_status("ERROR", "Historian: showing parse error page\n");
        render_page_error(image, "Data Error!", "Server response was invalid",
                          "Check Historian version.");
        return;
    }
    if (!data && page >= 0 && hist_pages[page] && hist_pages[page]->needs_wifi) {
        debug_status("ERROR", "Historian: showing generic server error page\n");
        render_page_error(image, "Server Error!", "Unable to reach the server",
                          "Check server status.");
        return;
    }

    if (page >= 0 && hist_pages[page])
        hist_pages[page]->render(image);
    else
        render_page_fallback(page, image);
}

const use_case_t use_case = {
    .name = "historian",
    .pages = hist_pages,
    .input_map = hist_input_map,
    .default_page = HIST_PAGE_GRAPH,
    .run = hist_run,
    .render = hist_render,
    .free_data = NULL,
    .render_config_page = hist_render_config_page,
    .handle_config_form = hist_handle_config_form,
    .needs_4gray = false,
    .show_display_name = true,
    .extra_routes = NULL,
    .extra_route_count = 0,
};
