#include "weathermap/client.h"
#include "weathermap/web_config.h"
#define LOG_MODULE LOG_MOD_WEATHERMAP
#include "debug.h"
#include "epaper_pages_shared.h"
#include "flash.h"
#include "fonts.h"
#include "hardware/watchdog.h"
#include "http_client.h"
#include "lwip/ip_addr.h"
#include "pico/stdlib.h"
#include "third_party/GUI/GUI_Paint.h"
#include "tls_trust_store.h"
#include "weathermap/png_stream.h"
#include "weathermap/weathermap_flash.h"
#include "webserver.h"

#include "st25_io.h"
#include "use_case.h"
#include "weathermap/epaper_pages.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Provider: BKG basemap (sgx.geodatenzentrum.de) — static cartographic base
#define BKG_HOST "sgx.geodatenzentrum.de"
#define BKG_IP0 141
#define BKG_IP1 74
#define BKG_IP2 64
#define BKG_IP3 40
#define BKG_PORT 443

// Provider: DWD radar (maps.dwd.de) — dynamic weather layers (hourly+)
// Note: We currently avoid DNS; if you know the IP, set it here.
#define DWD_HOST "maps.dwd.de"
#ifndef DWD_IP0
#define DWD_IP0 0
#define DWD_IP1 0
#define DWD_IP2 0
#define DWD_IP3 0
#endif
#define DWD_PORT 443

// Dynamic WMS path with 4:3 window
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define WMAP_DEFAULT_CENTER_LAT WEATHERMAP_DEFAULT_CENTER_LAT
#define WMAP_DEFAULT_CENTER_LON WEATHERMAP_DEFAULT_CENTER_LON
#define WMAP_DEFAULT_HALF_WIDTH_M                                                                  \
    WEATHERMAP_DEFAULT_HALF_WIDTH_M // Base: 40 km total horizontal span (adjust height by aspect)

typedef struct {
    bool valid;
    bool success;
    size_t bytes;
    char reason[48];
} geodata_status_t;

static geodata_status_t s_geodata_status = {0};
static bool weathermap_process_png_and_store(const uint8_t *png_data, size_t png_len);

static void geodata_status_set(bool success, const char *reason, size_t bytes) {
    s_geodata_status.valid = true;
    s_geodata_status.success = success;
    s_geodata_status.bytes = bytes;
    if (reason && *reason) {
        snprintf(s_geodata_status.reason, sizeof(s_geodata_status.reason), "%s", reason);
    } else {
        s_geodata_status.reason[0] = '\0';
    }
}

static void wmap_get_config(double *lat, double *lon, double *half_width_m) {
    double cfg_lat = WMAP_DEFAULT_CENTER_LAT;
    double cfg_lon = WMAP_DEFAULT_CENTER_LON;
    double cfg_half = WMAP_DEFAULT_HALF_WIDTH_M;

    weathermap_config_t cfg;
    if (load_weathermap_config(&cfg)) {
        if (isfinite(cfg.data.center_lat) && fabs(cfg.data.center_lat) <= 90.0) {
            cfg_lat = cfg.data.center_lat;
        }
        if (isfinite(cfg.data.center_lon) && fabs(cfg.data.center_lon) <= 180.0) {
            cfg_lon = cfg.data.center_lon;
        }
        if (isfinite(cfg.data.half_width_m) && cfg.data.half_width_m > 0.0) {
            cfg_half = cfg.data.half_width_m;
        }
    }

    if (lat)
        *lat = cfg_lat;
    if (lon)
        *lon = cfg_lon;
    if (half_width_m)
        *half_width_m = cfg_half;
}

// Resolve target PNG size from configured ePaper type
static inline void wmap_get_target_size(int *out_w, int *out_h) {
    int w = 400, h = 300; // default to 4.2" panel
    switch (device_config_flash.data.epapertype) {
    case EPAPER_WAVESHARE_7IN5_V2:
        w = 800;
        h = 480;
        break;
    case EPAPER_WAVESHARE_4IN2_V2:
        w = 400;
        h = 300;
        break;
    default:
        break;
    }
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
}

