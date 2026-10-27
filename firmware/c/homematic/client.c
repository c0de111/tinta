#include "homematic/client.h"
#include "homematic/web_config.h"
#define LOG_MODULE LOG_MOD_HOMEMATIC
#include "debug.h"
#include "epaper_pages_shared.h"
#include "hardware/watchdog.h"
#include "homematic/config.h"
#include "homematic/epaper_pages.h"
#include "homematic/homematic_flash.h"
#include "http_client.h"
#include "lwip/ip_addr.h"
#include "st25_io.h"
#include "use_case.h"
#include "wifi.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Forward declarations (static helpers used by sequential engine) ---

static int homematic_build_getvalue(char *buf, size_t n, const char *address, const char *key);
static int homematic_build_http_post(char *out, size_t n, const char *host, const char *xml,
                                     int xml_len);
static int homematic_build_getparamsetdesc(char *buf, size_t n, const char *address);
static int homematic_build_get_service_messages(char *buf, size_t n);

// --- Parsed data storage ---

hm_item_value_t homematic_values[HOMEMATIC_MAX_ITEMS];
static run_error_t s_last_run_error = RUN_OK;

char homematic_service_msgs[HM_MAX_SERVICE_MSGS][80];
char homematic_service_addr[HM_MAX_SERVICE_MSGS][24];
int homematic_service_count = 0;

// --- Data interpretation helpers ---

const char *derive_unit_for_key(const char *key) {
    if (!key)
        return "";
    // Temperature family
    if (strstr(key, "TEMP") || strcmp(key, "ACTUAL_TEMPERATURE") == 0 ||
        strcmp(key, "TEMPERATURE") == 0)
        return "degC";
    // Relative humidity
    if (strstr(key, "HUMID"))
        return "%";
    // Voltage/Power common keys
    if (strstr(key, "VOLT"))
        return "V";
    if (strstr(key, "POWER"))
        return "W";
    if (strstr(key, "ILLUM") || strstr(key, "BRIGHT"))
        return "lux";
    return "";
}

void summarize_addr(const char *full, char *out, size_t n) {
    if (!full || !out || n == 0) {
        return;
    }
    out[0] = 0;
    // Consider the device part before ':'
    const char *colon = strrchr(full, ':');
    const char *dev_end = colon ? colon : full + strlen(full);
    // Walk back 4 hex chars within device part
    const char *p = dev_end;
    int count = 0;
    while (p > full && count < 4) {
        p--;
        count++;
    }
    snprintf(out, n, "%.*s", count, p);
}

// --- HTTP response callback ---

