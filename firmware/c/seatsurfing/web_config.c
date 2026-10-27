#include "web_config.h"
#include "config.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "i2c_bus.h"
#include "pico/time.h"
#include "seatsurfing/device_mode.h"
#include "seatsurfing/seatsurfing_flash.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

void ss_render_config_page(char *buf, size_t sz, const char *msg) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));

    char ip_string[16] = "";
    if (seatsurfing_config_flash.data.ip[0] || seatsurfing_config_flash.data.ip[1] ||
        seatsurfing_config_flash.data.ip[2] || seatsurfing_config_flash.data.ip[3]) {
        snprintf(ip_string, sizeof(ip_string), "%d.%d.%d.%d", seatsurfing_config_flash.data.ip[0],
                 seatsurfing_config_flash.data.ip[1], seatsurfing_config_flash.data.ip[2],
                 seatsurfing_config_flash.data.ip[3]);
    }

    i2c_probe_result_t i2c_probe = {0};
    i2c_probe_expected_devices(&i2c_probe);
    const bool st25_present = i2c_probe.st25_user_present || i2c_probe.st25_system_present;
    const char *nfc_fieldset_attr = st25_present ? "" : " disabled";

    int n = scaffold_page_open(buf, sz, "SeatSurfing Settings", 30);

    if (msg && *msg) {
        const char *cls = (msg[0] == '\xe2') ? "flash-error" : "flash-ok"; // UTF-8 warning sign
        n += snprintf(buf + n, sz - n, "<p class=\"%s\">%s</p>", cls, msg);
    }

    n += snprintf(
        buf + n, sz - n,
        "<form method=\"POST\" action=\"/seatsurfing\">"
        "<div class=\"section\">"
        "<h2>Room Settings</h2>"
        "<label>Room name:<br><input type=\"text\" name=\"roomname\" value=\"%s\" "
        "maxlength=\"15\"></label>"
        "%s"
        "<div style=\"margin-top:1em;\">"
        "<strong>Room type</strong><br>"
        "<label class=\"inline\"><input type=\"radio\" name=\"type\" value=\"0\" %s> Office</label>"
        "<label class=\"inline\"><input type=\"radio\" name=\"type\" value=\"1\" %s> "
        "Meeting</label>"
        "<label class=\"inline\"><input type=\"radio\" name=\"type\" value=\"2\" %s> Lecture "
        "hall</label>"
        "</div>"
        "<div style=\"margin-top:1em;\">"
        "<strong>Number of seats:</strong> <span id=\"number_of_seats\">%u</span><br>"
        "<small>Number of seats is defined by filled Space Name fields below.</small>"
        "</div>"
        "</div>",
        device_config_flash.data.roomname,
        (seatsurfing_config_flash.data.device_mode == 1
             ? "<small>Set in Seatsurfing while device mode is on.</small>"
             : ""),
        (device_config_flash.data.type == 0 ? "checked" : ""),
        (device_config_flash.data.type == 1 ? "checked" : ""),
        (device_config_flash.data.type == 2 ? "checked" : ""),
        (unsigned)seatsurfing_config_flash.data.seat_count);

    n += snprintf(
        buf + n, sz - n,
        "<div class=\"section\">"
        "<h2>SeatSurfing API</h2>"
        "<label>API Host:<br><input type=\"text\" name=\"text1\" value=\"%s\"></label>"
        "<label>Username:<br><input type=\"text\" name=\"text2\" value=\"%s\"></label>"
        "<label>Password:<br><input type=\"text\" name=\"text3\" value=\"%s\"></label>"
        "<label>IP Address:<br><input type=\"text\" name=\"text4\" value=\"%s\" "
        "placeholder=\"leave empty for DNS+HTTPS\"></label>"
        "<label>Port:<br><input type=\"text\" name=\"text5\" value=\"%d\"></label>"
        "<label>Location ID:<br><input type=\"text\" name=\"text6\" value=\"%s\"></label>"
        "<label>Space Name 1:<br><input type=\"text\" name=\"text7\" value=\"%s\" "
        "oninput=\"updateDerivedSeats()\"></label>"
        "<label>Space Name 2:<br><input type=\"text\" name=\"text8\" value=\"%s\" "
        "oninput=\"updateDerivedSeats()\"></label>"
        "<label>Space Name 3:<br><input type=\"text\" name=\"text9\" value=\"%s\" "
        "oninput=\"updateDerivedSeats()\"></label>"
        "<label>Space Name 4:<br><input type=\"text\" name=\"text10\" value=\"%s\" "
        "oninput=\"updateDerivedSeats()\"></label>"
        "</div>",
        seatsurfing_config_flash.data.host, seatsurfing_config_flash.data.username,
        seatsurfing_config_flash.data.password, ip_string, seatsurfing_config_flash.data.port,
        seatsurfing_config_flash.data.location_id, seatsurfing_config_flash.data.space_ids[0],
        seatsurfing_config_flash.data.space_ids[1], seatsurfing_config_flash.data.space_ids[2],
        seatsurfing_config_flash.data.space_ids[3]);

    char claim_code[SS_CLAIM_CODE_LEN + 1];
    ss_device_claim_code(claim_code, sizeof(claim_code));
    n += snprintf(buf + n, sz - n,
                  "<div class=\"section\">"
                  "<h2>Device Mode</h2>"
                  "<small>Requires the Seatsurfing Plus plugin. The sign enrolls itself with a "
                  "claim code and shows the spaces an admin assigns to it (up to three on the "
                  "7.5\", the first on the 4.2\"); username, password, location and space names "
                  "above are not used.</small>"
                  "<div style=\"margin-top:1em;\">"
                  "<label class=\"inline\"><input type=\"checkbox\" "
                  "name=\"text13\" value=\"1\" %s> Use device mode</label>"
                  "</div>"
                  "<p class=\"small\">Device: %s<br>Claim code: %s</p>"
                  "</div>",
                  seatsurfing_config_flash.data.device_mode == 1 ? "checked" : "",
                  wifi_setup_ssid(), claim_code[0] ? claim_code : "(created on first connection)");

    n += snprintf(buf + n, sz - n,
                  "<fieldset%s class=\"section\" "
                  "style=\"border:none;padding:.72rem .82rem\">"
                  "<h2>NFC Booking</h2>"
                  "<small>SpaceAdmin account used to book seats via NFC tap (opcode 0x40).</small>"
                  "<label style=\"margin-top:0.8em\">Admin E-Mail:<br>"
                  "<input type=\"text\" name=\"text11\" value=\"%s\"></label>"
                  "<label>Admin Password:<br>"
                  "<input type=\"text\" name=\"text12\" value=\"%s\"></label>"
                  "</fieldset>"
                  "<p class=\"small\" style=\"text-align:center\">Seats are derived from filled "
                  "Space Name fields and stored automatically.</p>"
                  "<div style=\"text-align:center\">"
                  "<input type=\"submit\" value=\"Save\">"
                  "</div>"
                  "</form>"
                  "<script>"
                  "function updateDerivedSeats(){"
                  "var c=0;for(var i=7;i<=10;i++){"
                  "var e=document.querySelector('input[name=\"text'+i+'\"]');"
                  "if(e&&e.value.trim().length)c++;}"
                  "if(c===0)c=1;"
                  "var s=document.getElementById('number_of_seats');"
                  "if(s)s.textContent=c;}"
                  "window.addEventListener('load',updateDerivedSeats);"
                  "</script>",
                  nfc_fieldset_attr, seatsurfing_config_flash.data.booking_email,
                  seatsurfing_config_flash.data.booking_password);

    n += snprintf(buf + n, sz - n, "<p class=\"small\">%s</p>", timeout_info);
    scaffold_page_close(buf + n, sz - n, "/", NULL);
}

