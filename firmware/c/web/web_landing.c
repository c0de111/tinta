#include "web_landing.h"
#include "config.h"
#include "use_case.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include <stdio.h>
#include <string.h>

void build_landing_page(char *buf, size_t sz) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));

    int n = scaffold_page_open(buf, sz, "Tinta Setup", 30);

    char uc_label[32];
    snprintf(uc_label, sizeof(uc_label), "%s", use_case.name);
    if (uc_label[0] >= 'a' && uc_label[0] <= 'z')
        uc_label[0] = (char)(uc_label[0] - 32);

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<div style=\"display:flex;flex-direction:column;align-items:center;gap:.4em\">"
        "<a class=\"btn\" style=\"width:240px;margin:0\" href=\"/device_status\">Device Status</a>"
        "<a class=\"btn\" style=\"width:240px;margin:0\" href=\"/settings_transfer\">Import/Export "
        "Settings</a>");
    n += snprintf(buf + n, sz - (size_t)n,
                  "<a class=\"btn\" style=\"width:240px;margin:0\" href=\"/%s\">%s Settings</a>",
                  use_case.name, uc_label);
    n += snprintf(buf + n, sz - (size_t)n,
                  "<a class=\"btn\" style=\"width:240px;margin:0\" "
                  "href=\"/device_settings\">Device Settings</a>"
                  "<a class=\"btn\" style=\"width:240px;margin:0\" "
                  "href=\"/firmware_update\">Firmware Update</a>"
                  "<a class=\"btn\" style=\"width:240px;margin:0\" href=\"/shutdown\">Reboot</a>"
                  "</div>"
                  "<p class=\"small\">%s</p>",
                  timeout_info);

    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, NULL, NULL);
}
