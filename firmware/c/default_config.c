#include "device_config.h"
#include "flash.h"
#include "seatsurfing/config.h"
#include "wifi_config.h"

#ifdef USE_CASE_HISTORIAN
#include "historian/config.h"
#elif defined(USE_CASE_HOMEMATIC)
#include "homematic/config.h"
#elif defined(USE_CASE_WEATHERMAP)
#include "weathermap/config.h"
#endif

#if defined(USE_CASE_HISTORIAN)
#define DEFAULT_ROOMNAME "tinta-historian"
#elif defined(USE_CASE_SEATSURFING)
#define DEFAULT_ROOMNAME "tinta-seatsurf"
#elif defined(USE_CASE_HOMEMATIC)
#define DEFAULT_ROOMNAME "tinta-homematic"
#elif defined(USE_CASE_WEATHERMAP)
#define DEFAULT_ROOMNAME "tinta-weathermap"
#else
#define DEFAULT_ROOMNAME "tinta"
#endif

__attribute__((section(".wifi_config"))) __attribute__((used))
const wifi_config_t default_wifi_config = {.ssid = WIFI_SSID_UNSET,
                                           .password = "default_password",
                                           .crc32 = 0,
                                           .region = WIFI_REGION_WORLDWIDE,
                                           .region_check =
                                               WIFI_REGION_WORLDWIDE ^ WIFI_REGION_CHECK};

#ifdef USE_CASE_SEATSURFING
// Seatsurfing Konfiguration
__attribute__((section(".seatsurfing_config"))) __attribute__((used))
const seatsurfing_config_t default_seatsurfing_config = {
    .data = {.host = "seatsurfing.io",
             .ip = {192, 168, 178, 85},
             .port = 8080,
             .username = "default_esign@seatsurfing.local",
             .password = "default_password",
             .location_id = "default_location_id",
             .seat_count = 1,
             .space_ids = {"default_space_id", "", ""}},
    .crc32 = 0};
#elif defined(USE_CASE_HISTORIAN)
// Historian Konfiguration
__attribute__((section(".historian_config"))) __attribute__((used))
const historian_config_t default_historian_config = {.data = {.ip = {192, 168, 178, 42},
                                                              .port = 81,
                                                              .path = "/query/jsonrpc.gy",
                                                              .datapoint_id = 75,
                                                              .hours_back = 24,
                                                              .display_name = "Temperature Sensor"},
                                                     .crc32 = 0};
#elif defined(USE_CASE_HOMEMATIC)
// Homematic Konfiguration
__attribute__((section(".homematic_config"))) __attribute__((used))
const homematic_config_t default_homematic_config = {
    .data = {.ip = {192, 168, 178, 20},
             .port = 2010,
             .add_interface_prefix = false,
             .auto_label = false,
             .count = 3,
             .items =
                 {
                     {.address = "002820C98F3853:1",
                      .key = "ACTUAL_TEMPERATURE",
                      .fallback_label = "Ch1"},
                     {.address = "002820C98F3853:2",
                      .key = "ACTUAL_TEMPERATURE",
                      .fallback_label = "Ch2"},
                     {.address = "002820C98F3853:3",
                      .key = "ACTUAL_TEMPERATURE",
                      .fallback_label = "Ch3"},
                 }},
    .crc32 = 0};
#elif defined(USE_CASE_WEATHERMAP)
// Weathermap configuration (center and span defaults)
__attribute__((section(".weathermap_config"))) __attribute__((used))
const weathermap_config_t default_weathermap_config = {
    .data =
        {
            .center_lat = 53.326,
            .center_lon = 8.532,
            .half_width_m = 6000.0,
            .flags = WEATHERMAP_CONFIG_VERSION,
            .reserved = {0},
        },
    .crc32 = 0,
};
#endif

// Device/UI configuration
__attribute__((section(".device_config"))) __attribute__((used))
const device_config_t default_device_config = {
    .data = {.roomname = DEFAULT_ROOMNAME,
             .type = ROOM_TYPE_OFFICE,
             .epapertype = EPAPER_NONE, // Default to no ePaper for safe first boot
             .refresh_minutes_by_pushbutton = {30, 30, 30, 30, 30, 30, 30, 30},
             .show_query_date = true,
             .query_only_at_officehours = false,
             .switch_off_battery_voltage = 2.7f,
             .description = "Office Space",
             .number_of_seats = 1,
             .number_of_people_meeting = 1,
             .has_projector = false,
             .has_conferencesystem = false,
             .conversion_factor = 0.00169f,
             .wifi_reconnect_minutes = 5,
             .watchdog_time = 8000,
             .wifi_timeout = 5000,
             .number_wifi_attempts = 6,
             .max_wait_data_wifi = 500,
             .pushbutton1_pin = 7,
             .pushbutton2_pin = 6,
             .pushbutton3_pin = 5,
             .num_pushbuttons = 3,
             .telemetry_enabled = false,
             .telemetry_timeout_ms = 2000,
             .telemetry_host = "192.168.178.85",
             .telemetry_port = 3004,
             .telemetry_token = "",
             .telemetry_label = "",
             .autoset_rtc_enabled = true,
             .led_enabled = true},
    .crc32 = 0};