// cppcheck-suppress constParameterPointer
static void homematic_data_received(const char *body, size_t length, void *arg) {
    // If arg is a valid index (sequential single-call mode), only update that index
    if ((uintptr_t)arg < HOMEMATIC_MAX_ITEMS) {
        int idx = (int)(uintptr_t)arg;
        // On failure/null body, mark fault
        if (!body || length == 0) {
            homematic_values[idx].fault = true;
            homematic_values[idx].valid = true;
            homematic_values[idx].type = HM_TYPE_NONE;
            return;
        }

        const char *p = body;
        const char *end = NULL;
        const char *q = NULL;
        // Find first </value> boundary to limit search
        end = strstr(p, "</value>");
        if (!end)
            end = body + length;

        if ((q = strstr(p, "<double>")) && q < end) {
            double v = atof(q + 8);
            homematic_values[idx].type = HM_TYPE_DOUBLE;
            homematic_values[idx].dval = v;
            homematic_values[idx].valid = true;
        } else if ((q = strstr(p, "<i4>")) && q < end) {
            int v = atoi(q + 4);
            homematic_values[idx].type = HM_TYPE_I4;
            homematic_values[idx].ival = v;
            homematic_values[idx].valid = true;
        } else if ((q = strstr(p, "<boolean>")) && q < end) {
            int v = atoi(q + 9);
            homematic_values[idx].type = HM_TYPE_BOOL;
            homematic_values[idx].bval = (v != 0);
            homematic_values[idx].valid = true;
        } else if ((q = strstr(p, "<string>")) && q < end) {
            const char *r = strstr(q, "</string>");
            size_t len = (r && r < end) ? (size_t)(r - (q + 8)) : 0;
            if (len > sizeof(homematic_values[idx].sval) - 1)
                len = sizeof(homematic_values[idx].sval) - 1;
            memcpy(homematic_values[idx].sval, q + 8, len);
            homematic_values[idx].sval[len] = 0;
            homematic_values[idx].type = HM_TYPE_STRING;
            homematic_values[idx].valid = true;
        } else if (strstr(p, "<fault>") && strstr(p, "</fault>")) {
            homematic_values[idx].fault = true;
            homematic_values[idx].valid = true;
        } else {
            // Unknown format, mark as fault
            homematic_values[idx].fault = true;
            homematic_values[idx].valid = true;
        }
        return;
    }

    // Unit updates: sentinel 0x8000 | idx, body carries unit string
    if (((uintptr_t)arg & 0x8000) && ((uintptr_t)arg & 0x7FFF) < HOMEMATIC_MAX_ITEMS) {
        int idx = (int)((uintptr_t)arg & 0x7FFF);
        size_t ul = (length < sizeof(homematic_values[idx].unit) - 1)
                        ? length
                        : sizeof(homematic_values[idx].unit) - 1;
        if (body && ul > 0) {
            memcpy(homematic_values[idx].unit, body, ul);
            homematic_values[idx].unit[ul] = 0;
            dlog("Unit stored idx=%d '%s'\n", idx, homematic_values[idx].unit);
        } else {
            homematic_values[idx].unit[0] = 0;
            dlog("Unit empty for idx=%d\n", idx);
        }
        return;
    }

    // Service messages: sentinel 0x9000
    if ((uintptr_t)arg == 0x9000) {
        homematic_service_count = 0;
        if (body && length > 0) {
            const char *p = body;
            // Expected shape per CCU: array of [address, type, <boolean>1</boolean>]
            while (homematic_service_count < HM_MAX_SERVICE_MSGS &&
                   (p = strstr(p, "<array><data><value>"))) {
                const char *addr_start = p + strlen("<array><data><value>");
                // Robustly skip any nested tags (<array><data><value>, <string>, or whitespace)
                // until plain text starts
                for (;;) {
                    while (*addr_start == ' ' || *addr_start == '\n' || *addr_start == '\r' ||
                           *addr_start == '\t')
                        addr_start++;
                    if (*addr_start != '<')
                        break;
                    const char *gt = strchr(addr_start, '>');
                    if (!gt)
                        break; // malformed; fall through
                    addr_start = gt + 1;
                }
                // Address ends at next tag open or at </string>/</value>
                const char *addr_end = strchr(addr_start, '<');
                if (!addr_end)
                    break;
                // Store address/channel as Name surrogate
                size_t alen = (size_t)(addr_end - addr_start);
                if (alen >= sizeof(homematic_service_addr[0]))
                    alen = sizeof(homematic_service_addr[0]) - 1;
                memcpy(homematic_service_addr[homematic_service_count], addr_start, alen);
                homematic_service_addr[homematic_service_count][alen] = 0;

                // Type in next <value>
                const char *type_tag = strstr(addr_end, "<value>");
                if (!type_tag)
                    break;
                const char *type_end = strstr(type_tag + 7, "</value>");
                if (!type_end)
                    break;
                size_t tlen = (size_t)(type_end - (type_tag + 7));
                char typebuf[24];
                if (tlen >= sizeof(typebuf))
                    tlen = sizeof(typebuf) - 1;
                memcpy(typebuf, type_tag + 7, tlen);
                typebuf[tlen] = 0;

                // State in next <value>
                const char *state_tag = strstr(type_end, "<value>");
                if (!state_tag)
                    break;
                bool active = false;
                const char *b = strstr(state_tag, "<boolean>");
                if (b)
                    active = (atoi(b + 9) != 0);
                else {
                    const char *i4 = strstr(state_tag, "<i4>");
                    if (i4)
                        active = (atoi(i4 + 4) != 0);
                }

                if (active) {
                    const char *msg = NULL;
                    // ASCII-safe labels (fonts lack umlauts)
                    if (!strcmp(typebuf, "UNREACH") || !strcmp(typebuf, "STICKY_UNREACH"))
                        msg = "Geraetekommunikation gestoert";
                    else if (!strcmp(typebuf, "LOW_BAT") || !strcmp(typebuf, "LOW_BAT_ALARM"))
                        msg = "Batterieladezustand gering";
                    else if (!strcmp(typebuf, "SABOTAGE"))
                        msg = "Sabotage";
                    else if (!strcmp(typebuf, "ERROR"))
                        msg = "Fehler";
                    else if (!strcmp(typebuf, "DUTYCYCLE"))
                        msg = "Duty Cycle";
                    else if (!strcmp(typebuf, "RSSI_DEVICE"))
                        msg = "Funkverbindung schwach";
                    else
                        msg = typebuf; // fallback

                    snprintf(homematic_service_msgs[homematic_service_count],
                             sizeof(homematic_service_msgs[0]), "%s", msg);
                    dlog("[HOMEMATIC] Service[%d] addr=%s type=%s -> '%s'\n",
                         homematic_service_count, homematic_service_addr[homematic_service_count],
                         typebuf, homematic_service_msgs[homematic_service_count]);
                    homematic_service_count++;
                }

                p = state_tag + 7;
            }
        }
        dlog("[HOMEMATIC] Parsed %d service messages\n", homematic_service_count);
        return;
    }

    // Batch mode (e.g., multicall): reset and fill sequentially
    for (int i = 0; i < HOMEMATIC_MAX_ITEMS; i++) {
        homematic_values[i].valid = false;
        homematic_values[i].fault = false;
        homematic_values[i].type = HM_TYPE_NONE;
    }
    if (!body || length == 0)
        return;

    const char *p = body;
    int idx = 0;
    while (idx < HOMEMATIC_MAX_ITEMS && (p = strstr(p, "<value>"))) {
        const char *end = strstr(p, "</value>");
        if (!end)
            break;
        const char *fault = strstr(p, "<fault>");
        if (fault && fault < end) {
            homematic_values[idx].fault = true;
            homematic_values[idx].valid = true;
            idx++;
            p = end + 8;
            continue;
        }
        const char *q;
        if ((q = strstr(p, "<double>")) && q < end) {
            double v = atof(q + 8);
            homematic_values[idx].type = HM_TYPE_DOUBLE;
            homematic_values[idx].dval = v;
            homematic_values[idx].valid = true;
        } else if ((q = strstr(p, "<i4>")) && q < end) {
            int v = atoi(q + 4);
            homematic_values[idx].type = HM_TYPE_I4;
            homematic_values[idx].ival = v;
            homematic_values[idx].valid = true;
        } else if ((q = strstr(p, "<boolean>")) && q < end) {
            int v = atoi(q + 9);
            homematic_values[idx].type = HM_TYPE_BOOL;
            homematic_values[idx].bval = (v != 0);
            homematic_values[idx].valid = true;
        } else if ((q = strstr(p, "<string>")) && q < end) {
            const char *r = strstr(q, "</string>");
            size_t len = (r && r < end) ? (size_t)(r - (q + 8)) : 0;
            if (len > sizeof(homematic_values[idx].sval) - 1)
                len = sizeof(homematic_values[idx].sval) - 1;
            memcpy(homematic_values[idx].sval, q + 8, len);
            homematic_values[idx].sval[len] = 0;
            homematic_values[idx].type = HM_TYPE_STRING;
            homematic_values[idx].valid = true;
        } else {
            homematic_values[idx].valid = false;
        }
        idx++;
        p = end + 8;
    }
}

