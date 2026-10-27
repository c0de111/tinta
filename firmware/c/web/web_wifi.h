#ifndef WEB_WIFI_H
#define WEB_WIFI_H

#include <stddef.h>

void build_wifi_config_page(char *buf, size_t sz, const char *message);
// Appends the network list and the name, password and region inputs, under the given form
// field names.
int web_wifi_credential_fields(char *buf, size_t sz, int n, const char *ssid_field,
                               const char *password_field, const char *region_field);
void handle_form_wifi(const char *body, size_t len, char *buf, size_t sz);

#endif // WEB_WIFI_H
