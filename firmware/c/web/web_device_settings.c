#include "web_device_settings.h"
#include "config.h"
#include "use_case.h"
#include "web_wifi.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "flash.h"
#include "rtc.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include <stdio.h>
#include <string.h>

void build_device_config_page(char *buf, size_t sz, const char *message) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));
    int logo_width = 0, logo_height = 0, logo_bytes = 0;
    bool has_logo = get_flash_logo_info(&logo_width, &logo_height, &logo_bytes);
    rtc_time_t current = {0};
    rtc_read_time(&current);
    char current_raw[64], current_dst[64];
    snprintf(current_raw, sizeof(current_raw), "%02d:%02d:%02d %02d.%02d.%04d", current.hours,
             current.minutes, current.seconds, current.date, current.month, current.year + 2000);
    rtc_format_time(&current, current_dst, sizeof(current_dst));

    int n = scaffold_page_open(buf, sz, "Device Settings", 30);

    n += snprintf(buf + n, sz - (size_t)n,
                  "<style>"
                  "form{max-width:540px}"
                  "fieldset{max-width:540px;margin-left:auto;margin-right:auto}"
                  ".inline-btn{width:96%%;padding:.6em 1em;font-size:1em;margin-top:.4em;"
                  "box-sizing:border-box;border:1px solid #aaa;border-radius:6px;"
                  "background:#fff;cursor:pointer}"
                  ".inline-btn:hover{background:#f0f0f0}"
                  ".inline-btn.danger{background:#c62828;color:white;border:1px solid #9e1f1f}"
                  ".logo-status{margin-top:.5em;font-weight:bold;text-align:center}"
                  ".small-note{margin-top:.4em;font-size:.9em;color:#555}"
                  ".hidden-input{position:absolute;left:-9999px}"
                  "</style>");

    if (message && *message) {
        n += snprintf(buf + n, sz - (size_t)n, "<p class=\"flash-ok\">%s</p>", message);
    }

    n += snprintf(buf + n, sz - (size_t)n, "<form method=\"POST\" action=\"/device_config\">");

    if (use_case.show_display_name)
        n +=
            snprintf(buf + n, sz - (size_t)n,
                     "<fieldset><legend>Name &amp; Title</legend>"
                     "<label>Display title:<br>"
                     "<input type=\"text\" name=\"roomname\" value=\"%s\" maxlength=\"15\"></label>"
                     "</fieldset>",
                     device_config_flash.data.roomname);

    n += snprintf(buf + n, sz - (size_t)n,
                  "<fieldset><legend>ePaper Type</legend>"
                  "<label class=\"inline\"><input type=\"radio\" name=\"epapertype\" value=\"0\" "
                  "%s> None</label>"
                  "<label class=\"inline\"><input type=\"radio\" name=\"epapertype\" value=\"1\" "
                  "%s> 7.5 inch</label>"
                  "<label class=\"inline\"><input type=\"radio\" name=\"epapertype\" value=\"2\" "
                  "%s> 4.2 inch</label>"
                  "</fieldset>",
                  (device_config_flash.data.epapertype == 0 ? "checked" : ""),
                  (device_config_flash.data.epapertype == 1 ? "checked" : ""),
                  (device_config_flash.data.epapertype == 2 ? "checked" : ""));

    n +=
        snprintf(buf + n, sz - (size_t)n, "<fieldset><legend>Refresh Intervals (minutes)</legend>");
    for (int i = 0; i < 8; i++) {
        n += snprintf(buf + n, sz - (size_t)n,
                      "<label>Page %d: <input type=\"text\" inputmode=\"numeric\" "
                      "name=\"refresh%d\" value=\"%d\"></label>",
                      i, i, device_config_flash.data.refresh_minutes_by_pushbutton[i]);
    }
    n += snprintf(buf + n, sz - (size_t)n, "</fieldset>");

    n += snprintf(buf + n, sz - (size_t)n, "<fieldset><legend>WiFi Settings</legend>");
    n = web_wifi_credential_fields(buf, sz, n, "wifi_ssid", "wifi_password", "wifi_region");
    n +=
        snprintf(buf + n, sz - (size_t)n,
                 "<label>Number of WiFi Attempts:<br>"
                 "<input type=\"number\" name=\"number_wifi_attempts\" value=\"%d\" min=\"1\" "
                 "max=\"50\"></label>"
                 "<label>WiFi Timeout (ms):<br>"
                 "<input type=\"number\" name=\"wifi_timeout\" value=\"%d\" min=\"100\" "
                 "max=\"10000\"></label>"
                 "<label>Max Wait for Data (ms):<br>"
                 "<input type=\"number\" name=\"max_wait_data_wifi\" value=\"%d\" min=\"10\" "
                 "max=\"10000\"></label>"
                 "<label>WiFi Reconnect Minutes:<br>"
                 "<input type=\"number\" name=\"wifi_reconnect_minutes\" value=\"%d\" min=\"1\" "
                 "max=\"30\"></label>"
                 "</fieldset>",
                 device_config_flash.data.number_wifi_attempts,
                 device_config_flash.data.wifi_timeout, device_config_flash.data.max_wait_data_wifi,
                 device_config_flash.data.wifi_reconnect_minutes);

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<fieldset><legend>Telemetry</legend>"
        "<label class=\"inline\"><input type=\"checkbox\" name=\"telemetry_enabled\" "
        "value=\"1\" %s> Enable telemetry</label>"
        "<label>Telemetry Host (IPv4):<br>"
        "<input type=\"text\" name=\"telemetry_host\" value=\"%s\" maxlength=\"31\"></label>"
        "<label>Telemetry Port:<br>"
        "<input type=\"text\" name=\"telemetry_port\" value=\"%u\" maxlength=\"5\"></label>"
        "<label>Bearer Token:<br>"
        "<input type=\"text\" name=\"telemetry_token\" value=\"%s\" maxlength=\"95\"></label>"
        "<label>Telemetry Timeout (ms):<br>"
        "<input type=\"text\" name=\"telemetry_timeout_ms\" value=\"%d\" maxlength=\"5\"></label>"
        "<label>Telemetry Label (optional):<br>"
        "<input type=\"text\" name=\"telemetry_label\" value=\"%s\" maxlength=\"31\"></label>"
        "</fieldset>",
        (device_config_flash.data.telemetry_enabled ? "checked" : ""),
        (device_config_flash.data.telemetry_host[0] != '\0')
            ? device_config_flash.data.telemetry_host
            : "192.168.178.85",
        (unsigned)device_config_flash.data.telemetry_port, device_config_flash.data.telemetry_token,
        device_config_flash.data.telemetry_timeout_ms, device_config_flash.data.telemetry_label);

    n +=
        snprintf(buf + n, sz - (size_t)n,
                 "<fieldset><legend>Hardware</legend>"
                 "<label>Battery Cutoff Voltage (V):<br>"
                 "<input type=\"number\" step=\"0.1\" name=\"switch_off_battery_voltage\" "
                 "value=\"%.2f\" min=\"2.4\" max=\"3.9\"></label>"
                 "<label>Watchdog Timeout (ms):<br>"
                 "<input type=\"number\" name=\"watchdog_time\" value=\"%d\" min=\"6000\" "
                 "max=\"8000\"></label>"
                 "<label>Conversion Factor:<br>"
                 "<input type=\"text\" name=\"conversion_factor\" value=\"%.6f\" "
                 "step=\"any\"></label>"
                 "<label class=\"inline\"><input type=\"checkbox\" name=\"led_enabled\" "
                 "value=\"1\" %s> Enable LED</label>"
                 "</fieldset>",
                 device_config_flash.data.switch_off_battery_voltage,
                 device_config_flash.data.watchdog_time, device_config_flash.data.conversion_factor,
                 (device_config_flash.data.led_enabled ? "checked" : ""));

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<div style=\"margin-top:1em\">"
        "<label class=\"inline\"><input type=\"checkbox\" name=\"show_query_date\" "
        "value=\"1\" %s> Show query timestamp</label><br>"
        "<label class=\"inline\"><input type=\"checkbox\" name=\"query_only_at_officehours\""
        " value=\"1\" %s> Query only during office hours (Mon-Fri 7-19h)</label>"
        "</div>",
        (device_config_flash.data.show_query_date ? "checked" : ""),
        (device_config_flash.data.query_only_at_officehours ? "checked" : ""));

    n += snprintf(buf + n, sz - (size_t)n,
                  "<fieldset><legend>Clock</legend>"
                  "<label class=\"inline\"><input type=\"checkbox\" name=\"autoset_rtc_enabled\" "
                  "value=\"1\" %s> Auto-set clock from server</label>"
                  "<div class=\"small-note\">RTC (raw): <b>%s</b></div>"
                  "<div class=\"small-note\">RTC (DST): <b>%s</b></div>"
                  "<div id=\"clockPreview\" class=\"small-note\">Local time: determining...</div>"
                  "<button type=\"button\" class=\"inline-btn\" onclick=\"setClockInline()\">Set "
                  "clock from browser time</button>"
                  "<div id=\"clockStatus\" class=\"logo-status\"></div>"
                  "</fieldset>",
                  (device_config_flash.data.autoset_rtc_enabled ? "checked" : ""), current_raw,
                  current_dst);

    n += snprintf(buf + n, sz - (size_t)n,
                  "<fieldset><legend>Logo</legend>"
                  "<input type=\"file\" id=\"logoFileInput\" class=\"hidden-input\">"
                  "<div style=\"text-align:center\">"
                  "<button type=\"button\" class=\"inline-btn\" "
                  "onclick=\"document.getElementById('logoFileInput').click()\">"
                  "Choose logo file</button>"
                  "</div>"
                  "<div id=\"logoFileName\" class=\"small-note\" "
                  "style=\"text-align:center\">No file selected.</div>"
                  "<div style=\"text-align:center\">"
                  "<button type=\"button\" class=\"inline-btn\" "
                  "onclick=\"uploadLogoInline()\">Upload logo</button>"
                  "</div>"
                  "<progress id=\"logoProgress\" max=\"100\" value=\"0\" "
                  "style=\"display:block;margin:.5em auto\"></progress>"
                  "<div id=\"logoStatus\" class=\"logo-status\"></div>"
                  "<div class=\"small-note\" style=\"text-align:center\">"
                  "Max file size: %d bytes</div>",
                  LOGO_FLASH_SIZE);

    if (has_logo) {
        n += snprintf(buf + n, sz - (size_t)n,
                      "<div class=\"small-note\" style=\"text-align:center\">"
                      "<b>Stored logo:</b> %d x %d px, %d bytes</div>"
                      "<div style=\"text-align:center\">"
                      "<button type=\"button\" class=\"inline-btn danger\" "
                      "onclick=\"deleteLogoInline()\">Delete logo</button></div>",
                      logo_width, logo_height, logo_bytes);
    } else {
        n += snprintf(buf + n, sz - (size_t)n,
                      "<div class=\"small-note\" style=\"text-align:center\">"
                      "<i>No custom logo stored.</i></div>");
    }

    n += snprintf(buf + n, sz - (size_t)n,
                  "</fieldset>"
                  "<div style=\"text-align:center;margin-top:1em\">"
                  "<input type=\"submit\" value=\"Save\">"
                  "</div>"
                  "</form>");

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<script>"
        "const LOGO_MAX_SIZE=%d;"
        "function setLogoStatus(msg,color){"
        "var e=document.getElementById('logoStatus');"
        "if(e){e.textContent=msg;e.style.color=color||'#111';}}"
        "function setClockStatus(msg,color){"
        "var e=document.getElementById('clockStatus');"
        "if(e){e.textContent=msg;e.style.color=color||'#111';}}"
        "function buildClockLine(){"
        "var now=new Date();"
        "var weekday="
        "['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'][now.getDay()];"
        "var months=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];"
        "var day=now.getDate();"
        "var month=months[now.getMonth()];"
        "var year=now.getFullYear();"
        "var hour=now.getHours().toString().padStart(2,'0');"
        "var minute=now.getMinutes().toString().padStart(2,'0');"
        "return weekday+', '+day+'. '+month+' '+year+', '+hour+':'+minute;}"
        "function refreshClockPreview(){"
        "var e=document.getElementById('clockPreview');"
        "var line=buildClockLine();"
        "if(e)e.textContent='Local time: '+line;return line;}"
        "function setClockInline(){"
        "var line=refreshClockPreview();"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/clock',true);"
        "xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "xhr.onload=function(){"
        "if(xhr.status===200&&xhr.responseText.indexOf('\\u274c')===-1)"
        "setClockStatus('Clock set.','green');"
        "else setClockStatus('Clock update failed.','#c62828');};"
        "xhr.onerror=function(){setClockStatus('Clock update error.','#c62828');};"
        "setClockStatus('Setting clock...','#111');"
        "xhr.send('line='+encodeURIComponent(line));}"
        "function uploadLogoInline(){"
        "var file=document.getElementById('logoFileInput').files[0];"
        "var progress=document.getElementById('logoProgress');"
        "if(!file){setLogoStatus('Please select a file first.','#c62828');return;}"
        "if(file.size>LOGO_MAX_SIZE){"
        "setLogoStatus('File too large ('+file.size+' bytes, max '+LOGO_MAX_SIZE+"
        "' bytes).','#c62828');return;}"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/upload_logo',true);"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.upload.onprogress=function(ev){"
        "if(ev.lengthComputable&&progress)"
        "progress.value=Math.round(ev.loaded/ev.total*100);};"
        "xhr.onload=function(){"
        "if(xhr.status===200)setLogoStatus('Logo uploaded.','green');"
        "else setLogoStatus('Upload failed.','#c62828');};"
        "xhr.onerror=function(){setLogoStatus('Upload error.','#c62828');};"
        "setLogoStatus('Uploading...','#111');"
        "if(progress)progress.value=0;xhr.send(file);}"
        "function deleteLogoInline(){"
        "if(!confirm('Delete stored logo?'))return;"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/delete_logo',true);"
        "xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "xhr.onload=function(){"
        "if(xhr.status===200)setLogoStatus('Logo deleted.','green');"
        "else setLogoStatus('Delete failed.','#c62828');};"
        "xhr.onerror=function(){setLogoStatus('Delete error.','#c62828');};"
        "setLogoStatus('Deleting...','#111');xhr.send('delete=1');}"
        "refreshClockPreview();"
        "document.getElementById('logoFileInput').addEventListener('change',function(){"
        "var fn=document.getElementById('logoFileName');"
        "var f=this.files&&this.files[0];"
        "if(fn)fn.textContent=f?f.name:'No file selected.';});"
        "</script>",
        LOGO_FLASH_SIZE);

    n += snprintf(buf + n, sz - (size_t)n, "<p class=\"small\">%s</p>", timeout_info);
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, "/", NULL);
}

