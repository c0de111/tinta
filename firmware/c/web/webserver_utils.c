#include "webserver_utils.h"
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void url_decode(char *dst, const char *src, size_t dst_len) {
    char a, b;
    size_t i = 0;

    while (*src && i + 1 < dst_len) {
        if (*src == '%') {
            a = src[1];
            b = src[2];
            if (a && b && isxdigit((unsigned char)a) && isxdigit((unsigned char)b)) {
                // Umwandlung von zwei Hex-Zeichen in ein Byte
                a = (char)(isdigit((unsigned char)a) ? a - '0'
                                                     : toupper((unsigned char)a) - 'A' + 10);
                b = (char)(isdigit((unsigned char)b) ? b - '0'
                                                     : toupper((unsigned char)b) - 'A' + 10);
                dst[i++] = (char)(16 * a + b);
                src += 3;
            } else {
                // Invalid encoding, keep '%' as-is
                dst[i++] = *src++;
            }
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

void reset_upload_session(void) {
    upload_session.active = false;
    upload_session.header_complete = false;
    upload_session.header_length = 0;
    upload_session.total_received = 0;
    upload_session.expected_length = 0;
    upload_session.flash_offset = 0;
    upload_session.type = UPLOAD_NONE;
}

void parse_form_fields(const char *body, size_t len, web_submission_t *result) {
    memset(result, 0, sizeof(web_submission_t));

    const char *ptr = body;
    while (ptr < body + len) {
        const char *eq = strchr(ptr, '=');
        if (!eq || eq >= body + len)
            break;

        const char *key = ptr;
        const char *val = eq + 1;

        // Fehler behoben: Suche nach '&' nur im erlaubten Bereich
        const char *amp = memchr(val, '&', body + len - val);
        if (!amp)
            amp = body + len;

        size_t key_len = (size_t)(eq - key);
        size_t val_len = (size_t)(amp - val);

        // Helper buffer for temporary decoding
        char value_buf[MAX_FIELD_LENGTH] = {0};
        int j = 0;
        for (size_t i = 0; i < val_len && j < (int)sizeof(value_buf) - 1; i++) {
            if (val[i] == '+') {
                value_buf[j++] = ' ';
            } else if (val[i] == '%' && (i + 2) < val_len) {
                const char hex[3] = {val[i + 1], val[i + 2], 0};
                value_buf[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                value_buf[j++] = val[i];
            }
        }
        value_buf[j] = 0;

        // Allgemeine Textfelder (optional)
        if (key_len >= 5 && strncmp(key, "text", 4) == 0) {
            int idx = atoi(&key[4]) - 1;
            if (idx >= 0 && idx < 128) {
                strncpy(result->text[idx], value_buf, MAX_FIELD_LENGTH - 1);
                result->text[idx][MAX_FIELD_LENGTH - 1] = 0;
            }
        }

        // Abbruchfeld
        else if (key_len == 5 && strncmp(key, "abort", 5) == 0) {
            result->aborted = true;
        }

        // Zeit- und Datumsfelder
        else if (key_len == 4 && strncmp(key, "hour", 4) == 0)
            result->hour = atoi(value_buf);
        else if (key_len == 6 && strncmp(key, "minute", 6) == 0)
            result->minute = atoi(value_buf);
        else if (key_len == 6 && strncmp(key, "second", 6) == 0)
            result->second = atoi(value_buf);
        else if (key_len == 3 && strncmp(key, "day", 3) == 0)
            result->day = atoi(value_buf);
        else if (key_len == 4 && strncmp(key, "date", 4) == 0)
            result->date = atoi(value_buf);
        else if (key_len == 5 && strncmp(key, "month", 5) == 0)
            result->month = atoi(value_buf);
        else if (key_len == 4 && strncmp(key, "year", 4) == 0)
            result->year = atoi(value_buf);

        // Generic unit for timing (e.g., Morse unit)
        else if (key_len == 7 && strncmp(key, "unit_ms", 7) == 0)
            result->unit_ms = atoi(value_buf);
        else if (key_len == 6 && strncmp(key, "action", 6) == 0) {
            strncpy(result->action, value_buf, sizeof(result->action) - 1);
        } else if (key_len == 5 && strncmp(key, "align", 5) == 0) {
            strncpy(result->align, value_buf, sizeof(result->align) - 1);
        } else if (key_len == 9 && strncmp(key, "font_size", 9) == 0) {
            strncpy(result->font_size, value_buf, sizeof(result->font_size) - 1);
        } else if (key_len == 16 && strncmp(key, "service_messages", 16) == 0) {
            result->homematic_service_messages = true;
        }

        // Device configuration
        else if (key_len == 8 && strncmp(key, "roomname", 8) == 0) {
            strncpy(result->roomname, value_buf, sizeof(result->roomname) - 1);
        } else if (key_len == 4 && strncmp(key, "type", 4) == 0) {
            result->type = atoi(value_buf);
        } else if (key_len == 10 && strncmp(key, "epapertype", 10) == 0) {
            result->epapertype = atoi(value_buf);
        } else if (key_len >= 7 && strncmp(key, "refresh", 7) == 0 && key[7] >= '0' &&
                   key[7] <= '7') {
            int idx = key[7] - '0';
            result->refresh_minutes_by_pushbutton[idx] = atoi(value_buf);
        } else if (key_len == 15 && strncmp(key, "number_of_seats", 15) == 0) {
            result->number_of_seats = atoi(value_buf);
        } else if (key_len == 15 && strncmp(key, "show_query_date", 15) == 0) {
            result->show_query_date = true;
        } else if (key_len == 25 && strncmp(key, "query_only_at_officehours", 25) == 0) {
            result->query_only_at_officehours = true;
        } else if (key_len == 22 && strncmp(key, "wifi_reconnect_minutes", 22) == 0) {
            result->wifi_reconnect_minutes = atoi(value_buf);
        } else if (key_len == 13 && strncmp(key, "watchdog_time", 13) == 0) {
            result->watchdog_time = atoi(value_buf);
        } else if (key_len == 20 && strncmp(key, "number_wifi_attempts", 20) == 0) {
            result->number_wifi_attempts = atoi(value_buf);
        } else if (key_len == 12 && strncmp(key, "wifi_timeout", 12) == 0) {
            result->wifi_timeout = atoi(value_buf);
        } else if (key_len == 18 && strncmp(key, "max_wait_data_wifi", 18) == 0) {
            result->max_wait_data_wifi = atoi(value_buf);
        } else if (key_len == 17 && strncmp(key, "conversion_factor", 17) == 0) {
            result->conversion_factor = strtof(value_buf, NULL);
        } else if (key_len == 19 && strncmp(key, "autoset_rtc_enabled", 19) == 0) {
            result->autoset_rtc_enabled = true;
        } else if (key_len == 11 && strncmp(key, "led_enabled", 11) == 0) {
            result->led_enabled = true;
        } else if (key_len == 17 && strncmp(key, "telemetry_enabled", 17) == 0) {
            result->telemetry_enabled = true;
        } else if (key_len == 20 && strncmp(key, "telemetry_timeout_ms", 20) == 0) {
            result->telemetry_timeout_ms = atoi(value_buf);
        } else if (key_len == 14 && strncmp(key, "telemetry_host", 14) == 0) {
            strncpy(result->telemetry_host, value_buf, sizeof(result->telemetry_host) - 1);
        } else if (key_len == 14 && strncmp(key, "telemetry_port", 14) == 0) {
            result->telemetry_port = atoi(value_buf);
        } else if (key_len == 15 && strncmp(key, "telemetry_token", 15) == 0) {
            strncpy(result->telemetry_token, value_buf, sizeof(result->telemetry_token) - 1);
        } else if (key_len == 15 && strncmp(key, "telemetry_label", 15) == 0) {
            strncpy(result->telemetry_label, value_buf, sizeof(result->telemetry_label) - 1);
        } else if (key_len == 9 && strncmp(key, "wifi_ssid", 9) == 0) {
            strncpy(result->wifi_ssid, value_buf, sizeof(result->wifi_ssid) - 1);
        } else if (key_len == 11 && strncmp(key, "wifi_region", 11) == 0) {
            result->wifi_region = atoi(value_buf);
        } else if (key_len == 13 && strncmp(key, "wifi_password", 13) == 0) {
            strncpy(result->wifi_password, value_buf, sizeof(result->wifi_password) - 1);
        }
        ptr = amp + 1;
    }
}
