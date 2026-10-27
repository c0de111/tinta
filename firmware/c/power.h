#ifndef POWER_H
#define POWER_H

#include <stdbool.h>

void power_hold(void);
void power_watchdog_start(void);
void power_off(void);
void power_shutdown_default(void);
void power_shutdown_spurious(bool st25_present);

#endif
