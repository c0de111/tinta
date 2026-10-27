/**
 * @file    config.h
 * @brief   General configuration constants and macros for the eSign project.
 *
 * This file contains globally used constants, macros, and definitions
 * for configuring system-wide parameters such as pin assignments,
 * hardware capabilities, and project-specific settings.
 */

#ifndef CONFIG_H
#define CONFIG_H

// Include rooms.h, which defines RoomConfig
// #include "rooms.h"
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// USE CASE SELECTION - Build-Time Configuration
// =============================================================================

/**
 * @brief Use case selection for inki firmware.
 *
 * Uncomment exactly ONE of the following use cases to build firmware for:
 * - USE_CASE_SEATSURFING: Room booking and availability display (default)
 * - USE_CASE_HISTORIAN: Time-series data visualization and monitoring
 *
 * The selected use case determines:
 * - Which communication protocols are used
 * - What configuration pages are shown in setup mode
 * - How data is parsed and displayed on the ePaper
 * - Which configuration structures are included
 */

// Use case selection - can be overridden by build script (./build.sh --use-case ...)
// Default fallback if no build-time define is provided
#if !defined(USE_CASE_SEATSURFING) && !defined(USE_CASE_HISTORIAN) &&                              \
    !defined(USE_CASE_HOMEMATIC) && !defined(USE_CASE_WEATHERMAP) &&                               \
    !defined(USE_CASE_NEW_USECASE) && !defined(USE_CASE_NEXTCLOUD)
#define USE_CASE_SEATSURFING // Default fallback
#endif

// Stable numeric IDs stored in firmware header metadata
#define USE_CASE_ID_SEATSURFING 1
#define USE_CASE_ID_HISTORIAN 2
#define USE_CASE_ID_HOMEMATIC 3
#define USE_CASE_ID_WEATHERMAP 4
#define USE_CASE_ID_NEW_USECASE 5
// 6 is taken twice on other branches (home-assistant, roomvox)
#define USE_CASE_ID_NEXTCLOUD 7

// Validate use case selection: exactly one must be defined
#if defined(USE_CASE_SEATSURFING)
#if defined(USE_CASE_HISTORIAN) || defined(USE_CASE_HOMEMATIC) || defined(USE_CASE_NEW_USECASE)
#error                                                                                             \
    "Define exactly one use case: USE_CASE_SEATSURFING, USE_CASE_HISTORIAN, USE_CASE_HOMEMATIC, or USE_CASE_NEW_USECASE."
#endif
#define USE_CASE_NAME "SeatSurfing"
#define USE_CASE_ID USE_CASE_ID_SEATSURFING
#elif defined(USE_CASE_HISTORIAN)
#if defined(USE_CASE_HOMEMATIC) || defined(USE_CASE_NEW_USECASE)
#error                                                                                             \
    "Define exactly one use case: USE_CASE_SEATSURFING, USE_CASE_HISTORIAN, USE_CASE_HOMEMATIC, or USE_CASE_NEW_USECASE."
#endif
#define USE_CASE_NAME "Historian"
#define USE_CASE_ID USE_CASE_ID_HISTORIAN
#elif defined(USE_CASE_HOMEMATIC)
#if defined(USE_CASE_NEW_USECASE)
#error                                                                                             \
    "Define exactly one use case: USE_CASE_SEATSURFING, USE_CASE_HISTORIAN, USE_CASE_HOMEMATIC, or USE_CASE_NEW_USECASE."
