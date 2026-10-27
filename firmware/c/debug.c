#include "debug.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char debug_buffer[DEBUG_BUFFER_SIZE];
static size_t debug_buffer_index = 0;
static DebugMode current_debug_mode = DEBUG_NONE;
static log_level_t current_verbosity = LOG_TRACE; // show everything by default
static uint32_t current_module_mask = LOG_MOD_ALL;

void init_debug(void) {
    debug_buffer_index = 0;
    current_debug_mode = DEBUG_NONE;
    current_verbosity = LOG_TRACE;
    current_module_mask = LOG_MOD_ALL;
}

void set_debug_mode(DebugMode mode) { current_debug_mode = mode; }

void debug_set_verbosity(log_level_t max_level) { current_verbosity = max_level; }

// cppcheck-suppress unusedFunction
void debug_set_module_mask(uint32_t mask) { current_module_mask = mask; }

// --- Internal helpers ---

static void format_timestamp(char *buf, size_t len) {
    uint64_t us = time_us_64();
    if (us < 10000) {
        snprintf(buf, len, "\033[1m[%llu us]\033[0m ", (unsigned long long)us);
    } else {
        snprintf(buf, len, "\033[1m[%llu ms]\033[0m ", (unsigned long long)(us / 1000));
    }
}

// Append string to debug buffer, advance index. Returns bytes written.
static size_t buf_append(const char *str) {
    size_t rem = DEBUG_BUFFER_SIZE - debug_buffer_index;
    if (rem <= 1)
        return 0;
    int w = snprintf(&debug_buffer[debug_buffer_index], rem, "%s", str);
    if (w > 0) {
        size_t written = ((size_t)w < rem) ? (size_t)w : rem - 1;
        debug_buffer_index += written;
        return written;
    }
    return 0;
}

// Append formatted string to debug buffer via va_list.
static void buf_vappendf(const char *format, va_list args) {
    size_t rem = DEBUG_BUFFER_SIZE - debug_buffer_index;
    if (rem <= 1)
        return;
    int w = vsnprintf(&debug_buffer[debug_buffer_index], rem, format, args);
    if (w > 0) {
        debug_buffer_index += ((size_t)w < rem) ? (size_t)w : rem - 1;
    }
}

// Core output: timestamp + optional prefix + formatted message + reset
static void emit(const char *prefix, const char *suffix, const char *format, va_list args) {
    char ts[32];
    format_timestamp(ts, sizeof(ts));

    if (current_debug_mode == DEBUG_REALTIME || current_debug_mode == DEBUG_BOTH) {
        va_list args_rt;
        va_copy(args_rt, args);
        printf("%s%s", ts, prefix);
        vprintf(format, args_rt);
        if (suffix[0])
            printf("%s", suffix);
        va_end(args_rt);
    }

    if (current_debug_mode == DEBUG_BUFFERED || current_debug_mode == DEBUG_BOTH) {
        buf_append(ts);
        buf_append(prefix);
        va_list args_buf;
        va_copy(args_buf, args);
        buf_vappendf(format, args_buf);
        va_end(args_buf);
        if (suffix[0])
            buf_append(suffix);
    }
}

// --- Level-aware API ---

// Map status tag to color
static const char *status_tag_color(const char *tag) {
    if (strcmp(tag, "OK") == 0)
        return COLOR_BOLD_GREEN;
    if (strcmp(tag, "ERROR") == 0)
        return COLOR_BOLD_RED;
    if (strcmp(tag, "WARN") == 0)
        return COLOR_BOLD_YELLOW;
    if (strcmp(tag, "INFO") == 0)
        return COLOR_BOLD_CYAN;
    return COLOR_BOLD_WHITE;
}

void debug_status(const char *tag, const char *format, ...) {
    if (current_verbosity < LOG_STATUS)
        return;

    const char *color = status_tag_color(tag);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s[%s] ", color, tag);

    va_list args;
    va_start(args, format);
    emit(prefix, COLOR_RESET, format, args);
    va_end(args);
}

void debug_info(const char *format, ...) {
    if (current_verbosity < LOG_INFO)
        return;

    va_list args;
    va_start(args, format);
    emit(COLOR_GREEN, COLOR_RESET, format, args);
    va_end(args);
}

void debug_module_log(uint32_t module, log_level_t level, const char *format, ...) {
    if (current_verbosity < level)
        return;
    if (level >= LOG_DEBUG && !(module & current_module_mask))
        return;
    /* Legacy calls use LOG_MOD_ALL — suppress them when a specific mask is active */
    if (level >= LOG_DEBUG && module == LOG_MOD_ALL && current_module_mask != LOG_MOD_ALL)
        return;

    va_list args;
    va_start(args, format);
    emit("", "", format, args);
    va_end(args);
}

// --- Buffer flush ---

const char *debug_buffer_peek(size_t *len) {
    *len = debug_buffer_index;
    return debug_buffer;
}

void debug_flush(void) {
    if (current_debug_mode == DEBUG_BUFFERED || current_debug_mode == DEBUG_BOTH) {
        printf("%s", debug_buffer);
        debug_buffer_index = 0;
        fflush(stdout);
        stdio_flush();
        sleep_ms(250);
    }
}

#if INKI_DEBUG_USB_WAIT
bool debug_wait_for_usb(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!stdio_usb_connected() && waited < timeout_ms) {
        sleep_ms(10);
        waited += 10;
    }
    return stdio_usb_connected();
}
#endif
