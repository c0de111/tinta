#include "wifi.h"
#include "config.h"
#include "rtc.h"
#define LOG_MODULE LOG_MOD_WIFI
#include "debug.h"
#include "flash.h"
#include "hardware/watchdog.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"
#include "pico/unique_id.h"

static const struct {
    uint32_t code;
    const char *name;
    const char *key;
} regions[WIFI_REGION_COUNT] = {
    [WIFI_REGION_WORLDWIDE] = {CYW43_COUNTRY_WORLDWIDE, "Worldwide (XX)", "worldwide"},
    [WIFI_REGION_EUROPE] = {CYW43_COUNTRY_GERMANY, "Europe (DE)", "europe"}, // any EU code: ETSI
    [WIFI_REGION_USA] = {CYW43_COUNTRY_USA, "USA (US)", "usa"},
    [WIFI_REGION_JAPAN] = {CYW43_COUNTRY_JAPAN, "Japan (JP)", "japan"},
};

const char *wifi_region_key(int region) {
    return regions[region >= 0 && region < WIFI_REGION_COUNT ? region : WIFI_REGION_WORLDWIDE].key;
}

int wifi_region_from_key(const char *key) {
    for (int i = 0; i < WIFI_REGION_COUNT; i++)
        if (strcmp(key, regions[i].key) == 0)
            return i;
    return -1;
}
// WPA2 with AES (CCMP). The driver treats its "mixed" mode the same (cyw43.h), but this one
// says what happens and keeps TKIP out should a later driver tell them apart. WPA3 is ID 203.
static uint32_t auth = CYW43_AUTH_WPA2_AES_PSK;

static uint64_t s_cyw43_on_us = 0;
static uint8_t s_mac[6] = {0};
static bool s_mac_valid = false;

// Powers the chip with the radio rules of the configured region and says which in the log;
// mode is "" for the client, " (setup network)" for the access point.
static bool wifi_chip_on(const char *mode) {
    const wifi_region_t r = wifi_config_region(&wifi_config_flash);
    if (cyw43_arch_init_with_country(regions[r].code)) {
        debug_status("ERROR", "CYW43: init failed%s\n", mode);
        return false;
    }
    debug_status("OK", "CYW43: chip on%s, region %s\n", mode, regions[r].name);
    return true;
}

// Read MAC from the live chip and cache it. Safe to call with chip up in
// either STA or AP mode — both MACs exist in the CYW43 regardless of mode.
static void wifi_capture_mac(void) {
    if (s_mac_valid)
        return;
    cyw43_arch_lwip_begin();
    int rc = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, s_mac);
    cyw43_arch_lwip_end();
    if (rc == 0) {
        s_mac_valid = true;
        debug_status("OK", "MAC: %02X:%02X:%02X:%02X:%02X:%02X (%s)\n", s_mac[0], s_mac[1],
                     s_mac[2], s_mac[3], s_mac[4], s_mac[5], wifi_setup_ssid());
    } else {
        debug_status("ERROR", "WiFi: MAC read failed\n");
    }
}

static const char *link_name(int status) {
    switch (status) {
    case CYW43_LINK_DOWN:
        return "down";
    case CYW43_LINK_JOIN:
        return "joining";
    case CYW43_LINK_NOIP:
        return "associated, no IP";
    case CYW43_LINK_UP:
        return "up";
    case CYW43_LINK_FAIL:
        return "failed";
    case CYW43_LINK_NONET:
        return "network not found";
    case CYW43_LINK_BADAUTH:
        return "auth rejected";
    default:
        return "?";
    }
}

static const char *dhcp_name(int state) {
    switch (state) {
    case DHCP_STATE_OFF:
        return "off";
    case DHCP_STATE_INIT:
        return "init";
    case DHCP_STATE_SELECTING:
        return "discover sent";
    case DHCP_STATE_REQUESTING:
        return "request sent";
    case DHCP_STATE_REBOOTING:
        return "rebooting";
    case DHCP_STATE_BACKING_OFF:
        return "backing off";
    case DHCP_STATE_BOUND:
        return "bound";
    default:
        return "other";
    }
}

