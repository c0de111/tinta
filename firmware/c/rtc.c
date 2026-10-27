#include "rtc.h"
#include "config.h"
#include "version.h"
#define LOG_MODULE LOG_MOD_RTC
#include "debug.h"
#include "ds3231.h"
#include "flash.h"
#include "http_client.h"
#include "i2c_bus.h"
#include "rv3028.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    RTC_BACKEND_NONE = 0,
    RTC_BACKEND_DS3231,
    RTC_BACKEND_RV3028,
} rtc_backend_t;

static ds3231_t ds3231_ctx;
static rv3028_t rv3028_ctx;
static rtc_backend_t active_rtc_backend = RTC_BACKEND_NONE;

#define RTC_TIMEZONE_OFFSET_H 1
#define RTC_AUTOSET_THRESHOLD_S 60

// Apply +1h (to_local=true) or -1h (to_local=false) DST offset, wrapping 0-23.
static int dst_adjust_hour(int hour, bool to_local) {
    hour += to_local ? 1 : -1;
    if (hour >= 24)
        hour -= 24;
    if (hour < 0)
        hour += 24;
    return hour;
}

const char *rtc_day_name(int day) {
    switch (day) {
    case 1:
        return "Monday";
    case 2:
        return "Tuesday";
    case 3:
        return "Wednesday";
    case 4:
        return "Thursday";
    case 5:
        return "Friday";
    case 6:
        return "Saturday";
    case 7:
        return "Sunday";
    default:
        return "Invalid";
    }
}

const char *rtc_month_name(int month) {
    switch (month) {
    case 1:
        return "January";
    case 2:
        return "February";
    case 3:
        return "March";
    case 4:
        return "April";
    case 5:
        return "May";
    case 6:
        return "June";
    case 7:
        return "July";
    case 8:
        return "August";
    case 9:
        return "September";
    case 10:
        return "October";
    case 11:
        return "November";
    case 12:
        return "December";
    default:
        return "Invalid";
    }
}

bool rtc_is_dst_europe(const rtc_time_t *t) {
    int year = 2000 + t->year;
    int month = t->month;
    int day = t->date;
    int hour = t->hours;
    int minute = t->minutes;

    if (month < 3 || month > 10)
        return false;
    if (month > 3 && month < 10)
        return true;

    // Last Sunday of March or October
    int last_sunday = 31 - ((5 * year / 4 + (month == 3 ? 4 : 1)) % 7);

    if (month == 3) {
        return (day > last_sunday) ||
               (day == last_sunday && (hour > 1 || (hour == 1 && minute >= 0)));
    } else { // October
        return (day < last_sunday) ||
               (day == last_sunday && (hour < 2 || (hour == 2 && minute == 0)));
    }
}

bool rtc_parse_http_date(const char *header, rtc_time_t *out) {
    const char *d = strstr(header, "Date:");
    if (!d)
        d = strstr(header, "date:");
    if (!d)
        return false;

    d += 5;
    while (*d == ' ')
        d++;

    // Skip day name and comma: "Fri, "
    const char *comma = strchr(d, ',');
    if (!comma)
        return false;
    d = comma + 1;
    while (*d == ' ')
        d++;

    int day, year, hour, minute, second;
    char mon[4] = {0};
    if (sscanf(d, "%d %3s %d %d:%d:%d", &day, mon, &year, &hour, &minute, &second) != 6)
        return false;

    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0)
        return false;

    // Compute weekday (ISO 8601: 1=Mon..7=Sun) using Tomohiko Sakamoto's algorithm
    static const int t_tab[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3)
        y--;
    int dow = (y + y / 4 - y / 100 + y / 400 + t_tab[month - 1] + day) % 7;
    int weekday = (dow == 0) ? 7 : dow; // 0=Sun → 7, else 1=Mon..6=Sat as-is

    *out = (rtc_time_t){.seconds = (uint8_t)second,
                        .minutes = (uint8_t)minute,
                        .hours = (uint8_t)hour,
                        .day = (uint8_t)weekday,
                        .date = (uint8_t)day,
                        .month = (uint8_t)month,
                        .year = (uint8_t)(year - 2000)};
    return true;
}

int32_t rtc_local_utc_offset_s(void) {
    rtc_time_t now = {0};
    rtc_read_time(&now);
    return (RTC_TIMEZONE_OFFSET_H + (rtc_is_dst_europe(&now) ? 1 : 0)) * 3600;
}