// --- use_case_t: Homematic page catalog, input map, and export ---

static const page_def_t page_homematic_main = {"homematic", render_page_homematic, true};
static const page_def_t page_homematic_ph = {"placeholder", render_page_homematic_placeholder,
                                             false};

enum {
    HM_PAGE_HOMEMATIC,
    HM_PAGE_PLACEHOLDER,
    HM_PAGE_DECISION_MAKER,
    HM_PAGE_WIFI_SETUP,
    HM_PAGE_NFC_TEXT,
    HM_PAGE_NFC_IMAGE,
    HM_PAGE_COUNT
};

static const page_def_t *hm_pages[] = {
    [HM_PAGE_HOMEMATIC] = &page_homematic_main,
    [HM_PAGE_PLACEHOLDER] = &page_homematic_ph,
    [HM_PAGE_DECISION_MAKER] = &page_decision_maker,
    [HM_PAGE_WIFI_SETUP] = &page_wifi_setup,
    [HM_PAGE_NFC_TEXT] = &page_nfc_text,
    [HM_PAGE_NFC_IMAGE] = &page_nfc_image,
    NULL,
};

static const input_map_entry_t hm_input_map[] = {
    {INPUT_BUTTON(0), HM_PAGE_HOMEMATIC},
    {INPUT_BUTTON(1), HM_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(2), HM_PAGE_DECISION_MAKER},
    {INPUT_BUTTON(3), HM_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(4), PAGE_ACTION_SETUP},
    {INPUT_BUTTON(5), HM_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(6), HM_PAGE_PLACEHOLDER},
    {INPUT_BUTTON(7), PAGE_ACTION_SETUP},
    {INPUT_NFC(ST25_OPCODE_PAGE_0), HM_PAGE_HOMEMATIC},
    {INPUT_NFC(ST25_OPCODE_REFRESH), HM_PAGE_HOMEMATIC},
    {INPUT_NFC(ST25_OPCODE_PAGE_2), HM_PAGE_DECISION_MAKER},
    {INPUT_NFC(ST25_OPCODE_TEXT), HM_PAGE_NFC_TEXT},
    {INPUT_NFC(ST25_OPCODE_DRAW_IMAGE), HM_PAGE_NFC_IMAGE},
    {INPUT_BUTTON(16), HM_PAGE_NFC_TEXT},
    {INPUT_MAP_END, 0},
};

