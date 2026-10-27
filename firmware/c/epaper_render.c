#include "epaper_render.h"
#include "GUI_Paint.h"
#include "config.h"
#define LOG_MODULE LOG_MOD_EPAPER
#include "debug.h"
#include "device_config.h"
#include "epaper_layout.h"
#include "flash.h"
#include "sensors.h"
#include "version.h"
#include <stdio.h>
#include <string.h>

#include "epaper.h"

void epaper_draw_subimage(uint8_t *buffer, const SubImage *sub_image, int x, int y) {
    int buffer_width = epaper_get_width();
    int buffer_height = epaper_get_height();

    if (buffer_width == 0 || buffer_height == 0) {
        dlog("epaper_draw_subimage: display not initialized\n");
        return;
    }

    // If Paint is in 4Gray mode, draw via Paint API to ensure proper 2bpp packing
    if (Paint.Scale == 4) {
        for (int j = 0; j < sub_image->height; j++) {
            for (int i = 0; i < sub_image->width; i++) {
                if (x + i >= buffer_width || y + j >= buffer_height)
                    continue;
                int sub_index = (j * sub_image->width + i) / 8;
                int sub_bit = 7 - (i % 8);
                bool is_black = (sub_image->data[sub_index] & (1 << sub_bit)) != 0;
                UWORD col = is_black ? GRAY1 : GRAY4;
                Paint_SetPixel((UWORD)(x + i), (UWORD)(y + j), col);
            }
        }
    } else {
        // 1-bit buffer path: manipulate target buffer directly for speed
        for (int j = 0; j < sub_image->height; j++) {
            for (int i = 0; i < sub_image->width; i++) {
                if (x + i >= buffer_width || y + j >= buffer_height)
                    continue;
                int buffer_index = ((y + j) * buffer_width + (x + i)) / 8;
                int buffer_bit = 7 - ((x + i) % 8);
                int sub_index = (j * sub_image->width + i) / 8;
                int sub_bit = 7 - (i % 8);
                if ((sub_image->data[sub_index] & (1 << sub_bit)) != 0) {
                    buffer[buffer_index] &= ~(1 << buffer_bit); // Black pixel
                } else {
                    buffer[buffer_index] |= (1 << buffer_bit); // White pixel
                }
            }
        }
    }
}

void epaper_draw_toggle(int x, int y, const sFONT *font, bool is_on) {
    int h = font->Height;
    int th = h - 6;
    if (th < 12)
        th = h - 4;
    int top = y + (h - th) / 2;
    int bottom = top + th;
    int left = x;
    int r_track = th / 2;
    int cy = top + r_track;
    int lx = left + r_track;
    const char *txt = is_on ? "an" : "aus";
    int text_px = (int)strlen(txt) * font->Width;
    int r = r_track - 3;
    // cppcheck-suppress knownConditionTrueFalse
    if (r < 4)
        r = 4;
    int straight_len = text_px + 2 * (r + 4);
    int tw = 2 * r_track + straight_len;
    if (tw < th * 2)
        tw = th * 2;
    int right = left + tw;
    int rx = right - r_track;

    Paint_DrawLine(lx, top, rx, top, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(lx, bottom, rx, bottom, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawCircle(lx, cy, r_track, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawCircle(rx, cy, r_track, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

    int cx = is_on ? lx : rx;
    Paint_DrawCircle(cx, cy, r, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    int tx;
    if (is_on) {
        tx = lx + r + 4;
        if (tx + text_px > right - 2)
            tx = right - 2 - text_px;
    } else {
        tx = rx - r - 4 - text_px;
        if (tx < left + 2)
            tx = left + 2;
    }
    int ty = y;
    Paint_DrawString_EN(tx, ty, txt, (sFONT *)font, WHITE, BLACK);
}

static int battery_voltage_to_level(float voltage, VoltageInterval *table, int table_size) {
    for (int i = 0; i < table_size; i++) {
        if (voltage >= table[i].voltage_min && voltage <= table[i].voltage_max) {
            return table[i].group_value;
        }
    }
    return -1;
}

void epaper_draw_battery_icon(float voltage, uint8_t *buffer, int x, int y) {
    VoltageInterval interval_table[] = {{10, 2.8f, 3.4130f},    {20, 3.4130f, 3.6830f},
                                        {30, 3.6830f, 3.8000f}, {40, 3.8000f, 3.8910f},
                                        {50, 3.8910f, 3.9575f}, {60, 3.9575f, 4.0240f},
                                        {70, 4.0240f, 4.0830f}, {80, 4.0830f, 4.2290f},
                                        {90, 4.2290f, 4.2970f}, {100, 4.2970f, 4.9f}};

    int table_size = sizeof(interval_table) / sizeof(interval_table[0]);
    int battery_level = battery_voltage_to_level(voltage, interval_table, table_size);

    if (battery_level == 10) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_1], x, y);
    } else if (battery_level == 20) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_2], x, y);
    } else if (battery_level == 30) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_3], x, y);
    } else if (battery_level == 40) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_4], x, y);
    } else if (battery_level == 50) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_5], x, y);
    } else if (battery_level == 60) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_6], x, y);
    } else if (battery_level == 70) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_7], x, y);
    } else if (battery_level == 80) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_8], x, y);
    } else if (battery_level == 90) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_9], x, y);
    } else if (battery_level == 100) {
        epaper_draw_subimage(buffer, &battery_levels_64x97[BATTERY_LEVEL_10], x, y);
    } else {
        printf("Voltage %.2f is out of range!\n", voltage);
    }
}

