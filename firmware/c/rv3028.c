#include "rv3028.h"
#include "pico/time.h"

#include <stddef.h>
#include <string.h>

#define RV3028_REG_SECONDS 0x00u
#define RV3028_REG_MINUTES 0x01u
#define RV3028_REG_HOURS 0x02u
#define RV3028_REG_WEEKDAY 0x03u
#define RV3028_REG_DATE 0x04u
#define RV3028_REG_MONTH 0x05u
#define RV3028_REG_YEAR 0x06u

#define RV3028_REG_MINUTES_ALARM 0x07u
#define RV3028_REG_HOURS_ALARM 0x08u
#define RV3028_REG_WEEKDAY_DATE_ALARM 0x09u

#define RV3028_REG_STATUS 0x0Eu
#define RV3028_REG_CONTROL1 0x0Fu
#define RV3028_REG_CONTROL2 0x10u
#define RV3028_REG_EEPROM_BACKUP 0x37u
#define RV3028_REG_ID 0x28u

#define RV3028_REG_EEADDR 0x25u
#define RV3028_REG_EEDATA 0x26u
#define RV3028_REG_EECMD 0x27u
#define RV3028_EECMD_WRITE_ONE 0x21u
#define RV3028_EECMD_READ_ONE 0x22u

#define RV3028_STATUS_AF_BIT (1u << 2)
#define RV3028_STATUS_EEBUSY_BIT (1u << 7)
#define RV3028_STATUS_BSF_BIT (1u << 5)
#define RV3028_STATUS_PORF_BIT (1u << 0)
#define RV3028_CONTROL1_EERD_BIT (1u << 3)

#define RV3028_CONTROL1_WADA_BIT (1u << 5)
#define RV3028_CONTROL2_AIE_BIT (1u << 3)
#define RV3028_CONTROL2_12_24_BIT (1u << 1)

#define RV3028_BACKUP_FEDE_BIT (1u << 4)
#define RV3028_BACKUP_BSM_MASK (3u << 2)
#define RV3028_BACKUP_BSM_LSM (3u << 2)

static uint8_t bcd_to_bin(uint8_t value) {
    return (uint8_t)(((value >> 4u) * 10u) + (value & 0x0Fu));
}

static uint8_t bin_to_bcd(uint8_t value) {
    return (uint8_t)(((value / 10u) << 4u) | (value % 10u));
}

static bool bcd_value_in_range(uint8_t bcd, uint8_t min_value, uint8_t max_value) {
    const uint8_t low = (uint8_t)(bcd & 0x0Fu);
    const uint8_t high = (uint8_t)((bcd >> 4u) & 0x0Fu);
    if (low > 9u || high > 9u) {
        return false;
    }

    const uint8_t value = bcd_to_bin(bcd);
    return (value >= min_value) && (value <= max_value);
}

static int rv3028_read_bytes(rv3028_t *rtc, uint8_t reg, uint8_t *data, size_t len) {
    if (!rtc || !rtc->i2c || !data || len == 0u) {
        return -1;
    }

    const int write_rc = i2c_write_blocking(rtc->i2c, rtc->addr, &reg, 1, true);
    if (write_rc != 1) {
        return -2;
    }

    const int read_rc = i2c_read_blocking(rtc->i2c, rtc->addr, data, len, false);
    if (read_rc != (int)len) {
        return -3;
    }

    return 0;
}

static int rv3028_write_bytes(rv3028_t *rtc, uint8_t reg, const uint8_t *data, size_t len) {
    if (!rtc || !rtc->i2c || !data || len == 0u || len > 15u) {
        return -1;
    }

    uint8_t buffer[16];
    buffer[0] = reg;
    memcpy(&buffer[1], data, len);

    const int write_rc = i2c_write_blocking(rtc->i2c, rtc->addr, buffer, len + 1u, false);
    if (write_rc != (int)(len + 1u)) {
        return -2;
    }

    return 0;
}

static int rv3028_read_u8(rv3028_t *rtc, uint8_t reg, uint8_t *value) {
    return rv3028_read_bytes(rtc, reg, value, 1u);
}

static int rv3028_write_u8(rv3028_t *rtc, uint8_t reg, uint8_t value) {
    return rv3028_write_bytes(rtc, reg, &value, 1u);
}