// --- Sequential HTTP engine (unit → value → service for each item) ---

typedef struct {
    homematic_config_t eff;
    uint8_t total;
    bool unit_known[HOMEMATIC_MAX_ITEMS];
    char unit[HOMEMATIC_MAX_ITEMS][8];
    enum { HM_PHASE_UNIT, HM_PHASE_VALUE, HM_PHASE_SERVICE } phase;
} homematic_seq_t;

static homematic_seq_t g_hm_seq;
static volatile bool g_hm_step_done = false;
static volatile bool g_hm_step_ok = false;

static bool homematic_issue_single(uint8_t idx);
static bool homematic_issue_unit(uint8_t idx);
static bool homematic_issue_service(void);

// cppcheck-suppress constParameterCallback
static void homematic_single_completion(const char *body, size_t length, bool success, void *arg) {
    uint8_t idx = (uint8_t)(uintptr_t)arg;

    if (g_hm_seq.phase == HM_PHASE_UNIT) {
        char unit[8] = {0};
        if (success && body && length > 0) {
            const char *key = g_hm_seq.eff.data.items[idx].key;
            const char *p = body;
            const char *hit = NULL;
            while ((p = strstr(p, "<name>"))) {
                const char *name_end = strstr(p, "</name>");
                if (!name_end)
                    break;
                size_t nlen = (size_t)(name_end - (p + 6));
                if (nlen == strlen(key) && strncmp(p + 6, key, nlen) == 0) {
                    hit = p;
                    break;
                }
                p = name_end + 7;
            }
            if (!hit && strcmp(key, "ACTUAL_TEMPERATURE") == 0) {
                const char *alias = "TEMPERATURE";
                const char *q = body;
                while ((q = strstr(q, "<name>"))) {
                    const char *name_end = strstr(q, "</name>");
                    if (!name_end)
                        break;
                    size_t nlen = (size_t)(name_end - (q + 6));
                    if (nlen == strlen(alias) && strncmp(q + 6, alias, nlen) == 0) {
                        hit = q;
                        break;
                    }
                    q = name_end + 7;
                }
                if (hit)
                    dlog("Using alias TEMPERATURE for ACTUAL_TEMPERATURE (idx=%u)\n", idx);
            }
            if (hit) {
                const char *member_end = strstr(hit, "</member>");
                if (!member_end)
                    member_end = body + length;
                const char *val_tag = strstr(hit, "<value>");
                if (val_tag && val_tag < member_end) {
                    const char *struct_tag = strstr(val_tag, "<struct>");
                    if (struct_tag && struct_tag < member_end) {
                        const char *u = strstr(struct_tag, "<name>UNIT</name>");
                        if (u && u < member_end) {
                            const char *s = strstr(u, "<string>");
                            const char *e = s ? strstr(s, "</string>") : NULL;
                            if (s && e && e > s + 8) {
                                size_t ul = (size_t)(e - (s + 8));
                                if (ul >= sizeof(unit))
                                    ul = sizeof(unit) - 1;
                                memcpy(unit, s + 8, ul);
                                unit[ul] = 0;
                            }
                        }
                    }
                }
            }
        }
        strncpy(g_hm_seq.unit[idx], unit, sizeof(g_hm_seq.unit[idx]) - 1);
        g_hm_seq.unit_known[idx] = true;
        homematic_data_received(unit, strlen(unit), (void *)(uintptr_t)(0x8000 | idx));
    } else if (g_hm_seq.phase == HM_PHASE_VALUE) {
        if (success && body && length > 0) {
            homematic_data_received(body, length, (void *)(uintptr_t)idx);
        } else {
            homematic_data_received(NULL, 0, (void *)(uintptr_t)idx);
        }
    } else if (g_hm_seq.phase == HM_PHASE_SERVICE) {
        if (success && body && length > 0) {
            homematic_data_received(body, length, (void *)(uintptr_t)0x9000);
        } else {
            homematic_data_received(NULL, 0, (void *)(uintptr_t)0x9000);
        }
    }

    g_hm_step_done = true;
    g_hm_step_ok = success && body && length > 0 && !(body && strstr(body, "<fault>"));
}