void handle_form_device_config(const char *body, size_t len, char *buf, size_t sz) {
    web_submission_t result = {0};

    parse_form_fields(body, len, &result);

    device_config_t new_cfg = {.crc32 = 0};
    wifi_config_t new_wifi = {.crc32 = 0};

    memcpy(&new_cfg.data, &device_config_flash.data, sizeof(device_config_data_t));
    memcpy(&new_wifi, &wifi_config_flash, sizeof(wifi_config_t));

    if (use_case.show_display_name)
        strncpy(new_cfg.data.roomname, result.roomname, sizeof(new_cfg.data.roomname) - 1);
    new_cfg.data.epapertype = (EpaperType)result.epapertype;

    for (int i = 0; i < 8; i++) {
        new_cfg.data.refresh_minutes_by_pushbutton[i] = result.refresh_minutes_by_pushbutton[i];
    }

    new_cfg.data.show_query_date = result.show_query_date;
    new_cfg.data.query_only_at_officehours = result.query_only_at_officehours;
    new_cfg.data.wifi_reconnect_minutes = result.wifi_reconnect_minutes;
    new_cfg.data.watchdog_time = result.watchdog_time;
    new_cfg.data.number_wifi_attempts = result.number_wifi_attempts;
    new_cfg.data.wifi_timeout = result.wifi_timeout;
    new_cfg.data.max_wait_data_wifi = result.max_wait_data_wifi;
    new_cfg.data.conversion_factor = result.conversion_factor;
    new_cfg.data.autoset_rtc_enabled = result.autoset_rtc_enabled;
    new_cfg.data.led_enabled = result.led_enabled;
    new_cfg.data.telemetry_enabled = result.telemetry_enabled;
    new_cfg.data.telemetry_timeout_ms =
        (result.telemetry_timeout_ms >= 100 && result.telemetry_timeout_ms <= 10000)
            ? result.telemetry_timeout_ms
            : 2000;
    strncpy(new_cfg.data.telemetry_host, result.telemetry_host,
            sizeof(new_cfg.data.telemetry_host) - 1);
    new_cfg.data.telemetry_host[sizeof(new_cfg.data.telemetry_host) - 1] = '\0';
    new_cfg.data.telemetry_port = (result.telemetry_port >= 1 && result.telemetry_port <= 65535)
                                      ? (uint16_t)result.telemetry_port
                                      : 3004;
    strncpy(new_cfg.data.telemetry_token, result.telemetry_token,
            sizeof(new_cfg.data.telemetry_token) - 1);
    new_cfg.data.telemetry_token[sizeof(new_cfg.data.telemetry_token) - 1] = '\0';
    strncpy(new_cfg.data.telemetry_label, result.telemetry_label,
            sizeof(new_cfg.data.telemetry_label) - 1);
    new_cfg.data.telemetry_label[sizeof(new_cfg.data.telemetry_label) - 1] = '\0';

    if (result.wifi_ssid[0] != '\0' || result.wifi_password[0] != '\0') {
        strncpy(new_wifi.ssid, result.wifi_ssid, sizeof(new_wifi.ssid) - 1);
        new_wifi.ssid[sizeof(new_wifi.ssid) - 1] = '\0';
        strncpy(new_wifi.password, result.wifi_password, sizeof(new_wifi.password) - 1);
        new_wifi.password[sizeof(new_wifi.password) - 1] = '\0';
    }

    new_wifi.region = (uint8_t)result.wifi_region; // checked in save_wifi_config
    new_wifi.region_check = new_wifi.region ^ WIFI_REGION_CHECK;
    bool ok_wifi = save_wifi_config(&new_wifi);
    bool ok_dev = save_device_config(&new_cfg);

    const char *msg = NULL;
    if (ok_wifi && ok_dev) {
        msg = "\xe2\x9c\x94 Device + WiFi settings saved";
    } else if (!ok_dev && !ok_wifi) {
        msg = "\xe2\x9a\xa0 Error saving Device + WiFi settings";
    } else if (!ok_dev) {
        msg = "\xe2\x9a\xa0 WiFi saved, Device settings failed";
    } else {
        msg = "\xe2\x9a\xa0 Device settings saved, WiFi failed";
    }
    build_device_config_page(buf, sz, msg);

    if (ok_dev && ok_wifi) {
        dlog("Device and WiFi configuration saved\n");
    } else {
        if (!ok_dev) {
            dlog("Error saving device configuration\n");
        }
        if (!ok_wifi) {
            dlog("Error saving WiFi configuration\n");
        }
    }
}
