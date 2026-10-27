#include "web_export_settings.h"
#include "config.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "flash.h"
#ifdef USE_CASE_SEATSURFING
#include "seatsurfing/seatsurfing_flash.h"
#elif defined(USE_CASE_HISTORIAN)
#include "historian/historian_flash.h"
#elif defined(USE_CASE_HOMEMATIC)
#include "homematic/homematic_flash.h"
#elif defined(USE_CASE_WEATHERMAP)
#include "weathermap/weathermap_flash.h"
#endif
#include "rtc.h"
#include "use_case.h"
#include "version.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include "wifi.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_CLAMP(n, page)                                                                        \
    do {                                                                                           \
        if ((size_t)(n) >= sizeof(page))                                                           \
            (n) = (int)sizeof(page) - 1;                                                           \
    } while (0)

static void format_settings_version_tag(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }

    const char *src = (version && version[0] != '\0') ? version : "unknown";
    if (src[0] == 'v' || src[0] == 'V') {
        src++;
    }

    char cleaned[64];
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < sizeof(cleaned); i++) {
        char c = src[i];
        bool ok = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '-' || c == '_');
        cleaned[j++] = ok ? c : '_';
    }
    cleaned[j] = '\0';
    if (cleaned[0] == '\0') {
        strncpy(cleaned, "unknown", sizeof(cleaned) - 1);
        cleaned[sizeof(cleaned) - 1] = '\0';
    }

    snprintf(out, out_len, "tinta_settings_v%s", cleaned);
}

static void format_settings_export_date(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }

    if (build_date && strlen(build_date) >= 10) {
        snprintf(out, out_len, "%c%c%c%c%c%c%c%c", build_date[0], build_date[1], build_date[2],
                 build_date[3], build_date[5], build_date[6], build_date[8], build_date[9]);
        return;
    }

    strncpy(out, "00000000", out_len - 1);
    out[out_len - 1] = '\0';
}