static void wmap_center_to_bbox(double lat_deg, double lon_deg, double half_w_m, double half_h_m,
                                double *xmin, double *ymin, double *xmax, double *ymax) {
    const double R = 6378137.0;
    const double lat = lat_deg * (M_PI / 180.0);
    const double lon = lon_deg * (M_PI / 180.0);
    const double x = R * lon;
    const double y = R * log(tan(M_PI / 4.0 + lat / 2.0));
    *xmin = x - half_w_m;
    *xmax = x + half_w_m;
    *ymin = y - half_h_m;
    *ymax = y + half_h_m;
}

// Build WMS GetMap for BKG basemap (PNG8, gray)
static int bkg_build_http_get(char *dst, size_t dst_len) {
    double xmin, ymin, xmax, ymax;
    int tw, th;
    wmap_get_target_size(&tw, &th);
    double lat, lon, half_w_m;
    wmap_get_config(&lat, &lon, &half_w_m);
    // Adjust vertical half-size to match target aspect
    double half_h_m = half_w_m * ((double)th / (double)tw);
    wmap_center_to_bbox(lat, lon, half_w_m, half_h_m, &xmin, &ymin, &xmax, &ymax);
    char path[512];
    int pn = snprintf(path, sizeof(path),
                      "/wms_basemapde?service=WMS&version=1.1.1&request=GetMap&"
                      "layers=de_basemapde_web_raster_grau&styles=default&format=image/png8&"
                      "srs=EPSG:3857&bbox=%.1f,%.1f,%.1f,%.1f&width=%d&height=%d",
                      xmin, ymin, xmax, ymax, tw, th);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return -1;
    int n = snprintf(
        dst, dst_len,
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Tinta/weathermap\r\nConnection: close\r\n\r\n",
        path, BKG_HOST);
    return (n > 0 && (size_t)n < dst_len) ? n : -1;
}

// Build WMS GetMap for DWD radar (transparent PNG). Layer name may be adapted later.
// Common candidates: dwd:RX-Ref (composite reflectivity). We use PNG for alpha support.
static int dwd_build_http_get(char *dst, size_t dst_len) {
    double xmin, ymin, xmax, ymax;
    int tw, th;
    wmap_get_target_size(&tw, &th);
    double lat, lon, half_w_m;
    wmap_get_config(&lat, &lon, &half_w_m);
    double half_h_m = half_w_m * ((double)th / (double)tw);
    wmap_center_to_bbox(lat, lon, half_w_m, half_h_m, &xmin, &ymin, &xmax, &ymax);
    char path[512];
    // Transparent PNG, WMS 1.1.1, SRS EPSG:3857
    int pn = snprintf(path, sizeof(path),
                      "/geoserver/dwd/wms?service=WMS&version=1.1.1&request=GetMap&"
                      "layers=dwd:RX-Ref&styles=&format=image/png&transparent=true&"
                      "srs=EPSG:3857&bbox=%.1f,%.1f,%.1f,%.1f&width=%d&height=%d",
                      xmin, ymin, xmax, ymax, tw, th);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return -1;
    int n = snprintf(
        dst, dst_len,
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Tinta/geodata\r\nConnection: close\r\n\r\n",
        path, DWD_HOST);
    return (n > 0 && (size_t)n < dst_len) ? n : -1;
}

typedef struct {
    uint8_t *buf;
    size_t max_len;
    size_t out_len;
    bool complete;
    bool success;
} fetch_ctx_t;

static void fetch_complete_cb(const char *body, size_t length, bool success, void *arg) {
    fetch_ctx_t *ctx = (fetch_ctx_t *)arg;
    ctx->success = success && (body != NULL) && (length > 0);
    if (ctx->success) {
        if (length > ctx->max_len) {
            length = ctx->max_len; // clamp
        }
        memcpy(ctx->buf, body, length);
        ctx->out_len = length;
    }
    ctx->complete = true;
}

