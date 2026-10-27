#ifndef WIFI_H
#define WIFI_H

#include "lwip/ip_addr.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_SUCCESS = 0,
    WIFI_ERROR_CONNECTION = 1,
} wifi_result_t;

// Return a pointer to the device's 6-byte STA MAC. Cached after first read.
// wifi_connect() and wifi_ap_start() populate the cache for free (chip already
// up); if neither has run this wake, the first call cold-inits CYW43 (~1 s).
const uint8_t *wifi_mac(void);

// Read current link RSSI in dBm. Returns false if the link is down.
bool wifi_get_rssi(int32_t *out_dbm);

// Background poll (no-op on threadsafe_background, kept for call-site clarity).
void wifi_poll(void);

// Connect to Wi-Fi using flash-stored configuration.
// Returns a `wifi_result_t`; on success, station mode is enabled and connected.
wifi_result_t wifi_connect(void);

// Simple helper to log current Wi‑Fi RSSI (dBm) if connected; otherwise logs N/A.
void wifi_log_rssi(void);

// Radio power save while connected. wifi_connect() leaves it off, because it slows every
// exchange; switch it on for stretches that are mostly waiting for a server.
void wifi_set_power_save(bool on);

// Shut down the Wi-Fi subsystem.
void wifi_deinit(void);

// True once a home network has been entered. A sign without one is still as delivered and
// has nothing to connect to.
bool wifi_has_network(void);

// Name of the sign in the home network (DHCP hostname): "tinta-" and the last four hex digits
// of the MAC, the setup network's name in lower case.
const char *wifi_hostname(void);

// Name of a wifi_region_t in the settings file ("europe"), and back; -1 if unknown.
const char *wifi_region_key(int region);
int wifi_region_from_key(const char *key);

// Name and password of this sign's own setup network, the same on every start.
// The name ends in the last four hex digits of the MAC. The password is derived from the
// board's unique ID: unlike the MAC that ID is never sent over the air, and nothing has to
// be stored, so erasing the whole flash does not change it.
const char *wifi_setup_ssid(void);
const char *wifi_setup_password(void);

// Initialise CYW43 and open the setup network (WPA2-PSK). Caller must not have
// initialised the chip already.
bool wifi_ap_start(void);

// Networks the sign saw just before its setup network opened, strongest first. Hidden
// networks are left out; only 2.4 GHz is visible to the chip.
typedef struct {
    char ssid[33];
    int16_t rssi;
} wifi_network_t;
int wifi_found_networks(const wifi_network_t **list);

// Minutes until the next wake while joins keep failing; 0 if the last join worked (then the
// page's own interval applies).
int wifi_backoff_minutes(void);

// Number of devices currently joined to the setup network.
int wifi_ap_client_count(void);

// Resolve hostname to IPv4 address using lwIP DNS (synchronous, polling).
// Must be called after wifi_connect(). Returns true on success.
bool wifi_resolve_hostname(const char *hostname, ip_addr_t *out_ip, int timeout_ms);

#endif // WIFI_H