void rtc_format_time(const rtc_time_t *t, char *buffer, size_t buffer_size) {
    int display_hour = t->hours;
    if (rtc_is_dst_europe(t))
        display_hour = dst_adjust_hour(display_hour, true);

    snprintf(buffer, buffer_size, "%02i:%02i, %s, %02i. %s %04i", display_hour, t->minutes,
             rtc_day_name(t->day), t->date, rtc_month_name(t->month), 2000 + t->year);
}

void rtc_format_time_short(const rtc_time_t *t, char *buffer, size_t buffer_size) {
    int hour = t->hours;
    if (rtc_is_dst_europe(t))
        hour = dst_adjust_hour(hour, true);
    snprintf(buffer, buffer_size, "%02i:%02i", hour, t->minutes);
}

void rtc_to_iso8601(const rtc_time_t *t, char *buffer, size_t buffer_size) {
    // RTC stores CET (UTC+1); subtract offset to emit true UTC with Z suffix.
    int h = (int)t->hours - RTC_TIMEZONE_OFFSET_H;
    uint8_t d = t->date, m = t->month, y = t->year;
    if (h < 0) {
        h += 24;
        if (d > 1) {
            d--;
        } else {
            static const uint8_t mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            m = (m > 1) ? m - 1 : 12;
            if (m == 12)
                y = y > 0 ? y - 1 : 99;
            int yr = 2000 + y;
            d = (m == 2 && (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0))) ? 29 : mdays[m];
        }
    }
    snprintf(buffer, buffer_size, "20%02u-%02u-%02uT%02d:%02u:%02uZ", y, m, d, h, t->minutes,
             t->seconds);
}

void rtc_init(const i2c_probe_result_t *probe) {
    const bool ds3231_present = probe->ds3231_present;
    const bool rv3028_present = probe->rv3028_present;

    if (ds3231_present && rv3028_present) {
        dlog("Both DS3231 and RV-3028 detected, using DS3231\n");
        ds3231_init(&ds3231_ctx, i2c_default, DS3231_DEVICE_ADRESS, AT24C32_EEPROM_ADRESS_0);
        active_rtc_backend = RTC_BACKEND_DS3231;
        return;
    }

    if (ds3231_present) {
        ds3231_init(&ds3231_ctx, i2c_default, DS3231_DEVICE_ADRESS, AT24C32_EEPROM_ADRESS_0);
        active_rtc_backend = RTC_BACKEND_DS3231;
        debug_status("OK", "RTC: DS3231\n");
        return;
    }

    if (rv3028_present) {
        if (rv3028_init(&rv3028_ctx, i2c_default, RV3028_ADDR_DEFAULT) == 0) {
            active_rtc_backend = RTC_BACKEND_RV3028;
            // PORF: the RTC lost power since the last start; BSF: it ran on VBACKUP meanwhile.
            rtc_time_t t = {0};
            const int rc = rtc_read_time(&t);
            const uint8_t st = rv3028_ctx.boot_status;
            debug_status("OK", "RTC: RV-3028, %s20%02u-%02u-%02u %02u:%02u:%02u%s%s%s\n",
                         rc ? "read failed " : "", t.year, t.month, t.date, t.hours, t.minutes,
                         t.seconds, (st & 0x01u) ? ", lost power" : "",
                         (st & 0x20u) ? ", ran on backup" : "",
                         rv3028_ctx.switchover_eeprom_fixed ? ", backup switch-over stored" : "");
            if (!rv3028_ctx.switchover_ram)
                debug_status("ERROR",
                             "RTC: backup switch-over not set (0x37 = 0x%02X, EEPROM %d)\n",
                             rv3028_ctx.boot_backup, rv3028_ctx.boot_backup_eeprom);
            rv3028_clear_power_flags(&rv3028_ctx);
            return;
        }
        debug_status("ERROR", "RV-3028 detected but init failed\n");
    }

    active_rtc_backend = RTC_BACKEND_NONE;
    debug_status("ERROR", "No supported RTC detected\n");
}

bool rtc_supports_temperature(void) { return (active_rtc_backend == RTC_BACKEND_DS3231); }

float rtc_read_temperature_c(void) {
    if (!rtc_supports_temperature())
        return NAN;

    float t = 0.0f;
    if (ds3231_read_temperature(&ds3231_ctx, &t) != 0) {
        dlog("DS3231 temperature read failed\n");
        return NAN;
    }
    dlog("DS3231 temperature: %.2f °C\n", t);
    return t;
}

