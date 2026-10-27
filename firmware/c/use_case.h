#pragma once

#include "device_caps.h"
#include "epaper_pages_shared.h"
#include "webserver.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- Page definition ---

typedef struct page_def {
    const char *name;
    page_renderer_t render;
    bool needs_wifi;
    // The page stays: once shown, scheduled wakes redraw it until another staying page is
    // chosen. Pages without it are shown once and give way to the staying one again.
    bool stays;
} page_def_t;

// --- Input mapping ---

// Source namespaces: buttons 0-7, NFC opcodes 0x100+, future web commands 0x200+
#define INPUT_BUTTON(n) (n)
#define INPUT_NFC(op) (0x100 | (op))
// A phone read the tag's web address and wrote nothing. main.c turns this into the page that
// stays on the panel; the input_map entry is the fallback when none is stored.
#define INPUT_NFC_READ 0x300

// Sentinel / special values
#define PAGE_ACTION_SETUP (-1)
#define INPUT_MAP_END (-2)
#define PAGE_WIFI_ERROR (-3)
#define PAGE_DNS_ERROR (-4)
#define PAGE_AUTH_ERROR (-5)
#define PAGE_HTTP_ERROR (-6)
#define PAGE_PARSE_ERROR (-7)
#define PAGE_TRACKER_ERROR (-8) // series unreachable — a different server than the rest

// run_error_t: use-case-internal error classification (private to each client.c).
// Defined here as a shared enum so all use cases speak the same vocabulary.
typedef enum {
    RUN_OK = 0,
    RUN_ERROR_DNS,
    RUN_ERROR_CONNECTION,
    RUN_ERROR_AUTH,
    RUN_ERROR_PARSE,
} run_error_t;

// run_result_t: explicit output of run(). data != NULL on success; error_page is a PAGE_*
// sentinel (negative) on failure and should be used as render_page by main.c.
typedef struct {
    void *data;
    int error_page;
} run_result_t;

typedef struct {
    int source;
    int page_index; // index into pages[], or PAGE_ACTION_SETUP
} input_map_entry_t;

// --- Use case contract ---

typedef struct {
    const char *name;

    // Declarative page catalog and input mapping
    const page_def_t **pages;           // NULL-terminated catalog
    const input_map_entry_t *input_map; // sentinel-terminated (INPUT_MAP_END)
    int default_page;

    // Lifecycle: use case owns data fetch and render.
    // run() returns {data, error_page}: data != NULL on success, error_page set on failure.
    // page: the page to fetch for. input: the wake source behind it (INPUT_BUTTON/INPUT_NFC),
    // for pages where two sources mean different things.
    run_result_t (*run)(int page, int input);
    void (*render)(uint8_t *image, device_caps_t caps, int page, const void *data);
    void (*free_data)(void *data);

    // Optional: the web address that belongs to a page, or NULL for none. main.c keeps the
    // NFC tag offering the address of the page on the panel, so a phone held to the sign
    // opens it without an app. Must not fetch or write anything.
    const char *(*page_url)(int page);

    // Optional: seconds until the next wake, given run()'s data (NULL if run() did not run).
    // 0 or a NULL pointer leaves the device's own refresh interval in charge.
    uint32_t (*next_wake_s)(const void *data);

    // Setup webserver: use-case config page and form handler.
    // render_config_page: write HTML form into buf; msg is flash feedback ("" on first load).
    // handle_config_form: parse + save form data; return flash message string (literal).
    void (*render_config_page)(char *buf, size_t sz, const char *msg);
    const char *(*handle_config_form)(const char *body, size_t len);

    // Display capabilities declared by the use case.
    bool needs_4gray;       // true = initialise ePaper in 4Gray mode (e.g. weathermap)
    bool show_display_name; // true = show "Display title" (roomname) field in device settings

    // Extra HTTP routes registered by this use case (NULL-safe; webserver checks
    // extra_route_count).
    const route_t *extra_routes;
    size_t extra_route_count;
} use_case_t;

// Linker-resolved: exactly one use case is compiled in per firmware build.
extern const use_case_t use_case;

// --- Shared dispatch helpers ---

// Resolve an input source (INPUT_BUTTON/INPUT_NFC) to a page index via use_case.input_map.
static inline int resolve_page_index(int source) {
    for (const input_map_entry_t *m = use_case.input_map; m->source != INPUT_MAP_END; m++) {
        if (m->source == source) {
            return m->page_index;
        }
    }
    return use_case.default_page;
}

// The catalog entry of a page index, or NULL for a sentinel or an index past the catalog.
static inline const page_def_t *use_case_page(int page) {
    for (int i = 0; page >= 0 && i <= page; i++) {
        if (!use_case.pages[i])
            return NULL;
    }
    return page >= 0 ? use_case.pages[page] : NULL;
}

static inline bool use_case_page_needs_wifi(int page) {
    const page_def_t *def = use_case_page(page);
    return def && def->needs_wifi;
}
