#include "morse.h"
#include "config.h"
#define LOG_MODULE LOG_MOD_MORSE
#include "debug.h"
#include "led.h"
#include "pico/time.h"
#include <ctype.h>
#include <string.h>

#ifndef LED_MORSE_UNIT_MS
#define LED_MORSE_UNIT_MS 150
#endif

static volatile bool g_enabled = true;
static int g_unit_ms = LED_MORSE_UNIT_MS;
static char g_seq[1024]; // Encoded sequence of '.', '-', ' ' (letter gap), '/' (word gap)
static int g_idx = 0;
static int g_phase =
    0; // 0=start element, 1=ON symbol, 2=OFF intra, 3=OFF letter extra, 4=OFF word extra
static absolute_time_t g_next;

static const char *code_for(char c) {
    switch (c) {
    case 'A':
        return ".-";
    case 'B':
        return "-...";
    case 'C':
        return "-.-.";
    case 'D':
        return "-..";
    case 'E':
        return ".";
    case 'F':
        return "..-.";
    case 'G':
        return "--.";
    case 'H':
        return "....";
    case 'I':
        return "..";
    case 'J':
        return ".---";
    case 'K':
        return "-.-";
    case 'L':
        return ".-..";
    case 'M':
        return "--";
    case 'N':
        return "-.";
    case 'O':
        return "---";
    case 'P':
        return ".--.";
    case 'Q':
        return "--.-";
    case 'R':
        return ".-.";
    case 'S':
        return "...";
    case 'T':
        return "-";
    case 'U':
        return "..-";
    case 'V':
        return "...-";
    case 'W':
        return ".--";
    case 'X':
        return "-..-";
    case 'Y':
        return "-.--";
    case 'Z':
        return "--..";
    case '0':
        return "-----";
    case '1':
        return ".----";
    case '2':
        return "..---";
    case '3':
        return "...--";
    case '4':
        return "....-";
    case '5':
        return ".....";
    case '6':
        return "-....";
    case '7':
        return "--...";
    case '8':
        return "---..";
    case '9':
        return "----.";
    default:
        return NULL;
    }
}

static void encode_text(const char *text, char *out, size_t out_len) {
    size_t pos = 0;
    bool first_symbol = true;
    for (const char *p = text; *p && pos + 2 < out_len; ++p) {
        char c = toupper((unsigned char)*p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            // Word separator: only if previous token wasn't a separator
            if (!first_symbol && pos < out_len - 1) {
                out[pos++] = '/';
            }
            first_symbol = true;
            continue;
        }
        const char *code = code_for(c);
        if (!code) {
            // treat unknown as space
            if (!first_symbol && pos < out_len - 1)
                out[pos++] = '/';
            first_symbol = true;
            continue;
        }
        if (!first_symbol && pos < out_len - 1) {
            out[pos++] = ' ';
        }
        for (const char *k = code; *k && pos < out_len - 1; ++k) {
            out[pos++] = *k; // '.' or '-'
        }
        first_symbol = false;
    }
    out[pos] = '\0';
}

void morse_init(void) {
    morse_set_message("TINTA");
    g_enabled = true;
    g_unit_ms = LED_MORSE_UNIT_MS;
    g_idx = 0;
    g_phase = 0;
    g_next = make_timeout_time_ms(0);
}

void morse_set_enabled(bool enabled) { g_enabled = enabled; }

void morse_set_unit_ms(int unit_ms) {
    if (unit_ms > 10 && unit_ms < 2000)
        g_unit_ms = unit_ms;
}

void morse_set_message(const char *text) {
    if (!text || !*text)
        text = "TINTA";
    encode_text(text, g_seq, sizeof(g_seq));
    g_idx = 0;
    g_phase = 0;
    g_next = make_timeout_time_ms(0);
}

void morse_tick(void) {
#if LED_MORSE_ENABLED
    if (!g_enabled)
        return;
    if (absolute_time_diff_us(get_absolute_time(), g_next) >= 0)
        return;

    int duration = 0;
    switch (g_phase) {
    case 0: {
        char c = g_seq[g_idx];
        if (c == '\0') {
            // End of sequence: long word gap then restart
#if LED_USE_EXT
            ext_led_off();
#endif
#if LED_USE_BOARD
            board_led_off();
#endif
            duration = 7 * g_unit_ms;
            g_phase = 4;
        } else if (c == ' ') {
            // Letter gap extra (after intra gap already done)
#if LED_USE_EXT
            ext_led_off();
#endif
#if LED_USE_BOARD
            board_led_off();
#endif
            duration = 2 * g_unit_ms;
            g_phase = 3;
            g_idx++;
        } else if (c == '/') {
            // Word gap extra (in addition to last intra gap)
#if LED_USE_EXT
            ext_led_off();
#endif
#if LED_USE_BOARD
            board_led_off();
#endif
            duration = 6 * g_unit_ms;
            g_phase = 3;
            g_idx++;
        } else { // '.' or '-'
#if LED_USE_EXT
            ext_led_on();
#endif
#if LED_USE_BOARD
            board_led_on();
#endif
            duration = (c == '.') ? (1 * g_unit_ms) : (3 * g_unit_ms);
            g_phase = 1;
        }
        break;
    }
    case 1: // finish symbol ON, do intra-element OFF (1 unit)
#if LED_USE_EXT
        ext_led_off();
#endif
#if LED_USE_BOARD
        board_led_off();
#endif
        duration = 1 * g_unit_ms;
        g_phase = 2;
        break;
    case 2: // end intra-element gap, advance to next sequence char
        g_idx++;
        g_phase = 0;
        duration = 0;
        break;
    case 3: // completed extra gap, resume
        g_phase = 0;
        duration = 0;
        break;
    case 4: // end of word gap at end-of-sequence; restart from beginning
        g_idx = 0;
        g_phase = 0;
        duration = 0;
        break;
    default:
        dlog("morse: invalid phase=%d, resetting\n", g_phase);
        g_idx = 0;
        g_phase = 0;
        duration = 0;
        break;
    }
    g_next = (duration > 0) ? make_timeout_time_ms(duration) : make_timeout_time_ms(0);
#endif
}
