#include "historian/epaper_pages.h"
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
#include "historian/client.h"
#include "historian/historian_flash.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void render_temperature_graph(uint8_t *image_buffer, int x, int y, int width, int height) {
    if (historian_data.count == 0) {
        Paint_DrawString_EN(x + 10, y + height / 2, "Loading data...", &font_ubuntu_mono_10pt,
                            WHITE, BLACK);
        return;
    }

    // Draw frame
    Paint_DrawRectangle(x, y, x + width, y + height, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

    // Draw horizontal grid lines
    for (int i = 1; i < 4; i++) {
        int grid_y = y + (height * i) / 4;
        Paint_DrawLine(x + 1, grid_y, x + width - 1, grid_y, BLACK, DOT_PIXEL_1X1,
                       LINE_STYLE_DOTTED);
    }

    // Draw vertical grid lines (10 Ticks)
    int tick_spacing = width / 10;

    // Time range in milliseconds
    uint64_t time_range_ms = historian_data.points[historian_data.count - 1].timestamp -
                             historian_data.points[0].timestamp;

    for (int i = 0; i <= 10; i++) {
        int tick_x = x + (i * tick_spacing);
        // Vertical line
        Paint_DrawLine(tick_x, y + 1, tick_x, y + height - 1, BLACK, DOT_PIXEL_1X1,
                       LINE_STYLE_DOTTED);

        // Time labels for x-axis
        if (i % 2 == 0) { // At 0, 2, 4, 6, 8, 10
            // Interpolate timestamp for this position
            uint64_t tick_time_ms = historian_data.points[0].timestamp + (time_range_ms * i) / 10;

            // Convert to seconds for time_t
            time_t tick_time_sec = tick_time_ms / 1000;

            // UTC to local time (MEZ/MESZ)
            const struct tm *tick_tm = gmtime(&tick_time_sec);

            // Determine if DST applies for this date
            bool is_dst = false;
            if (tick_tm->tm_mon >= 2 && tick_tm->tm_mon <= 9) { // March to October
                // Simplified: April-September = definitely DST
                if (tick_tm->tm_mon >= 3 && tick_tm->tm_mon <= 8) {
                    is_dst = true;
                }
            }

            // UTC -> MEZ/MESZ
            tick_time_sec += 3600; // +1h for MEZ
            if (is_dst) {
                tick_time_sec += 3600; // +1h additional for MESZ
            }

            // Convert again with adjusted time
            tick_tm = gmtime(&tick_time_sec);

            char time_label[8];
            snprintf(time_label, sizeof(time_label), "%02d:%02d", tick_tm->tm_hour,
                     tick_tm->tm_min);

            // Time below x-axis
            Paint_DrawString_EN(tick_x - 15, y + height + 5, time_label, &font_ubuntu_mono_6pt,
                                WHITE, BLACK);
        }
    }

    // Calculate scale
    float temp_range = historian_data.max_value - historian_data.min_value;
    if (temp_range < 0.1f)
        temp_range = 0.1f;

    // Draw temperature curve (with thicker line)
    int prev_x = -1, prev_y = -1;
    uint64_t start_time_ms = historian_data.points[0].timestamp;
    uint64_t end_time_ms = historian_data.points[historian_data.count - 1].timestamp;
    uint64_t total_time_range_ms = end_time_ms - start_time_ms;

    for (int i = 0; i < historian_data.count; i++) {
        // X-position based on actual timestamp
        uint64_t time_offset_ms = historian_data.points[i].timestamp - start_time_ms;
        int point_x = x + (time_offset_ms * width) / total_time_range_ms;

        float normalized = (historian_data.points[i].value - historian_data.min_value) / temp_range;
        int point_y = y + height - (int)(normalized * height) - 1;

        // Clamp to graph bounds
        if (point_y < y)
            point_y = y;
        if (point_y > y + height)
            point_y = y + height;

        if (prev_x >= 0) {
            // Thicker line for better visibility
            Paint_DrawLine(prev_x, prev_y, point_x, point_y, BLACK, DOT_PIXEL_3X3,
                           LINE_STYLE_SOLID);
        }

        // Draw points for better visibility
        if (historian_data.count < 50) { // Only if not too many points
            Paint_DrawCircle(point_x, point_y, 2, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        }

        prev_x = point_x;
        prev_y = point_y;
    }

    // Labels without degree symbol (not displayable)
    char label[32];

    // Min temperature (bottom left)
    snprintf(label, sizeof(label), "Min: %.1f C", historian_data.min_value);
    Paint_DrawString_EN(x + 5, y + height - 40, label, &font_ubuntu_mono_9pt, WHITE, BLACK);

    // Max temperature (top left)
    snprintf(label, sizeof(label), "Max: %.1f C", historian_data.max_value);
    Paint_DrawString_EN(x + 5, y + 30, label, &font_ubuntu_mono_9pt, WHITE, BLACK);

    // Current temperature (right side)
    snprintf(label, sizeof(label), "Now: %.1f C", historian_data.last_value);
    Paint_DrawString_EN(x + width - 240, y + height - 40, label, &font_ubuntu_mono_12pt_bold, WHITE,
                        BLACK);

    // Title with actual time range from data
    time_t start_sec = historian_data.points[0].timestamp / 1000;
    time_t end_sec = historian_data.points[historian_data.count - 1].timestamp / 1000;

    // UTC -> Local time for display
    bool is_dst_now = true;                      // August = MESZ
    start_sec += 3600 + (is_dst_now ? 3600 : 0); // MEZ/MESZ
    end_sec += 3600 + (is_dst_now ? 3600 : 0);   // MEZ/MESZ

    const struct tm *start_tm = gmtime(&start_sec);
    const struct tm *end_tm = gmtime(&end_sec);

    char title[64];
    snprintf(title, sizeof(title), "Temperature (%02d:%02d - %02d:%02d %s)", start_tm->tm_hour,
             start_tm->tm_min, end_tm->tm_hour, end_tm->tm_min, is_dst_now ? "MESZ" : "MEZ");
    Paint_DrawString_EN(x, y - 20, title, &font_ubuntu_mono_8pt, WHITE, BLACK);
}

void render_page_historian(uint8_t *image_buffer) {
    if (device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2) {
        render_temperature_graph(image_buffer, 20, 20, 760, 400);

        char info[128];
        snprintf(info, sizeof(info), "%d data points", historian_data.count);
        Paint_DrawString_EN(50, 450, info, &font_ubuntu_mono_8pt, WHITE, BLACK);
        int logo_x = epaper_get_width() - inki_octopus_100_95.width - 10;
        if (logo_x < 0) {
            logo_x = 0;
        }
        if (!epaper_draw_custom_logo(image_buffer, logo_x, 10)) {
            epaper_draw_subimage(image_buffer, &inki_octopus_100_95, logo_x, 10);
        }

    } else if (device_config_flash.data.epapertype == EPAPER_WAVESHARE_4IN2_V2) {
        render_temperature_graph(image_buffer, 10, 10, 380, 250);

        char info[64];
        snprintf(info, sizeof(info), "%d points", historian_data.count);
        Paint_DrawString_EN(20, 280, info, &font_ubuntu_mono_8pt, WHITE, BLACK);
        int logo_x = epaper_get_width() - inki_octopus_100_95.width - 10;
        if (logo_x < 0) {
            logo_x = 0;
        }
        if (!epaper_draw_custom_logo(image_buffer, logo_x, 10)) {
            epaper_draw_subimage(image_buffer, &inki_octopus_100_95, logo_x, 10);
        }
    } else {
        render_page_fallback(0, image_buffer);
    }
    epaper_draw_firmware_info();
}

void render_page_historian_placeholder(uint8_t *image_buffer) {
    render_page_placeholder(image_buffer, "tinta-historian");
}
