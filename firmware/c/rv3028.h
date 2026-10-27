#ifndef RV3028_H
#define RV3028_H

#include "hardware/i2c.h"
#include <stdbool.h>
#include <stdint.h>

#define RV3028_ADDR_DEFAULT 0x52u

typedef struct {
    i2c_inst_t *i2c;
    uint8_t addr;
    // As found at init, before anything is written: for the start-up log.
    uint8_t boot_status;        // 0x0E: EEBusy, CLKF, BSF, UF, TF, AF, EVF, PORF
    uint8_t boot_backup;        // 0x37 in RAM: TCE, FEDE, BSM, TCR
    int16_t boot_backup_eeprom; // the same register in the EEPROM, -1 if it could not be read
    // Switch-over to VBACKUP (BSM = LSM): what init achieved.
    bool switchover_ram;          // read back from RAM
    bool switchover_eeprom_fixed; // the EEPROM copy lacked it and was written this start
} rv3028_t;

typedef struct {
    uint8_t seconds; // 0..59
    uint8_t minutes; // 0..59
    uint8_t hours;   // 0..23
    uint8_t day;     // internal convention: 1..7
    uint8_t date;    // 1..31
    uint8_t month;   // 1..12
    uint8_t year;    // 0..99
} rv3028_time_t;

int rv3028_init(rv3028_t *rtc, i2c_inst_t *i2c, uint8_t addr);
int rv3028_read_time(rv3028_t *rtc, rv3028_time_t *time_data);
int rv3028_set_time(rv3028_t *rtc, const rv3028_time_t *time_data);
int rv3028_set_daily_alarm_hour_minute(rv3028_t *rtc, uint8_t hour, uint8_t minute);
int rv3028_enable_alarm_interrupt(rv3028_t *rtc, bool enable);
int rv3028_read_alarm_flag(rv3028_t *rtc, bool *flag_set);
int rv3028_clear_alarm_flag(rv3028_t *rtc);
// Clears PORF and BSF, so the next start tells whether power was lost since this one.
int rv3028_clear_power_flags(rv3028_t *rtc);

#endif
