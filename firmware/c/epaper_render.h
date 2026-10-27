#ifndef EPAPER_RENDER_H
#define EPAPER_RENDER_H

#include "DEV_Config.h"
#include "ImageResources.h"
#include "fonts.h"
#include <stdbool.h>

#include "epaper_layout.h"

void epaper_draw_subimage(uint8_t *buffer, const SubImage *sub_image, int x, int y);
void epaper_draw_toggle(int x, int y, const sFONT *font, bool is_on);
void epaper_draw_battery_icon(float voltage, uint8_t *buffer, int x, int y);
bool epaper_draw_custom_logo(uint8_t *buffer, int x, int y);
void epaper_draw_firmware_info(void);
void epaper_draw_4gray_test(void);

#endif
