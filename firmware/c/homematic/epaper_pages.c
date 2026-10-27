#include "homematic/epaper_pages.h"
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include "ImageResources.h"
#include "config.h"
#include "debug.h"
#include "device_config.h"
#include "epaper.h"
#include "epaper_pages_shared.h"
#include "epaper_render.h"
#include "fonts.h"
#include "homematic/client.h"
#include "homematic/homematic_flash.h"
#include <stdio.h>
#include <string.h>

void render_page_homematic(uint8_t *image_buffer) {
    const bool is_epaper_75 = (device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2);
    const bool is_epaper_42 = (device_config_flash.data.epapertype == EPAPER_WAVESHARE_4IN2_V2);

    if (!is_epaper_75 && !is_epaper_42) {
        render_page_fallback(0, image_buffer);
        return;
    }

    // Prepare clean background for the Homematic overview
    Paint_Clear(WHITE);

    int logo_x = epaper_get_width() - inki_octopus_100_95.width - 10;
    if (logo_x < 0) {
        logo_x = 0;
    }
    if (!epaper_draw_custom_logo(image_buffer, logo_x, 10)) {
        epaper_draw_subimage(image_buffer, &inki_octopus_100_95, logo_x, 10);
    }

    const sFONT *title_font = is_epaper_75 ? (const sFONT *)&font_ubuntu_mono_18pt_bold
                                           : (const sFONT *)&font_ubuntu_mono_12pt_bold;

    Paint_DrawString_EN(20, is_epaper_75 ? 40 : 20, device_config_flash.data.roomname,
                        (sFONT *)title_font, WHITE, BLACK);

    int y = is_epaper_75 ? 90 : 65;
    const sFONT *f_top = &font_ubuntu_mono_10pt;
    for (int i = 0; i < HOMEMATIC_MAX_ITEMS; i++) {
        if (i >= ((int)homematic_config_flash.data.count)) {
            break;
        }

        char label[48];
        const char *flab = homematic_config_flash.data.items[i].fallback_label;
        if (flab && *flab) {
            snprintf(label, sizeof(label), "%s:", flab);
        } else {
            snprintf(label, sizeof(label), "%s:", homematic_config_flash.data.items[i].address);
        }

        char val[64] = "\xe2\x80\x94"; // em dash
        if (homematic_values[i].valid && !homematic_values[i].fault) {
            switch (homematic_values[i].type) {
            case HM_TYPE_DOUBLE:
                snprintf(val, sizeof(val), "%.1f", homematic_values[i].dval);
                break;
            case HM_TYPE_I4:
                snprintf(val, sizeof(val), "%d", homematic_values[i].ival);
                break;
            case HM_TYPE_BOOL:
                val[0] = '\0';
                break;
            case HM_TYPE_STRING:
                snprintf(val, sizeof(val), "%s", homematic_values[i].sval);
                break;
            default:
                break;
            }
        } else if (homematic_values[i].fault) {
            snprintf(val, sizeof(val), "fault");
        }

        const int line_start_x = is_epaper_75 ? 40 : 20;
        int x = line_start_x;
        Paint_DrawString_EN(x, y, label, (sFONT *)f_top, WHITE, BLACK);
        x += (int)strlen(label) * f_top->Width + f_top->Width;

        if (homematic_values[i].valid && homematic_values[i].type == HM_TYPE_BOOL) {
            epaper_draw_toggle(x, y, f_top, homematic_values[i].bval);
            x += (f_top->Height - 6) * 2 + f_top->Width;
        } else {
            Paint_DrawString_EN(x, y, val, (sFONT *)f_top, WHITE, BLACK);
            x += (int)strlen(val) * f_top->Width;
        }

        const char *unit = homematic_values[i].unit[0]
                               ? homematic_values[i].unit
                               : derive_unit_for_key(homematic_config_flash.data.items[i].key);
        if (unit && unit[0] &&
            !(homematic_values[i].valid && homematic_values[i].type == HM_TYPE_BOOL)) {
            if (strcmp(unit, "degC") == 0 || strcmp(unit, "\xc2\xb0"
                                                          "C") == 0) {
                int r = (f_top->Width >= 12) ? 3 : 2;
                int cx = x + r + 1;
                int cy = y + r + 1;
                Paint_DrawCircle(cx, cy, r, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
                char cstr[2] = {'C', '\0'};
                Paint_DrawString_EN(cx + r + 1, y, cstr, (sFONT *)f_top, WHITE, BLACK);
            } else {
                char space_unit[16];
                snprintf(space_unit, sizeof(space_unit), " %s", unit);
                Paint_DrawString_EN(x, y, space_unit, (sFONT *)f_top, WHITE, BLACK);
            }
        }

        y += is_epaper_75 ? 34 : 30;
    }

    if (homematic_service_count > 0) {
        const sFONT *small = &font_ubuntu_mono_6pt;
        int display_count = homematic_service_count;
        int by = Paint.Height - 10 - (display_count * (small->Height + 2));
        by -= 10;
        if (by < y + 10) {
            by = y + 10;
        }
        Paint_DrawString_EN(40, by - (small->Height + 4), "Servicemeldungen:", (sFONT *)small,
                            WHITE, BLACK);
        for (int i = 0; i < display_count; i++) {
            char short_addr[16];
            summarize_addr(homematic_service_addr[i], short_addr, sizeof(short_addr));

            char line[160];
            snprintf(line, sizeof(line), "%s - %s",
                     short_addr[0] ? short_addr : homematic_service_addr[i],
                     homematic_service_msgs[i]);

            int max_cols = (Paint.Width - 80) / small->Width;
            int len = (int)strlen(line);
            if (len > max_cols && max_cols > 3) {
                char tmp[160];
                int cut = max_cols - 3;
                if (cut > (int)sizeof(tmp) - 4) {
                    cut = (int)sizeof(tmp) - 4;
                }
                memcpy(tmp, line, cut);
                memcpy(tmp + cut, "...", 3);
                tmp[cut + 3] = 0;
                strcpy(line, tmp);
            }

            Paint_DrawString_EN(40, by + i * (small->Height + 2), line, (sFONT *)small, WHITE,
                                BLACK);
        }
    }
    epaper_draw_firmware_info();
}

void render_page_homematic_placeholder(uint8_t *image_buffer) {
    render_page_placeholder(image_buffer, "tinta-homematic");
}
