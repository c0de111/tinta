#ifndef SENSORS_H
#define SENSORS_H

#include <stddef.h>
#include <stdint.h>

float read_battery_voltage(float conversion_factor);

// Reads the battery and keeps the value as this wake's reference sample. Call before WiFi
// comes up: the radio pulls the cells down, later reads are lower.
float read_boot_battery_voltage(float conversion_factor);
float boot_battery_voltage(void);
float read_coin_cell_voltage(float conversion_factor);
float read_onchip_temperature_c(void);
uint8_t read_strap_pins(void);

typedef struct {
    size_t heap_used_bytes;     // exact at call time
    size_t heap_headroom_bytes; // exact at call time
    size_t stack_margin_bytes;  // approx (core0 snapshot)
    uintptr_t heap_base_addr;   // &__bss_end__
    uintptr_t heap_end_addr;    // sbrk(0)
    uintptr_t heap_limit_addr;  // &__StackLimit
    uintptr_t sp_addr;          // current core0 SP snapshot
} memory_info_t;

void get_memory_info(memory_info_t *out);

#endif