static bool parse_bool_value(const char *s, bool *out) {
    if (!s || !out)
        return false;
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
        strcmp(s, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
        strcmp(s, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_ipv4_value(const char *s, uint8_t out_ip[4]) {
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        return false;
    }
    out_ip[0] = (uint8_t)a;
    out_ip[1] = (uint8_t)b;
    out_ip[2] = (uint8_t)c;
    out_ip[3] = (uint8_t)d;
    return true;
}

static bool extract_urlencoded_field(const char *body, size_t len, const char *key, char *out,
                                     size_t out_len) {
    if (!body || !key || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    size_t key_len = strlen(key);
    const char *ptr = body;
    const char *end = body + len;

    while (ptr < end) {
        const char *amp = memchr(ptr, '&', (size_t)(end - ptr));
        const char *seg_end = amp ? amp : end;
        const char *eq = memchr(ptr, '=', (size_t)(seg_end - ptr));
        if (eq) {
            size_t klen = (size_t)(eq - ptr);
            if (klen == key_len && strncmp(ptr, key, key_len) == 0) {
                size_t vlen = (size_t)(seg_end - (eq + 1));
                char encoded[4096];
                if (vlen >= sizeof(encoded)) {
                    vlen = sizeof(encoded) - 1;
                }
                memcpy(encoded, eq + 1, vlen);
                encoded[vlen] = '\0';
                url_decode(out, encoded, out_len);
                return true;
            }
        }
        if (!amp) {
            break;
        }
        ptr = amp + 1;
    }
    return false;
}

static void append_htmlf(char *dst, size_t dst_len, size_t *off, const char *fmt, ...) {
    if (!dst || !off || *off >= dst_len) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(dst + *off, dst_len - *off, fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    if ((size_t)written >= dst_len - *off) {
        *off = dst_len - 1;
        dst[*off] = '\0';
        return;
    }
    *off += (size_t)written;
}

static int find_key_index(const char *key, const char *const *keys, size_t key_count) {
    for (size_t i = 0; i < key_count; i++) {
        if (strcmp(key, keys[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_import_metadata_key(const char *key) {
    if (!key) {
        return false;
    }
    return strcmp(key, "contains_secrets") == 0 || strcmp(key, "device.mac") == 0 ||
           strcmp(key, "build_signature") == 0 || strcmp(key, "build_date") == 0 ||
           strncmp(key, "meta.", 5) == 0;
}

static void append_key_report_section(char *report, size_t report_len, size_t *off,
                                      const char *title, const char *const *keys, size_t key_count,
                                      const bool *seen, const bool *issues, bool section_touched,
                                      int *ok_count, int *issue_count, int *old_count) {
    if (key_count == 0) {
        return;
    }
    append_htmlf(report, report_len, off, "<h3>%s</h3><ul>", title);
    for (size_t i = 0; i < key_count; i++) {
        if (seen[i]) {
            if (issues[i]) {
                append_htmlf(report, report_len, off,
                             "<li class='status-issue'><code>%s</code> invalid value (kept "
                             "old)</li>",
                             keys[i]);
                (*issue_count)++;
            } else {
                append_htmlf(report, report_len, off,
                             "<li class='status-ok'><code>%s</code> imported</li>", keys[i]);
                (*ok_count)++;
            }
        } else if (section_touched) {
            append_htmlf(report, report_len, off,
                         "<li class='status-issue'><code>%s</code> missing in import (kept "
                         "old)</li>",
                         keys[i]);
            (*issue_count)++;
        } else {
            append_htmlf(report, report_len, off,
                         "<li class='status-old'><code>%s</code> kept old (not in file)</li>",
                         keys[i]);
            (*old_count)++;
        }
    }
    append_htmlf(report, report_len, off, "</ul>");
}

static void build_settings_transfer_page_with_report(char *buf, size_t sz, const char *message,
                                                     const char *report_html) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));

    int n = scaffold_page_open(buf, sz, "Settings Import/Export", 0);

    n += snprintf(buf + n, sz - (size_t)n,
                  "<style>"
                  ".section{max-width:540px;margin-left:auto;margin-right:auto}"
                  ".filemeta{font-size:.95em;color:#333;margin-top:.35em;min-height:1.2em;"
                  "text-align:center}"
                  "button.btn{display:inline-block;padding:.6em 1em;margin:.35em;"
                  "border:1px solid #777;border-radius:6px;background:#f3f3f3;color:#000;"
                  "cursor:pointer;font-size:1em}"
                  "button.btn:disabled{opacity:.5;cursor:not-allowed}"
                  ".report{max-width:420px;margin:1em auto;text-align:left;border:1px solid #ddd;"
                  "border-radius:8px;padding:.85em}"
                  ".report h2{margin:0 0 .5em 0;font-size:1.05em;text-align:center}"
                  ".report ul{margin:.4em 0 .8em 1.1em;padding:0}"
                  ".report li{margin:.25em 0}"
                  ".status-ok{color:#086b1a}"
                  ".status-issue{color:#b00020}"
                  ".status-old{color:#b26a00}"
                  ".hidden-input{position:absolute;left:-9999px}"
                  "</style>");

    if (message && *message) {
        const char *cls;
        if (strstr(message, "\xe2\x9d\x8c") != NULL)
            cls = "flash-error";
        else if (strstr(message, "\xe2\x9a\xa0") != NULL)
            cls = "flash-error";
        else
            cls = "flash-ok";
        n += snprintf(buf + n, sz - (size_t)n, "<p class=\"%s\">%s</p>", cls, message);
    }

    n += snprintf(buf + n, sz - (size_t)n,
                  "<p class=\"small\">Use case: <b>%s</b></p>"
                  "<div class=\"section\">"
                  "<h2>Export</h2>"
                  "<div style=\"text-align:center\">"
                  "<a class=\"btn\" href=\"/settings_export.txt\">Export settings file</a>"
                  "</div>"
                  "<p class=\"small\" style=\"text-align:center\">Contains credentials/secrets. "
                  "Handle this file as sensitive.</p>"
                  "</div>"
                  "<div class=\"section\">"
                  "<h2>Import</h2>"
                  "<form id=\"settingsImportForm\" method=\"POST\" action=\"/settings_import\">"
                  "<input class=\"hidden-input\" type=\"file\" id=\"settingsFile\" "
                  "accept=\".txt,text/plain\" "
                  "required>"
                  "<div style=\"text-align:center\">"
                  "<button type=\"button\" class=\"btn\" id=\"pickFileBtn\">Choose file</button>"
                  "</div>"
                  "<div class=\"filemeta\" id=\"fileName\">No file selected.</div>"
                  "<textarea id=\"settings\" name=\"settings\" style=\"display:none\"></textarea>"
                  "<div style=\"text-align:center\">"
                  "<button class=\"btn\" id=\"importBtn\" type=\"submit\" disabled>Import settings "
                  "file</button>"
                  "</div>"
                  "</form>"
                  "</div>",
                  use_case.name);

    if (report_html && *report_html) {
        n += snprintf(buf + n, sz - (size_t)n, "%s", report_html);
    }

    n += snprintf(buf + n, sz - (size_t)n,
                  "<script>"
                  "var form=document.getElementById('settingsImportForm');"
                  "var f=document.getElementById('settingsFile');"
                  "var pick=document.getElementById('pickFileBtn');"
                  "var fn=document.getElementById('fileName');"
                  "var ib=document.getElementById('importBtn');"
                  "var t=document.getElementById('settings');"
                  "pick.addEventListener('click',function(){f.click();});"
                  "f.addEventListener('change',function(){"
                  "var file=f.files&&f.files[0];"
                  "if(!file){fn.textContent='No file selected.';ib.disabled=true;return;}"
                  "fn.textContent=file.name;ib.disabled=false;});"
                  "form.addEventListener('submit',function(e){"
                  "e.preventDefault();"
                  "var file=f.files&&f.files[0];"
                  "if(!file){alert('Select a settings file first.');return;}"
                  "ib.disabled=true;"
                  "var r=new FileReader();"
                  "r.onload=function(){t.value=String(r.result||'');form.submit();};"
                  "r.onerror=function(){ib.disabled=false;alert('Failed to read file.');};"
                  "r.readAsText(file);});"
                  "</script>");

    n += snprintf(buf + n, sz - (size_t)n, "<p class=\"small\">%s</p>", timeout_info);
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, "/", NULL);
}

void build_settings_transfer_page(char *buf, size_t sz, const char *message) {
    build_settings_transfer_page_with_report(buf, sz, message, NULL);
}

void build_settings_export_txt(char *buf, size_t sz, char *disposition, size_t disp_sz) {
    char settings_tag[80];
    char settings_date[16];
    char download_name[128];
    char source_device_id[32];
    char exported_at[32];
    size_t off = 0;

    format_settings_version_tag(settings_tag, sizeof(settings_tag));
    format_settings_export_date(settings_date, sizeof(settings_date));
    snprintf(download_name, sizeof(download_name), "tinta_settings_%s_%s.txt", use_case.name,
             settings_date);
    snprintf(disposition, disp_sz, "attachment; filename=\"%s\"", download_name);
    const uint8_t *mac = wifi_mac();
    snprintf(source_device_id, sizeof(source_device_id), "tinta-%02X%02X%02X%02X%02X%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    strncpy(exported_at, "unknown", sizeof(exported_at) - 1);
    exported_at[sizeof(exported_at) - 1] = '\0';
    rtc_time_t now = {0};
    if (rtc_read_time(&now) == 0) {
        rtc_format_time(&now, exported_at, sizeof(exported_at));
    }

    off += (size_t)snprintf(buf + off, sz - off, "%s\n", settings_tag);
    off += (size_t)snprintf(buf + off, sz - off, "use_case=%s\n", use_case.name);
    off += (size_t)snprintf(buf + off, sz - off, "contains_secrets=1\n");
    off += (size_t)snprintf(buf + off, sz - off, "meta.source_device_id=%s\n", source_device_id);
    off += (size_t)snprintf(buf + off, sz - off, "meta.exported_at=%s\n", exported_at);
    off += (size_t)snprintf(buf + off, sz - off, "device.mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    off += (size_t)snprintf(buf + off, sz - off, "build_signature=%s\n",
                            (version && version[0] != '\0') ? version : "unknown");
    off += (size_t)snprintf(buf + off, sz - off, "build_date=%s\n",
                            (build_date && build_date[0] != '\0') ? build_date : "unknown");
    off += (size_t)snprintf(buf + off, sz - off, "\n");
    off += (size_t)snprintf(buf + off, sz - off, "wifi.ssid=%s\n", wifi_config_flash.ssid);
    off += (size_t)snprintf(buf + off, sz - off, "wifi.password=%s\n", wifi_config_flash.password);
    off += (size_t)snprintf(buf + off, sz - off, "wifi.region=%s\n",
                            wifi_region_key(wifi_config_region(&wifi_config_flash)));
    off += (size_t)snprintf(buf + off, sz - off, "\n");
    off += (size_t)snprintf(buf + off, sz - off, "device.roomname=%s\n",
                            device_config_flash.data.roomname);
    off += (size_t)snprintf(buf + off, sz - off, "device.type=%d\n",
                            (int)device_config_flash.data.type);
    off += (size_t)snprintf(buf + off, sz - off, "device.epapertype=%d\n",
                            (int)device_config_flash.data.epapertype);
    for (int i = 0; i < 8; i++) {
        off += (size_t)snprintf(buf + off, sz - off, "device.refresh%d=%d\n", i,
                                device_config_flash.data.refresh_minutes_by_pushbutton[i]);
    }
    off += (size_t)snprintf(buf + off, sz - off, "device.show_query_date=%d\n",
                            device_config_flash.data.show_query_date ? 1 : 0);
    off += (size_t)snprintf(buf + off, sz - off, "device.query_only_at_officehours=%d\n",
                            device_config_flash.data.query_only_at_officehours ? 1 : 0);
    off += (size_t)snprintf(buf + off, sz - off, "device.wifi_reconnect_minutes=%d\n",
                            device_config_flash.data.wifi_reconnect_minutes);
    off += (size_t)snprintf(buf + off, sz - off, "device.watchdog_time=%d\n",
                            device_config_flash.data.watchdog_time);
    off += (size_t)snprintf(buf + off, sz - off, "device.number_wifi_attempts=%d\n",
                            device_config_flash.data.number_wifi_attempts);
    off += (size_t)snprintf(buf + off, sz - off, "device.wifi_timeout=%d\n",
                            device_config_flash.data.wifi_timeout);
    off += (size_t)snprintf(buf + off, sz - off, "device.max_wait_data_wifi=%d\n",
                            device_config_flash.data.max_wait_data_wifi);
    off += (size_t)snprintf(buf + off, sz - off, "device.switch_off_battery_voltage=%.3f\n",
                            device_config_flash.data.switch_off_battery_voltage);
    off += (size_t)snprintf(buf + off, sz - off, "device.conversion_factor=%.6f\n",
                            device_config_flash.data.conversion_factor);
    off += (size_t)snprintf(buf + off, sz - off, "device.telemetry_enabled=%d\n",
                            device_config_flash.data.telemetry_enabled ? 1 : 0);
    off += (size_t)snprintf(buf + off, sz - off, "device.telemetry_host=%s\n",
                            device_config_flash.data.telemetry_host);
    off += (size_t)snprintf(buf + off, sz - off, "device.telemetry_port=%u\n",
                            (unsigned)device_config_flash.data.telemetry_port);
    off += (size_t)snprintf(buf + off, sz - off, "device.telemetry_token=%s\n",
                            device_config_flash.data.telemetry_token);
    off += (size_t)snprintf(buf + off, sz - off, "device.telemetry_timeout_ms=%d\n",
                            device_config_flash.data.telemetry_timeout_ms);
    off += (size_t)snprintf(buf + off, sz - off, "device.telemetry_label=%s\n",
                            device_config_flash.data.telemetry_label);
    off += (size_t)snprintf(buf + off, sz - off, "device.autoset_rtc_enabled=%d\n",
                            device_config_flash.data.autoset_rtc_enabled ? 1 : 0);
    off += (size_t)snprintf(buf + off, sz - off, "device.led_enabled=%d\n",
                            device_config_flash.data.led_enabled ? 1 : 0);
    off += (size_t)snprintf(buf + off, sz - off, "\n");

#if defined(USE_CASE_SEATSURFING)
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.host=%s\n",
                            seatsurfing_config_flash.data.host);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.username=%s\n",
                            seatsurfing_config_flash.data.username);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.password=%s\n",
                            seatsurfing_config_flash.data.password);
    off +=
        (size_t)snprintf(buf + off, sz - off, "seatsurfing.ip=%u.%u.%u.%u\n",
                         seatsurfing_config_flash.data.ip[0], seatsurfing_config_flash.data.ip[1],
                         seatsurfing_config_flash.data.ip[2], seatsurfing_config_flash.data.ip[3]);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.port=%u\n",
                            (unsigned)seatsurfing_config_flash.data.port);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.location_id=%s\n",
                            seatsurfing_config_flash.data.location_id);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.seat_count=%u\n",
                            (unsigned)seatsurfing_config_flash.data.seat_count);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.space1=%s\n",
                            seatsurfing_config_flash.data.space_ids[0]);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.space2=%s\n",
                            seatsurfing_config_flash.data.space_ids[1]);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.space3=%s\n",
                            seatsurfing_config_flash.data.space_ids[2]);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.space4=%s\n",
                            seatsurfing_config_flash.data.space_ids[3]);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.booking_email=%s\n",
                            seatsurfing_config_flash.data.booking_email);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.booking_password=%s\n",
                            seatsurfing_config_flash.data.booking_password);
    off += (size_t)snprintf(buf + off, sz - off, "seatsurfing.device_mode=%u\n",
                            seatsurfing_config_flash.data.device_mode == 1 ? 1u : 0u);
