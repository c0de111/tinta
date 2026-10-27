#include "epaper.h"
#include "DEV_Config.h"
#include "EPD_2in9_V2.h"
#include "EPD_4in2_V2.h"
#include "EPD_7in5_V2.h"
#include "GUI_Paint.h"
#define LOG_MODULE LOG_MOD_EPAPER
#include "debug.h"
#include "flash.h"
#include "hardware/watchdog.h"
#include <stdlib.h>

static uint16_t s_width = 0;
static uint16_t s_height = 0;
static bool s_is_4gray = false;

uint16_t epaper_get_width(void) { return s_width; }
uint16_t epaper_get_height(void) { return s_height; }
bool epaper_is_4gray(void) { return s_is_4gray; }

uint8_t *epaper_init(bool is_4gray) {

    if (device_config_flash.data.epapertype == EPAPER_NONE) {
        dlog("No ePaper configured for this room.\n");
        return NULL;
    }

    watchdog_update();

    // Once per run: DEV_Module_Init() also runs stdio_init_all(), and a second USB stdio init
    // while the first is live claims its IRQs again. Pins and SPI stay set up, the exit is empty.
    static bool module_ready;
    if (!module_ready) {
        if (DEV_Module_Init() != 0) {
            dlog("Error initializing ePaper hardware module.\n");
            return NULL;
        }
        module_ready = true;
    }

    dtrace("Disabling watchdog for ePaper setup...\n");
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    UDOUBLE buffer_size = 0;

    s_is_4gray = is_4gray;

    switch (device_config_flash.data.epapertype) {
    case EPAPER_WAVESHARE_7IN5_V2:
        dlog("Initializing Waveshare 7.5-inch V2 ePaper...\n");
        s_width = EPD_7IN5_V2_WIDTH;
        s_height = EPD_7IN5_V2_HEIGHT;
        if (is_4gray) {
            EPD_7IN5_V2_Init_4Gray();
            EPD_7IN5_V2_Clear();
            buffer_size = ((EPD_7IN5_V2_WIDTH % 8 == 0) ? (EPD_7IN5_V2_WIDTH / 4)
                                                        : (EPD_7IN5_V2_WIDTH / 4 + 1)) *
                          EPD_7IN5_V2_HEIGHT;
        } else {
            EPD_7IN5_V2_Init_Fast();
            buffer_size = ((EPD_7IN5_V2_WIDTH % 8 == 0) ? (EPD_7IN5_V2_WIDTH / 8)
                                                        : (EPD_7IN5_V2_WIDTH / 8 + 1)) *
                          EPD_7IN5_V2_HEIGHT;
        }
        break;

    case EPAPER_WAVESHARE_4IN2_V2:
        dlog("Initializing Waveshare 4.2-inch ePaper...\n");
        s_width = EPD_4IN2_V2_WIDTH;
        s_height = EPD_4IN2_V2_HEIGHT;
        if (is_4gray) {
            // Waveshare official pattern: First clear in regular mode, then switch to 4Gray
            EPD_4IN2_V2_Init();
            EPD_4IN2_V2_Clear();
            sleep_ms(500); // Official timing from Waveshare examples
            EPD_4IN2_V2_Init_4Gray();
            buffer_size = ((EPD_4IN2_V2_WIDTH % 8 == 0) ? (EPD_4IN2_V2_WIDTH / 4)
                                                        : (EPD_4IN2_V2_WIDTH / 4 + 1)) *
                          EPD_4IN2_V2_HEIGHT;
        } else {
            EPD_4IN2_V2_Init_Fast(Seconds_1S);
            buffer_size = ((EPD_4IN2_V2_WIDTH % 8 == 0) ? (EPD_4IN2_V2_WIDTH / 8)
                                                        : (EPD_4IN2_V2_WIDTH / 8 + 1)) *
                          EPD_4IN2_V2_HEIGHT;
        }
        break;

    case EPAPER_WAVESHARE_2IN9_V2:
        dlog("Initializing Waveshare 2.9-inch V2 ePaper...\n");
        s_width = EPD_2IN9_V2_WIDTH;
        s_height = EPD_2IN9_V2_HEIGHT;
        EPD_2IN9_V2_Init();
        EPD_2IN9_V2_Clear();
        buffer_size =
            ((EPD_2IN9_V2_WIDTH % 8 == 0) ? (EPD_2IN9_V2_WIDTH / 8) : (EPD_2IN9_V2_WIDTH / 8 + 1)) *
            EPD_2IN9_V2_HEIGHT;
        break;

    default:
        dlog("Unsupported ePaper type: %d\n", device_config_flash.data.epapertype);
        hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS); // Re-enable watchdog
        return NULL;
    }

    dtrace("Re-enabling watchdog...\n");
    watchdog_enable(device_config_flash.data.watchdog_time, 0);
    watchdog_update();

    // Create a new image cache
    uint8_t *image_buf = (uint8_t *)malloc(buffer_size);
    if (image_buf == NULL) {
        debug_status("ERROR", "ePaper: buffer allocation failed (%lu bytes)\n",
                     (unsigned long)buffer_size);
        hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
        return NULL;
    }

    Paint_NewImage(
        image_buf,
        (device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2) ? EPD_7IN5_V2_WIDTH
                                                                          : EPD_4IN2_V2_WIDTH,
        (device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2) ? EPD_7IN5_V2_HEIGHT
                                                                          : EPD_4IN2_V2_HEIGHT,
        0, WHITE);

    Paint_SelectImage(image_buf);
    if (is_4gray) {
        Paint_SetScale(4);
        Paint_Clear(GRAY4);
    } else {
        Paint_Clear(WHITE);
    }

    watchdog_update();

    const char *type_name =
        (device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2) ? "7.5\" V2" : "4.2\" V2";
    debug_status("OK", "ePaper: %s, %lu bytes\n", type_name, (unsigned long)buffer_size);
    return image_buf;
}