typedef struct {
    size_t out_len;
    bool success;
    bool complete;
} inline_ctx_t;

// cppcheck-suppress unusedFunction
static void inline_complete_cb(const char *body, size_t length, bool success, void *arg) {
    inline_ctx_t *ictx = (inline_ctx_t *)arg;
    if (success && body && length > 0) {
        ictx->success = weathermap_process_png_and_store((const uint8_t *)body, length);
        ictx->out_len = length;
    } else {
        ictx->success = false;
        ictx->out_len = 0;
    }
    ictx->complete = true;
}

// cppcheck-suppress unusedFunction
bool weathermap_fetch_png(uint8_t *out_buf, size_t max_len, size_t *out_len) {
    if (!out_buf || max_len == 0 || !out_len)
        return false;

    // Build simple HTTP/1.0 GET with Host header; TLS will be enabled by port==443
    char req[512];
    int n = bkg_build_http_get(req, sizeof(req));
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        dlog("[WEATHERMAP] Request build failed\n");
        return false;
    }

    ip_addr_t ip;
    IP4_ADDR(&ip, BKG_IP0, BKG_IP1, BKG_IP2, BKG_IP3);

    fetch_ctx_t ctx = {
        .buf = out_buf, .max_len = max_len, .out_len = 0, .complete = false, .success = false};

    http_result_t r = http_request_async(&ip, BKG_PORT, req, fetch_complete_cb, &ctx);
    if (r != HTTP_SUCCESS) {
        dlog("[WEATHERMAP] http_request_async failed: %d\n", r);
        return false;
    }

    // Wait for completion with a timeout budget - curl completes in ~1s
    int waits = 0;
    const int max_waits = 500; // 500 * 10ms = 5s (should be enough based on curl)
    while (!ctx.complete && waits < max_waits) {
        sleep_ms(10);
        watchdog_update();
        waits++;

        // Log progress every 1 second
        if (waits % 100 == 0) {
            dlog("[WEATHERMAP] Still waiting... (%ds elapsed)\n", waits / 100);
        }
    }

    if (!ctx.complete || !ctx.success) {
        dlog("[WEATHERMAP] Fetch failed or timed out\n");
        return false;
    }

    *out_len = ctx.out_len;
    return true;
}

// Streaming stage context
typedef struct {
    volatile bool complete;
    volatile bool success;
    size_t total;
    bool is_bmp;
} stage_ctx_t;

static void stream_header_cb(const char *header, size_t header_len, int status_code,
                             int content_length, void *arg) {
    stage_ctx_t *sctx = (stage_ctx_t *)arg;
    dlog("[WMAP] HTTP status: %d, Content-Length: %d\n", status_code, content_length);
    if (status_code >= 200 && status_code < 300) {
        wmap_staging_begin();
        sctx->success = true;
    } else {
        sctx->success = false;
    }
    // Detect BMP via Content-Type header (if present)
    sctx->is_bmp = false;
    if (header && header_len > 0) {
        if (strstr(header, "Content-Type:") &&
            (strstr(header, "image/bmp") || strstr(header, "image/x-bmp"))) {
            sctx->is_bmp = true;
        }
    }
}

static void stream_data_cb(const uint8_t *data, size_t len, void *arg) {
    stage_ctx_t *sctx = (stage_ctx_t *)arg;
    if (!sctx->success)
        return; // ignore if already failed
    if (!wmap_staging_append(data, len)) {
        sctx->success = false;
    } else {
        sctx->total += len;
        if ((sctx->total & 0x7FFF) == 0) { // log roughly every 32 KB boundary
            dlog("[WMAP] Streamed %u bytes...\n", (unsigned)sctx->total);
        }
    }
}

