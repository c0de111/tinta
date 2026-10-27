# Minimum required version of CMake
cmake_minimum_required(VERSION 3.12)

set(PICO_BOARD pico_w)
set(PICO_WIFI 1)
set(PICO_CYW43_ARCH_DEFAULT "pico_cyw43_arch_lwip_threadsafe_background")

# Include the Pico SDK and set up the project
include(pico_sdk_import.cmake)
project(inki C CXX ASM)

# Set C/C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Add external libraries and dependencies as subdirectories
add_subdirectory(third_party/Config)    # ePaper-specific configuration utilities (third-party)
add_subdirectory(third_party/e-Paper)  # ePaper display libraries (third-party)
add_subdirectory(third_party/Fonts)    # Font resources for ePaper (third-party)
add_subdirectory(third_party/GUI)      # Drawing routines for ePaper (third-party)
add_subdirectory(third_party/fonts_custom)        # Custom Ubuntu Mono fonts
add_subdirectory(third_party/ds3231)   # RTC driver (DS3231)
add_subdirectory(third_party/st25dv)   # ST25DV NFC tag driver

set(PICO_CYW43_ARCH_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/c)
add_compile_definitions(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND)
# set(PICO_CYW43_ARCH_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/c)

# --- Use case source selection ---
# Derive lowercase use case name from USE_CASE_DEFINE (e.g. USE_CASE_SEATSURFING -> seatsurfing)
if(USE_CASE_DEFINE)
    string(REPLACE "USE_CASE_" "" _UC_NAME "${USE_CASE_DEFINE}")
    string(TOLOWER "${_UC_NAME}" USE_CASE)
else()
    set(USE_CASE "seatsurfing")
endif()

if(USE_CASE STREQUAL "seatsurfing")
    set(UC_SOURCES
        seatsurfing/client.c
        seatsurfing/device_mode.c
        seatsurfing/epaper_pages.c
        seatsurfing/web_config.c
        seatsurfing/seatsurfing_flash.c
        third_party/cjson/cJSON.c
    )
    set(UC_TRUST_STORE_HEADER "certs/trust_store_seatsurfing_pem.h")
elseif(USE_CASE STREQUAL "historian")
    set(UC_SOURCES
        historian/client.c
        historian/epaper_pages.c
        historian/web_config.c
        historian/historian_flash.c
        third_party/cjson/cJSON.c
    )
    set(UC_TRUST_STORE_HEADER "")
elseif(USE_CASE STREQUAL "homematic")
    set(UC_SOURCES
        homematic/client.c
        homematic/epaper_pages.c
        homematic/web_config.c
        homematic/homematic_flash.c
    )
    set(UC_TRUST_STORE_HEADER "")
elseif(USE_CASE STREQUAL "weathermap")
    set(UC_SOURCES
        weathermap/client.c
        weathermap/epaper_pages.c
        weathermap/web_config.c
        weathermap/weathermap_flash.c
        weathermap/png_stream.c
        third_party/miniz/miniz_tinfl.c
    )
    set(UC_TRUST_STORE_HEADER "certs/trust_store_weathermap_pem.h")
elseif(USE_CASE STREQUAL "nextcloud")
    set(UC_SOURCES
        nextcloud/client.c
        nextcloud/epaper_pages.c
        nextcloud/ical_text.c
        nextcloud/log_upload.c
        nextcloud/planer_parser.c
        nextcloud/tasks_parser.c
        nextcloud/tracker.c
        nextcloud/tracker_parse.c
    )
    set(UC_TRUST_STORE_HEADER "")
else()
    message(FATAL_ERROR "Unknown USE_CASE '${USE_CASE}'. Valid: seatsurfing, historian, homematic, weathermap, nextcloud")
endif()
message(STATUS "Use case: ${USE_CASE}, sources: ${UC_SOURCES}")