// --- EXPERIMENT: precomputed WPA2 key (build.sh --exp-wifi-pmk, off by default) ---------
//
// The radio derives the WPA2 key from the passphrase on every join (PBKDF2, 4096 rounds),
// which costs about 2 s. Here the key is derived once, kept in flash (load_wifi_pmk) and
// passed as 64 hex characters, which WPA treats as the finished key. It is derived again
// only when SSID or passphrase change. Access points refuse a correct key now and then, so
// a network falls back to the passphrase only after refusing the key on three wakes in a row.
#if INKI_EXP_WIFI_PMK
static char s_pmk_hex[65];
static bool s_pmk_ready = false;

static bool pbkdf2_wpa2(const char *passphrase, const char *ssid, uint8_t pmk[32]) {
    const mbedtls_md_info_t *sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_context_t hmac;
    mbedtls_md_init(&hmac);
    if (!sha1 || mbedtls_md_setup(&hmac, sha1, 1) != 0 ||
        mbedtls_md_hmac_starts(&hmac, (const uint8_t *)passphrase, strlen(passphrase)) != 0) {
        mbedtls_md_free(&hmac);
        return false;
    }

    uint8_t out[40];
    for (uint8_t block = 1; block <= 2; block++) {
        uint8_t u[20];
        const uint8_t index[4] = {0, 0, 0, block};
        mbedtls_md_hmac_update(&hmac, (const uint8_t *)ssid, strlen(ssid));
        mbedtls_md_hmac_update(&hmac, index, sizeof(index));
        mbedtls_md_hmac_finish(&hmac, u);
        mbedtls_md_hmac_reset(&hmac);
        uint8_t *t = out + (block - 1) * 20;
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            mbedtls_md_hmac_update(&hmac, u, 20);
            mbedtls_md_hmac_finish(&hmac, u);
            mbedtls_md_hmac_reset(&hmac);
            for (int k = 0; k < 20; k++)
                t[k] ^= u[k];
            if ((i & 0xFF) == 0)
                watchdog_update();
        }
    }
    mbedtls_md_free(&hmac);
    memcpy(pmk, out, 32);
    return true;
}

static uint8_t s_pmk[32];
static uint32_t s_pmk_strikes;  // from flash: earlier wakes in which the key was refused
static int s_pmk_failed_joins;  // this wake: failed joins with the key
static int s_pmk_refused_joins; // of those, refused authentication
#define WIFI_PMK_TRIES_PER_WAKE 2

// The key comes from flash; it is derived (about 1 s on the Pico) only when the stored one
// does not belong to the current SSID and passphrase.
static void wifi_pmk_prepare(void) {
    const size_t len = strlen(wifi_config_flash.password);
    s_pmk_ready = false;
    s_pmk_failed_joins = 0;
    s_pmk_refused_joins = 0;
    if (len < 8 || len > 63) // open network, or already a 64-character key
        return;

    switch (load_wifi_pmk(s_pmk, &s_pmk_strikes)) {
    case WIFI_PMK_REJECTED:
        debug_status("INFO", "WiFi: stored key was refused by this network, using passphrase\n");
        return;
    case WIFI_PMK_VALID:
        debug_status("INFO", "WiFi: key from flash\n");
        break;
    default: {
        uint64_t t0 = time_us_64();
        if (!pbkdf2_wpa2(wifi_config_flash.password, wifi_config_flash.ssid, s_pmk)) {
            debug_status("WARN", "WiFi: key derivation failed, using passphrase\n");
            return;
        }
        save_wifi_pmk(s_pmk, 0);
        debug_status("INFO", "WiFi: key derived in %lu ms and stored\n",
                     (unsigned long)((time_us_64() - t0) / 1000));
        break;
    }
    }
    for (int i = 0; i < 32; i++)
        snprintf(s_pmk_hex + 2 * i, 3, "%02x", s_pmk[i]);
    s_pmk_ready = true;
}

static const char *wifi_join_key(void) {
    return s_pmk_ready ? s_pmk_hex : wifi_config_flash.password;
}

// A scanned join with the key failed. A second try with the key costs a fraction of what the
// passphrase costs, so the key gets WIFI_PMK_TRIES_PER_WAKE tries before the passphrase.
static void wifi_pmk_give_up(int rc) {
    if (!s_pmk_ready)
        return;
    s_pmk_failed_joins++;
    if (rc == PICO_ERROR_BADAUTH)
        s_pmk_refused_joins++;
    if (s_pmk_failed_joins < WIFI_PMK_TRIES_PER_WAKE) {
        debug_status("WARN", "WiFi: join with the stored key failed (rc=%d), trying it again\n",
                     rc);
        return;
    }
    s_pmk_ready = false;
    debug_status("WARN", "WiFi: join with the stored key failed (rc=%d), back to passphrase\n", rc);
}