static void stream_complete_cb(bool success, void *arg) {
    stage_ctx_t *sctx = (stage_ctx_t *)arg;
    if (success && sctx->success) {
        uint32_t bytes = 0;
        if (wmap_staging_end(&bytes)) {
            set_weathermap_meta(bytes);
            sctx->total = bytes;
            sctx->success = true;
            (void)wmap_staging_ptr();
        } else {
            sctx->success = false;
        }
    } else {
        wmap_staging_abort();
        sctx->success = false;
    }
    sctx->complete = true;
}

// Generic inline fetch for a prebuilt GET to a specific provider (by IP/port/Host in req)
static bool geodata_fetch_and_store_inline(const ip_addr_t *ip, uint16_t port, const char *req,
                                           size_t *out_len) {
    stage_ctx_t sctx = {.complete = false, .success = false, .total = 0, .is_bmp = false};
    s_geodata_status.valid = false;
    http_result_t r = http_request_async_stream(ip, port, req, stream_header_cb, stream_data_cb,
                                                stream_complete_cb, &sctx);
    if (r != HTTP_SUCCESS) {
        geodata_status_set(false, "connect error", 0);
        clear_weathermap_meta();
        return false;
    }
    const uint32_t idle_abort_ms = 10000; // abort if no progress for 10 s
    const uint32_t hard_abort_ms = 60000; // fail-safe max duration
    absolute_time_t start = get_absolute_time();
    absolute_time_t last_progress = start;
    size_t last_total = 0;
    const char *failure_reason = NULL;

    while (!sctx.complete) {
        sleep_ms(10);
        watchdog_update();

        if (sctx.total != last_total) {
            last_total = sctx.total;
            last_progress = get_absolute_time();
        }

        if (absolute_time_diff_us(last_progress, get_absolute_time()) / 1000 > idle_abort_ms) {
            dlog("[GEODATA] Fetch stalled: %u bytes, no progress for %u ms\n", (unsigned)sctx.total,
                 idle_abort_ms);
            failure_reason = "stalled";
            break;
        }

        if (absolute_time_diff_us(start, get_absolute_time()) / 1000 > hard_abort_ms) {
            dlog("[GEODATA] Fetch timed out after %u ms (received %u bytes)\n", hard_abort_ms,
                 (unsigned)sctx.total);
            failure_reason = "timeout";
            break;
        }
    }
    if (out_len)
        *out_len = sctx.total;

    if (sctx.complete && sctx.success) {
        geodata_status_set(true, NULL, sctx.total);
        return true;
    }

    if (!failure_reason) {
        failure_reason = sctx.success ? "aborted" : "transfer error";
    }
    geodata_status_set(false, failure_reason, sctx.total);
    clear_weathermap_meta();
    return false;
}

typedef struct {
    size_t out_len;
    bool complete;
    bool success;
} count_ctx_t;

static void count_complete_cb(const char *body, size_t length, bool success, void *arg) {
    count_ctx_t *ctx = (count_ctx_t *)arg;
    ctx->success = success;
    ctx->out_len = length;
    ctx->complete = true;
}

// cppcheck-suppress unusedFunction
bool weathermap_fetch_count(size_t *out_len) {
    if (!out_len)
        return false;
    char req[512];
    int n = bkg_build_http_get(req, sizeof(req));
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        return false;
    }
    ip_addr_t ip;
    IP4_ADDR(&ip, BKG_IP0, BKG_IP1, BKG_IP2, BKG_IP3);
    count_ctx_t ctx = {.out_len = 0, .complete = false, .success = false};
    http_result_t r = http_request_async_count_only(&ip, BKG_PORT, req, count_complete_cb, &ctx);
    if (r != HTTP_SUCCESS)
        return false;
    int waits = 0;
    const int max_waits = 500; // 5s should be enough based on curl
    while (!ctx.complete && waits < max_waits) {
        sleep_ms(10);
        watchdog_update();
        waits++;
        if (waits % 100 == 0) {
            dlog("[WEATHERMAP] Count fetch still waiting... (%ds elapsed)\n", waits / 100);
        }
    }
    if (!ctx.complete || !ctx.success)
        return false;
    *out_len = ctx.out_len;
    return true;
}