add_executable(inki_slot0
    main.c          # Main application logic
    led.c           # LED control helpers (external + Pico W)
    morse.c         # Morse engine for status LED
    ${CMAKE_CURRENT_BINARY_DIR}/version.c  # Auto-generated version info
    wifi.c          # Wi-Fi handling logic
    http_client.c   # Robust HTTP client with historian architecture
    http_parse.c    # Pure HTTP header parse helpers (content-length, status, host)
    inki_monitor.c  # Optional telemetry POST to inki-monitor (runtime, fail-safe)
    ${UC_SOURCES}
    ImageResources.c     # Image data for ePaper display
    debug.c         # Debug functions
    flash.c         # Persistent config handling
    webserver.c
    web/webserver_utils.c   # Utility functions extracted from webserver
    web/captive_portal.c    # Captive portal DHCP + DNS servers
    web/ota.c               # OTA state machine + flash writer
    web/web_logo.c          # Logo serve + delete + upload handlers
    web/web_landing.c       # Landing page
    web/web_status.c        # Device status page
    web/web_wifi.c          # Wi-Fi settings page + form handler
    web/web_clock.c         # Clock settings page + form handler
    web/web_firmware.c      # Firmware update + logo upload pages
    web/web_device_settings.c # Device settings page + form handler
    web/web_export_settings.c # Settings import/export + form handler
    web/webserver_scaffold.c # Unified page scaffold (HTML/CSS wrapper)
    i2c_bus.c         # I2C bus init + device probing
    boot_input.c        # Unified boot input (pushbuttons + NFC + RTC alarm)
    sensors.c           # ADC sensors (battery, coin cell, temperature, strap pins)
    power.c             # Power hold, shutdown, gate control
    st25_io.c           # ST25 NFC bus adapter and wake logic
    nfc_ndef.c          # Web address on the tag, read by phones without an app
    net_update.c        # Firmware update over the network (buttons 3 + 4, --exp-net-update)
    third_party/qrcodegen/qrcodegen.c # QR code for the setup page
    rv3028.c            # RV-3028 RTC driver (runtime backend support)
    rtc.c               # RTC facade (DS3231/RV-3028 backend abstraction)
    epaper.c            # ePaper hardware init, display, sleep
    epaper_render.c     # ePaper shared render helpers (subimage, battery, logo)
    epaper_pages_shared.c # Shared ePaper pages (DnD, UDM, info, error, wifi setup)
    base64.c        # Base64 encoding for HTTP auth
    tls_trust_store.c  # TLS trust store (HARICA root embedding, stubs for non-TLS use cases)
    ${CMAKE_CURRENT_BINARY_DIR}/header_slot0.c
)

add_executable(inki_slot1
    main.c          # Main application logic
    led.c           # LED control helpers (external + Pico W)
    morse.c         # Morse engine for status LED
    ${CMAKE_CURRENT_BINARY_DIR}/version.c  # Auto-generated version info
    wifi.c          # Wi-Fi handling logic
    http_client.c   # Robust HTTP client with historian architecture
    http_parse.c    # Pure HTTP header parse helpers (content-length, status, host)
    inki_monitor.c  # Optional telemetry POST to inki-monitor (runtime, fail-safe)
    ${UC_SOURCES}
    ImageResources.c     # Image data for ePaper display
    debug.c         # Debug functions
    flash.c         # Persistent config handling
    webserver.c
    web/webserver_utils.c   # Utility functions extracted from webserver
    web/captive_portal.c    # Captive portal DHCP + DNS servers
    web/ota.c               # OTA state machine + flash writer
    web/web_logo.c          # Logo serve + delete + upload handlers
    web/web_landing.c       # Landing page
    web/web_status.c        # Device status page
    web/web_wifi.c          # Wi-Fi settings page + form handler
    web/web_clock.c         # Clock settings page + form handler
    web/web_firmware.c      # Firmware update + logo upload pages
    web/web_device_settings.c # Device settings page + form handler
    web/web_export_settings.c # Settings import/export + form handler
    web/webserver_scaffold.c # Unified page scaffold (HTML/CSS wrapper)
    i2c_bus.c         # I2C bus init + device probing
    boot_input.c        # Unified boot input (pushbuttons + NFC + RTC alarm)
    sensors.c           # ADC sensors (battery, coin cell, temperature, strap pins)
    power.c             # Power hold, shutdown, gate control
    st25_io.c           # ST25 NFC bus adapter and wake logic
    nfc_ndef.c          # Web address on the tag, read by phones without an app
    net_update.c        # Firmware update over the network (buttons 3 + 4, --exp-net-update)
    third_party/qrcodegen/qrcodegen.c # QR code for the setup page
    rv3028.c            # RV-3028 RTC driver (runtime backend support)
    rtc.c               # RTC facade (DS3231/RV-3028 backend abstraction)
    epaper.c            # ePaper hardware init, display, sleep
    epaper_render.c     # ePaper shared render helpers (subimage, battery, logo)
    epaper_pages_shared.c # Shared ePaper pages (DnD, UDM, info, error, wifi setup)
    base64.c        # Base64 encoding for HTTP auth
    tls_trust_store.c  # TLS trust store (HARICA root embedding, stubs for non-TLS use cases)
    ${CMAKE_CURRENT_BINARY_DIR}/header_slot1.c
)