bool rtc_alarm_flag_is_set(void) {
    bool flag = false;
    if (active_rtc_backend == RTC_BACKEND_DS3231) {
        if (ds3231_read_alarm2_flag(&ds3231_ctx, &flag) != 0)
            return false;
    } else if (active_rtc_backend == RTC_BACKEND_RV3028) {
        if (rv3028_read_alarm_flag(&rv3028_ctx, &flag) != 0)
            return false;
    }
    return flag;
}

// cppcheck-suppress unusedFunction
bool rtc_is_available(void) { return (active_rtc_backend != RTC_BACKEND_NONE); }

int rtc_read_time(rtc_time_t *out) {
    if (!out)
        return -1;
    if (active_rtc_backend == RTC_BACKEND_NONE)
        return -1;

    if (active_rtc_backend == RTC_BACKEND_RV3028) {
        rv3028_time_t rv_time = {0};
        const int rc = rv3028_read_time(&rv3028_ctx, &rv_time);
        if (rc != 0)
            return rc;

        *out = (rtc_time_t){
            .seconds = rv_time.seconds,
            .minutes = rv_time.minutes,
            .hours = rv_time.hours,
            .day = rv_time.day,
            .date = rv_time.date,
            .month = rv_time.month,
            .year = rv_time.year,
        };
        return 0;
    }

    ds3231_data_t ds_data = {0};
    const int rc = ds3231_read_current_time(&ds3231_ctx, &ds_data);
    if (rc != 0)
        return rc;

    *out = (rtc_time_t){
        .seconds = ds_data.seconds,
        .minutes = ds_data.minutes,
        .hours = ds_data.hours,
        .day = ds_data.day,
        .date = ds_data.date,
        .month = ds_data.month,
        .year = ds_data.year,
    };
    return 0;
}

static int rtc_set_time(const rtc_time_t *t) {
    if (active_rtc_backend == RTC_BACKEND_NONE)
        return -1;

    if (active_rtc_backend == RTC_BACKEND_RV3028) {
        rv3028_time_t rv = {
            .seconds = t->seconds,
            .minutes = t->minutes,
            .hours = t->hours,
            .day = t->day,
            .date = t->date,
            .month = t->month,
            .year = t->year,
        };
        return rv3028_set_time(&rv3028_ctx, &rv);
    }

    ds3231_data_t ds = {
        .seconds = t->seconds,
        .minutes = t->minutes,
        .hours = t->hours,
        .day = t->day,
        .date = t->date,
        .month = t->month,
        .year = t->year,
        .century = 1,
        .am_pm = false,
    };
    return ds3231_configure_time(&ds3231_ctx, &ds);
}

static int month_from_short_name(const char *name) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) {
        if (strncmp(name, months[i], 3) == 0)
            return i + 1;
    }
    return 0;
}

static int weekday_from_name(const char *name) {
    const char *days[] = {"Monday", "Tuesday",  "Wednesday", "Thursday",
                          "Friday", "Saturday", "Sunday"};
    for (int i = 0; i < 7; i++) {
        if (strncmp(name, days[i], strlen(days[i])) == 0)
            return i + 1;
    }
    return 0;
}

void rtc_set_time_from_string(const char *line) {
    if (active_rtc_backend == RTC_BACKEND_NONE) {
        dlog("RTC set skipped: no RTC backend\n");
        return;
    }

    char weekday_str[10] = {0};
    char month_str[4] = {0};
    int day, month, year, hour, minute;

    dlog("RTC set requested from line: ");
    dlog(line);

    // Format: "Sunday, 21. Apr 2025, 13:45"
    int parsed = sscanf(line, "%9[^,], %d. %3s %d , %d:%d", weekday_str, &day, month_str, &year,
                        &hour, &minute);

    if (parsed != 6) {
        dlog("RTC time parse failed.\n");
        dlog("sscanf parsed items: ");
        char buf[2];
        snprintf(buf, sizeof(buf), "%d", parsed);
        dlog(buf);
        dlog("\n");
        return;
    }

    int weekday = weekday_from_name(weekday_str);
    if (weekday == 0) {
        dlog("Invalid weekday name.\n");
        return;
    }

    month = month_from_short_name(month_str);
    if (month == 0) {
        dlog("Invalid month name in RTC string.\n");
        return;
    }

    rtc_time_t temp = {
        .year = year - 2000, .month = month, .date = day, .hours = hour, .minutes = minute};

    if (rtc_is_dst_europe(&temp)) {
        hour = dst_adjust_hour(hour, false);
        if (hour == 23) { // wrapped backwards past midnight
            day -= 1;
            if (day == 0) {
                dlog("DST adjustment underflowed date — skipping RTC set.\n");
                return;
            }
        }
    }

    dlog("Final time to set: ");
    char msg[64];
    snprintf(msg, sizeof(msg), "%02d:%02d %02d.%02d.%04d (weekday: %d)", hour, minute, day, month,
             year, weekday);
    dlog(msg);

    rtc_time_t new_time = {.seconds = 0,
                           .minutes = minute,
                           .hours = hour,
                           .day = weekday,
                           .date = day,
                           .month = month,
                           .year = year - 2000};

    rtc_set_time(&new_time);
}

