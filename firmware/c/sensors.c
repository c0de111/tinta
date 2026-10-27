#include "sensors.h"
#define LOG_MODULE LOG_MOD_SENSORS
#include "debug.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <unistd.h>

float read_battery_voltage(float conversion_factor) {
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);

    uint16_t adc_result = adc_read();
    float voltage = (float)adc_result * conversion_factor;

    dlog("Battery voltage: %.3f V\n", voltage);
    return voltage;
}

static float s_boot_battery_voltage = 0.0f;

float read_boot_battery_voltage(float conversion_factor) {
    s_boot_battery_voltage = read_battery_voltage(conversion_factor);
    return s_boot_battery_voltage;
}

float boot_battery_voltage(void) { return s_boot_battery_voltage; }

float read_coin_cell_voltage(float conversion_factor) {
    const uint gpio_mosfet = 14;

    gpio_init(gpio_mosfet);
    gpio_set_dir(gpio_mosfet, GPIO_OUT);

    gpio_put(gpio_mosfet, 1);
    sleep_ms(5);

    adc_init();
    adc_gpio_init(27);
    adc_select_input(1);

    uint16_t adc_result = adc_read();
    float voltage = (float)adc_result * conversion_factor;

    gpio_put(gpio_mosfet, 0);

    dlog("Coin cell voltage: %.3f V\n", voltage);
    return voltage;
}

float read_onchip_temperature_c(void) {
    const int samples = 8;
    uint32_t acc = 0;

    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    (void)adc_read();

    for (int i = 0; i < samples; i++) {
        acc += adc_read();
    }

    adc_set_temp_sensor_enabled(false);

    float avg_raw = (float)acc / (float)samples;
    float v_sense = avg_raw * 3.3f / 4096.0f;
    float temp_c = 27.0f - (v_sense - 0.706f) / 0.001721f;

    dlog("On-chip temperature: %.1f C\n", temp_c);
    return temp_c;
}

uint8_t read_strap_pins(void) {
    const uint strap_pins[4] = {0, 1, 2, 3};
    uint8_t strap_bits = 0;

    for (int i = 0; i < 4; i++) {
        uint pin = strap_pins[i];
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
        sleep_ms(1);
        int val = gpio_get(pin);
        strap_bits |= (val & 1) << i;
        gpio_disable_pulls(pin);
    }

    dlog("Strap read GP3..GP0 = %d%d%d%d (mask=0x%02X)\n", (strap_bits >> 3) & 1,
         (strap_bits >> 2) & 1, (strap_bits >> 1) & 1, strap_bits & 1, strap_bits);

    return strap_bits;
}

// Linker-provided symbols (defined in the application linker script):
extern char __bss_end__;  // NOLINT(bugprone-reserved-identifier)
extern char __StackLimit; // NOLINT(bugprone-reserved-identifier)

void get_memory_info(memory_info_t *out) {
    if (!out)
        return;

    uintptr_t heap_end = (uintptr_t)sbrk(0);
    uintptr_t heap_base = (uintptr_t)&__bss_end__;
    uintptr_t heap_limit = (uintptr_t)&__StackLimit;

    out->heap_used_bytes = (heap_end >= heap_base) ? (size_t)(heap_end - heap_base) : 0;
    out->heap_headroom_bytes = (heap_limit >= heap_end) ? (size_t)(heap_limit - heap_end) : 0;

    register uintptr_t sp_reg __asm__("sp");
    out->stack_margin_bytes = (sp_reg >= heap_end) ? (size_t)(sp_reg - heap_end) : 0;
    out->heap_base_addr = heap_base;
    out->heap_end_addr = heap_end;
    out->heap_limit_addr = heap_limit;
    out->sp_addr = sp_reg;
}
