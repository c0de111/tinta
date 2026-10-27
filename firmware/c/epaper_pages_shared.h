#pragma once

#include <stdint.h>

// Page renderer function signature — all page renderers conform to this type.
typedef void (*page_renderer_t)(uint8_t *image_buffer);

// --- Shared pages (use-case independent) ---

// Setup mode, in the order a user meets them: how to join the sign's own network, then how
// to open its settings page (code, tag and address all lead to SETUP_PAGE_URL).
void render_page_setup_join(uint8_t *image_buffer);
// client_name: the device that joined, shown in the title; NULL or empty to leave it out.
// Firmware update over the network: title row as on the setup pages, the state, the version
// it concerns with its build date, what follows from it, and at the foot where it comes from.
// Empty or NULL fields are left out; text is broken between words.
typedef struct {
    const char *title;   // "Firmware update", "Update failed"
    const char *state;   // "Installing", "No Wi-Fi."
    const char *version; // "v0.17.0-595"
    const char *built;   // "built 8 Oct 2026"
    const char *tip;     // "Please wait, this takes a minute."
    const char *source;  // update server
} update_page_t;
void render_page_update(uint8_t *image_buffer, const update_page_t *page);
void render_page_setup_open(uint8_t *image_buffer, const char *client_name);
#define SETUP_PAGE_URL "http://192.168.4.1"
void render_page_welcome(uint8_t *image_buffer);

// Generic placeholder with centered title
void render_page_placeholder(uint8_t *image_buffer, const char *title);

// Fallback for unassigned page index
void render_page_fallback(int page, uint8_t *image_buffer);

// Error pages (server error, WiFi error, etc.)
void render_page_error(uint8_t *image_buffer, const char *title, const char *detail,
                       const char *tip);

// --- Shared page_def_t constants (defined in epaper_pages_shared.c, type in use_case.h) ---
struct page_def;
extern const struct page_def page_dnd;
extern const struct page_def page_decision_maker;
extern const struct page_def page_device_info;
extern const struct page_def page_nfc_text;
extern const struct page_def page_nfc_image;
extern const struct page_def page_wifi_setup;
extern const struct page_def page_placeholder;