static bool homematic_issue_single(uint8_t idx) {
    static char http_request[HTTP_REQUEST_MAX];
    static char xml_body[HTTP_REQUEST_MAX];

    char addr[64];
    const char *raw = g_hm_seq.eff.data.items[idx].address;
    if (g_hm_seq.eff.data.add_interface_prefix && strncmp(raw, "HmIP-RF.", 8) != 0)
        snprintf(addr, sizeof(addr), "HmIP-RF.%s", raw);
    else
        snprintf(addr, sizeof(addr), "%s", raw);

    int xml_len = homematic_build_getvalue(xml_body, sizeof(xml_body), addr,
                                           g_hm_seq.eff.data.items[idx].key);
    if (xml_len < 0)
        return false;

    char host[16];
    snprintf(host, sizeof(host), "%d.%d.%d.%d", homematic_config_flash.data.ip[0],
             homematic_config_flash.data.ip[1], homematic_config_flash.data.ip[2],
             homematic_config_flash.data.ip[3]);
    int req_len =
        homematic_build_http_post(http_request, sizeof(http_request), host, xml_body, xml_len);
    if (req_len < 0)
        return false;

    ip_addr_t ip;
    IP4_ADDR(&ip, homematic_config_flash.data.ip[0], homematic_config_flash.data.ip[1],
             homematic_config_flash.data.ip[2], homematic_config_flash.data.ip[3]);

    http_result_t result = http_request_async(&ip, homematic_config_flash.data.port, http_request,
                                              homematic_single_completion, (void *)(uintptr_t)idx);
    return (result == HTTP_SUCCESS);
}