// --- PNG processing to 2-bit and flash store (streaming)
static bool weathermap_process_png_and_store(const uint8_t *png_data, size_t png_len) {
    // Stream decode PNG (inflate + unfilter) and write line-by-line into slot1 image area
    return png_stream_decode_to_flash_from_xip(png_data, png_len);
}

static void weathermap_draw_error_page(void) {
    Paint_SelectImage(Paint.Image);
    Paint_Clear(GRAY4);

    const sFONT *title_font = &Font16;
    const sFONT *body_font = &Font12;

    Paint_DrawString_EN(20, 20, "Weather map unavailable", (sFONT *)title_font, GRAY4, GRAY1);

    if (s_geodata_status.valid && !s_geodata_status.success) {
        char detail[64];
        double kib = s_geodata_status.bytes / 1024.0;
        const char *reason = s_geodata_status.reason[0] ? s_geodata_status.reason : "fetch error";
        snprintf(detail, sizeof(detail), "%s after %.1f KiB", reason, kib);
        Paint_DrawString_EN(20, 48, detail, (sFONT *)body_font, GRAY4, GRAY1);
        Paint_DrawString_EN(20, 66, "Map will be retried next wake.", (sFONT *)body_font, GRAY4,
                            GRAY1);
    } else {
        Paint_DrawString_EN(20, 48, "No cached data available.", (sFONT *)body_font, GRAY4, GRAY1);
    }
}

bool weathermap_render_from_flash(void) {
    // Decode staged PNG in slot1 and draw directly into Paint at (10,10)
    uint32_t staged_bytes = 0;
    if (!get_weathermap_meta(&staged_bytes) || staged_bytes < 64) {
        if (s_geodata_status.valid && !s_geodata_status.success) {
            dlog("[WEATHERMAP] Rendering fallback: %s after %u bytes\n",
                 s_geodata_status.reason[0] ? s_geodata_status.reason : "error",
                 (unsigned)s_geodata_status.bytes);
            weathermap_draw_error_page();
            return true;
        }
        dlog("[WEATHERMAP] No staged PNG marker; cannot draw.\n");
        return false;
    }
    const uint8_t *png = (const uint8_t *)FLASH_PTR(FIRMWARE_SLOT1_FLASH_OFFSET);
    bool ok = png_stream_draw_to_paint_from_xip(png, (size_t)staged_bytes, 0, 0);
    if (ok) {
        // Draw a simple scale bar at bottom-left: length = 1/10 of WMS width (in pixels)
        // and label with real-world length (based on bbox width in meters).
        const int margin = 6;
        const int shift_x = 20; // shift scale bar to the right
        const int bar_h = 6;
        const int scale_px = Paint.Width / 10; // scale bar is 1/10th of current canvas width
        int x0 = margin + shift_x;
        int y1 = Paint.Height - margin;
        int y0 = y1 - bar_h;
        if (y0 < 0)
            y0 = 0;
        int x1 = x0 + scale_px;
        // Draw filled black rectangle as scale bar
        Paint_DrawRectangle((UWORD)x0, (UWORD)y0, (UWORD)x1, (UWORD)y1, GRAY1, DOT_PIXEL_1X1,
                            DRAW_FILL_FULL);

        // Build label text for physical length (meters or km)
        char label[32];
        double half_w_m = WMAP_DEFAULT_HALF_WIDTH_M;
        wmap_get_config(NULL, NULL, &half_w_m);
        double meters = (2.0 * half_w_m) / 10.0;
        if (meters >= 1000.0) {
            double km = meters / 1000.0;
            // Use 0 decimals if close to integer, else 1 decimal
            if (fabs(km - (int)km) < 0.05) {
                snprintf(label, sizeof(label), "%d km", (int)km);
            } else {
                snprintf(label, sizeof(label), "%.1f km", km);
            }
        } else {
            // Round to nearest 10 m for neatness
            int m10 = (int)((meters + 5.0) / 10.0) * 10;
            snprintf(label, sizeof(label), "%d m", m10);
        }
        // Place label just above the bar, left-aligned
        const sFONT *f = &Font12;
        // Increase spacing between text and scale by 5px (was 2px)
        const int label_gap = 7;
        int ty = y0 - (int)f->Height - label_gap;
        if (ty < 0)
            ty = y1 + label_gap; // fallback below the bar if not enough room above

        // Draw a white rectangle slightly larger than the text box (4 px larger total: 2 px padding
        // each side)
        int text_w = (int)strlen(label) * (int)f->Width;
        int text_h = (int)f->Height;
        int pad = 2;
        int rx0 = x0 - pad;
        int ry0 = ty - pad;
        int rx1 = x0 + text_w + pad;
        int ry1 = ty + text_h + pad;
        if (ry0 < 0)
            ry0 = 0;
        if (rx1 > (int)Paint.Width)
            rx1 = (int)Paint.Width;
        if (ry1 > (int)Paint.Height)
            ry1 = (int)Paint.Height;
        Paint_DrawRectangle((UWORD)rx0, (UWORD)ry0, (UWORD)rx1, (UWORD)ry1, GRAY4, DOT_PIXEL_1X1,
                            DRAW_FILL_FULL);

        // Draw black text on white background inside the rectangle.
        // Note: Paint_DrawString_EN swaps its color args when calling Paint_DrawChar, so pass
        // reversed here.
        Paint_DrawString_EN((UWORD)x0, (UWORD)ty, label, (sFONT *)f, GRAY4, GRAY1);
    }
    if (!ok) {
        dlog("[WEATHERMAP] png_stream_draw_to_paint failed.\n");
    }
    return ok;
}