// Bookkeeping after a successful join. With the key: any strikes are cleared. With the
// passphrase after the key was refused on every try of this wake: one strike more.
static void wifi_pmk_joined(void) {
    if (s_pmk_ready) {
        if (s_pmk_strikes > 0)
            save_wifi_pmk(s_pmk, 0);
        return;
    }
    if (s_pmk_refused_joins < WIFI_PMK_TRIES_PER_WAKE)
        return;
    save_wifi_pmk(s_pmk, s_pmk_strikes + 1);
    debug_status("WARN", "WiFi: key refused, passphrase worked (strike %lu of %d)\n",
                 (unsigned long)(s_pmk_strikes + 1), WIFI_PMK_MAX_STRIKES);
}
#else
static void wifi_pmk_prepare(void) {}
static const char *wifi_join_key(void) { return wifi_config_flash.password; }
static void wifi_pmk_give_up(int rc) { (void)rc; }
static void wifi_pmk_joined(void) {}
#endif

// How long the chip stays on after leaving, so the disassociation frame is on the air before
// the power goes.
#define WIFI_LEAVE_SETTLE_MS 20

// Leaves the network if the radio is associated (with or without an IP address), so the access
// point drops the sign at once instead of at its idle timeout. An access point that still
// counts the sign as joined turns away the next direct join (AP hint).
static void wifi_leave_if_joined(void) {
    if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_JOIN)
        return;
    const int rc = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    sleep_ms(WIFI_LEAVE_SETTLE_MS);
    dlog("WiFi: left the network (rc=%d)\n", rc);
}

// A hinted join goes straight to one access point on one channel instead of scanning for
// the network name. If that access point does not answer in time, the caller scans as usual.
#define WIFI_HINT_TIMEOUT_MS 3000
// The discover sent in the instant of association is often lost. lwIP would repeat it on its
// 500 ms raster; repeat it at 100, 200 and 300 ms after association instead.
#define DHCP_EARLY_RETRY_MS 100
#define DHCP_EARLY_RETRIES 3
// Below this a hint is not kept: the next wake should look for a better access point.
#define WIFI_HINT_MIN_RSSI_DBM (-70)

// A scan that reports the network missing is repeated at once, since a scan misses a beacon
// now and then. Five in a row mean it is not there.
#define WIFI_NONET_SCANS_MAX 5

// After failed joins the sign wakes sooner first and then ever later: a router restart is over
// soon, a router switched off for the night or a holiday is not. First wifi_reconnect_minutes,
// doubled with every further failure, at most this. A button or a tap still wakes it at once.
#define WIFI_BACKOFF_MAX_MIN 120
static int s_nonet_scans; // consecutive "not found" scans in this wake

// --- EXPERIMENT: ask for the last address again (build.sh --exp-dhcp-reboot, off by default) ---
//
// RFC 2131, 3.2: a client that still knows its address should ask for it with a REQUEST
// (INIT-REBOOT) instead of starting over with DISCOVER. Some routers answer a DISCOVER only
// after a second or more, the same on every wake; a router that probes an address before
// offering it would explain that, and INIT-REBOOT has no offer to probe.
// lwIP has the REBOOTING state but no way to enter it with the address of an earlier power
// cycle: the address is put in by hand and the state set to BOUND, then lwIP's link-up handler
// reboots. A NAK sends lwIP back to DISCOVER by itself; silence is cut short after
// DHCP_REBOOT_WAIT_MS.
#define DHCP_REBOOT_WAIT_MS 300

static int wifi_start_join(const uint8_t *hint_bssid, uint8_t hint_channel) {
    const char *key = wifi_join_key();
    if (!hint_bssid)
        return cyw43_arch_wifi_connect_async(wifi_config_flash.ssid, key, auth);
    return cyw43_wifi_join(&cyw43_state, strlen(wifi_config_flash.ssid),
                           (const uint8_t *)wifi_config_flash.ssid, strlen(key),
                           (const uint8_t *)key, auth, hint_bssid, hint_channel);
}

