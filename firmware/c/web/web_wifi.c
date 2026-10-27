#include "web_wifi.h"
#include "config.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "flash.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Network names are chosen by whoever runs the network and may hold any character.
static int append_escaped(char *buf, size_t sz, int n, const char *s) {
    for (; *s && (size_t)n + 7 < sz; s++) {
        switch (*s) {
        case '&':
            n += snprintf(buf + n, sz - (size_t)n, "&amp;");
            break;
        case '<':
            n += snprintf(buf + n, sz - (size_t)n, "&lt;");
            break;
        case '>':
            n += snprintf(buf + n, sz - (size_t)n, "&gt;");
            break;
        case '"':
            n += snprintf(buf + n, sz - (size_t)n, "&quot;");
            break;
        default:
            buf[n++] = *s;
            buf[n] = '\0';
        }
    }
    return n;
}

static const char *signal_bars(int16_t rssi) {
    return rssi >= -60   ? "\xe2\x96\xae\xe2\x96\xae\xe2\x96\xae"  // three full bars
           : rssi >= -72 ? "\xe2\x96\xae\xe2\x96\xae\xe2\x96\xaf"  // two
                         : "\xe2\x96\xae\xe2\x96\xaf\xe2\x96\xaf"; // one
}

// The networks seen at the start of setup; choosing one fills in the name below, which can
// still be typed by hand (hidden networks, or one that came up later).
static int append_network_list(char *buf, size_t sz, int n, const char *ssid_field) {
    const wifi_network_t *found;
    const int count = wifi_found_networks(&found);
    if (count == 0)
        return n + snprintf(buf + n, sz - (size_t)n,
                            "<p class=\"small\">No networks found, please type the name.</p>");
    n += snprintf(buf + n, sz - (size_t)n,
                  "<label>Network:<br><select "
                  "onchange=\"if(this.value)this.form.%s.value=this.value\">"
                  "<option value=\"\">Choose your network\xe2\x80\xa6</option>",
                  ssid_field);
    for (int i = 0; i < count && (size_t)n + 200 < sz; i++) {
        n += snprintf(buf + n, sz - (size_t)n, "<option value=\"");
        n = append_escaped(buf, sz, n, found[i].ssid);
        n += snprintf(buf + n, sz - (size_t)n, "\">");
        n = append_escaped(buf, sz, n, found[i].ssid);
        n += snprintf(buf + n, sz - (size_t)n, " %s</option>", signal_bars(found[i].rssi));
    }
    return n + snprintf(buf + n, sz - (size_t)n, "</select></label>");
}

int web_wifi_credential_fields(char *buf, size_t sz, int n, const char *ssid_field,
                               const char *password_field, const char *region_field) {
    n = append_network_list(buf, sz, n, ssid_field);
    // A sign as delivered holds placeholders, which must not look like a choice.
    const bool known = wifi_has_network();
    n += snprintf(buf + n, sz - (size_t)n,
                  "<label>SSID:<br><input type=\"text\" name=\"%s\" value=\"", ssid_field);
    if (known)
        n = append_escaped(buf, sz, n, wifi_config_flash.ssid);
    n += snprintf(buf + n, sz - (size_t)n,
                  "\"></label><label>Password:<br><input type=\"text\" name=\"%s\" value=\"",
                  password_field);
    if (known)
        n = append_escaped(buf, sz, n, wifi_config_flash.password);
    n += snprintf(buf + n, sz - (size_t)n, "\"></label>");

    // Radio rules: which channels the sign may use. Worldwide is allowed everywhere.
    static const char *const labels[WIFI_REGION_COUNT] = {"Worldwide", "Europe", "USA", "Japan"};
    const wifi_region_t current = wifi_config_region(&wifi_config_flash);
    n += snprintf(buf + n, sz - (size_t)n, "<label>Region:<br><select name=\"%s\">", region_field);
    for (int i = 0; i < WIFI_REGION_COUNT; i++)
        n += snprintf(buf + n, sz - (size_t)n, "<option value=\"%d\"%s>%s</option>", i,
                      i == (int)current ? " selected" : "", labels[i]);
    return n + snprintf(buf + n, sz - (size_t)n,
                        "</select></label><p class=\"small\">Worldwide works everywhere. Choose "
                        "your region to use all Wi-Fi channels allowed there (Europe and Japan: "
                        "1-13).</p>");
}

void build_wifi_config_page(char *buf, size_t sz, const char *message) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));

    int n = scaffold_page_open(buf, sz, "Wi-Fi Settings", 0);

    if (message && *message) {
        n += snprintf(buf + n, sz - (size_t)n, "<p class=\"flash-ok\">%s</p>", message);
    }

    n += snprintf(buf + n, sz - (size_t)n,
                  "<form method=\"POST\" action=\"/wifi\">"
                  "<div class=\"section\">"
                  "<h2>Wi-Fi Credentials</h2>");
    n = web_wifi_credential_fields(buf, sz, n, "text1", "text2", "text3");
    n += snprintf(buf + n, sz - (size_t)n,
                  "</div>"
                  "<div class=\"section\" style=\"text-align:center\">"
                  "<input type=\"submit\" value=\"Save\">"
                  "</div>"
                  "</form>");

    n += snprintf(buf + n, sz - (size_t)n, "<p class=\"small\">%s</p>", timeout_info);
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, "/", NULL);
}

void handle_form_wifi(const char *body, size_t len, char *buf, size_t sz) {
    web_submission_t result = {0};

    parse_form_fields(body, len, &result);

    wifi_config_t new_cfg = {.crc32 = 0};
    strncpy(new_cfg.ssid, result.text[0], sizeof(new_cfg.ssid) - 1);
    strncpy(new_cfg.password, result.text[1], sizeof(new_cfg.password) - 1);
    new_cfg.region = (uint8_t)atoi(result.text[2]); // checked in save_wifi_config
    new_cfg.region_check = new_cfg.region ^ WIFI_REGION_CHECK;

    bool ok = save_wifi_config(&new_cfg);

    build_wifi_config_page(buf, sz, "\xe2\x9c\x94 WiFi data saved");

    if (ok) {
        dlog("SSID & password saved\n");
    } else {
        dlog("Error saving data\n");
    }
}
