#include "boot_input.h"
#define LOG_MODULE LOG_MOD_BOOT
#include "debug.h"
#include "flash.h"
#include "hardware/gpio.h"
#include "nfc_ndef.h"
#include "pico/stdlib.h"
#include "rtc.h"
#include "st25_io.h"
#include "use_case.h"
#include <stdio.h>
#include <string.h>

// Module state — read by telemetry and NFC renderers via extern
const char *wake_source = "unknown";
char nfc_text_buf[ST25_TEXT_MAX_LEN + 1] = "";
uint8_t nfc_image_buf[ST25_IMAGE_BYTES] = {0};
char nfc_booking_email[64] = "";
static char wake_source_buf[32];

// The 100 nF on each button pin starts at 0 V and charges through the internal pull-up
// (tau 5-8 ms), so an idle pin reads LOW until it has settled.
#define BUTTON_SETTLE_MS 12
#define BUTTON_WINDOW_MS 20
#define BUTTON_MIN_LOW_SAMPLES 3
#define BUTTON_LATE_WINDOW_MS 300

static absolute_time_t buttons_armed_at;
static bool buttons_armed;

static void button_pins(uint8_t pins[3]) {
    const uint8_t configured[3] = {device_config_flash.data.pushbutton1_pin,
                                   device_config_flash.data.pushbutton2_pin,
                                   device_config_flash.data.pushbutton3_pin};
    int count = device_config_flash.data.num_pushbuttons;
    if (count > 3)
        count = 3;
    for (int i = 0; i < 3; i++)
        pins[i] = i < count ? configured[i] : 0xFF;
}

