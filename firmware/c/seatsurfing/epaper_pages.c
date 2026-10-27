#include "seatsurfing/epaper_pages.h"
#include "DEV_Config.h"
#include "ImageResources.h"
#include "config.h"
#include "device_config.h"
#include "epaper_layout.h"
#include "epaper_render.h"
#include "fonts.h"
#include "rtc.h"
#include "seatsurfing/client.h"
#include "seatsurfing/seatsurfing_flash.h"
#include <stdio.h>
#include <string.h>

// --- SeatSurfing page 0 layout arrays ---

// 7.5" office, 1 seat: [0]=roomname [1]=status [2]=desk [3]=timestamp
static const layout_elem_t ss_office_75_1seat[] = {
    {ELEM_VAR, 40, 50, &font_ubuntu_mono_28pt_bold, .var_index = 0},
    {ELEM_LOGO, 680, 20, .image = &inki_octopus_100_95},
    {ELEM_VAR, 40, 150, &font_ubuntu_mono_20pt_bold, .var_index = 1},
    {ELEM_VAR, 40, 220, &font_ubuntu_mono_14pt, .var_index = 2},
    {ELEM_VAR, 30, 440, &font_ubuntu_mono_9pt, .var_index = 3},
};

// 7.5" office, 2 seats: [0]=roomname [1]=s1_status [2]=s1_desk [3]=s0_status [4]=s0_desk
// [5]=timestamp
static const layout_elem_t ss_office_75_2seat[] = {
    {ELEM_VAR, 40, 50, &font_ubuntu_mono_28pt_bold, .var_index = 0},
    {ELEM_LOGO, 680, 20, .image = &inki_octopus_100_95},
    {ELEM_VLINE, 380, 130, .end_coord = 260},
    {ELEM_VAR, 40, 150, &font_ubuntu_mono_20pt_bold, .var_index = 1},
    {ELEM_VAR, 40, 220, &font_ubuntu_mono_14pt, .var_index = 2},
    {ELEM_VAR, 400, 150, &font_ubuntu_mono_20pt_bold, .var_index = 3},
    {ELEM_VAR, 400, 220, &font_ubuntu_mono_14pt, .var_index = 4},
    {ELEM_VAR, 30, 440, &font_ubuntu_mono_9pt, .var_index = 5},
};

// 7.5" office, 3 seats: [0]=roomname [1]=s2_status [2]=s2_desk [3]=s1_status
//                        [4]=s1_desk [5]=s0_status [6]=s0_desk [7]=timestamp
static const layout_elem_t ss_office_75_3seat[] = {
    {ELEM_VAR, 40, 50, &font_ubuntu_mono_28pt_bold, .var_index = 0},
    {ELEM_LOGO, 680, 20, .image = &inki_octopus_100_95},
    {ELEM_VLINE, 380, 130, .end_coord = 260},
    {ELEM_VAR, 40, 150, &font_ubuntu_mono_20pt_bold, .var_index = 1},
    {ELEM_VAR, 40, 220, &font_ubuntu_mono_14pt, .var_index = 2},
    {ELEM_VAR, 400, 150, &font_ubuntu_mono_20pt_bold, .var_index = 3},
    {ELEM_VAR, 400, 220, &font_ubuntu_mono_14pt, .var_index = 4},
    {ELEM_VAR, 400, 295, &font_ubuntu_mono_20pt_bold, .var_index = 5},
    {ELEM_VAR, 400, 365, &font_ubuntu_mono_14pt, .var_index = 6},
    {ELEM_VAR, 30, 440, &font_ubuntu_mono_9pt, .var_index = 7},
};

// 7.5" conference: [0]=roomname
static const layout_elem_t ss_conference_75[] = {
    {ELEM_VAR, 70, 60, &font_ubuntu_mono_28pt_bold, .var_index = 0},
};

// 4.2" office (1 seat): [0]=roomname [1]=status [2]=desk
static const layout_elem_t ss_office_42[] = {
    {ELEM_VAR, 20, 40, &font_ubuntu_mono_11pt_bold, .var_index = 0},
    {ELEM_LOGO, 290, 10, .image = &inki_octopus_100_95},
    {ELEM_VAR, 20, 150, &font_ubuntu_mono_14pt_bold, .var_index = 1},
    {ELEM_VAR, 40, 220, &font_ubuntu_mono_14pt, .var_index = 2},
};

// --- Device-mode claim page layout arrays: [0]=claim code [1]=device name ---

static const layout_elem_t ss_claim_75[] = {
    {ELEM_TEXT, 40, 40, &font_ubuntu_mono_20pt_bold, .text = "Seatsurfing setup"},
    {ELEM_TEXT, 40, 120, &font_ubuntu_mono_14pt, .text = "Enter this code under"},
    {ELEM_TEXT, 40, 160, &font_ubuntu_mono_14pt, .text = "Plus Features > Manage devices:"},
    {ELEM_VAR, 232, 241, &font_ubuntu_mono_28pt_bold, .var_index = 0},
    {ELEM_TEXT, 40, 345, &font_ubuntu_mono_14pt, .text = "Then press the left button."},
    {ELEM_VAR, 40, 410, &font_ubuntu_mono_11pt, .var_index = 1},
};