// After power-up the RV-3028 copies its configuration from EEPROM into RAM. A register
// written before that ends is overwritten by the copy, so every access to the
// configuration waits for EEBusy to clear first.
static bool rv3028_wait_eeprom_idle(rv3028_t *rtc) {
    for (int ms = 0; ms < 100; ms++) {
        uint8_t status = 0u;
        if (rv3028_read_u8(rtc, RV3028_REG_STATUS, &status) != 0)
            return false;
        if (!(status & RV3028_STATUS_EEBUSY_BIT))
            return true;
        sleep_ms(1);
    }
    return false;
}

// One EEPROM byte by command (datasheet: EERD = 1, EEADDR, EECMD 00h then 22h to read or 21h
// to write, wait for EEBusy). Returns the byte read, the byte written, or -1.
static int rv3028_eeprom_byte(rv3028_t *rtc, uint8_t addr, int write_value) {
    uint8_t control1 = 0u;
    if (!rv3028_wait_eeprom_idle(rtc) || rv3028_read_u8(rtc, RV3028_REG_CONTROL1, &control1) != 0)
        return -1;
    if (rv3028_write_u8(rtc, RV3028_REG_CONTROL1, (uint8_t)(control1 | RV3028_CONTROL1_EERD_BIT)) !=
        0)
        return -1;
    int result = -1;
    const bool write = write_value >= 0;
    if (rv3028_write_u8(rtc, RV3028_REG_EEADDR, addr) == 0 &&
        (!write || rv3028_write_u8(rtc, RV3028_REG_EEDATA, (uint8_t)write_value) == 0) &&
        rv3028_write_u8(rtc, RV3028_REG_EECMD, 0x00u) == 0 &&
        rv3028_write_u8(rtc, RV3028_REG_EECMD,
                        write ? RV3028_EECMD_WRITE_ONE : RV3028_EECMD_READ_ONE) == 0 &&
        rv3028_wait_eeprom_idle(rtc)) {
        uint8_t data = 0u;
        if (write)
            result = write_value;
        else if (rv3028_read_u8(rtc, RV3028_REG_EEDATA, &data) == 0)
            result = data;
    }
    rv3028_write_u8(rtc, RV3028_REG_CONTROL1, control1);
    return result;
}

// Without a coin cell the RTC lives on VBACKUP (fed from the batteries) while Tinta is off,
// which needs the switch-over in level mode: BSM = 11, FEDE = 1.
static uint8_t with_switchover(uint8_t backup) {
    return (uint8_t)((backup & (uint8_t)~RV3028_BACKUP_BSM_MASK) | RV3028_BACKUP_BSM_LSM |
                     RV3028_BACKUP_FEDE_BIT);
}

// Sets the switch-over in RAM and checks it. The EEPROM copy, which the RTC loads after every
// loss of power, gets it too if it lacks it: a battery change must not stop the clock.
static int rv3028_configure_backup_switchover(rv3028_t *rtc) {
    rtc->switchover_ram = false;
    rtc->switchover_eeprom_fixed = false;
    uint8_t backup = 0u;
    if (!rv3028_wait_eeprom_idle(rtc))
        return -1;
    int rc = rv3028_read_u8(rtc, RV3028_REG_EEPROM_BACKUP, &backup);
    if (rc != 0)
        return rc;
    if (with_switchover(backup) != backup)
        rv3028_write_u8(rtc, RV3028_REG_EEPROM_BACKUP, with_switchover(backup));
    uint8_t check = 0u;
    rtc->switchover_ram = rv3028_read_u8(rtc, RV3028_REG_EEPROM_BACKUP, &check) == 0 &&
                          check == with_switchover(backup);

    const int stored = rtc->boot_backup_eeprom;
    if (stored >= 0 && with_switchover((uint8_t)stored) != (uint8_t)stored &&
        rv3028_eeprom_byte(rtc, RV3028_REG_EEPROM_BACKUP, with_switchover((uint8_t)stored)) >= 0)
        rtc->switchover_eeprom_fixed = rv3028_eeprom_byte(rtc, RV3028_REG_EEPROM_BACKUP, -1) ==
                                       with_switchover((uint8_t)stored);
    return 0;
}