// One join attempt. Feeds the watchdog while waiting, so the timeout may exceed
// the watchdog period, and logs every link and DHCP state change with its time.
static int wifi_join_once(int attempt, int timeout_ms, const uint8_t *hint_bssid,
                          uint8_t hint_channel, const uint8_t *reboot_ip) {
    uint64_t t0 = time_us_64();
    int err = wifi_start_join(hint_bssid, hint_channel);
    if (err) {
        debug_status("ERROR", "WiFi #%d: driver refused join (rc=%d)\n", attempt, err);
        return err;
    }

    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    int link = INT32_MIN;
    int dhcp_state = -1;
    int dhcp_tries = -1;
    bool bssid_logged = false;
    uint32_t join_state = 0;
    bool associated = false;
    uint32_t associated_ms = 0;
    int early_retries = 0;
    uint32_t reboot_ms = 0;
    bool reboot_sent = false, reboot_given_up = false;
#if !INKI_EXP_DHCP_REBOOT
    reboot_ip = NULL;
#endif

    for (;;) {
        watchdog_update();
        uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);

        cyw43_arch_lwip_begin();
        int new_link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        struct dhcp *dhcp = netif_dhcp_data(netif);
        int new_dhcp_state = dhcp ? dhcp->state : -1;
        int new_dhcp_tries = dhcp ? dhcp->tries : -1;
        cyw43_arch_lwip_end();

        // Driver join progress (cyw43_ctrl.c): 0x200 authenticated, 0x400 link, 0x800 keyed.
        // Shows where the time before "associated" goes: before the first frame or after it.
        uint32_t new_join_state = cyw43_state.wifi_join_state;
        if (new_join_state != join_state) {
            join_state = new_join_state;
            debug_status("INFO", "WiFi #%d +%lu ms: join 0x%03lX%s%s%s\n", attempt,
                         (unsigned long)ms, (unsigned long)join_state,
                         join_state & 0x200 ? " auth" : "", join_state & 0x400 ? " link" : "",
                         join_state & 0x800 ? " keyed" : "");
        }
        if (new_link != link) {
            link = new_link;
            debug_status("INFO", "WiFi #%d +%lu ms: link %s\n", attempt, (unsigned long)ms,
                         link_name(link));
        }
        if (new_dhcp_state != dhcp_state || new_dhcp_tries != dhcp_tries) {
            dhcp_state = new_dhcp_state;
            dhcp_tries = new_dhcp_tries;
            if (dhcp_state >= 0)
                debug_status("INFO", "WiFi #%d +%lu ms: DHCP %s (try %d)\n", attempt,
                             (unsigned long)ms, dhcp_name(dhcp_state), dhcp_tries);
        }
        if (!bssid_logged && link >= CYW43_LINK_NOIP) {
            uint8_t bssid[6] = {0};
            cyw43_arch_lwip_begin();
            int rc = cyw43_wifi_get_bssid(&cyw43_state, bssid);
            cyw43_arch_lwip_end();
            if (rc == 0)
                debug_status("INFO", "WiFi #%d +%lu ms: AP %02X:%02X:%02X:%02X:%02X:%02X\n",
                             attempt, (unsigned long)ms, bssid[0], bssid[1], bssid[2], bssid[3],
                             bssid[4], bssid[5]);
            bssid_logged = true;
        }

        if (link == CYW43_LINK_NOIP) {
            if (!associated) {
                associated = true;
                associated_ms = ms;
            }
            if (reboot_ip && !reboot_sent && dhcp_state == DHCP_STATE_SELECTING) {
                reboot_sent = true;
                reboot_ms = ms;
                cyw43_arch_lwip_begin();
                struct dhcp *d = netif_dhcp_data(netif);
                if (d) {
                    memcpy(&d->offered_ip_addr.addr, reboot_ip, 4);
                    d->state = DHCP_STATE_BOUND;
                    dhcp_network_changed_link_up(netif);
                }
                cyw43_arch_lwip_end();
                debug_status("INFO", "WiFi #%d +%lu ms: DHCP asks for %u.%u.%u.%u again\n", attempt,
                             (unsigned long)ms, reboot_ip[0], reboot_ip[1], reboot_ip[2],
                             reboot_ip[3]);
            } else if (reboot_sent && !reboot_given_up && dhcp_state == DHCP_STATE_REBOOTING &&
                       ms >= reboot_ms + DHCP_REBOOT_WAIT_MS) {
                reboot_given_up = true;
                cyw43_arch_lwip_begin();
                struct dhcp *d = netif_dhcp_data(netif);
                if (d) {
                    d->state = DHCP_STATE_INIT;
                    dhcp_network_changed_link_up(netif); // INIT: starts a DISCOVER
                }
                cyw43_arch_lwip_end();
                associated_ms = ms; // early retries count from the new discover
                debug_status("WARN",
                             "WiFi #%d +%lu ms: no answer to the old address, asking anew\n",
                             attempt, (unsigned long)ms);
            }
            // dhcp_fine_tmr() runs the pending discover timeout (one tick) right away.
            if (dhcp_state == DHCP_STATE_SELECTING && early_retries < DHCP_EARLY_RETRIES &&
                ms >= associated_ms + (uint32_t)(early_retries + 1) * DHCP_EARLY_RETRY_MS) {
                early_retries++;
                cyw43_arch_lwip_begin();
                dhcp_fine_tmr();
                cyw43_arch_lwip_end();
            }
        }

        if (link >= CYW43_LINK_NOIP)
            s_nonet_scans = 0; // found: a later "not found" starts the count anew
        if (link == CYW43_LINK_UP)
            return PICO_OK;
        if (link == CYW43_LINK_BADAUTH)
            return PICO_ERROR_BADAUTH;
        if (link == CYW43_LINK_FAIL)
            return PICO_ERROR_CONNECT_FAILED;
        if (ms >= (uint32_t)timeout_ms) {
            debug_status("WARN", "WiFi #%d: timeout after %lu ms\n", attempt, (unsigned long)ms);
            return PICO_ERROR_TIMEOUT;
        }
        if (hint_bssid &&
            (link == CYW43_LINK_NONET || (link < CYW43_LINK_NOIP && ms >= WIFI_HINT_TIMEOUT_MS)))
            return PICO_ERROR_TIMEOUT;
        if (link == CYW43_LINK_NONET) {
            if (++s_nonet_scans >= WIFI_NONET_SCANS_MAX)
                return PICO_ERROR_NOT_FOUND;
            err = cyw43_arch_wifi_connect_async(wifi_config_flash.ssid, wifi_join_key(), auth);
            if (err) {
                debug_status("ERROR", "WiFi #%d: driver refused rejoin (rc=%d)\n", attempt, err);
                return err;
            }
        }
        sleep_ms(2);
    }
}