static const layout_elem_t ss_claim_42[] = {
    {ELEM_TEXT, 20, 20, &font_ubuntu_mono_11pt_bold, .text = "Seatsurfing setup"},
    {ELEM_TEXT, 20, 70, &font_ubuntu_mono_9pt, .text = "Enter this code under"},
    {ELEM_TEXT, 20, 96, &font_ubuntu_mono_9pt, .text = "Plus Features > Devices:"},
    {ELEM_VAR, 64, 137, &font_ubuntu_mono_22pt_bold, .var_index = 0},
    {ELEM_TEXT, 20, 205, &font_ubuntu_mono_9pt, .text = "Then press the left button."},
    {ELEM_VAR, 20, 240, &font_ubuntu_mono_9pt, .var_index = 1},
};

void render_page_ss_claim(uint8_t *image_buffer, device_caps_t caps, const char *claim_code,
                          const char *device_name) {
    const char *vars[LAYOUT_MAX_VARS] = {claim_code, device_name};
    if (caps.epaper_width >= 800)
        epaper_render_layout(image_buffer, ss_claim_75,
                             sizeof(ss_claim_75) / sizeof(ss_claim_75[0]), vars);
    else
        epaper_render_layout(image_buffer, ss_claim_42,
                             sizeof(ss_claim_42) / sizeof(ss_claim_42[0]), vars);
    epaper_draw_firmware_info();
}

void render_page_seatsurfing(uint8_t *image_buffer) {

    const layout_elem_t *layout = NULL;
    int layout_count = 0;
    const char *vars[LAYOUT_MAX_VARS] = {0};
    char s0[64], s1[64], s2[64], timebuf[64];

    if (device_config_flash.data.type == ROOM_TYPE_OFFICE &&
        device_config_flash.data.number_of_seats >= 1 &&
        device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2) {

        uint8_t seat_count = device_config_flash.data.number_of_seats;
        if (seat_count > 3)
            seat_count = 3;

        const size_t max_name_chars = 12;
        ss_format_seat(&seatsurfing_data[0], s0, sizeof(s0), max_name_chars);
        ss_format_seat(&seatsurfing_data[1], s1, sizeof(s1), max_name_chars);
        ss_format_seat(&seatsurfing_data[2], s2, sizeof(s2), max_name_chars);

        rtc_time_t rtc_data = {0};
        rtc_read_time(&rtc_data);
        int display_hour = rtc_data.hours;
        if (rtc_is_dst_europe(&rtc_data)) {
            display_hour += 1;
            if (display_hour >= 24)
                display_hour -= 24;
        }
        const char *month_full = rtc_month_name(rtc_data.month);
        const char month_abbr[4] = {month_full[0], month_full[1], month_full[2], '\0'};
        snprintf(timebuf, sizeof(timebuf), "%s, %02d. %s %04d, %02d:%02d",
                 rtc_day_name(rtc_data.day), rtc_data.date, month_abbr, 2000 + rtc_data.year,
                 display_hour, rtc_data.minutes);

        if (seat_count == 1) {
            layout = ss_office_75_1seat;
            layout_count = sizeof(ss_office_75_1seat) / sizeof(ss_office_75_1seat[0]);
            vars[0] = device_config_flash.data.roomname;
            vars[1] = s0;
            vars[2] = seatsurfing_data[0].desk_name;
            vars[3] = timebuf;
        } else if (seat_count == 2) {
            layout = ss_office_75_2seat;
            layout_count = sizeof(ss_office_75_2seat) / sizeof(ss_office_75_2seat[0]);
            vars[0] = device_config_flash.data.roomname;
            vars[1] = s1;
            vars[2] = seatsurfing_data[1].desk_name;
            vars[3] = s0;
            vars[4] = seatsurfing_data[0].desk_name;
            vars[5] = timebuf;
        } else {
            layout = ss_office_75_3seat;
            layout_count = sizeof(ss_office_75_3seat) / sizeof(ss_office_75_3seat[0]);
            vars[0] = device_config_flash.data.roomname;
            vars[1] = s2;
            vars[2] = seatsurfing_data[2].desk_name;
            vars[3] = s1;
            vars[4] = seatsurfing_data[1].desk_name;
            vars[5] = s0;
            vars[6] = seatsurfing_data[0].desk_name;
            vars[7] = timebuf;
        }
    } else if (device_config_flash.data.type == ROOM_TYPE_CONFERENCE &&
               device_config_flash.data.epapertype == EPAPER_WAVESHARE_7IN5_V2) {

        layout = ss_conference_75;
        layout_count = sizeof(ss_conference_75) / sizeof(ss_conference_75[0]);
        vars[0] = device_config_flash.data.roomname;

    } else if (device_config_flash.data.type == ROOM_TYPE_OFFICE &&
               device_config_flash.data.number_of_seats >= 1 &&
               device_config_flash.data.epapertype == EPAPER_WAVESHARE_4IN2_V2) {

        ss_format_seat(&seatsurfing_data[0], s0, sizeof(s0), 17);

        layout = ss_office_42;
        layout_count = sizeof(ss_office_42) / sizeof(ss_office_42[0]);
        vars[0] = device_config_flash.data.roomname;
        vars[1] = s0;
        vars[2] = seatsurfing_data[0].desk_name;
    }

    if (layout) {
        epaper_render_layout(image_buffer, layout, layout_count, vars);
    }
    epaper_draw_firmware_info();
}