int rv3028_init(rv3028_t *rtc, i2c_inst_t *i2c, uint8_t addr) {
    if (!rtc || !i2c) {
        return -1;
    }

    rtc->i2c = i2c;
    rtc->addr = (addr == 0u) ? RV3028_ADDR_DEFAULT : addr;
    rtc->boot_status = 0u;
    rtc->boot_backup = 0u;
    rv3028_wait_eeprom_idle(rtc);
    rv3028_read_u8(rtc, RV3028_REG_STATUS, &rtc->boot_status);
    rv3028_read_u8(rtc, RV3028_REG_EEPROM_BACKUP, &rtc->boot_backup);
    rtc->boot_backup_eeprom = (int16_t)rv3028_eeprom_byte(rtc, RV3028_REG_EEPROM_BACKUP, -1);
    return rv3028_configure_backup_switchover(rtc);
}

int rv3028_clear_power_flags(rv3028_t *rtc) {
    uint8_t status = 0u;
    int rc = rv3028_read_u8(rtc, RV3028_REG_STATUS, &status);
    if (rc != 0)
        return rc;
    status = (uint8_t)(status & (uint8_t)~(RV3028_STATUS_PORF_BIT | RV3028_STATUS_BSF_BIT));
    return rv3028_write_u8(rtc, RV3028_REG_STATUS, status);
}

