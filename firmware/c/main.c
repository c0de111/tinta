#include "boot_input.h"
#include "config.h"
#include "debug.h"
#include "device_caps.h"
#include "epaper.h"
#include "epaper_pages_shared.h"
#include "epaper_render.h"
#include "flash.h"
#include "http_client.h"
#include "i2c_bus.h"
#include "inki_monitor.h"
#include "net_update.h"
#include "nfc_ndef.h"
#include "pico/stdlib.h"
#include "power.h"
#include "rtc.h"
#include "sensors.h"
#include "use_case.h"
#include "webserver.h"
#include "wifi.h"
#include <stdint.h>

#if PICO_SDK_VERSION_MAJOR != 2 || PICO_SDK_VERSION_MINOR != 1 || PICO_SDK_VERSION_REVISION != 0
#warning                                                                                           \
    "This firmware was developed and tested with pico-sdk 2.1.0. Other versions may cause issues."
#endif

typedef struct {
    float battery_voltage;
    float coin_cell_voltage;
    i2c_probe_result_t probe;
} boot_t;

typedef struct {
    void *data;      // from use_case.run(), NULL if nothing was fetched
    int render_page; // the page, or a PAGE_* error sentinel in its place
} fetched_t;

static device_caps_t build_device_caps(const i2c_probe_result_t *probe) {
    uint8_t strap = read_strap_pins();
    return (device_caps_t){
        .epaper_width = epaper_get_width(),
        .epaper_height = epaper_get_height(),
        .epaper_4gray = epaper_is_4gray(),
        .st25_present = probe->st25_user_present,
        .bmp581_present = probe->bmp581_present,
        .ds3231_present = probe->ds3231_present,
        .rv3028_present = probe->rv3028_present,
        .coin_cell_present = (strap != 0x05),
    };
}

// The page that stays on the panel, or fallback if none is stored or the stored one is not
// (or no longer) a staying page of this use case.
static int resident_page_or(int fallback) {
    const int stored = load_resident_page();
    const page_def_t *def = use_case_page(stored);
    return def && def->stays ? stored : fallback;
}

// Why the sign has to be set up before it can show anything, or NULL. As delivered no home
// network has been entered yet: whatever woke the sign starts the setup. A sign that knows a
// network and cannot reach it is a different case and shows the WiFi error page.
static const char *setup_reason(void) {
    if (device_config_flash.data.epapertype == EPAPER_NONE)
        return "No ePaper configured";
    if (!wifi_has_network())
        return "No home network stored";
    return NULL;
}

// The setup ended and the sign still knows no network: leave the invitation on the panel and
// switch off until someone touches it again. No alarm is set, so nothing wakes it by itself.
static void show_welcome_and_shut_down(const boot_input_t *input) {
    uint8_t *image = epaper_init(false);
    if (image) {
        render_page_welcome(image);
        epaper_flush_and_sleep(image);
    }
    boot_input_close(input);
    debug_status("OK", "Not set up — showing the welcome page, shutting down\n");
    debug_flush();
    power_off();
}

static boot_t boot_system(void) {
    init_debug();
    set_debug_mode(DEBUG_BUFFERED); // BUFFERED captures all boot logs (also before USB is ready);
                                    // flush before power down
    debug_set_verbosity(LOG_STATUS);

    power_hold();
    boot_input_arm();
    debug_status("OK", "System initializing - tinta-%s\n", use_case.name);

    stdio_init_all();
    http_client_init();

#if INKI_DEBUG_USB_WAIT
    if (debug_wait_for_usb(2500))
        debug_status("OK", "USB connected\n");
    else
        debug_status("WARN", "USB timeout\n");
#endif

    flash_log_status();
    power_watchdog_start();

    boot_t boot = {0};
    boot.battery_voltage = read_boot_battery_voltage(device_config_flash.data.conversion_factor);
    boot.coin_cell_voltage = read_coin_cell_voltage(device_config_flash.data.conversion_factor);
    debug_status(boot.battery_voltage < 3.4f ? "WARN" : "OK", "Battery: %.2fV\n",
                 boot.battery_voltage);

    i2c_bus_init();
    i2c_probe_expected_devices(&boot.probe);
    rtc_init(&boot.probe);
    return boot;
}

static int choose_page(const boot_input_t *input) {
    const int page = resolve_page_index(input->source);
    // A scheduled wake and a phone held to the sign both mean the page that is on the panel,
    // not the one a wake without a button would otherwise pick.
    if (input->scheduled || input->source == INPUT_NFC_READ)
        return resident_page_or(page);
    return page;
}