#endif
#define USE_CASE_NAME "Homematic"
#define USE_CASE_ID USE_CASE_ID_HOMEMATIC
#elif defined(USE_CASE_WEATHERMAP)
#define USE_CASE_NAME "Weathermap"
#define USE_CASE_ID USE_CASE_ID_WEATHERMAP
#elif defined(USE_CASE_NEXTCLOUD)
#define USE_CASE_NAME "Nextcloud"
#define USE_CASE_ID USE_CASE_ID_NEXTCLOUD
#elif defined(USE_CASE_NEW_USECASE)
#define USE_CASE_NAME "NewUseCase"
#define USE_CASE_ID USE_CASE_ID_NEW_USECASE
#else
#error                                                                                             \
    "No use case defined! Define USE_CASE_SEATSURFING, USE_CASE_HISTORIAN, USE_CASE_HOMEMATIC, USE_CASE_WEATHERMAP, USE_CASE_NEXTCLOUD, or USE_CASE_NEW_USECASE."
#endif

#define WIFI_SETUP_TIMEOUT_MS (15 * 60 * 1000)

// -----------------------------------------------------------------------------
// Optional debug helpers (controlled via CMake/build.sh flags)
// -----------------------------------------------------------------------------
#ifndef INKI_DEBUG_USB_WAIT
#define INKI_DEBUG_USB_WAIT 0
#endif

#ifndef INKI_DEBUG_TLS_CB
#define INKI_DEBUG_TLS_CB 0
#endif

// Per-wake timing lines ([TIMING] ...) for DNS, TLS and HTTP, and alternating WiFi power save.
#ifndef INKI_DEBUG_TIMING
#define INKI_DEBUG_TIMING 0
#endif

// -----------------------------------------------------------------------------
// Timezone and DST configuration
// -----------------------------------------------------------------------------

/**
 * @brief The base timezone offset in hours from UTC.
 *
 * This should reflect the standard (non-DST) timezone, e.g.:
 * - Central Europe (MEZ): +1
 * - UK/Ireland: 0
 * - US Eastern Standard Time (EST): -5
 */
#define TIMEZONE_OFFSET_HOURS (+1)

/**
 * @brief Enable DST support with a region-specific rule.
 *
 * Define one of the following to enable DST calculations.
 * You may later extend this with other rules like USE_DST_US, USE_DST_AU, etc.
 */
#define USE_DST_EUROPE

#define DS3231_SDA_PIN 20
#define DS3231_SCL_PIN 21
#define I2C_FREQ 400 * 1000 // max at 400 kHz
#define GATE_PIN 22         // GATE PIN, MOSFET for power supply control

// -----------------------------------------------------------------------------
// ST25 NFC Tag (optional, auto-detected at runtime via I2C probe)
// -----------------------------------------------------------------------------
#define ST25_VCC_EN_PIN 18           // GPIO to enable ST25 VCC (set -1 to disable)
#define ST25_VEH_ADC_INPUT 2         // ADC input for V_EH (ADC2 = GP28)
#define ST25_POWER_ON_DELAY_MS 10    // Delay after VCC enable before I2C access
#define ST25_I2C_OP_TIMEOUT_US 20000 // I2C operation timeout for ST25

#define EPAPER_ON

#define QR_ENABLED
// #define BATTERY_STATUS

// -----------------------------------------------------------------------------
// LED Configuration
// -----------------------------------------------------------------------------

// Use external front LED connected to a GPIO (1=enable, 0=disable)
#ifndef LED_USE_EXT
#define LED_USE_EXT 1
#endif

// GPIO number for external LED (default GP16)
#ifndef LED_EXT_GPIO
#define LED_EXT_GPIO 16
#endif

// Use Pico W onboard LED (1=enable, 0=disable)
// Note: Only usable after cyw43 is initialized (e.g., in Wi‑Fi setup)
#ifndef LED_USE_BOARD
#define LED_USE_BOARD 1
#endif

// Blink Morse code during Wi‑Fi setup (1=enable, 0=disable)
#ifndef LED_MORSE_ENABLED
#define LED_MORSE_ENABLED 1
#endif

// Morse timing unit in milliseconds
#ifndef LED_MORSE_UNIT_MS
#define LED_MORSE_UNIT_MS 150
#endif

/**
 * @brief Maximum lengths for Wi-Fi credentials.
 */
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

#endif // CONFIG_H