// Remember the access point of this join for the next wake, unless its signal is too weak.
static void wifi_update_hint(bool rssi_valid, int32_t rssi) {
    if (!rssi_valid || rssi < WIFI_HINT_MIN_RSSI_DBM) {
        clear_wifi_hint();
        return;
    }
    uint8_t bssid[6] = {0};
    uint8_t channel_info[12] = {0}; // hw_channel, target_channel, scan_channel (le32 each)
    cyw43_arch_lwip_begin();
    int rc = cyw43_wifi_get_bssid(&cyw43_state, bssid);
    if (rc == 0)
        rc = cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(channel_info), channel_info,
                         CYW43_ITF_STA);
    cyw43_arch_lwip_end();
    // The lease goes with the access point: asked for again on the next wake (if enabled).
    uint8_t ip[4] = {0};
    const ip4_addr_t *addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    if (addr)
        memcpy(ip, &addr->addr, 4);
    if (rc == 0 && channel_info[0] >= 1 && channel_info[0] <= 14)
        save_wifi_hint(bssid, channel_info[0], ip);
}

wifi_result_t wifi_connect(void) {
    dlog("Initializing Wi-Fi...\n");
    wifi_pmk_prepare();

    s_cyw43_on_us = time_us_64();
    if (!wifi_chip_on(""))
        return WIFI_ERROR_CONNECTION;
    cyw43_arch_enable_sta_mode();
    wifi_capture_mac();

    cyw43_arch_lwip_begin();
    int pm_rc = cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    cyw43_arch_lwip_end();
    debug_status(pm_rc == 0 ? "INFO" : "WARN", "WiFi: power save off (rc=%d)\n", pm_rc);

    // The name the router shows and the local DNS knows: "tinta-2915", the setup network's name
    // in lower case. Unique per sign, valid as a hostname (RFC 1123), independent of the room.
    netif_set_hostname(netif_default, wifi_hostname());

    uint64_t t0 = time_us_64();
    int wifi_connected = -1;
    int wifi_attempt_count = 0;
    s_nonet_scans = 0;

    uint8_t hint_bssid[6];
    uint8_t hint_channel;
    uint8_t hint_ip[4];
    if (load_wifi_hint(hint_bssid, &hint_channel, hint_ip)) {
        static const uint8_t no_ip[4] = {0};
        debug_status("INFO", "WiFi: AP hint %02X:%02X:%02X:%02X:%02X:%02X ch %d\n", hint_bssid[0],
                     hint_bssid[1], hint_bssid[2], hint_bssid[3], hint_bssid[4], hint_bssid[5],
                     hint_channel);
        wifi_connected = wifi_join_once(0, device_config_flash.data.wifi_timeout, hint_bssid,
                                        hint_channel, memcmp(hint_ip, no_ip, 4) ? hint_ip : NULL);
        if (wifi_connected != 0) {
            // The hint stays: one miss does not mean the access point is gone, and a join to
            // another one overwrites it. The key is not blamed either, the scan tries it again.
            debug_status("WARN", "WiFi: hint failed (rc=%d), scanning\n", wifi_connected);
            wifi_leave_if_joined();
        }
    }
    while (wifi_connected != 0 && wifi_connected != PICO_ERROR_NOT_FOUND &&
           wifi_attempt_count < device_config_flash.data.number_wifi_attempts) {
        wifi_attempt_count++;
        wifi_connected = wifi_join_once(wifi_attempt_count, device_config_flash.data.wifi_timeout,
                                        NULL, 0, NULL);
        if (wifi_connected != 0) {
            if (wifi_connected != PICO_ERROR_NOT_FOUND)
                wifi_pmk_give_up(wifi_connected);
            wifi_leave_if_joined();
        }
    }
    uint32_t elapsed_ms = (uint32_t)((time_us_64() - t0) / 1000);

    if (wifi_connected == PICO_ERROR_NOT_FOUND)
        debug_status("WARN", "WiFi: network not found in %d scans, giving up for this wake\n",
                     WIFI_NONET_SCANS_MAX);
    if (wifi_connected != 0) {
        debug_status("ERROR", "WiFi: failed after %d attempts (%.1fs)\n", wifi_attempt_count,
                     (float)elapsed_ms / 1000.0f);
        wifi_deinit();
        // Written after the radio is off: no current peaks while the flash is busy.
        save_wifi_failures(load_wifi_failures() + 1);
        debug_status("WARN", "WiFi: failed %d times in a row, next try in %d min\n",
                     load_wifi_failures(), wifi_backoff_minutes());
        return WIFI_ERROR_CONNECTION;
    }

    int32_t rssi = 0;
    bool rssi_valid = wifi_get_rssi(&rssi);
    wifi_update_hint(rssi_valid, rssi);
    if (load_wifi_failures() > 0) // the hint did not change, or could not be read
        save_wifi_failures(0);
    wifi_pmk_joined();

    if (wifi_attempt_count == 0)
        debug_status("OK", "WiFi: \"%s\" (connected in %.1fs, AP hint, %ld dBm)\n",
                     wifi_config_flash.ssid, (float)elapsed_ms / 1000.0f, (long)rssi);
    else
        debug_status("OK", "WiFi: \"%s\" (connected in %.1fs, %d attempt%s, %ld dBm)\n",
                     wifi_config_flash.ssid, (float)elapsed_ms / 1000.0f, wifi_attempt_count,
                     wifi_attempt_count == 1 ? "" : "s", (long)rssi);
    return WIFI_SUCCESS;
}