#elif defined(USE_CASE_HISTORIAN)
    off += (size_t)snprintf(buf + off, sz - off, "historian.ip=%u.%u.%u.%u\n",
                            historian_config_flash.data.ip[0], historian_config_flash.data.ip[1],
                            historian_config_flash.data.ip[2], historian_config_flash.data.ip[3]);
    off += (size_t)snprintf(buf + off, sz - off, "historian.port=%u\n",
                            (unsigned)historian_config_flash.data.port);
    off += (size_t)snprintf(buf + off, sz - off, "historian.path=%s\n",
                            historian_config_flash.data.path);
    off += (size_t)snprintf(buf + off, sz - off, "historian.datapoint_id=%d\n",
                            historian_config_flash.data.datapoint_id);
    off += (size_t)snprintf(buf + off, sz - off, "historian.hours_back=%d\n",
                            historian_config_flash.data.hours_back);
    off += (size_t)snprintf(buf + off, sz - off, "historian.display_name=%s\n",
                            historian_config_flash.data.display_name);
#elif defined(USE_CASE_HOMEMATIC)
    off += (size_t)snprintf(buf + off, sz - off, "homematic.ip=%u.%u.%u.%u\n",
                            homematic_config_flash.data.ip[0], homematic_config_flash.data.ip[1],
                            homematic_config_flash.data.ip[2], homematic_config_flash.data.ip[3]);
    off += (size_t)snprintf(buf + off, sz - off, "homematic.port=%u\n",
                            (unsigned)homematic_config_flash.data.port);
    off += (size_t)snprintf(buf + off, sz - off, "homematic.count=%u\n",
                            (unsigned)homematic_config_flash.data.count);
    for (int i = 0; i < HOMEMATIC_MAX_ITEMS; i++) {
        off += (size_t)snprintf(buf + off, sz - off, "homematic.item%d.address=%s\n", i + 1,
                                homematic_config_flash.data.items[i].address);
        off += (size_t)snprintf(buf + off, sz - off, "homematic.item%d.key=%s\n", i + 1,
                                homematic_config_flash.data.items[i].key);
        off += (size_t)snprintf(buf + off, sz - off, "homematic.item%d.label=%s\n", i + 1,
                                homematic_config_flash.data.items[i].fallback_label);
    }