void epaper_flush_and_sleep(uint8_t *image) {
    if (image == NULL) {
        dlog("No valid image buffer to display. Skipping ePaper operations.\n");
        return;
    }

    watchdog_update();

    dtrace("EPD_Display called for epaper type: %d\n", device_config_flash.data.epapertype);
    switch (device_config_flash.data.epapertype) {
    case EPAPER_WAVESHARE_7IN5_V2:
        if (s_is_4gray)
            EPD_7IN5_V2_Display_4Gray(image);
        else
            EPD_7IN5_V2_Display(image);
        break;

    case EPAPER_WAVESHARE_4IN2_V2:
        if (s_is_4gray)
            EPD_4IN2_V2_Display_4Gray(image);
        else
            EPD_4IN2_V2_Display(image);
        break;

    case EPAPER_WAVESHARE_2IN9_V2:
        EPD_2IN9_V2_Display(image);
        break;

    default:
        dlog("Unsupported ePaper type: %d\n", device_config_flash.data.epapertype);
        free(image);
        return;
    }

    // Free allocated memory for the image
    free(image);
    watchdog_update();

    dtrace("Entering ePaper sleep mode for type: %d\n", device_config_flash.data.epapertype);
    switch (device_config_flash.data.epapertype) {
    case EPAPER_WAVESHARE_7IN5_V2:
        EPD_7IN5_V2_Sleep();
        break;

    case EPAPER_WAVESHARE_4IN2_V2:
        EPD_4IN2_V2_Sleep();
        break;

    case EPAPER_WAVESHARE_2IN9_V2:
        EPD_2IN9_V2_Sleep();
        break;

    default:
        dlog("Unsupported ePaper type during sleep: %d\n", device_config_flash.data.epapertype);
        return;
    }

    // Short delay to ensure the sleep command is processed
    DEV_Delay_ms(200);

    dtrace("Shutting down the ePaper module...\n");
    DEV_Module_Exit();
    watchdog_update();
}