void boot_input_arm(void) {
    uint8_t pins[3];
    button_pins(pins);
    for (int i = 0; i < 3; i++) {
        if (pins[i] == 0xFF)
            continue;
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
    buttons_armed_at = get_absolute_time();
    buttons_armed = true;
}

// All buttons sampled together once per millisecond; a button counts as pressed when it
// reads LOW often enough, so a contact that opens briefly or closes late is still seen.
static int sample_pushbuttons(int window_ms) {
    uint8_t pins[3];
    int low_samples[3] = {0};
    button_pins(pins);

    for (int ms = 0; ms < window_ms; ms++) {
        for (int i = 0; i < 3; i++) {
            if (pins[i] != 0xFF && !gpio_get(pins[i]))
                low_samples[i]++;
        }
        sleep_ms(1);
    }

    int buttons = 0;
    for (int i = 0; i < 3; i++) {
        if (low_samples[i] >= BUTTON_MIN_LOW_SAMPLES)
            buttons |= 1 << i;
    }
    return buttons;
}

bool any_button_down(void) {
    if (!buttons_armed)
        boot_input_arm();
    uint8_t pins[3];
    button_pins(pins);
    for (int i = 0; i < 3; i++)
        if (pins[i] != 0xFF && !gpio_get(pins[i]))
            return true;
    return false;
}

static int read_pushbuttons(void) {
    if (!buttons_armed)
        boot_input_arm();
    sleep_until(delayed_by_ms(buttons_armed_at, BUTTON_SETTLE_MS));
    return sample_pushbuttons(BUTTON_WINDOW_MS);
}

// A button woke the unit but none was seen: keep looking while the user may still be holding.
static int read_pushbuttons_late(int *waited_ms) {
    for (*waited_ms = 0; *waited_ms < BUTTON_LATE_WINDOW_MS; *waited_ms += BUTTON_WINDOW_MS) {
        int buttons = sample_pushbuttons(BUTTON_WINDOW_MS);
        if (buttons) {
            *waited_ms += BUTTON_WINDOW_MS;
            return buttons;
        }
    }
    return 0;
}

// --- NFC reading + protocol handshake ---

static st25_boot_result_t read_nfc(void) {
    st25_boot_result_t st25 = st25_boot_check();

    if (st25.present && st25.request_valid) {
        // Delay lets phone finish RF readback before I2C writes block RF
        sleep_ms(200);
        st25_save_processed_nonce(st25.req.nonce);
        if (!st25_clear_request()) {
            dlog("[ST25] early clear failed!\n");
        }

        if (st25.req.opcode == ST25_OPCODE_TEXT) {
            st25_read_text(nfc_text_buf, sizeof(nfc_text_buf));
        } else if (st25.req.opcode == ST25_OPCODE_DRAW_IMAGE) {
            st25_read_image(nfc_image_buf, sizeof(nfc_image_buf));
        } else if (st25.req.opcode == ST25_OPCODE_BOOK_SEAT) {
            st25_read_text(nfc_booking_email, sizeof(nfc_booking_email));
        } else if (st25.req.opcode == ST25_OPCODE_TRACK_VALUE) {
            st25_read_text(nfc_text_buf, sizeof(nfc_text_buf));
        }
    }

    return st25;
}

// A phone that is still talking to the tag keeps its I2C side busy, so the tag can look absent
// right after a tap. Keep asking for a while before believing it.
#define NFC_BUSY_WINDOW_MS 2000
#define NFC_BUSY_RETRY_MS 100

static st25_boot_result_t read_nfc_when_free(int *waited_ms) {
    st25_boot_result_t st25 = {0};
    for (*waited_ms = 0; *waited_ms < NFC_BUSY_WINDOW_MS && !st25.present;
         *waited_ms += NFC_BUSY_RETRY_MS) {
        sleep_ms(NFC_BUSY_RETRY_MS);
        st25 = read_nfc();
    }
    return st25;
}

// --- Unified boot input (classification) ---

boot_input_t read_boot_input(void) {
    boot_input_t result = {0};

    int buttons = read_pushbuttons();
    st25_boot_result_t st25 = read_nfc();
    bool alarm_flag = rtc_alarm_flag_is_set();

    // Nothing explains this wake yet: no button, no alarm, no tag. Look a little longer for a
    // button, then for a tag that a phone kept busy.
    int late_ms = 0;
    int tag_busy_ms = 0;
    if (!st25.present && !alarm_flag && buttons == 0) {
        buttons = read_pushbuttons_late(&late_ms);
        if (buttons == 0)
            st25 = read_nfc_when_free(&tag_busy_ms);
    }
    result.st25_present = st25.present;

    if (st25.present && tag_busy_ms) {
        debug_status("OK", "ST25: present (it_sts=0x%02X, busy for %d ms)\n", st25.it_sts,
                     tag_busy_ms);
    } else if (st25.present) {
        debug_status("OK", "ST25: present (it_sts=0x%02X)\n", st25.it_sts);
    } else {
        debug_status("INFO", "ST25: not found\n");
    }

    // Wake source classification
    const bool rf_seen = st25.it_sts != 0 || tag_busy_ms;
    const bool rf_without_request = st25.present && rf_seen && buttons == 0;

    if (st25.present && st25.request_valid) {
        result.source = INPUT_NFC(st25.req.opcode);
        snprintf(wake_source_buf, sizeof(wake_source_buf), "nfc:0x%02X", st25.req.opcode);
        debug_status("OK", "Wake: NFC (opcode=0x%02X)\n", st25.req.opcode);
    } else if (alarm_flag) {
        result.scheduled = buttons == 0;
        result.source = INPUT_BUTTON(buttons);
        snprintf(wake_source_buf, sizeof(wake_source_buf), "rtc");
        debug_status("OK", "Wake: RTC ALARM\n");
    } else if (rf_without_request && nfc_ndef_present()) {
        // RF activity without a request, on a tag that offers a web address: a phone read it.
        result.source = INPUT_NFC_READ;
        snprintf(wake_source_buf, sizeof(wake_source_buf), "nfc:read");
        debug_status("OK", "Wake: NFC read (it_sts=0x%02X)\n", st25.it_sts);
    } else if (rf_without_request) {
        result.reject = true;
        snprintf(wake_source_buf, sizeof(wake_source_buf), "spurious");
        debug_status("WARN", "Rejecting boot: spurious NFC (it_sts=0x%02X)\n", st25.it_sts);
    } else {
        if (buttons == 0 && late_ms == 0)
            buttons = read_pushbuttons_late(&late_ms);
        result.source = INPUT_BUTTON(buttons);
        if (buttons && late_ms) {
            snprintf(wake_source_buf, sizeof(wake_source_buf), "button:%d:late", buttons);
            debug_status("WARN", "Wake: BUTTON (buttons=%d, seen late after %d ms)\n", buttons,
                         late_ms);
        } else {
            snprintf(wake_source_buf, sizeof(wake_source_buf), "button:%d", buttons);
            debug_status("OK", "Wake: BUTTON (buttons=%d)\n", buttons);
        }
    }

    wake_source = wake_source_buf;
    return result;
}

void boot_input_close(const boot_input_t *input) {
    if (!input->st25_present)
        return;
    st25_clear_request();
    st25_power_off();
}
