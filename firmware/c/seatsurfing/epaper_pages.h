#ifndef SEATSURFING_EPAPER_PAGES_H
#define SEATSURFING_EPAPER_PAGES_H

#include "DEV_Config.h"
#include "device_caps.h"

void render_page_seatsurfing(uint8_t *image_buffer);

// Device mode: claim code for the admin to enter in Seatsurfing.
void render_page_ss_claim(uint8_t *image_buffer, device_caps_t caps, const char *claim_code,
                          const char *device_name);

#endif // SEATSURFING_EPAPER_PAGES_H