static bool wm_fetch_ok;
static run_error_t s_last_run_error = RUN_OK;

static void geodata_fetch(void) {
    // Ensure HTTP client/TLS is initialized
    extern bool http_client_init(void);
    http_client_init();

    // Ensure config exists with sane defaults before fetching
    weathermap_config_t cfg_init;
    if (!load_weathermap_config(&cfg_init)) {
        init_weathermap_config(&cfg_init);
    }

    // Always refetch on boot for now (later: schedule basemap vs radar differently)
    dlog("[GEODATA] Boot fetch enabled — fetching now.\n");

    // Try DWD radar first if IP is configured; else fall back to BKG basemap
    char req[512];
    size_t count = 0;
    bool ok = false;

    if ((DWD_IP0 | DWD_IP1 | DWD_IP2 | DWD_IP3) != 0) {
        int n = dwd_build_http_get(req, sizeof(req));
        if (n > 0 && (size_t)n < sizeof(req)) {
            ip_addr_t dwd_ip;
            IP4_ADDR(&dwd_ip, DWD_IP0, DWD_IP1, DWD_IP2, DWD_IP3);
            dlog("[GEODATA] HTTPS request (radar) to %s (%d.%d.%d.%d):\n%.*s\n", DWD_HOST, DWD_IP0,
                 DWD_IP1, DWD_IP2, DWD_IP3, n, req);
            ok = geodata_fetch_and_store_inline(&dwd_ip, DWD_PORT, req, &count);
        }
        if (!ok) {
            dlog("[GEODATA] DWD fetch failed or not configured — falling back to BKG basemap.\n");
        }
    }

    if (!ok) {
        int n2 = bkg_build_http_get(req, sizeof(req));
        if (n2 <= 0 || (size_t)n2 >= sizeof(req)) {
            dlog("[GEODATA] BKG request build failed\n");
            return;
        }
        ip_addr_t bkg_ip;
        IP4_ADDR(&bkg_ip, BKG_IP0, BKG_IP1, BKG_IP2, BKG_IP3);
        dlog("[GEODATA] HTTPS request (basemap) to %s (%d.%d.%d.%d):\n%.*s\n", BKG_HOST, BKG_IP0,
             BKG_IP1, BKG_IP2, BKG_IP3, n2, req);
        ok = geodata_fetch_and_store_inline(&bkg_ip, BKG_PORT, req, &count);
    }
    if (!ok) {
        s_last_run_error = RUN_ERROR_CONNECTION;
        debug_status("ERROR", "Weathermap: fetch failed\n");
        return;
    }

    dlog("[GEODATA] TLS fetch OK, bytes=%u\n", (unsigned)count);
    set_weathermap_meta((uint32_t)count);

    const uint8_t *data = wmap_staging_ptr();
    size_t data_len = wmap_staging_size();
    if (!(data && data_len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
          data[3] == 'G')) {
        s_last_run_error = RUN_ERROR_PARSE;
        debug_status("ERROR", "Weathermap: invalid or missing PNG in staging buffer\n");
    }
}
// --- Extra HTTP routes for this use case ---

