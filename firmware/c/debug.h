#ifndef DEBUG_H
#define DEBUG_H

#include <stddef.h>
#include <stdint.h>

#define DEBUG_BUFFER_SIZE 8192

// Output destination (where logs go)
typedef enum {
    DEBUG_NONE,
    DEBUG_REALTIME,
    DEBUG_BUFFERED,
    DEBUG_BOTH,
} DebugMode;

// Verbosity levels (which logs to show), lowest = most important
typedef enum {
    LOG_STATUS, // boot health checks — always shown ([OK], [ERROR], [WARN], [INFO])
    LOG_INFO,   // orchestrator milestones ("ADC read", "render_page", ...)
    LOG_DEBUG,  // module key events ("HTTP connected", "DS3231 detected", ...)
    LOG_TRACE,  // module internals (per-packet, per-byte, register dumps)
} log_level_t;

// Per-module bitmask for DEBUG/TRACE filtering
typedef enum {
    LOG_MOD_MAIN = (1 << 0),
    LOG_MOD_HTTP = (1 << 1),
    LOG_MOD_WIFI = (1 << 2),
    LOG_MOD_FLASH = (1 << 3),
    LOG_MOD_RTC = (1 << 4),
    LOG_MOD_EPAPER = (1 << 5),
    LOG_MOD_ST25 = (1 << 6),
    LOG_MOD_WEBSERVER = (1 << 7),
    LOG_MOD_BOOT = (1 << 8),
    LOG_MOD_POWER = (1 << 9),
    LOG_MOD_SENSORS = (1 << 10),
    LOG_MOD_TLS = (1 << 11),
    LOG_MOD_SEATSURFING = (1 << 12),
    LOG_MOD_HISTORIAN = (1 << 13),
    LOG_MOD_HOMEMATIC = (1 << 14),
    LOG_MOD_WEATHERMAP = (1 << 15),
    LOG_MOD_MONITOR = (1 << 16),
    LOG_MOD_PNG = (1 << 17),
    LOG_MOD_MORSE = (1 << 18),
    LOG_MOD_NEXTCLOUD = (1 << 20), // 19 is LOG_MOD_ROOMVOX on feature/roomvox
    LOG_MOD_ALL = 0x7FFFFFFF,
} log_module_t;

// --- Core API ---
//
// Two independent axes: destination (DebugMode) and verbosity (log_level_t).
//
// Verbosity controls which messages are shown:
//   debug_set_verbosity(LOG_TRACE)  — show everything (default)
//   debug_set_verbosity(LOG_INFO)   — STATUS + INFO only (quiet production)
//   debug_set_verbosity(LOG_STATUS) — boot health checks only
//
// Module mask controls which modules emit DEBUG/TRACE output:
//   debug_set_module_mask(LOG_MOD_ALL)                      — all (default)
//   debug_set_module_mask(LOG_MOD_HTTP | LOG_MOD_TLS)       — targeted debugging
//   debug_set_module_mask(LOG_MOD_ALL & ~LOG_MOD_WEBSERVER) — exclude one module
//
// STATUS and INFO always pass regardless of module mask.

void init_debug(void);
void set_debug_mode(DebugMode mode);
void debug_set_verbosity(log_level_t max_level);
void debug_set_module_mask(uint32_t mask);

// Level-aware logging
void debug_status(const char *tag, const char *format, ...); // LOG_STATUS
void debug_info(const char *format, ...);                    // LOG_INFO
void debug_module_log(uint32_t module, log_level_t level, const char *format,
                      ...); // LOG_DEBUG/TRACE

// Flush buffered logs to USB serial
void debug_flush(void);
// What has been logged since the last flush, in DEBUG_BUFFERED or DEBUG_BOTH mode.
// Not terminated at *len; valid until the next log call.
const char *debug_buffer_peek(size_t *len);

// Poll until USB is connected or timeout_ms elapses; returns true if connected.
#if INKI_DEBUG_USB_WAIT
bool debug_wait_for_usb(uint32_t timeout_ms);
#endif

// --- Per-module convenience macros ---
// Each .c file defines LOG_MODULE before using these:
//   #define LOG_MODULE LOG_MOD_HTTP
#ifdef LOG_MODULE
#define dlog(fmt, ...) debug_module_log(LOG_MODULE, LOG_DEBUG, fmt, ##__VA_ARGS__)
#define dtrace(fmt, ...) debug_module_log(LOG_MODULE, LOG_TRACE, fmt, ##__VA_ARGS__)
#else
#define dlog(fmt, ...) debug_module_log(LOG_MOD_ALL, LOG_DEBUG, fmt, ##__VA_ARGS__)
#define dtrace(fmt, ...) debug_module_log(LOG_MOD_ALL, LOG_TRACE, fmt, ##__VA_ARGS__)
#endif

// ANSI color codes
#define COLOR_RESET "\033[0m"

#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"

#define COLOR_BOLD_RED "\033[1;31m"
#define COLOR_BOLD_GREEN "\033[1;32m"
#define COLOR_BOLD_YELLOW "\033[1;33m"
#define COLOR_BOLD_BLUE "\033[1;34m"
#define COLOR_BOLD_MAGENTA "\033[1;35m"
#define COLOR_BOLD_CYAN "\033[1;36m"
#define COLOR_BOLD_WHITE "\033[1;37m"

#endif // DEBUG_H