// Arms the daily alarm for hour:minute in RTC time (no DST).
static void program_alarm(int hour, int minute) {
    if (active_rtc_backend == RTC_BACKEND_RV3028) {
        int rc = rv3028_set_daily_alarm_hour_minute(&rv3028_ctx, (uint8_t)hour, (uint8_t)minute);
        if (rc != 0)
            debug_status("ERROR", "RV-3028 alarm setup failed (rc=%d)\n", rc);

        rc = rv3028_enable_alarm_interrupt(&rv3028_ctx, true);
        if (rc != 0)
            debug_status("ERROR", "RV-3028 alarm interrupt enable failed (rc=%d)\n", rc);

        rc = rv3028_clear_alarm_flag(&rv3028_ctx);
        if (rc != 0)
            debug_status("ERROR", "RV-3028 alarm flag clear failed (rc=%d)\n", rc);

        dlog("RV-3028 alarm set for %02d:%02d (RTC time)\n", hour, minute);
    } else {
        ds3231_alarm_2_t alarm2 = {
            .minutes = minute, .hours = hour, .date = 0, .day = 0, .am_pm = false};

        ds3231_enable_alarm_interrupt(&ds3231_ctx, true);
        ds3231_set_alarm_2(&ds3231_ctx, &alarm2, ON_MATCHING_MINUTE_AND_HOUR);
        dlog("Alarm2 set for %02d:%02d (RTC time)\n", alarm2.hours, alarm2.minutes);

        ds3231_clear_alarm2(&ds3231_ctx);
    }
}

void rtc_set_alarm(int page, int minutes) {
    if (active_rtc_backend == RTC_BACKEND_NONE) {
        debug_status("WARN", "No RTC: skipping alarm\n");
        return;
    }

    rtc_time_t current_time;
    rtc_read_time(&current_time);

    int local_hour = current_time.hours;
    int local_minute = current_time.minutes;
    int day = current_time.day;

    bool dst_active = rtc_is_dst_europe(&current_time);
    if (dst_active) {
        local_hour = dst_adjust_hour(local_hour, true);
        if (local_hour < current_time.hours) // wrapped past midnight
            day = (day % 7) + 1;
    }

    int refresh =
        minutes > 0 ? minutes : device_config_flash.data.refresh_minutes_by_pushbutton[page & 0x07];
    int total_minutes = local_hour * 60 + local_minute + refresh;
    int alarm_hour = (total_minutes / 60) % 24;
    int alarm_minute = total_minutes % 60;

    if (device_config_flash.data.query_only_at_officehours) {
        if (day == 6 || day == 7) {
            dlog("Skipping operation: Weekend detected.\n");
            alarm_hour = 6;
            alarm_minute = 0;
        }
        if (alarm_hour >= 19 || alarm_hour < 6) {
            alarm_hour = 6;
            alarm_minute = 0;
        }
    }

    // Capture local wake time before DST un-adjust (user-visible time)
    int wake_hour_local = alarm_hour;
    int wake_minute_local = alarm_minute;

    if (dst_active)
        alarm_hour = dst_adjust_hour(alarm_hour, false);

    program_alarm(alarm_hour, alarm_minute);

    debug_status("OK", "Next wake: %02d:%02d (+%d min%s)\n", wake_hour_local, wake_minute_local,
                 refresh, minutes > 0 ? ", Wi-Fi back-off" : "");
}