static void weathermap_serve_png(struct tcp_pcb *tpcb);
static void weathermap_serve_clear(struct tcp_pcb *tpcb);

static const route_t wm_extra_routes[] = {
    {"/weathermap.png", HTTP_GET, ROUTE_ACTION, {.page = weathermap_serve_png}},
    {"/weathermap_clear", HTTP_GET, ROUTE_ACTION, {.page = weathermap_serve_clear}},
};

// --- use_case_t: Weathermap page catalog, input map, and export ---

static const page_def_t page_weathermap_main = {"weathermap", render_page_weathermap, true};

enum {
    WM_PAGE_WEATHERMAP,
    WM_PAGE_DND,
    WM_PAGE_DECISION_MAKER,
    WM_PAGE_DEVICE_INFO,
    WM_PAGE_WIFI_SETUP,
    WM_PAGE_NFC_TEXT,
    WM_PAGE_NFC_IMAGE,
    WM_PAGE_COUNT
};

static const page_def_t *wm_pages[] = {
    [WM_PAGE_WEATHERMAP] = &page_weathermap_main,    [WM_PAGE_DND] = &page_dnd,
    [WM_PAGE_DECISION_MAKER] = &page_decision_maker, [WM_PAGE_DEVICE_INFO] = &page_device_info,
    [WM_PAGE_WIFI_SETUP] = &page_wifi_setup,         [WM_PAGE_NFC_TEXT] = &page_nfc_text,
    [WM_PAGE_NFC_IMAGE] = &page_nfc_image,           NULL,
};

static const input_map_entry_t wm_input_map[] = {
    {INPUT_BUTTON(0), WM_PAGE_WEATHERMAP},
    {INPUT_BUTTON(1), WM_PAGE_DND},
    {INPUT_BUTTON(2), WM_PAGE_DECISION_MAKER},
    {INPUT_BUTTON(3), WM_PAGE_DEVICE_INFO},
    {INPUT_BUTTON(4), WM_PAGE_WIFI_SETUP},
    {INPUT_BUTTON(5), WM_PAGE_DND},
    {INPUT_BUTTON(6), WM_PAGE_DECISION_MAKER},
    {INPUT_BUTTON(7), PAGE_ACTION_SETUP},
    {INPUT_NFC(ST25_OPCODE_PAGE_0), WM_PAGE_WEATHERMAP},
    {INPUT_NFC(ST25_OPCODE_REFRESH), WM_PAGE_WEATHERMAP},
    {INPUT_NFC(ST25_OPCODE_PAGE_2), WM_PAGE_DECISION_MAKER},
    {INPUT_NFC(ST25_OPCODE_TEXT), WM_PAGE_NFC_TEXT},
    {INPUT_NFC(ST25_OPCODE_DRAW_IMAGE), WM_PAGE_NFC_IMAGE},
    {INPUT_BUTTON(16), WM_PAGE_NFC_TEXT},
    {INPUT_MAP_END, 0},
};

