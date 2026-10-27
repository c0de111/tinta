#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "lwip/tcp.h"
#include "pico/types.h"
#include <stdbool.h>

#define MAX_FORM_FIELDS 128
#define MAX_FIELD_LENGTH 128
#define USER_INTERACTION_TIMEOUT_MS                                                                \
    (5 * 60 * 1000) // reset active time for user interaction: 5 minutes

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Formularfelder
    char roomname[16];
    int type;
    int epapertype;
    int refresh_minutes_by_pushbutton[8];
    int number_of_seats;
    int wifi_reconnect_minutes;
    int watchdog_time;
    int number_wifi_attempts;
    int wifi_timeout;
    int max_wait_data_wifi;
    bool show_query_date;
    bool query_only_at_officehours;
    float conversion_factor;
    bool telemetry_enabled;
    bool autoset_rtc_enabled;
    bool led_enabled;
    int telemetry_timeout_ms;
    char telemetry_host[32];
    int telemetry_port;
    char telemetry_token[MAX_FIELD_LENGTH];
    char telemetry_label[32];
    char wifi_ssid[64];
    char wifi_password[64];
    int wifi_region;

    // Optional bestehende Felder
    char text[128][MAX_FIELD_LENGTH];
    bool aborted;
    int hour, minute, second;
    int day, date, month, year;
    int unit_ms; // generic millisecond value for forms like Morse
    char action[16];
    char align[8];
    char font_size[16];
    // Homematic (ephemeral toggles)
    bool homematic_service_messages;
} web_submission_t;

typedef enum {
    UPLOAD_NONE,
    UPLOAD_LOGO,
    UPLOAD_FIRMWARE,
    UPLOAD_FORM_WIFI,
    UPLOAD_FORM_USECASE,
    UPLOAD_FORM_DEVICE,
    UPLOAD_FORM_CLOCK,
    UPLOAD_FORM_SETTINGS_IMPORT
} upload_type_t;

typedef struct {
    upload_type_t type;
    size_t total_received;
    size_t expected_length;
    bool active;

    bool header_complete;
    size_t header_length;
    char header_buffer[4096];

    uint32_t flash_offset;
    char form_buffer[4096];

    // int upload_percent; // global or in upload_session
    int flash_estimated_duration;
} upload_session_t;

extern upload_session_t upload_session;

typedef void (*submission_handler_t)(const web_submission_t *data);

// Route table types — used by webserver.c and use_case_t extra_routes.
typedef enum { HTTP_GET, HTTP_POST } http_method_t;

typedef enum {
    ROUTE_PAGE,   // GET page: void (*)(tpcb)
    ROUTE_ACTION, // GET/POST action: void (*)(tpcb)
    ROUTE_FORM,   // POST form: dispatched generically via form_type metadata
    ROUTE_BINARY  // POST binary: handler receives pre-parsed content_length + body
} route_type_t;

typedef struct {
    const char *path;
    http_method_t method;
    route_type_t type;
    union {
        void (*page)(struct tcp_pcb *tpcb);
        void (*binary)(struct tcp_pcb *tpcb, int content_length, const char *body, size_t body_len);
        upload_type_t form_type;
    } handler;
} route_t;

// Full setup-mode event loop: AP mode, DHCP/DNS, OTA state machine.
// Caller must call wifi_ap_start() first.
// Returns when the setup timeout expires; caller handles shutdown.
void webserver_run(void);

// Passes the global shutdown time to the webserver (for remaining-runtime display)
void webserver_set_shutdown_time(absolute_time_t t);

// Utility functions for HTML page generation
void add_timeout_info(char *buf, size_t buf_size);
void send_response(struct tcp_pcb *tpcb, const char *body);
bool webserver_upload_in_progress(void);
bool webserver_firmware_upload_active(void);
void send_binary_response(struct tcp_pcb *tpcb, const char *content_type, const uint8_t *data,
                          size_t len);

#ifdef __cplusplus
}
#endif

#endif // WEBSERVER_H