static bool homematic_issue_unit(uint8_t idx) {
    static char http_request[HTTP_REQUEST_MAX];
    static char xml_body[HTTP_REQUEST_MAX];

    char addr[64];
    const char *raw = g_hm_seq.eff.data.items[idx].address;
    if (g_hm_seq.eff.data.add_interface_prefix && strncmp(raw, "HmIP-RF.", 8) != 0)
        snprintf(addr, sizeof(addr), "HmIP-RF.%s", raw);
    else
        snprintf(addr, sizeof(addr), "%s", raw);

    int xml_len = homematic_build_getparamsetdesc(xml_body, sizeof(xml_body), addr);
    if (xml_len < 0)
        return false;

    char host[16];
    snprintf(host, sizeof(host), "%d.%d.%d.%d", homematic_config_flash.data.ip[0],
             homematic_config_flash.data.ip[1], homematic_config_flash.data.ip[2],
             homematic_config_flash.data.ip[3]);
    int req_len =
        homematic_build_http_post(http_request, sizeof(http_request), host, xml_body, xml_len);
    if (req_len < 0)
        return false;

    ip_addr_t ip;
    IP4_ADDR(&ip, homematic_config_flash.data.ip[0], homematic_config_flash.data.ip[1],
             homematic_config_flash.data.ip[2], homematic_config_flash.data.ip[3]);

    http_result_t result = http_request_async(&ip, homematic_config_flash.data.port, http_request,
                                              homematic_single_completion, (void *)(uintptr_t)idx);
    return (result == HTTP_SUCCESS);
}

static bool homematic_issue_service(void) {
    static char http_request[HTTP_REQUEST_MAX];
    static char xml_body[HTTP_REQUEST_MAX];

    int xml_len = homematic_build_get_service_messages(xml_body, sizeof(xml_body));
    if (xml_len < 0)
        return false;

    char host[16];
    snprintf(host, sizeof(host), "%d.%d.%d.%d", homematic_config_flash.data.ip[0],
             homematic_config_flash.data.ip[1], homematic_config_flash.data.ip[2],
             homematic_config_flash.data.ip[3]);
    int req_len =
        homematic_build_http_post(http_request, sizeof(http_request), host, xml_body, xml_len);
    if (req_len < 0)
        return false;

    ip_addr_t ip;
    IP4_ADDR(&ip, homematic_config_flash.data.ip[0], homematic_config_flash.data.ip[1],
             homematic_config_flash.data.ip[2], homematic_config_flash.data.ip[3]);

    http_result_t result = http_request_async(&ip, homematic_config_flash.data.port, http_request,
                                              homematic_single_completion, (void *)(uintptr_t)0);
    return (result == HTTP_SUCCESS);
}

// --- Lifecycle: run (fetch) and render ---

static bool hm_wait_step(int timeout_ms) {
    int waited = 0;
    while (!g_hm_step_done && waited < timeout_ms) {
        sleep_ms(50);
        watchdog_update();
        waited += 50;
    }
    bool ok = g_hm_step_done && g_hm_step_ok;
    g_hm_step_done = false;
    g_hm_step_ok = false;
    return ok;
}