int wifi_backoff_minutes(void) {
    const int failures = load_wifi_failures();
    if (failures <= 0)
        return 0;
    int minutes = device_config_flash.data.wifi_reconnect_minutes;
    if (minutes < 1)
        minutes = 1;
    for (int i = 1; i < failures && minutes < WIFI_BACKOFF_MAX_MIN; i++)
        minutes *= 2;
    return minutes < WIFI_BACKOFF_MAX_MIN ? minutes : WIFI_BACKOFF_MAX_MIN;
}

bool wifi_get_rssi(int32_t *out_dbm) {
    if (!out_dbm)
        return false;
    *out_dbm = 0;
    bool ok = false;
    cyw43_arch_lwip_begin();
    int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    if (link >= CYW43_LINK_JOIN) {
        int32_t rssi = 0;
        if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) {
            *out_dbm = rssi;
            ok = true;
        }
    }
    cyw43_arch_lwip_end();
    return ok;
}

void wifi_poll(void) { cyw43_arch_poll(); }

void wifi_set_power_save(bool on) {
    cyw43_arch_lwip_begin();
    const int rc = cyw43_wifi_pm(&cyw43_state, on ? CYW43_DEFAULT_PM : CYW43_NONE_PM);
    cyw43_arch_lwip_end();
    debug_status(rc == 0 ? "INFO" : "WARN", "WiFi: power save %s (rc=%d)\n", on ? "on" : "off", rc);
}