void rtc_set_alarm_in(uint32_t seconds) {
    if (active_rtc_backend == RTC_BACKEND_NONE) {
        debug_status("WARN", "No RTC: skipping alarm\n");
        return;
    }

    // The alarm matches on hour:minute only, so the target is rounded to the nearest full
    // minute. It is never armed less than 60 s ahead, so the match cannot slip past while it
    // is being set.
    rtc_time_t now;
    rtc_read_time(&now);
    uint32_t minutes_ahead = (now.seconds + seconds + 30) / 60;
    if (minutes_ahead * 60 < now.seconds + 60u)
        minutes_ahead = (now.seconds + 60u + 59) / 60;
    uint32_t total_minutes = now.hours * 60u + now.minutes + minutes_ahead;
    int alarm_hour = (int)((total_minutes / 60) % 24);
    int alarm_minute = (int)(total_minutes % 60);

    program_alarm(alarm_hour, alarm_minute);

    int wake_hour_local = rtc_is_dst_europe(&now) ? dst_adjust_hour(alarm_hour, true) : alarm_hour;
    debug_status("OK", "Next wake: %02d:%02d (+%lu min, set by use case)\n", wake_hour_local,
                 alarm_minute, (unsigned long)minutes_ahead);
}

void rtc_disable_alarm(void) {
    if (active_rtc_backend == RTC_BACKEND_RV3028) {
        rv3028_enable_alarm_interrupt(&rv3028_ctx, false);
        rv3028_clear_alarm_flag(&rv3028_ctx);
    } else if (active_rtc_backend == RTC_BACKEND_DS3231) {
        ds3231_enable_alarm_interrupt(&ds3231_ctx, false);
        ds3231_clear_alarm2(&ds3231_ctx);
    }
}

// Days since 1970-01-01 in the proleptic Gregorian calendar.
static int64_t days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int year_of_era = (int)(year - era * 400);
    int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

bool rtc_parse_iso8601(const char *text, int64_t *epoch_s) {
    int year, month, day, hour, minute, second, consumed = 0;
    if (!text || sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute,
                        &second, &consumed) != 6)
        return false;

    const char *p = text + consumed;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9')
            p++;
    }

    int offset_s = 0;
    if (*p == '+' || *p == '-') {
        int offset_h, offset_min;
        if (sscanf(p + 1, "%2d:%2d", &offset_h, &offset_min) != 2)
            return false;
        offset_s = (offset_h * 3600 + offset_min * 60) * (*p == '+' ? 1 : -1);
    } else if (*p != 'Z') {
        return false;
    }

    *epoch_s =
        days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offset_s;
    return true;
}

static int64_t rtc_time_to_seconds(const rtc_time_t *t) {
    int64_t days = (int64_t)(t->year + 2000) * 365 + (t->year + 2000) / 4 + t->date;
    days += (int64_t)t->month * 30;
    return days * 86400 + (int64_t)t->hours * 3600 + (int64_t)t->minutes * 60 + t->seconds;
}

bool rtc_time_plausible(void) {
    rtc_time_t t = {0};
    if (active_rtc_backend == RTC_BACKEND_NONE || rtc_read_time(&t) != 0)
        return false;
    // build_date starts "YYYY-MM-DD"; the clock keeps local time, so allow the day before.
    const char *b = build_date;
    const long built =
        strtol(b, NULL, 10) * 10000 + strtol(b + 5, NULL, 10) * 100 + strtol(b + 8, NULL, 10);
    const long now = (2000L + t.year) * 10000 + t.month * 100L + t.date;
    return now >= built - 1;
}

void rtc_sync_from_server(void) {
    if (!device_config_flash.data.autoset_rtc_enabled)
        return;
    if (active_rtc_backend == RTC_BACKEND_NONE)
        return;

    rtc_time_t server_utc = {0};
    if (!http_get_server_time(&server_utc)) {
        debug_status("WARN", "RTC sync: no server time available\n");
        return;
    }

    int hour_local = server_utc.hours + RTC_TIMEZONE_OFFSET_H;
    if (hour_local >= 24) {
        hour_local -= 24;
        server_utc.date += 1;
    }
    server_utc.hours = (uint8_t)hour_local;

    rtc_time_t rtc_now = {0};
    if (rtc_read_time(&rtc_now) != 0) {
        debug_status("ERROR", "RTC sync: read failed\n");
        return;
    }

    int64_t dev = rtc_time_to_seconds(&server_utc) - rtc_time_to_seconds(&rtc_now);
    if (dev < 0)
        dev = -dev;

    if (dev <= RTC_AUTOSET_THRESHOLD_S) {
        debug_status("OK", "RTC sync: in sync (dev %llds)\n", (long long)dev);
        return;
    }

    int rc = rtc_set_time(&server_utc);
    if (rc != 0) {
        debug_status("ERROR", "RTC sync: write failed (rc=%d)\n", rc);
        return;
    }

    debug_status("OK", "RTC sync: corrected (dev %llds) -> %02d:%02d:%02d\n", (long long)dev,
                 server_utc.hours, server_utc.minutes, server_utc.seconds);
}