const char *ss_handle_config_form(const char *body, size_t len) {
    webserver_set_shutdown_time(make_timeout_time_ms(USER_INTERACTION_TIMEOUT_MS));

    web_submission_t result = {0};
    parse_form_fields(body, len, &result);

    seatsurfing_config_t new_cfg = {.crc32 = 0};

    strncpy(new_cfg.data.host, result.text[0], sizeof(new_cfg.data.host) - 1);
    strncpy(new_cfg.data.username, result.text[1], sizeof(new_cfg.data.username) - 1);
    strncpy(new_cfg.data.password, result.text[2], sizeof(new_cfg.data.password) - 1);

    int ip0, ip1, ip2, ip3;
    if (sscanf(result.text[3], "%d.%d.%d.%d", &ip0, &ip1, &ip2, &ip3) == 4) {
        new_cfg.data.ip[0] = (uint8_t)ip0;
        new_cfg.data.ip[1] = (uint8_t)ip1;
        new_cfg.data.ip[2] = (uint8_t)ip2;
        new_cfg.data.ip[3] = (uint8_t)ip3;
    }

    new_cfg.data.port = (uint16_t)atoi(result.text[4]);
    strncpy(new_cfg.data.location_id, result.text[5], sizeof(new_cfg.data.location_id) - 1);

    memset(new_cfg.data.space_ids, 0, sizeof(new_cfg.data.space_ids));
    uint8_t seat_count = 0;
    for (uint8_t i = 0; i < SEATSURFING_MAX_SEATS; i++) {
        if (result.text[6 + i][0]) {
            strncpy(new_cfg.data.space_ids[i], result.text[6 + i],
                    sizeof(new_cfg.data.space_ids[i]) - 1);
            seat_count = (uint8_t)(i + 1);
        }
    }
    if (seat_count == 0) {
        seat_count = 1;
        strncpy(new_cfg.data.space_ids[0], seatsurfing_config_flash.data.space_ids[0],
                sizeof(new_cfg.data.space_ids[0]) - 1);
    }
    new_cfg.data.seat_count = seat_count;

    strncpy(new_cfg.data.booking_email, result.text[10], sizeof(new_cfg.data.booking_email) - 1);
    strncpy(new_cfg.data.booking_password, result.text[11],
            sizeof(new_cfg.data.booking_password) - 1);
    new_cfg.data.device_mode = (result.text[12][0] == '1');

    bool ok = save_seatsurfing_config(&new_cfg);
    if (ok)
        dlog("Seatsurfing config saved.\n");
    else
        dlog("Error saving seatsurfing config.\n");

    device_config_t dev_cfg = {.crc32 = 0};
    memcpy(&dev_cfg.data, &device_config_flash.data, sizeof(device_config_data_t));
    if (result.roomname[0] != '\0')
        strncpy(dev_cfg.data.roomname, result.roomname, sizeof(dev_cfg.data.roomname) - 1);
    if (strstr(body, "type=") != NULL && result.type >= 0 && result.type <= 2)
        dev_cfg.data.type = (RoomType)result.type;
    dev_cfg.data.number_of_seats = seat_count;
    if (!save_device_config(&dev_cfg))
        dlog("Error syncing SeatSurfing-derived room settings.\n");

    return ok ? "\xe2\x9c\x94 seatsurfing settings stored" : "\xe2\x9a\xa0 error saving settings";
}