void wifi_log_rssi(void) {
    int32_t rssi = 0;
    if (wifi_get_rssi(&rssi)) {
        dlog("Wi-Fi RSSI: %ld dBm\n", (long)rssi);
    } else {
        dlog("Wi-Fi RSSI: N/A\n");
    }
}

void wifi_deinit(void) {
    wifi_leave_if_joined();
    uint32_t on_ms = (uint32_t)((time_us_64() - s_cyw43_on_us) / 1000);
    debug_status("OK", "CYW43: chip off (on for %.1fs)\n", (float)on_ms / 1000.0f);
    cyw43_arch_deinit();
}

bool wifi_has_network(void) {
    return wifi_config_flash.ssid[0] != '\0' &&
           strncmp(wifi_config_flash.ssid, WIFI_SSID_UNSET, sizeof(wifi_config_flash.ssid)) != 0;
}

const char *wifi_hostname(void) {
    static char name[16]; // netif_set_hostname keeps the pointer
    if (!name[0]) {
        const uint8_t *mac = wifi_mac();
        snprintf(name, sizeof(name), "tinta-%02x%02x", mac[4], mac[5]);
    }
    return name;
}

const char *wifi_setup_ssid(void) {
    static char ssid[16];
    if (!ssid[0]) {
        const uint8_t *mac = wifi_mac();
        snprintf(ssid, sizeof(ssid), "Tinta-%02X%02X", mac[4], mac[5]);
    }
    return ssid;
}

const char *wifi_setup_password(void) {
    // Easy to read off the panel and to type: no l, i, o, 0 or 1.
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    static const char purpose[] = "tinta-setup-v1";
    static char password[10]; // "xxxx-xxxx"
    if (!password[0]) {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        uint8_t keyed[sizeof(id.id) + sizeof(purpose)];
        memcpy(keyed, id.id, sizeof(id.id));
        memcpy(keyed + sizeof(id.id), purpose, sizeof(purpose));

        uint8_t digest[32] = {0};
        const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!sha256 || mbedtls_md(sha256, keyed, sizeof(keyed), digest) != 0)
            debug_status("ERROR", "WiFi: setup password could not be derived\n");

        char *p = password;
        for (int i = 0; i < 8; i++) {
            if (i == 4)
                *p++ = '-';
            *p++ = alphabet[digest[i] % (sizeof(alphabet) - 1)];
        }
        *p = '\0';
    }
    return password;
}

#define WIFI_SCAN_MAX 16
#define WIFI_SCAN_MAX_MS 6000
static wifi_network_t s_found[WIFI_SCAN_MAX];
static int s_found_count;

int wifi_found_networks(const wifi_network_t **list) {
    *list = s_found;
    return s_found_count;
}

// One entry per name, with the strongest access point; when the list is full a stronger
// network pushes out the weakest.
static int on_scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (r->ssid_len == 0 || r->ssid_len > 32 || r->ssid[0] == '\0')
        return 0;
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = '\0';

    int weakest = 0;
    for (int i = 0; i < s_found_count; i++) {
        if (strcmp(s_found[i].ssid, ssid) == 0) {
            if (r->rssi > s_found[i].rssi)
                s_found[i].rssi = r->rssi;
            return 0;
        }
        if (s_found[i].rssi < s_found[weakest].rssi)
            weakest = i;
    }
    int slot;
    if (s_found_count < WIFI_SCAN_MAX)
        slot = s_found_count++;
    else if (r->rssi > s_found[weakest].rssi)
        slot = weakest;
    else
        return 0;
    memcpy(s_found[slot].ssid, ssid, sizeof(ssid));
    s_found[slot].rssi = r->rssi;
    return 0;
}