static run_result_t hm_run(int page, int input) {
    (void)page;
    (void)input;
    s_last_run_error = RUN_OK;
    memset(&g_hm_seq, 0, sizeof(g_hm_seq));
    for (uint8_t i = 0;
         i < homematic_config_flash.data.count && g_hm_seq.total < HOMEMATIC_MAX_ITEMS; i++) {
        const char *a = homematic_config_flash.data.items[i].address;
        const char *k = homematic_config_flash.data.items[i].key;
        if (a && *a && k && *k) {
            g_hm_seq.eff.data.items[g_hm_seq.total] = homematic_config_flash.data.items[i];
            g_hm_seq.total++;
        }
    }
    g_hm_seq.eff.data.count = g_hm_seq.total;
    g_hm_seq.eff.data.add_interface_prefix = false;

    dlog("[HOMEMATIC] Sequential mode with %u items\n", g_hm_seq.total);

    int timeout_ms = device_config_flash.data.max_wait_data_wifi * 50;
    bool had_success = false;

    for (uint8_t i = 0; i < g_hm_seq.total; i++) {
        g_hm_seq.phase = HM_PHASE_UNIT;
        g_hm_step_done = false;
        g_hm_step_ok = false;
        if (homematic_issue_unit(i))
            hm_wait_step(timeout_ms); // unit always proceeds regardless of result

        g_hm_seq.phase = HM_PHASE_VALUE;
        g_hm_step_done = false;
        g_hm_step_ok = false;
        if (!homematic_issue_single(i))
            continue;
        bool ok = hm_wait_step(timeout_ms);
        if (!ok) {
            g_hm_step_done = false;
            g_hm_step_ok = false;
            if (homematic_issue_single(i))
                ok = hm_wait_step(timeout_ms);
        }
        if (ok)
            had_success = true;
    }

    g_hm_seq.phase = HM_PHASE_SERVICE;
    g_hm_step_done = false;
    g_hm_step_ok = false;
    if (homematic_issue_service()) {
        bool svc_ok = hm_wait_step(timeout_ms);
        if (svc_ok)
            had_success = true;
    }

    if (!had_success) {
        s_last_run_error = RUN_ERROR_CONNECTION;
        debug_status("ERROR", "Homematic: all requests failed\n");
        return (run_result_t){.error_page = PAGE_HTTP_ERROR};
    }
    return (run_result_t){.data = homematic_values};
}

static void hm_render(uint8_t *image, device_caps_t caps, int page, const void *data) {
    (void)caps;
    if (page == PAGE_WIFI_ERROR) {
        debug_status("ERROR", "Homematic: showing WiFi error page\n");
        render_page_error(image, "WiFi Error!", "Unable to connect to WiFi",
                          "Check WiFi settings.");
        return;
    }
    if (page == PAGE_HTTP_ERROR) {
        debug_status("ERROR", "Homematic: showing server error page\n");
        render_page_error(image, "Server Error!", "Cannot reach Homematic CCU",
                          "Check IP and port in settings.");
        return;
    }
    if (!data && page >= 0 && hm_pages[page] && hm_pages[page]->needs_wifi) {
        debug_status("ERROR", "Homematic: showing generic server error page\n");
        render_page_error(image, "Server Error!", "Unable to reach the server",
                          "Check server status.");
        return;
    }

    if (page >= 0 && hm_pages[page])
        hm_pages[page]->render(image);
    else
        render_page_fallback(page, image);
}

const use_case_t use_case = {
    .name = "homematic",
    .pages = hm_pages,
    .input_map = hm_input_map,
    .default_page = HM_PAGE_HOMEMATIC,
    .run = hm_run,
    .render = hm_render,
    .free_data = NULL,
    .render_config_page = hm_render_config_page,
    .handle_config_form = hm_handle_config_form,
    .needs_4gray = false,
    .show_display_name = true,
    .extra_routes = NULL,
    .extra_route_count = 0,
};

// --- XML-RPC request builders (always compiled, no #ifdef needed) ---

static void append_prefixed_address(char *out, size_t out_n, const char *address, bool add_prefix) {
    if (add_prefix && strncmp(address, "HmIP-RF.", 8) != 0) {
        snprintf(out, out_n, "HmIP-RF.%s", address);
    } else {
        snprintf(out, out_n, "%s", address);
    }
}