bool epaper_draw_custom_logo(uint8_t *buffer, int x, int y) {
    const logo_header_t *header = (const logo_header_t *)FLASH_PTR(LOGO_FLASH_OFFSET);

    if (memcmp(header->magic, "LOGO", 4) != 0) {
        dlog("No valid flash logo found\n");
        return false;
    }

    dlog("Flash logo: %dx%d px, %d bytes\n", header->width, header->height, header->datalen);

    const uint8_t *bitmap = FLASH_PTR(LOGO_FLASH_OFFSET + sizeof(logo_header_t));

    SubImage logo_image = {.data = bitmap, .width = header->width, .height = header->height};

    epaper_draw_subimage(buffer, &logo_image, x, y);
    return true;
}

void epaper_draw_firmware_info(void) {
    char buffer[128];

    snprintf(buffer, sizeof(buffer), "%s %s %s, U=%.2fV", program_name, version, build_date,
             boot_battery_voltage());

    sFONT *font;
    int y;
    switch (device_config_flash.data.epapertype) {
    case EPAPER_WAVESHARE_7IN5_V2:
        font = &Font12;
        y = 464;
        break;
    case EPAPER_WAVESHARE_4IN2_V2:
        font = &Font8;
        y = 292;
        break;
    case EPAPER_WAVESHARE_2IN9_V2:
        font = &Font12;
        y = 284;
        break;
    default:
        dlog("Unsupported ePaper type: %d\n", device_config_flash.data.epapertype);
        return;
    }

    int x = (int)Paint.Width - 2 - (int)strlen(buffer) * font->Width;
    if (x < 0)
        x = 0;
    Paint_DrawString_EN(x, y, buffer, font, WHITE, BLACK);
}

void epaper_draw_4gray_test(void) {
    Paint_DrawString_EN(50, 30, "4-Level Grayscale Test", &font_ubuntu_mono_8pt_bold, GRAY4, GRAY1);

    Paint_DrawCircle(100, 100, 30, GRAY4, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(75, 140, "White", &font_ubuntu_mono_8pt, GRAY4, GRAY1);

    Paint_DrawCircle(300, 100, 30, GRAY3, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(275, 140, "Light", &font_ubuntu_mono_8pt, GRAY4, GRAY1);

    Paint_DrawCircle(100, 200, 30, GRAY2, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(80, 240, "Dark", &font_ubuntu_mono_8pt, GRAY4, GRAY1);

    Paint_DrawCircle(300, 200, 30, GRAY1, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(275, 240, "Black", &font_ubuntu_mono_8pt, GRAY4, GRAY1);
}

void epaper_render_layout(uint8_t *buffer, const layout_elem_t *elements, int count,
                          const char *vars[]) {
    for (int i = 0; i < count; i++) {
        const layout_elem_t *e = &elements[i];
        switch (e->type) {
        case ELEM_TEXT:
            if (e->text && e->font) {
                Paint_DrawString_EN(e->x, e->y, e->text, (sFONT *)e->font, WHITE, BLACK);
            }
            break;

        case ELEM_VAR:
            if (vars && vars[e->var_index] && e->font) {
                Paint_DrawString_EN(e->x, e->y, vars[e->var_index], (sFONT *)e->font, WHITE, BLACK);
            }
            break;

        case ELEM_LOGO:
            if (!epaper_draw_custom_logo(buffer, e->x, e->y)) {
                if (e->image) {
                    epaper_draw_subimage(buffer, e->image, e->x, e->y);
                }
            }
            break;

        case ELEM_SUBIMAGE:
            if (e->image) {
                epaper_draw_subimage(buffer, e->image, e->x, e->y);
            }
            break;

        case ELEM_VLINE:
            Paint_DrawLine(e->x, e->y, e->x, e->end_coord, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
            break;
        }
    }
}
