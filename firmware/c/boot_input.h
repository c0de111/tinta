#ifndef BOOT_INPUT_H
#define BOOT_INPUT_H

#include "st25_io.h"
#include <stdbool.h>

// Boot input: unified result from all boot-time input sources
// (pushbuttons, NFC, RTC alarm). The source field uses the INPUT_BUTTON()
// and INPUT_NFC() namespaces defined in use_case.h.
typedef struct {
    int source;        // INPUT_BUTTON(n), INPUT_NFC(op) or INPUT_NFC_READ
    bool reject;       // spurious NFC wake — caller should shut down
    bool scheduled;    // woken by the alarm alone: no button held, no NFC request
    bool st25_present; // ST25 chip detected (for cleanup at shutdown)
} boot_input_t;

// Switch on the button pull-ups. Call as early as possible after power hold, so the pins
// have settled by the time read_boot_input() samples them.
void boot_input_arm(void);

// True while any button is held, read once (the pins must be armed).
bool any_button_down(void);

// Read all boot-time input sources and return unified result.
// Call after I2C bus is initialized.
boot_input_t read_boot_input(void);

// Clears any request written to the tag while the sign was awake and switches the tag off.
void boot_input_close(const boot_input_t *input);

// Module state — read by telemetry (http_client.c) and NFC renderers
extern const char *wake_source;
extern char nfc_text_buf[];
extern uint8_t nfc_image_buf[];
extern char nfc_booking_email[]; // populated on ST25_OPCODE_BOOK_SEAT (0x40)

#endif // BOOT_INPUT_H
