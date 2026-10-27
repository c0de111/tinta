#include "power.h"
#include "config.h"
#include "debug.h"
#include "flash.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "led.h"
#include "pico/stdlib.h"
#include "rtc.h"
#include "sensors.h"
#include "st25_io.h"

static void release_gate(void) {
    sleep_ms(5);
    gpio_set_dir(GATE_PIN, GPIO_IN);
    watchdog_update();
}

static void halt(void) {
    while (true) {
        watchdog_update();
        sleep_ms(500);
    }
}

static const char *strap_board_name(uint8_t strap) {
    switch (strap) {
    case 0x00:
        return "legacy/unstrapped";
    case 0x01:
        return "L1 7.5\"";
    case 0x02:
        return "L1 4.2\"";
    case 0x03:
        return "L2 7.5\"";
    case 0x04:
        return "L2 4.2\"";
    case 0x05:
        return "L2 7.5\" minimal";
    default:
        return "unknown";
    }
}

void power_hold(void) {
    gpio_init(GATE_PIN);
    gpio_set_dir(GATE_PIN, GPIO_OUT);
    gpio_put(GATE_PIN, 1);

    uint8_t strap = read_strap_pins();
    debug_status("OK", "Power hold (%s, strap=0x%02X)\n", strap_board_name(strap), strap);

    ext_led_init(16);
    led_set_enabled(device_config_flash.data.led_enabled);
    ext_led_on();
}

void power_watchdog_start(void) {
    watchdog_enable(device_config_flash.data.watchdog_time, 0);
    debug_status("OK", "Watchdog enabled (%d ms)\n", device_config_flash.data.watchdog_time);
}

static void power_shutdown(int page) {
    rtc_set_alarm(page, 0);
    release_gate();
    halt();
}

void power_off(void) {
    release_gate();
    halt();
}

void power_shutdown_default(void) { power_shutdown(0); }

void power_shutdown_spurious(bool st25_present) {
    ext_led_on();
    sleep_ms(50);
    ext_led_off();
    if (st25_present)
        st25_power_off();
    release_gate();
    halt();
}