#endif

    if (off >= sz) {
        off = sz - 1;
    }
    buf[off] = '\0';
}

void handle_form_settings_import(const char *body, size_t len, char *buf, size_t sz) {
    static const char *const wifi_keys[] = {"wifi.ssid", "wifi.password", "wifi.region"};
    static const char *const device_keys[] = {
        "device.roomname",
        "device.type",
        "device.epapertype",
        "device.refresh0",
        "device.refresh1",
        "device.refresh2",
        "device.refresh3",
        "device.refresh4",
        "device.refresh5",
        "device.refresh6",
        "device.refresh7",
        "device.show_query_date",
        "device.query_only_at_officehours",
        "device.wifi_reconnect_minutes",
        "device.watchdog_time",
        "device.number_wifi_attempts",
        "device.wifi_timeout",
        "device.max_wait_data_wifi",
        "device.switch_off_battery_voltage",
        "device.conversion_factor",
        "device.telemetry_enabled",
        "device.telemetry_host",
        "device.telemetry_port",
        "device.telemetry_token",
        "device.telemetry_timeout_ms",
        "device.telemetry_label",
        "device.autoset_rtc_enabled",
        "device.led_enabled",
    };
    enum {
        WIFI_KEY_COUNT = (int)(sizeof(wifi_keys) / sizeof(wifi_keys[0])),
        DEVICE_KEY_COUNT = (int)(sizeof(device_keys) / sizeof(device_keys[0])),
        MAX_USE_KEYS = 32,
        MAX_UNKNOWN_KEYS = 16
    };

    char decoded[4096];
    if (!extract_urlencoded_field(body, len, "settings", decoded, sizeof(decoded))) {
        build_settings_transfer_page(buf, sz, "\xe2\x9d\x8c Missing settings field.");
        return;
    }

    wifi_config_t new_wifi = wifi_config_flash;
    device_config_t new_dev = device_config_flash;
#if defined(USE_CASE_SEATSURFING)
    seatsurfing_config_t new_use = seatsurfing_config_flash;
#elif defined(USE_CASE_HISTORIAN)
    historian_config_t new_use = historian_config_flash;
#elif defined(USE_CASE_HOMEMATIC)
    homematic_config_t new_use = homematic_config_flash;
#endif

    const char *use_keys[MAX_USE_KEYS] = {0};
    size_t use_key_count = 0;
#if defined(USE_CASE_SEATSURFING)
    static const char *const use_keys_seatsurfing[] = {
        "seatsurfing.host",
        "seatsurfing.username",
        "seatsurfing.password",
        "seatsurfing.ip",
        "seatsurfing.port",
        "seatsurfing.location_id",
        "seatsurfing.seat_count",
        "seatsurfing.space1",
        "seatsurfing.space2",
        "seatsurfing.space3",
        "seatsurfing.space4",
        "seatsurfing.booking_email",
        "seatsurfing.booking_password",
        "seatsurfing.device_mode",
    };
    use_key_count = sizeof(use_keys_seatsurfing) / sizeof(use_keys_seatsurfing[0]);
    for (size_t i = 0; i < use_key_count; i++) {
        use_keys[i] = use_keys_seatsurfing[i];
    }
#elif defined(USE_CASE_HISTORIAN)
    static const char *const use_keys_historian[] = {
        "historian.ip",           "historian.port",       "historian.path",
        "historian.datapoint_id", "historian.hours_back", "historian.display_name",
    };
    use_key_count = sizeof(use_keys_historian) / sizeof(use_keys_historian[0]);
    for (size_t i = 0; i < use_key_count; i++) {
        use_keys[i] = use_keys_historian[i];
    }
#elif defined(USE_CASE_HOMEMATIC)
    char use_key_buf[HOMEMATIC_MAX_ITEMS * 3][40];
    use_keys[0] = "homematic.ip";
    use_keys[1] = "homematic.port";
    use_keys[2] = "homematic.count";
    use_key_count = 3;
    for (int i = 0; i < HOMEMATIC_MAX_ITEMS; i++) {
        snprintf(use_key_buf[i * 3 + 0], sizeof(use_key_buf[i * 3 + 0]), "homematic.item%d.address",
                 i + 1);
        snprintf(use_key_buf[i * 3 + 1], sizeof(use_key_buf[i * 3 + 1]), "homematic.item%d.key",
                 i + 1);
        snprintf(use_key_buf[i * 3 + 2], sizeof(use_key_buf[i * 3 + 2]), "homematic.item%d.label",
                 i + 1);
        use_keys[use_key_count++] = use_key_buf[i * 3 + 0];
        use_keys[use_key_count++] = use_key_buf[i * 3 + 1];
        use_keys[use_key_count++] = use_key_buf[i * 3 + 2];
    }
#endif

    bool wifi_seen[WIFI_KEY_COUNT];
    bool wifi_issues[WIFI_KEY_COUNT];
    bool device_seen[DEVICE_KEY_COUNT];
    bool device_issues[DEVICE_KEY_COUNT];
    bool use_seen[MAX_USE_KEYS];
    bool use_issues[MAX_USE_KEYS];
    memset(wifi_seen, 0, sizeof(wifi_seen));
    memset(wifi_issues, 0, sizeof(wifi_issues));
    memset(device_seen, 0, sizeof(device_seen));
    memset(device_issues, 0, sizeof(device_issues));
    memset(use_seen, 0, sizeof(use_seen));
    memset(use_issues, 0, sizeof(use_issues));

    bool touched_wifi = false;
    bool touched_device = false;
    bool touched_use = false;
    char unknown_keys[MAX_UNKNOWN_KEYS][48];
    int unknown_key_total = 0;
    int unknown_key_stored = 0;

    bool use_case_seen = false;
    bool use_case_match = false;

    char *saveptr = NULL;
    char *line = strtok_r(decoded, "\n", &saveptr);
    while (line) {
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') {
            line[llen - 1] = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        if (strcmp(line, "INKI_SETTINGS_V1") == 0 || strncmp(line, "inki_settings_v", 15) == 0 ||
            strncmp(line, "tinta_settings_v", 16) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "use_case") == 0) {
            use_case_seen = true;
            use_case_match = (strcmp(val, use_case.name) == 0);
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (is_import_metadata_key(key)) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (strncmp(key, "wifi.", 5) == 0) {
            touched_wifi = true;
        }
        if (strncmp(key, "device.", 7) == 0) {
            touched_device = true;
        }
#if defined(USE_CASE_SEATSURFING)
        if (strncmp(key, "seatsurfing.", 12) == 0) {
            touched_use = true;
        }
#elif defined(USE_CASE_HISTORIAN)
        if (strncmp(key, "historian.", 10) == 0) {
            touched_use = true;
        }
#elif defined(USE_CASE_HOMEMATIC)
        if (strncmp(key, "homematic.", 10) == 0) {
            touched_use = true;
        }
#endif

        int wifi_idx = find_key_index(key, wifi_keys, WIFI_KEY_COUNT);
        int device_idx = find_key_index(key, device_keys, DEVICE_KEY_COUNT);
        int use_idx = find_key_index(key, use_keys, use_key_count);

        bool known_key = (wifi_idx >= 0 || device_idx >= 0 || use_idx >= 0);
        bool key_issue = false;

        if (strcmp(key, "wifi.ssid") == 0) {
            strncpy(new_wifi.ssid, val, sizeof(new_wifi.ssid) - 1);
            new_wifi.ssid[sizeof(new_wifi.ssid) - 1] = '\0';
        } else if (strcmp(key, "wifi.password") == 0) {
            strncpy(new_wifi.password, val, sizeof(new_wifi.password) - 1);
            new_wifi.password[sizeof(new_wifi.password) - 1] = '\0';
        } else if (strcmp(key, "wifi.region") == 0) {
            const int region = wifi_region_from_key(val);
            new_wifi.region = (uint8_t)(region >= 0 ? region : atoi(val)); // files before 10-2026
            new_wifi.region_check = new_wifi.region ^ WIFI_REGION_CHECK;
        } else if (strcmp(key, "device.roomname") == 0) {
            strncpy(new_dev.data.roomname, val, sizeof(new_dev.data.roomname) - 1);
            new_dev.data.roomname[sizeof(new_dev.data.roomname) - 1] = '\0';
        } else if (strcmp(key, "device.type") == 0) {
            new_dev.data.type = (RoomType)atoi(val);
        } else if (strcmp(key, "device.epapertype") == 0) {
            new_dev.data.epapertype = (EpaperType)atoi(val);
        } else if (strncmp(key, "device.refresh", 14) == 0 && key[14] >= '0' && key[14] <= '7' &&
                   key[15] == '\0') {
            int idx = key[14] - '0';
            new_dev.data.refresh_minutes_by_pushbutton[idx] = atoi(val);
        } else if (strcmp(key, "device.show_query_date") == 0) {
            bool b;
            if (parse_bool_value(val, &b)) {
                new_dev.data.show_query_date = b;
            } else {
                key_issue = true;
            }
        } else if (strcmp(key, "device.query_only_at_officehours") == 0) {
            bool b;
            if (parse_bool_value(val, &b)) {
                new_dev.data.query_only_at_officehours = b;
            } else {
                key_issue = true;
            }
        } else if (strcmp(key, "device.wifi_reconnect_minutes") == 0) {
            new_dev.data.wifi_reconnect_minutes = atoi(val);
        } else if (strcmp(key, "device.watchdog_time") == 0) {
            new_dev.data.watchdog_time = atoi(val);
        } else if (strcmp(key, "device.number_wifi_attempts") == 0) {
            new_dev.data.number_wifi_attempts = atoi(val);
        } else if (strcmp(key, "device.wifi_timeout") == 0) {
            new_dev.data.wifi_timeout = atoi(val);
        } else if (strcmp(key, "device.max_wait_data_wifi") == 0) {
            new_dev.data.max_wait_data_wifi = atoi(val);
        } else if (strcmp(key, "device.switch_off_battery_voltage") == 0) {
            new_dev.data.switch_off_battery_voltage = strtof(val, NULL);
        } else if (strcmp(key, "device.conversion_factor") == 0) {
            new_dev.data.conversion_factor = strtof(val, NULL);
        } else if (strcmp(key, "device.telemetry_enabled") == 0) {
            bool b;
            if (parse_bool_value(val, &b)) {
                new_dev.data.telemetry_enabled = b;
            } else {
                key_issue = true;
            }
        } else if (strcmp(key, "device.telemetry_host") == 0) {
            strncpy(new_dev.data.telemetry_host, val, sizeof(new_dev.data.telemetry_host) - 1);
            new_dev.data.telemetry_host[sizeof(new_dev.data.telemetry_host) - 1] = '\0';
        } else if (strcmp(key, "device.telemetry_port") == 0) {
            new_dev.data.telemetry_port = (uint16_t)atoi(val);
        } else if (strcmp(key, "device.telemetry_token") == 0) {
            strncpy(new_dev.data.telemetry_token, val, sizeof(new_dev.data.telemetry_token) - 1);
            new_dev.data.telemetry_token[sizeof(new_dev.data.telemetry_token) - 1] = '\0';
        } else if (strcmp(key, "device.telemetry_timeout_ms") == 0) {
            new_dev.data.telemetry_timeout_ms = atoi(val);
        } else if (strcmp(key, "device.telemetry_label") == 0) {
            strncpy(new_dev.data.telemetry_label, val, sizeof(new_dev.data.telemetry_label) - 1);
            new_dev.data.telemetry_label[sizeof(new_dev.data.telemetry_label) - 1] = '\0';
        } else if (strcmp(key, "device.autoset_rtc_enabled") == 0) {
            bool b;
            if (parse_bool_value(val, &b)) {
                new_dev.data.autoset_rtc_enabled = b;
            } else {
                key_issue = true;
            }
        } else if (strcmp(key, "device.led_enabled") == 0) {
            bool b;
            if (parse_bool_value(val, &b)) {
                new_dev.data.led_enabled = b;
            } else {
                key_issue = true;
            }
#if defined(USE_CASE_SEATSURFING)
        } else if (strcmp(key, "seatsurfing.host") == 0) {
            strncpy(new_use.data.host, val, sizeof(new_use.data.host) - 1);
            new_use.data.host[sizeof(new_use.data.host) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.username") == 0) {
            strncpy(new_use.data.username, val, sizeof(new_use.data.username) - 1);
            new_use.data.username[sizeof(new_use.data.username) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.password") == 0) {
            strncpy(new_use.data.password, val, sizeof(new_use.data.password) - 1);
            new_use.data.password[sizeof(new_use.data.password) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.ip") == 0) {
            if (!parse_ipv4_value(val, new_use.data.ip)) {
                key_issue = true;
            }
        } else if (strcmp(key, "seatsurfing.port") == 0) {
            new_use.data.port = (uint16_t)atoi(val);
        } else if (strcmp(key, "seatsurfing.location_id") == 0) {
            strncpy(new_use.data.location_id, val, sizeof(new_use.data.location_id) - 1);
            new_use.data.location_id[sizeof(new_use.data.location_id) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.seat_count") == 0) {
            new_use.data.seat_count = (uint8_t)atoi(val);
        } else if (strcmp(key, "seatsurfing.space1") == 0) {
            strncpy(new_use.data.space_ids[0], val, sizeof(new_use.data.space_ids[0]) - 1);
            new_use.data.space_ids[0][sizeof(new_use.data.space_ids[0]) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.space2") == 0) {
            strncpy(new_use.data.space_ids[1], val, sizeof(new_use.data.space_ids[1]) - 1);
            new_use.data.space_ids[1][sizeof(new_use.data.space_ids[1]) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.space3") == 0) {
            strncpy(new_use.data.space_ids[2], val, sizeof(new_use.data.space_ids[2]) - 1);
            new_use.data.space_ids[2][sizeof(new_use.data.space_ids[2]) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.space4") == 0) {
            strncpy(new_use.data.space_ids[3], val, sizeof(new_use.data.space_ids[3]) - 1);
            new_use.data.space_ids[3][sizeof(new_use.data.space_ids[3]) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.booking_email") == 0) {
            strncpy(new_use.data.booking_email, val, sizeof(new_use.data.booking_email) - 1);
            new_use.data.booking_email[sizeof(new_use.data.booking_email) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.booking_password") == 0) {
            strncpy(new_use.data.booking_password, val, sizeof(new_use.data.booking_password) - 1);
            new_use.data.booking_password[sizeof(new_use.data.booking_password) - 1] = '\0';
        } else if (strcmp(key, "seatsurfing.device_mode") == 0) {
            new_use.data.device_mode = (val[0] == '1');
#elif defined(USE_CASE_HISTORIAN)
        } else if (strcmp(key, "historian.ip") == 0) {
            if (!parse_ipv4_value(val, new_use.data.ip)) {
                key_issue = true;
            }
        } else if (strcmp(key, "historian.port") == 0) {
            new_use.data.port = (uint16_t)atoi(val);
        } else if (strcmp(key, "historian.path") == 0) {
            strncpy(new_use.data.path, val, sizeof(new_use.data.path) - 1);
            new_use.data.path[sizeof(new_use.data.path) - 1] = '\0';
        } else if (strcmp(key, "historian.datapoint_id") == 0) {
            new_use.data.datapoint_id = atoi(val);
        } else if (strcmp(key, "historian.hours_back") == 0) {
            new_use.data.hours_back = atoi(val);
        } else if (strcmp(key, "historian.display_name") == 0) {
            strncpy(new_use.data.display_name, val, sizeof(new_use.data.display_name) - 1);
            new_use.data.display_name[sizeof(new_use.data.display_name) - 1] = '\0';
#elif defined(USE_CASE_HOMEMATIC)
        } else if (strcmp(key, "homematic.ip") == 0) {
            if (!parse_ipv4_value(val, new_use.data.ip)) {
                key_issue = true;
            }
        } else if (strcmp(key, "homematic.port") == 0) {
            new_use.data.port = (uint16_t)atoi(val);
        } else if (strcmp(key, "homematic.count") == 0) {
            new_use.data.count = (uint8_t)atoi(val);
        } else if (strncmp(key, "homematic.item", 13) == 0) {
            char *num_end = NULL;
            long item_num = strtol(key + 13, &num_end, 10);
            int idx = (int)item_num - 1;
            const char *suffix = num_end;
            if (idx >= 0 && idx < HOMEMATIC_MAX_ITEMS && suffix && suffix[0] == '.') {
                if (strcmp(suffix, ".address") == 0) {
                    strncpy(new_use.data.items[idx].address, val,
                            sizeof(new_use.data.items[idx].address) - 1);
                    new_use.data.items[idx].address[sizeof(new_use.data.items[idx].address) - 1] =
                        '\0';
                } else if (strcmp(suffix, ".key") == 0) {
                    strncpy(new_use.data.items[idx].key, val,
                            sizeof(new_use.data.items[idx].key) - 1);
                    new_use.data.items[idx].key[sizeof(new_use.data.items[idx].key) - 1] = '\0';
                } else if (strcmp(suffix, ".label") == 0) {
                    strncpy(new_use.data.items[idx].fallback_label, val,
                            sizeof(new_use.data.items[idx].fallback_label) - 1);
                    new_use.data.items[idx]
                        .fallback_label[sizeof(new_use.data.items[idx].fallback_label) - 1] = '\0';
                } else {
                    key_issue = true;
                }
            } else {
                key_issue = true;
            }
#endif
        }

        if (known_key) {
            if (wifi_idx >= 0) {
                wifi_seen[wifi_idx] = true;
                if (key_issue) {
                    wifi_issues[wifi_idx] = true;
                }
            } else if (device_idx >= 0) {
                device_seen[device_idx] = true;
                if (key_issue) {
                    device_issues[device_idx] = true;
                }
            } else if (use_idx >= 0) {
                use_seen[use_idx] = true;
                if (key_issue) {
                    use_issues[use_idx] = true;
                }
            }
        } else {
            unknown_key_total++;
            if (unknown_key_stored < MAX_UNKNOWN_KEYS) {
                strncpy(unknown_keys[unknown_key_stored], key, sizeof(unknown_keys[0]) - 1);
                unknown_keys[unknown_key_stored][sizeof(unknown_keys[0]) - 1] = '\0';
                unknown_key_stored++;
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (!use_case_seen) {
        build_settings_transfer_page(buf, sz, "\xe2\x9d\x8c Missing use_case in import file.");
        return;
    }

    if (!use_case_match) {
        build_settings_transfer_page(buf, sz,
                                     "\xe2\x9d\x8c use_case does not match current firmware.");
        return;
    }

    int imported_count = 0;
    int issue_count = 0;
    int kept_old_count = 0;

    char *report_html = malloc(4096);
    if (report_html) {
        size_t report_off = 0;
        report_html[0] = '\0';
        append_htmlf(report_html, 4096, &report_off, "<div class='report'>");
        append_htmlf(report_html, 4096, &report_off, "<h2>Import details</h2>");
        append_key_report_section(report_html, 4096, &report_off, "Wi-Fi keys", wifi_keys,
                                  WIFI_KEY_COUNT, wifi_seen, wifi_issues, touched_wifi,
                                  &imported_count, &issue_count, &kept_old_count);
        append_key_report_section(report_html, 4096, &report_off, "Device keys", device_keys,
                                  DEVICE_KEY_COUNT, device_seen, device_issues, touched_device,
                                  &imported_count, &issue_count, &kept_old_count);
        append_key_report_section(report_html, 4096, &report_off, "Use-case keys", use_keys,
                                  use_key_count, use_seen, use_issues, touched_use, &imported_count,
                                  &issue_count, &kept_old_count);
        if (unknown_key_stored > 0) {
            append_htmlf(report_html, 4096, &report_off, "<h3>Unknown keys</h3><ul>");
            for (int i = 0; i < unknown_key_stored; i++) {
                append_htmlf(report_html, 4096, &report_off,
                             "<li class='status-old'><code>%s</code> ignored</li>",
                             unknown_keys[i]);
            }
            if (unknown_key_total > unknown_key_stored) {
                append_htmlf(report_html, 4096, &report_off,
                             "<li class='status-old'>... and %d more unknown keys</li>",
                             unknown_key_total - unknown_key_stored);
            }
            append_htmlf(report_html, 4096, &report_off, "</ul>");
        }
        append_htmlf(report_html, 4096, &report_off,
                     "<div class='report-summary'>Imported: <b>%d</b> | Issues: <b>%d</b> | Kept "
                     "old: <b>%d</b> | Unknown ignored: <b>%d</b></div>",
                     imported_count, issue_count, kept_old_count, unknown_key_total);
        append_htmlf(report_html, 4096, &report_off, "</div>");
    } else {
        for (int i = 0; i < WIFI_KEY_COUNT; i++) {
            if (wifi_seen[i]) {
                if (wifi_issues[i]) {
                    issue_count++;
                }
            } else if (touched_wifi) {
                issue_count++;
            }
        }
        for (int i = 0; i < DEVICE_KEY_COUNT; i++) {
            if (device_seen[i]) {
                if (device_issues[i]) {
                    issue_count++;
                }
            } else if (touched_device) {
                issue_count++;
            }
        }
        for (size_t i = 0; i < use_key_count; i++) {
            if (use_seen[i]) {
                if (use_issues[i]) {
                    issue_count++;
                }
            } else if (touched_use) {
                issue_count++;
            }
        }
    }

    bool ok_wifi = save_wifi_config(&new_wifi);
    bool ok_dev = save_device_config(&new_dev);
#if defined(USE_CASE_SEATSURFING)
    bool ok_use = save_seatsurfing_config(&new_use);
#elif defined(USE_CASE_HISTORIAN)
    bool ok_use = save_historian_config(&new_use);
#elif defined(USE_CASE_HOMEMATIC)
    bool ok_use = save_homematic_config(&new_use);
#else
    bool ok_use = true;
#endif

    if (ok_wifi && ok_dev && ok_use) {
        if (issue_count > 0) {
            build_settings_transfer_page_with_report(buf, sz,
                                                     "\xe2\x9a\xa0 Settings imported with issues.",
                                                     report_html ? report_html : "");
        } else {
            build_settings_transfer_page_with_report(buf, sz,
                                                     "\xe2\x9c\x94 Settings imported and saved.",
                                                     report_html ? report_html : "");
        }
    } else {
        build_settings_transfer_page_with_report(
            buf, sz, "\xe2\x9d\x8c Error while saving imported settings.",
            report_html ? report_html : "");
    }

    if (report_html) {
        free(report_html);
    }
}