add_executable(inki_bootloader
    bootloader.c
)

add_executable(inki_default_config
    default_config.c
)

pico_set_linker_script(inki_bootloader ${CMAKE_CURRENT_SOURCE_DIR}/inki_bootloader.ld)
pico_set_linker_script(inki_slot0 ${CMAKE_CURRENT_SOURCE_DIR}/inki_flash_slot0.ld)
pico_set_linker_script(inki_slot1 ${CMAKE_CURRENT_SOURCE_DIR}/inki_flash_slot1.ld)
# pico_set_linker_script(inki_default_config ${CMAKE_CURRENT_SOURCE_DIR}/inki_default_config.ld)
target_link_options(inki_default_config PRIVATE
    -nostartfiles
    -nostdlib
    -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/inki_default_config.ld  # Dein .ld-Skript
)

# Ensure default_config is built with the same use-case define
if(USE_CASE_DEFINE)
    target_compile_definitions(inki_default_config PRIVATE ${USE_CASE_DEFINE})
endif()

# Minimal includes needed to compile default_config with historian headers
target_include_directories(inki_default_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Avoid pulling RTC/Pico SDK for config-only build
target_compile_definitions(inki_default_config PRIVATE HISTORIAN_CONFIG_NO_RTC=1)
target_compile_definitions(inki_default_config PRIVATE HISTORIAN_CONFIG_NO_LWIP=1)
target_compile_definitions(pico_cyw43_arch_lwip_threadsafe_background INTERFACE
    PICO_CYW43_ARCH_THREADSAFE_BACKGROUND=1
)

# Add include directories for the project
target_include_directories(inki_slot0 PRIVATE
    third_party/e-Paper      # ePaper libraries
    third_party/GUI          # Drawing utilities for ePaper
    third_party/Fonts        # Font resources
    third_party/Config       # ePaper-specific configuration utilities
    third_party/fonts_custom           # Custom Ubuntu Mono fonts
    third_party/cjson                  # cJSON library
    third_party/st25dv                 # ST25DV NFC tag driver
    ${CMAKE_CURRENT_LIST_DIR} # Current project directory
    ${CMAKE_CURRENT_SOURCE_DIR}          # damit das Projekt-Verzeichnis (z. B. `c/`) durchsucht wird
    ${CMAKE_CURRENT_SOURCE_DIR}/web      # Web UI page files
)

# Add include directories for the project
target_include_directories(inki_slot1 PRIVATE
    third_party/e-Paper      # ePaper libraries
    third_party/GUI          # Drawing utilities for ePaper
    third_party/Fonts        # Font resources
    third_party/Config       # ePaper-specific configuration utilities
    third_party/fonts_custom           # Custom Ubuntu Mono fonts
    third_party/cjson                  # cJSON library
    third_party/st25dv                 # ST25DV NFC tag driver
    ${CMAKE_CURRENT_LIST_DIR} # Current project directory
    ${CMAKE_CURRENT_SOURCE_DIR}          # damit das Projekt-Verzeichnis (z. B. `c/`) durchsucht wird
    ${CMAKE_CURRENT_SOURCE_DIR}/web      # Web UI page files
)
# --- Git version and build date for firmware metadata ---
find_package(Git REQUIRED)

# Get Git version string, e.g. "v1.0.7-30-g3131d-dirty"
execute_process(
    COMMAND git describe --tags --always --dirty --abbrev=5
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get build date in YYYY-MM-DD format
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")

# Print version info for logging/debugging
message(STATUS "Firmware Git version: ${GIT_VERSION}")
message(STATUS "Build date: ${BUILD_DATE}")

# --- Generate version.c from template ---
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version_template.c.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.c
    @ONLY
)

# --- Generate firmware header source files (slot 0 and slot 1) ---
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/header_slot0_template.c.in
    ${CMAKE_CURRENT_BINARY_DIR}/header_slot0.c
    @ONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/header_slot1_template.c.in
    ${CMAKE_CURRENT_BINARY_DIR}/header_slot1.c
    @ONLY
)

