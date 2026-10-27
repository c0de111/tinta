#ifndef RTC_H
#define RTC_H

#include "i2c_bus.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rtc_time_t {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day; // weekday: 1=Monday .. 7=Sunday
    uint8_t date;
    uint8_t month;
    uint8_t year; // offset from 2000
} rtc_time_t;

void rtc_init(const i2c_probe_result_t *probe);
bool rtc_is_available(void);
int rtc_read_time(rtc_time_t *out);
void rtc_set_time_from_string(const char *line);
void rtc_sync_from_server(void);
// False if the clock reads a date before this firmware was built: it lost power and has not
// been set since (a fresh RTC starts at 2000-01-01).
bool rtc_time_plausible(void);
// Wakes the sign after the refresh interval of page, or after minutes if that is positive.
void rtc_set_alarm(int page, int minutes);
// Arms the alarm `seconds` from now, rounded to the nearest full minute, at least 60 s ahead.
// The alarm has no date: 24 h or more wraps to the same time of day, so it fires within a day.
void rtc_set_alarm_in(uint32_t seconds);
void rtc_disable_alarm(void);
bool rtc_alarm_flag_is_set(void);
bool rtc_supports_temperature(void);
float rtc_read_temperature_c(void);
bool rtc_is_dst_europe(const rtc_time_t *t);
// Offset of the device's local time from UTC right now, DST included (CET/CEST).
int32_t rtc_local_utc_offset_s(void);
void rtc_format_time(const rtc_time_t *t, char *buffer, size_t buffer_size);
void rtc_format_time_short(const rtc_time_t *t, char *buffer, size_t buffer_size);
// Format as ISO 8601 UTC: "2026-04-08T09:30:00Z" (buffer must be >= 21 bytes)
void rtc_to_iso8601(const rtc_time_t *t, char *buffer, size_t buffer_size);
const char *rtc_day_name(int day);
const char *rtc_month_name(int month);
// Parse HTTP Date header (RFC 7231) into rtc_time_t (UTC). Returns false if header
// absent/malformed.
bool rtc_parse_http_date(const char *header, rtc_time_t *out);
// Parse an RFC 3339 timestamp ("...Z" or "...+02:00", fractional seconds allowed) into
// seconds since 1970-01-01 UTC. Meant for differences between two server timestamps.
bool rtc_parse_iso8601(const char *text, int64_t *epoch_s);

#endif
