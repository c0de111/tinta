#include "web_status.h"
#include "config.h"
#include "use_case.h"
#include "web_firmware.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "flash.h"
#include "i2c_bus.h"
#include "rtc.h"
#include "sensors.h"
#include "version.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include "wifi.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void build_device_status_page(char *buf, size_t sz) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));

    char buffer[2048];
    char buffer2[256];
    const uint8_t *mac_buf = wifi_mac();

    rtc_time_t now;

    rtc_read_time(&now);
    float vcc = read_battery_voltage(device_config_flash.data.conversion_factor);
    float vbat = read_coin_cell_voltage(device_config_flash.data.conversion_factor);
    float temp_c = read_onchip_temperature_c();
    const bool show_rtc_temperature = rtc_supports_temperature();
    float rtc_temp_c = NAN;
    if (show_rtc_temperature) {
        rtc_temp_c = rtc_read_temperature_c();
    }

    i2c_probe_result_t i2c_probe = {0};
    i2c_probe_expected_devices(&i2c_probe);

    memory_info_t mem = {0};
    get_memory_info(&mem);

    const char *vcc_color = (vcc > 3.5f) ? "green" : (vcc > 3.0f ? "orange" : "red");
    const char *vbat_color = (vbat > 3.1f) ? "green" : (vbat > 2.9f ? "orange" : "red");

    const bool st25_present = i2c_probe.st25_user_present || i2c_probe.st25_system_present;

    const char *ds3231_color =
        (i2c_probe.ds3231_present && i2c_probe.ds3231_status_ok && i2c_probe.ds3231_temp_ok)
            ? "green"
            : (i2c_probe.ds3231_present ? "orange" : "red");
    const char *bmp581_color = (i2c_probe.bmp581_present && i2c_probe.bmp581_chip_id_ok)
                                   ? "green"
                                   : (i2c_probe.bmp581_present ? "orange" : "red");
    const char *rv3028_color = (i2c_probe.rv3028_present && i2c_probe.rv3028_id_ok)
                                   ? "green"
                                   : (i2c_probe.rv3028_present ? "orange" : "red");
    const char *st25_color =
        (st25_present && i2c_probe.st25_ic_ref_ok) ? "green" : (st25_present ? "orange" : "red");

    const char *ds3231_state = i2c_probe.ds3231_present ? "detected" : "not detected";
    const char *bmp581_state = i2c_probe.bmp581_present ? "detected" : "not detected";
    const char *rv3028_state = i2c_probe.rv3028_present ? "detected" : "not detected";
    const char *st25_state = st25_present ? "detected" : "not detected";
    const char *use_case_slug = "unknown";
    switch (USE_CASE_ID) {
    case USE_CASE_ID_SEATSURFING:
        use_case_slug = "seatsurfing";
        break;
    case USE_CASE_ID_HISTORIAN:
        use_case_slug = "historian";
        break;
    case USE_CASE_ID_HOMEMATIC:
        use_case_slug = "homematic";
        break;
    case USE_CASE_ID_WEATHERMAP:
        use_case_slug = "weathermap";
        break;
    case USE_CASE_ID_NEXTCLOUD:
        use_case_slug = "nextcloud";
        break;
    case USE_CASE_ID_NEW_USECASE:
        use_case_slug = "newusecase";
        break;
    default:
        break;
    }
    char device_type_label[32];
    snprintf(device_type_label, sizeof(device_type_label), "tinta-%s", use_case_slug);

    char bmp581_addr_str[8] = "0x47/46";
    char rtc_temp_row[96] = "";

    if (i2c_probe.bmp581_addr != 0xFFu) {
        snprintf(bmp581_addr_str, sizeof(bmp581_addr_str), "0x%02X", i2c_probe.bmp581_addr);
    }

    if (show_rtc_temperature) {
        char rtc_temp_str[16] = "n/a";
        if (isnan(rtc_temp_c)) {
            snprintf(rtc_temp_str, sizeof(rtc_temp_str), "n/a");
        } else {
            float qf = rtc_temp_c * 4.0f;
            int q = (int)((qf >= 0.0f) ? (qf + 0.5f) : (qf - 0.5f));
            int whole = q / 4;
            int rem = q % 4;
            if (rem < 0) {
                rem += 4;
                whole -= 1;
            }
            switch (rem) {
            case 0:
                snprintf(rtc_temp_str, sizeof(rtc_temp_str), "%d.0", whole);
                break;
            case 1:
                snprintf(rtc_temp_str, sizeof(rtc_temp_str), "%d.25", whole);
                break;
            case 2:
                snprintf(rtc_temp_str, sizeof(rtc_temp_str), "%d.5", whole);
                break;
            default:
                snprintf(rtc_temp_str, sizeof(rtc_temp_str), "%d.75", whole);
                break;
            }
        }

        snprintf(rtc_temp_row, sizeof(rtc_temp_row),
                 "<tr><td>RTC Temperature</td><td class='value'>%s &deg;C</td></tr>", rtc_temp_str);
    }

    rtc_format_time(&now, buffer2, sizeof(buffer2));

    int logo_width = 0;
    int logo_height = 0;
    int logo_size = 0;
    bool has_logo = get_flash_logo_info(&logo_width, &logo_height, &logo_size);
    char logo_info_str[96] = "not present";
    if (has_logo) {
        snprintf(logo_info_str, sizeof(logo_info_str), "%dx%d px, %d Bytes", logo_width,
                 logo_height, logo_size);
    }

    firmware_slot_info_t info0 = {0}, info1 = {0};
    bool has0 = get_firmware_slot_info(0, &info0);
    bool has1 = get_firmware_slot_info(1, &info1);

    char slot0_row[512];
    char slot1_row[512];
    if (has0) {
        char slot0_use_case[64] = "n/a";
        format_slot_use_case(0, slot0_use_case, sizeof(slot0_use_case), false);
        snprintf(slot0_row, sizeof(slot0_row),
                 "<tr><td>Slot 0</td><td class='mono'>%s</td><td "
                 "class='mono'>%s</td><td>%s</td><td class='mono'>%u</td><td>%u</td></tr>",
                 info0.git_version, slot0_use_case, info0.build_date, info0.size, info0.valid_flag);
    } else {
        snprintf(slot0_row, sizeof(slot0_row),
                 "<tr><td>Slot 0</td><td colspan='5'><span class='red value'>empty or "
                 "invalid</span></td></tr>");
    }

    if (has1) {
        char slot1_use_case[64] = "n/a";
        format_slot_use_case(1, slot1_use_case, sizeof(slot1_use_case), false);
        snprintf(slot1_row, sizeof(slot1_row),
                 "<tr><td>Slot 1</td><td class='mono'>%s</td><td "
                 "class='mono'>%s</td><td>%s</td><td class='mono'>%u</td><td>%u</td></tr>",
                 info1.git_version, slot1_use_case, info1.build_date, info1.size, info1.valid_flag);
    } else {
        snprintf(slot1_row, sizeof(slot1_row),
                 "<tr><td>Slot 1</td><td colspan='5'><span class='red value'>empty or "
                 "invalid</span></td></tr>");
    }

    int n = scaffold_page_open(buf, sz, "Device Status", 300);

    snprintf(
        buffer, sizeof(buffer),
        "<div class='section'><h2>Device And Network</h2>"
        "<table class='kv'>"
        "<tr><td>Type</td><td class='value'>%s</td></tr>"
        "<tr><td>SSID</td><td class='value'>%s</td></tr>"
        "<tr><td>MAC Address</td><td class='value mono'>%02X:%02X:%02X:%02X:%02X:%02X</td></tr>"
        "<tr><td>Wi-Fi Reconnect</td><td><span class='value'>%d min</span></td></tr>"
        "<tr><td>Wi-Fi Timeout</td><td><span class='value'>%d s</span></td></tr>"
        "<tr><td>Refresh Intervals</td><td class='mono'>[%d, %d, %d, %d, %d, %d, %d, %d]</td></tr>"
        "</table></div>",
        device_type_label, wifi_config_flash.ssid, mac_buf[0], mac_buf[1], mac_buf[2], mac_buf[3],
        mac_buf[4], mac_buf[5], device_config_flash.data.wifi_reconnect_minutes,
        device_config_flash.data.wifi_timeout,
        device_config_flash.data.refresh_minutes_by_pushbutton[0],
        device_config_flash.data.refresh_minutes_by_pushbutton[1],
        device_config_flash.data.refresh_minutes_by_pushbutton[2],
        device_config_flash.data.refresh_minutes_by_pushbutton[3],
        device_config_flash.data.refresh_minutes_by_pushbutton[4],
        device_config_flash.data.refresh_minutes_by_pushbutton[5],
        device_config_flash.data.refresh_minutes_by_pushbutton[6],
        device_config_flash.data.refresh_minutes_by_pushbutton[7]);
    n += snprintf(buf + n, sz - (size_t)n, "%s", buffer);

    snprintf(buffer, sizeof(buffer),
             "<div class='section'><h2>Time And Power</h2>"
             "<table class='kv'>"
             "<tr><td>RTC (Raw)</td><td class='value'>%02d:%02d, %s, %02d. %s %04d</td></tr>"
             "<tr><td>RTC (DST)</td><td class='value'>%s</td></tr>"
             "%s"
             "<tr><td>MCU Temperature</td><td class='value'>%.1f &deg;C</td></tr>"
             "<tr><td>Vcc</td><td><span class='value %s'>%.3f V</span></td></tr>"
             "<tr><td>Vbat</td><td><span class='value %s'>%.3f V</span></td></tr>"
             "<tr><td>ADC Conversion</td><td class='value mono'>%.8f</td></tr>"
             "</table></div>",
             now.hours, now.minutes, rtc_day_name(now.day), now.date, rtc_month_name(now.month),
             2000 + now.year, buffer2, rtc_temp_row, temp_c, vcc_color, vcc, vbat_color, vbat,
             device_config_flash.data.conversion_factor);
    n += snprintf(buf + n, sz - (size_t)n, "%s", buffer);

    snprintf(buffer, sizeof(buffer),
             "<div class='section'><h2>I2C Diagnostics</h2>"
             "<div class='diag-wrap'><table class='diag-table'>"
             "<thead><tr><th>Device</th><th>Addr</th><th>Status</th></tr></thead>"
             "<tbody>"
             "<tr><td>DS3231</td><td class='mono'>0x68</td><td class='value %s'>%s</td></tr>"
             "<tr><td>BMP581</td><td class='mono'>%s</td><td class='value %s'>%s</td></tr>"
             "<tr><td>RV-3028</td><td class='mono'>0x52</td><td class='value %s'>%s</td></tr>"
             "<tr><td>ST25DV</td><td class='mono'>0x53/0x57</td><td class='value %s'>%s</td></tr>"
             "</tbody></table></div></div>",
             ds3231_color, ds3231_state, bmp581_addr_str, bmp581_color, bmp581_state, rv3028_color,
             rv3028_state, st25_color, st25_state);
    n += snprintf(buf + n, sz - (size_t)n, "%s", buffer);

    snprintf(buffer, sizeof(buffer),
             "<div class='section'><h2>Memory</h2>"
             "<table class='kv'>"
             "<tr><td>Heap Used</td><td class='value'>%u KB</td></tr>"
             "<tr><td>Heap Headroom</td><td class='value'>%u KB</td></tr>"
             "<tr><td>Stack Margin</td><td class='value'>%u KB (approx)</td></tr>"
             "<tr><td>Heap Base</td><td class='value mono'>0x%08X</td></tr>"
             "<tr><td>Heap End</td><td class='value mono'>0x%08X</td></tr>"
             "<tr><td>Stack Limit</td><td class='value mono'>0x%08X</td></tr>"
             "<tr><td>SP (core0)</td><td class='value mono'>0x%08X</td></tr>"
             "</table></div>",
             (unsigned)(mem.heap_used_bytes / 1024U), (unsigned)(mem.heap_headroom_bytes / 1024U),
             (unsigned)(mem.stack_margin_bytes / 1024U), (unsigned)mem.heap_base_addr,
             (unsigned)mem.heap_end_addr, (unsigned)mem.heap_limit_addr, (unsigned)mem.sp_addr);
    n += snprintf(buf + n, sz - (size_t)n, "%s", buffer);

    snprintf(buffer, sizeof(buffer),
             "<div class='section'><h2>Flash And Firmware</h2>"
             "<table class='kv'>"
             "<tr><td>Logo In Flash</td><td class='value %s'>%s</td></tr>"
             "<tr><td>Active Firmware</td><td class='value mono'>%s</td></tr>"
             "</table>"
             "<div class='diag-wrap'><table class='diag-table'>"
             "<thead><tr><th>Slot</th><th>Version</th><th>Use Case</th><th>Build</th><th>Size "
             "(B)</th><th>Valid</th></tr></thead>"
             "<tbody>%s%s</tbody>"
             "</table></div></div>",
             has_logo ? "green" : "red", logo_info_str, get_active_firmware_slot_info(), slot0_row,
             slot1_row);
    n += snprintf(buf + n, sz - (size_t)n, "%s", buffer);

    n += snprintf(buf + n, sz - (size_t)n, "<p class=\"small\">%s</p>", timeout_info);
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, "/", NULL);
}