// --- Lifecycle: run (fetch) and render ---

static run_result_t wm_run(int page, int input) {
    (void)page;
    (void)input;
    s_last_run_error = RUN_OK;
    tls_init();
    wm_fetch_ok = false;
    geodata_fetch();
    const uint8_t *data = wmap_staging_ptr();
    size_t data_len = wmap_staging_size();
    if (data && data_len >= 8 && data[0] == 0x89 && data[1] == 'P') {
        wm_fetch_ok = true;
        return (run_result_t){.data = &wm_fetch_ok};
    }
    int ep = (s_last_run_error == RUN_ERROR_PARSE) ? PAGE_PARSE_ERROR : PAGE_HTTP_ERROR;
    return (run_result_t){.error_page = ep};
}

static void wm_render(uint8_t *image, device_caps_t caps, int page, const void *data) {
    (void)caps;
    if (page == PAGE_WIFI_ERROR) {
        debug_status("ERROR", "Weathermap: showing WiFi error page\n");
        render_page_error(image, "WiFi Error!", "Unable to connect to WiFi",
                          "Check WiFi settings.");
        return;
    }
    if (page == PAGE_HTTP_ERROR) {
        debug_status("ERROR", "Weathermap: showing fetch error page\n");
        render_page_error(image, "Fetch Error!", "Cannot load map data",
                          "Check network and TLS config.");
        return;
    }
    if (page == PAGE_PARSE_ERROR) {
        debug_status("ERROR", "Weathermap: showing PNG error page\n");
        render_page_error(image, "Map Error!", "Invalid map data received",
                          "Check server response.");
        return;
    }
    if (!data && page >= 0 && wm_pages[page] && wm_pages[page]->needs_wifi) {
        debug_status("ERROR", "Weathermap: showing generic fetch error page\n");
        render_page_error(image, "Fetch Error!", "Unable to load map data", "Check configuration.");
        return;
    }

    if (page >= 0 && wm_pages[page])
        wm_pages[page]->render(image);
    else
        render_page_fallback(page, image);
}

const use_case_t use_case = {
    .name = "weathermap",
    .pages = wm_pages,
    .input_map = wm_input_map,
    .default_page = WM_PAGE_WEATHERMAP,
    .run = wm_run,
    .render = wm_render,
    .free_data = NULL,
    .render_config_page = wm_render_config_page,
    .handle_config_form = wm_handle_config_form,
    .needs_4gray = true,
    .show_display_name = true,
    .extra_routes = wm_extra_routes,
    .extra_route_count = sizeof(wm_extra_routes) / sizeof(wm_extra_routes[0]),
};

// =============================================================================
// Web route handlers
// =============================================================================

static void weathermap_serve_png(struct tcp_pcb *tpcb) {
    uint32_t staged_bytes = 0;
    if (!get_weathermap_meta(&staged_bytes) || staged_bytes < 8) {
        send_response(tpcb, "<html><body><h3>No cached map</h3></body></html>");
        return;
    }
    const uint8_t *data = FLASH_PTR(FIRMWARE_SLOT1_FLASH_OFFSET);
    if (!(data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')) {
        send_response(tpcb, "<html><body><h3>Invalid PNG in cache</h3></body></html>");
        return;
    }
    send_binary_response(tpcb, "image/png", data, staged_bytes);
}

static void weathermap_serve_clear(struct tcp_pcb *tpcb) {
    weathermap_flash_clear_image();
    clear_weathermap_meta();
    char page[512];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" "
             "content=\"width=device-width, initial-scale=1\">"
             "<title>Weathermap</title><style>body{font-family:sans-serif;margin:2em;}</style>"
             "</head><body>"
             "<h3>Weathermap cleared</h3><p>The cached image and marker were erased. A fresh "
             "fetch will occur on next boot.</p>"
             "<p><a href=\"/\">Back</a></p>"
             "</body></html>");
    send_response(tpcb, page);
}