// Runs the setup web interface. Afterwards (button, save+exit or timeout) a normal run follows
// with the page that stays; the log is collected again for the upload (setup only printed it).
static int run_setup(const boot_input_t *input) {
    debug_status("INFO", "%s: launching web interface (source=0x%X)\n", use_case.name,
                 input->source);
    webserver_run();
    if (!wifi_has_network())
        show_welcome_and_shut_down(input);
    set_debug_mode(DEBUG_BOTH);
    return resident_page_or(0);
}

static fetched_t fetch_page(int page, int input, const boot_t *boot) {
    fetched_t fetched = {NULL, page};
    if (!use_case_page_needs_wifi(page))
        return fetched;
    if (wifi_connect() != WIFI_SUCCESS) {
        fetched.render_page = PAGE_WIFI_ERROR;
        return fetched;
    }
    if (use_case.run) {
        // A clock that lost power (battery change) would make this run filter and date its
        // page by the year 2000. The fetch brings the server's time: set the clock from it and
        // load the page once more, now with the right date.
        const bool clock_lost = !rtc_time_plausible();
        run_result_t result = use_case.run(page, input);
        if (clock_lost) {
            rtc_sync_from_server();
            if (rtc_time_plausible()) {
                debug_status("INFO", "RTC: was not set, page loaded again\n");
                result = use_case.run(page, input);
            }
        }
        fetched.data = result.data;
        if (!fetched.data && result.error_page)
            fetched.render_page = result.error_page;
    }
    rtc_sync_from_server();
    inki_monitor_send_telemetry(boot->battery_voltage, boot->coin_cell_voltage,
                                fetched.data != NULL);
    wifi_log_rssi();
    wifi_deinit();
    return fetched;
}

// True if the page itself is on the panel now, not an error page in its place.
static bool draw_page(int page, const fetched_t *fetched, const boot_t *boot) {
    uint8_t *image = epaper_init(use_case.needs_4gray);
    if (image) {
        use_case.render(image, build_device_caps(&boot->probe), fetched->render_page,
                        fetched->data);
        epaper_flush_and_sleep(image);
    }

    if (use_case.free_data)
        use_case.free_data(fetched->data);

    if (!image) {
        debug_status("ERROR", "ePaper init or buffer allocation failed\n");
        debug_status("WARN", "Falling back to WiFi setup mode\n");
        webserver_run();
        return false;
    }
    return fetched->render_page == page;
}

static void remember_page(int page, const boot_input_t *input) {
    const page_def_t *def = use_case_page(page);
    if (!def)
        return;
    if (def->stays)
        save_resident_page(page);
    if (input->st25_present && use_case.page_url)
        nfc_ndef_offer_url(use_case.page_url(page), page);
}

int main(void) {
    const boot_t boot = boot_system();

    boot_input_t input = read_boot_input();
    if (input.reject) {
        debug_flush();
        power_shutdown_spurious(input.st25_present);
    }

#if INKI_NET_UPDATE
    // Buttons 3 + 4 together: fetch new firmware from the update server (ID 206).
    if (input.source == INPUT_BUTTON(6))
        net_update_run(input.st25_present);
#endif

    int page = choose_page(&input);
    const char *reason = setup_reason();
    if (reason)
        debug_status("INFO", "%s — entering WiFi setup mode\n", reason);
    if (reason || page == PAGE_ACTION_SETUP)
        page = run_setup(&input);

    const bool wifi_needed = use_case_page_needs_wifi(page);
    debug_status("OK", "Page: %d (wifi=%s)\n", page, wifi_needed ? "yes" : "no");

    const fetched_t fetched = fetch_page(page, input.source, &boot);
    // A server that says when to look again (Seatsurfing device mode) keeps its own schedule,
    // office hours included.
    const uint32_t next_wake_s = use_case.next_wake_s ? use_case.next_wake_s(fetched.data) : 0;
    if (draw_page(page, &fetched, &boot))
        remember_page(page, &input);
    boot_input_close(&input);

    if (next_wake_s)
        rtc_set_alarm_in(next_wake_s);
    else
        rtc_set_alarm(page, wifi_needed ? wifi_backoff_minutes() : 0);
    debug_status("OK", "System shutting down\n");
    debug_flush();
    power_off();
}