// Looks for networks as a client, before the setup network takes the radio: a scan while the
// setup network runs leaves its channel and can drop the device that is setting the sign up.
static void scan_networks(void) {
    s_found_count = 0;
    memset(s_found, 0, sizeof(s_found));
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts = {0};
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan_result) != 0) {
        debug_status("WARN", "WiFi: network scan could not start\n");
        cyw43_arch_disable_sta_mode();
        return;
    }
    const absolute_time_t until = make_timeout_time_ms(WIFI_SCAN_MAX_MS);
    while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(until)) {
        watchdog_update();
        sleep_ms(50);
    }
    cyw43_arch_disable_sta_mode();

    for (int i = 1; i < s_found_count; i++) // strongest first
        for (int j = i; j > 0 && s_found[j].rssi > s_found[j - 1].rssi; j--) {
            const wifi_network_t t = s_found[j];
            s_found[j] = s_found[j - 1];
            s_found[j - 1] = t;
        }
    debug_status("OK", "WiFi: %d networks found\n", s_found_count);
}

bool wifi_ap_start(void) {
    const char *ssid = wifi_setup_ssid(); // reads the MAC with the chip still off
    if (!wifi_chip_on(" (setup network)"))
        return false;
    scan_networks();
    cyw43_arch_enable_ap_mode(ssid, wifi_setup_password(), CYW43_AUTH_WPA2_AES_PSK);
    wifi_capture_mac();
    clear_wifi_hint(); // setup mode: the next join looks for the network afresh
    return true;
}

int wifi_ap_client_count(void) {
    uint8_t macs[8 * 6];
    int count = sizeof(macs) / 6; // in: room in macs, out: devices joined
    cyw43_wifi_ap_get_stas(&cyw43_state, &count, macs);
    return count;
}

typedef struct {
    volatile bool done;
    bool success;
    ip_addr_t ip;
} dns_result_t;

static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    dns_result_t *res = (dns_result_t *)arg;
    if (ipaddr) {
        res->ip = *ipaddr;
        res->success = true;
    }
    res->done = true;
}

bool wifi_resolve_hostname(const char *hostname, ip_addr_t *out_ip, int timeout_ms) {
    if (!hostname || !out_ip)
        return false;

    // Stack-allocated; safe because dns_callback fires only inside cyw43_arch_poll(),
    // which is called from the loop below before this frame returns.
    dns_result_t result = {.done = false, .success = false};

#if INKI_DEBUG_TIMING
    uint32_t dns_start_ms = to_ms_since_boot(get_absolute_time());
#endif
    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(hostname, &result.ip, dns_callback, &result);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        *out_ip = result.ip;
        dlog("[WIFI] DNS cache hit: %s\n", hostname);
        return true;
    }
    if (err != ERR_INPROGRESS) {
        dlog("[WIFI] DNS error %d for %s\n", (int)err, hostname);
        return false;
    }

    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    while (!result.done && time_us_64() < deadline) {
        sleep_ms(10);
        watchdog_update();
    }

    if (!result.done || !result.success) {
        dlog("[WIFI] DNS timeout/failure for %s\n", hostname);
        return false;
    }

    *out_ip = result.ip;
    dlog("[WIFI] DNS resolved: %s\n", hostname);
#if INKI_DEBUG_TIMING
    debug_status("INFO", "[TIMING] DNS %lu ms\n",
                 (unsigned long)(to_ms_since_boot(get_absolute_time()) - dns_start_ms));
#endif
    return true;
}

const uint8_t *wifi_mac(void) {
    if (s_mac_valid)
        return s_mac;

    // Cold path: chip is off. Init → STA mode → read → deinit.
    if (!wifi_chip_on(" (MAC read)"))
        return s_mac;
    cyw43_arch_enable_sta_mode();
    wifi_capture_mac();
    cyw43_arch_deinit();
    return s_mac;
}