static int homematic_build_getvalue(char *buf, size_t n, const char *address, const char *key) {
    return snprintf(buf, n,
                    "<?xml version=\"1.0\"?>\n"
                    "<methodCall>\n"
                    "  <methodName>getValue</methodName>\n"
                    "  <params>\n"
                    "    <param><value><string>%s</string></value></param>\n"
                    "    <param><value><string>%s</string></value></param>\n"
                    "  </params>\n"
                    "</methodCall>",
                    address, key);
}

// cppcheck-suppress unusedFunction
int homematic_build_multicall(char *buf, size_t n, const homematic_config_t *cfg) {
    if (!buf || !cfg)
        return -1;
    size_t used = 0;
    int m = snprintf(buf + used, (used < n) ? (n - used) : 0,
                     "<?xml version=\"1.0\"?>\n"
                     "<methodCall>\n"
                     "  <methodName>system.multicall</methodName>\n"
                     "  <params><param><value><array><data>\n");
    if (m < 0)
        return -1;
    used += m;

    for (uint8_t i = 0; i < cfg->data.count; i++) {
        char addr[64];
        append_prefixed_address(addr, sizeof(addr), cfg->data.items[i].address,
                                cfg->data.add_interface_prefix);
        m = snprintf(
            buf + used, (used < n) ? (n - used) : 0,
            "    <value><struct>\n"
            "      "
            "<member><name>methodName</name><value><string>getValue</string></value></member>\n"
            "      <member><name>params</name><value><array><data>\n"
            "        <value><string>%s</string></value>\n"
            "        <value><string>%s</string></value>\n"
            "      </data></array></value></member>\n"
            "    </struct></value>\n",
            addr, cfg->data.items[i].key);
        if (m < 0)
            return -1;
        used += m;
        if (used >= n)
            return -1;
    }

    m = snprintf(buf + used, (used < n) ? (n - used) : 0,
                 "  </data></array></value></param></params>\n"
                 "</methodCall>");
    if (m < 0)
        return -1;
    used += m;
    if (used >= n)
        return -1;
    return (int)used;
}

static int homematic_build_http_post(char *out, size_t n, const char *host, const char *xml,
                                     int xml_len) {
    if (!out || !host || !xml || xml_len < 0)
        return -1;
    return snprintf(out, n,
                    "POST / HTTP/1.0\r\n"
                    "Host: %s\r\n"
                    "Content-Type: text/xml\r\n"
                    "Accept: text/xml\r\n"
                    "User-Agent: Tinta/homematic\r\n"
                    "Connection: close\r\n"
                    "Content-Length: %d\r\n"
                    "\r\n"
                    "%.*s",
                    host, xml_len, xml_len, xml);
}

static int homematic_build_getparamsetdesc(char *buf, size_t n, const char *address) {
    if (!buf || !address)
        return -1;
    return snprintf(buf, n,
                    "<?xml version=\"1.0\"?>\n"
                    "<methodCall>\n"
                    "  <methodName>getParamsetDescription</methodName>\n"
                    "  <params>\n"
                    "    <param><value><string>%s</string></value></param>\n"
                    "    <param><value><string>VALUES</string></value></param>\n"
                    "  </params>\n"
                    "</methodCall>",
                    address);
}

static int homematic_build_get_service_messages(char *buf, size_t n) {
    if (!buf)
        return -1;
    // Some CCU APIs accept an optional boolean parameter includeInternal; pass true
    return snprintf(buf, n,
                    "<?xml version=\"1.0\"?>\n"
                    "<methodCall>\n"
                    "  <methodName>getServiceMessages</methodName>\n"
                    "  <params>\n"
                    "    <param><value><boolean>1</boolean></value></param>\n"
                    "  </params>\n"
                    "</methodCall>\n");
}