int rv3028_read_time(rv3028_t *rtc, rv3028_time_t *time_data) {
    if (!time_data) {
        return -1;
    }

    uint8_t raw[7] = {0};
    int rc = rv3028_read_bytes(rtc, RV3028_REG_SECONDS, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    uint8_t control2 = 0u;
    rc = rv3028_read_u8(rtc, RV3028_REG_CONTROL2, &control2);
    if (rc != 0) {
        return rc;
    }
    const bool use_12h_mode = (control2 & RV3028_CONTROL2_12_24_BIT) != 0u;

    const uint8_t sec_bcd = (uint8_t)(raw[0] & 0x7Fu);
    const uint8_t min_bcd = (uint8_t)(raw[1] & 0x7Fu);
    if (!bcd_value_in_range(sec_bcd, 0u, 59u) || !bcd_value_in_range(min_bcd, 0u, 59u)) {
        return -4;
    }

    uint8_t hour = 0u;
    if (use_12h_mode) {
        const bool is_pm = (raw[2] & 0x20u) != 0u;
        const uint8_t hour12_bcd = (uint8_t)(raw[2] & 0x1Fu);
        if (!bcd_value_in_range(hour12_bcd, 1u, 12u)) {
            return -5;
        }
        hour = bcd_to_bin(hour12_bcd);
        if (hour == 12u) {
            hour = 0u;
        }
        if (is_pm) {
            hour = (uint8_t)(hour + 12u);
        }
    } else {
        const uint8_t hour_bcd = (uint8_t)(raw[2] & 0x3Fu);
        if (!bcd_value_in_range(hour_bcd, 0u, 23u)) {
            return -5;
        }
        hour = bcd_to_bin(hour_bcd);
    }

    const uint8_t weekday = (uint8_t)(raw[3] & 0x07u);
    if (weekday > 6u) {
        return -6;
    }

    const uint8_t date_bcd = (uint8_t)(raw[4] & 0x3Fu);
    const uint8_t month_bcd = (uint8_t)(raw[5] & 0x1Fu);
    const uint8_t year_bcd = raw[6];

    if (!bcd_value_in_range(date_bcd, 1u, 31u) || !bcd_value_in_range(month_bcd, 1u, 12u) ||
        !bcd_value_in_range(year_bcd, 0u, 99u)) {
        return -7;
    }

    time_data->seconds = bcd_to_bin(sec_bcd);
    time_data->minutes = bcd_to_bin(min_bcd);
    time_data->hours = hour;
    time_data->day = (uint8_t)(weekday + 1u); // map RV weekday 0..6 to internal 1..7
    time_data->date = bcd_to_bin(date_bcd);
    time_data->month = bcd_to_bin(month_bcd);
    time_data->year = bcd_to_bin(year_bcd);

    return 0;
}

int rv3028_set_time(rv3028_t *rtc, const rv3028_time_t *time_data) {
    if (!time_data) {
        return -1;
    }

    if (time_data->seconds > 59u || time_data->minutes > 59u || time_data->hours > 23u ||
        time_data->day < 1u || time_data->day > 7u || time_data->date < 1u ||
        time_data->date > 31u || time_data->month < 1u || time_data->month > 12u ||
        time_data->year > 99u) {
        return -2;
    }

    uint8_t control2 = 0u;
    int rc = rv3028_read_u8(rtc, RV3028_REG_CONTROL2, &control2);
    if (rc != 0) {
        return rc;
    }

    control2 = (uint8_t)(control2 & (uint8_t)(~RV3028_CONTROL2_12_24_BIT)); // force 24h mode
    rc = rv3028_write_u8(rtc, RV3028_REG_CONTROL2, control2);
    if (rc != 0) {
        return rc;
    }

    uint8_t encoded[7] = {0};
    encoded[0] = bin_to_bcd(time_data->seconds);
    encoded[1] = bin_to_bcd(time_data->minutes);
    encoded[2] = (uint8_t)(bin_to_bcd(time_data->hours) & 0x3Fu);
    encoded[3] = (uint8_t)((time_data->day - 1u) & 0x07u); // map internal 1..7 to RV weekday 0..6
    encoded[4] = (uint8_t)(bin_to_bcd(time_data->date) & 0x3Fu);
    encoded[5] = (uint8_t)(bin_to_bcd(time_data->month) & 0x1Fu);
    encoded[6] = bin_to_bcd(time_data->year);

    return rv3028_write_bytes(rtc, RV3028_REG_SECONDS, encoded, sizeof(encoded));
}

int rv3028_set_daily_alarm_hour_minute(rv3028_t *rtc, uint8_t hour, uint8_t minute) {
    if (hour > 23u || minute > 59u) {
        return -1;
    }

    uint8_t control1 = 0u;
    int rc = rv3028_read_u8(rtc, RV3028_REG_CONTROL1, &control1);
    if (rc != 0) {
        return rc;
    }

    control1 =
        (uint8_t)(control1 & (uint8_t)(~RV3028_CONTROL1_WADA_BIT)); // weekday/date mode default
    rc = rv3028_write_u8(rtc, RV3028_REG_CONTROL1, control1);
    if (rc != 0) {
        return rc;
    }

    uint8_t alarm_data[3] = {0};
    alarm_data[0] = (uint8_t)(bin_to_bcd(minute) & 0x7Fu); // AE_M = 0 (enabled)
    alarm_data[1] = (uint8_t)(bin_to_bcd(hour) & 0x3Fu);   // AE_H = 0 (enabled), 24h mode
    alarm_data[2] = 0x80u;                                 // AE_WD = 1 (disabled) => daily alarm

    return rv3028_write_bytes(rtc, RV3028_REG_MINUTES_ALARM, alarm_data, sizeof(alarm_data));
}

int rv3028_enable_alarm_interrupt(rv3028_t *rtc, bool enable) {
    uint8_t control2 = 0u;
    int rc = rv3028_read_u8(rtc, RV3028_REG_CONTROL2, &control2);
    if (rc != 0) {
        return rc;
    }

    if (enable) {
        control2 = (uint8_t)(control2 | RV3028_CONTROL2_AIE_BIT);
    } else {
        control2 = (uint8_t)(control2 & (uint8_t)(~RV3028_CONTROL2_AIE_BIT));
    }

    return rv3028_write_u8(rtc, RV3028_REG_CONTROL2, control2);
}

int rv3028_read_alarm_flag(rv3028_t *rtc, bool *flag_set) {
    uint8_t status = 0u;
    int rc = rv3028_read_u8(rtc, RV3028_REG_STATUS, &status);
    if (rc != 0) {
        return rc;
    }
    *flag_set = (status & RV3028_STATUS_AF_BIT) != 0u;
    return 0;
}

int rv3028_clear_alarm_flag(rv3028_t *rtc) {
    uint8_t status = 0u;
    int rc = rv3028_read_u8(rtc, RV3028_REG_STATUS, &status);
    if (rc != 0) {
        return rc;
    }

    status = (uint8_t)(status & (uint8_t)(~RV3028_STATUS_AF_BIT));
    return rv3028_write_u8(rtc, RV3028_REG_STATUS, status);
}