target_sources(inki_slot0 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/version.c
    ${CMAKE_CURRENT_BINARY_DIR}/header_slot0.c
)

target_sources(inki_slot1 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/version.c
    ${CMAKE_CURRENT_BINARY_DIR}/header_slot1.c
)

option(USB_BOOTLOADER_ENABLE "Enable USB support in bootloader" OFF)
option(INKI_DEBUG_USB_WAIT "Enable debug USB wait helper in application startup" OFF)
option(INKI_DEBUG_TLS_CB "Enable debug TLS callback helper in application TLS setup" OFF)
option(INKI_DEBUG_TIMING "Log DNS, TLS and HTTP timing per wake" OFF)
option(INKI_EXP_WIFI_PMK "Experiment: hand the radio a precomputed WPA2 key (see wifi.c)" OFF)
option(INKI_LOG_UPLOAD "Nextcloud: upload the log of each wake to the tracker host" OFF)
option(INKI_NET_UPDATE "Experiment: buttons 3 + 4 fetch new firmware from the update server" OFF)
option(INKI_EXP_DHCP_REBOOT "Experiment: ask DHCP for the last address again (INIT-REBOOT)" OFF)

if(USB_BOOTLOADER_ENABLE)
pico_enable_stdio_usb(inki_bootloader 1)
    add_definitions(-DUSB_BOOTLOADER_ENABLE=1)
else()
pico_enable_stdio_usb(inki_bootloader 0)
endif()

pico_enable_stdio_uart(inki_bootloader 0)
pico_enable_stdio_usb(inki_slot0 1)
pico_enable_stdio_uart(inki_slot0 0)
pico_enable_stdio_usb(inki_slot1 1)
pico_enable_stdio_uart(inki_slot1 0)

target_link_libraries(inki_slot0
    ePaper                         # ePaper driver
    GUI                            # Drawing utilities for ePaper
    Fonts                          # Font resources for ePaper
    fonts_custom                   # Custom Ubuntu Mono fonts
    Config                         # ePaper-specific configuration utilities
    pico_stdlib                    # Standard Pico SDK library
    hardware_spi                   # SPI hardware support
    pico_cyw43_arch_lwip_threadsafe_background # Thread-safe Wi-Fi
    pico_ds3231                    # DS3231 RTC driver
    st25dv                         # ST25DV NFC tag driver
    hardware_i2c                   # I2C hardware support (ST25 bus adapter)
    hardware_adc                   # ADC hardware support
    pico_rand                      # Random number utilities
    pico_unique_id                 # Board ID, source of the setup password
    pico_lwip
    pico_lwip_mbedtls
    pico_mbedtls
)
target_link_libraries(inki_slot1
    ePaper                         # ePaper driver
    GUI                            # Drawing utilities for ePaper
    Fonts                          # Font resources for ePaper
    fonts_custom                   # Custom Ubuntu Mono fonts
    Config                         # ePaper-specific configuration utilities
    pico_stdlib                    # Standard Pico SDK library
    hardware_spi                   # SPI hardware support
    pico_cyw43_arch_lwip_threadsafe_background # Thread-safe Wi-Fi
    pico_ds3231                    # DS3231 RTC driver
    st25dv                         # ST25DV NFC tag driver
    hardware_i2c                   # I2C hardware support (ST25 bus adapter)
    hardware_adc                   # ADC hardware support
    pico_rand                      # Random number utilities
    pico_unique_id                 # Board ID, source of the setup password
    pico_lwip
    pico_lwip_mbedtls
    pico_mbedtls
)

target_link_libraries(inki_bootloader
    pico_stdlib
)

# Use case selection - can be overridden by build script
if(USE_CASE_DEFINE)
    target_compile_definitions(inki_slot0 PRIVATE ${USE_CASE_DEFINE})
    target_compile_definitions(inki_slot1 PRIVATE ${USE_CASE_DEFINE})
    message(STATUS "Use case override: ${USE_CASE_DEFINE}")
endif()

if(UC_TRUST_STORE_HEADER)
    target_compile_definitions(inki_slot0 PRIVATE TRUST_STORE_HEADER="${UC_TRUST_STORE_HEADER}")
    target_compile_definitions(inki_slot1 PRIVATE TRUST_STORE_HEADER="${UC_TRUST_STORE_HEADER}")
    message(STATUS "Trust store: ${UC_TRUST_STORE_HEADER}")
endif()

if(INKI_DEBUG_USB_WAIT)
    target_compile_definitions(inki_slot0 PRIVATE INKI_DEBUG_USB_WAIT=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_DEBUG_USB_WAIT=1)
endif()

if(INKI_DEBUG_TLS_CB)
    target_compile_definitions(inki_slot0 PRIVATE INKI_DEBUG_TLS_CB=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_DEBUG_TLS_CB=1)
endif()

if(INKI_DEBUG_TIMING)
    target_compile_definitions(inki_slot0 PRIVATE INKI_DEBUG_TIMING=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_DEBUG_TIMING=1)
endif()

if(INKI_EXP_WIFI_PMK)
    target_compile_definitions(inki_slot0 PRIVATE INKI_EXP_WIFI_PMK=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_EXP_WIFI_PMK=1)
endif()

if(INKI_LOG_UPLOAD)
    target_compile_definitions(inki_slot0 PRIVATE INKI_LOG_UPLOAD=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_LOG_UPLOAD=1)
endif()

if(INKI_NET_UPDATE)
    target_compile_definitions(inki_slot0 PRIVATE INKI_NET_UPDATE=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_NET_UPDATE=1)
endif()

if(INKI_EXP_DHCP_REBOOT)
    target_compile_definitions(inki_slot0 PRIVATE INKI_EXP_DHCP_REBOOT=1)
    target_compile_definitions(inki_slot1 PRIVATE INKI_EXP_DHCP_REBOOT=1)
endif()

message(STATUS "Debug flags: INKI_DEBUG_USB_WAIT=${INKI_DEBUG_USB_WAIT}, INKI_DEBUG_TLS_CB=${INKI_DEBUG_TLS_CB}, INKI_DEBUG_TIMING=${INKI_DEBUG_TIMING}, INKI_EXP_WIFI_PMK=${INKI_EXP_WIFI_PMK}, INKI_LOG_UPLOAD=${INKI_LOG_UPLOAD}, INKI_NET_UPDATE=${INKI_NET_UPDATE}, INKI_EXP_DHCP_REBOOT=${INKI_EXP_DHCP_REBOOT}")
#[[
target_link_libraries(inki_default_config
    pico_stdlib
)]]


# --- ST25 tune helper (standalone, no ePaper/WiFi) ---
if(BUILD_ST25_TUNE)
    add_executable(inki_st25_tune
        st25_tune_main.c
        third_party/st25dv/st25dv.c
        third_party/st25dv/st25dv_reg.c
    )
    target_include_directories(inki_st25_tune PRIVATE
        third_party/st25dv
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(inki_st25_tune
        pico_stdlib
        hardware_i2c
        hardware_adc
    )
    if(PICO_CYW43_SUPPORTED)
        target_link_libraries(inki_st25_tune pico_cyw43_arch_none)
        target_compile_definitions(inki_st25_tune PRIVATE ST25_TUNE_HAS_CYW43=1)
    else()
        target_compile_definitions(inki_st25_tune PRIVATE ST25_TUNE_HAS_CYW43=0)
    endif()
    pico_enable_stdio_usb(inki_st25_tune 1)
    pico_enable_stdio_uart(inki_st25_tune 0)
    pico_add_extra_outputs(inki_st25_tune)
    message(STATUS "ST25 tune helper: ENABLED")
endif()

# Generate UF2/BIN/HEX outputs via SDK helpers (requires elf2uf2/picotool setup)
pico_add_extra_outputs(inki_bootloader)
pico_add_extra_outputs(inki_slot0)
pico_add_extra_outputs(inki_slot1)
pico_add_extra_outputs(inki_default_config)
