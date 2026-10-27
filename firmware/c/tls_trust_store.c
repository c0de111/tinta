#include "tls_trust_store.h"
#include "config.h"

#ifdef TRUST_STORE_HEADER

#define LOG_MODULE LOG_MOD_TLS
#include "debug.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "pico/time.h"
#if INKI_DEBUG_TLS_CB
#include "mbedtls/debug.h"
#endif

#include TRUST_STORE_HEADER // defines: trust_store_pem[], trust_store_pem_len (includes NUL)

static struct altcp_tls_config *s_tls_cfg = NULL;

#if INKI_DEBUG_TLS_CB
static void tls_enable_debug_callback(void) {
    mbedtls_debug_set_threshold(4);
    dlog("[TLS] INKI_DEBUG_TLS_CB enabled (mbedTLS debug threshold=4)\n");
}
#endif

void tls_init(void) {
    if (s_tls_cfg)
        return;

    dlog("[TLS] Trust store: %u bytes\n", (unsigned)trust_store_pem_len);

    s_tls_cfg =
        altcp_tls_create_config_client((const u8_t *)trust_store_pem, (size_t)trust_store_pem_len);

    if (!s_tls_cfg) {
        dlog("[TLS] Failed to create ALTCP TLS client config\n");
        return;
    }

#if INKI_DEBUG_TLS_CB
    tls_enable_debug_callback();
#endif

    dlog("[TLS] ALTCP TLS config created successfully\n");
}

struct altcp_tls_config *tls_get_client_config(void) { return s_tls_cfg; }

#if INKI_DEBUG_TIMING
static uint32_t s_verify_done_ms;
static int s_chain_length;
static uint32_t s_verify_flags;

// mbedTLS calls this once per certificate after the whole chain has been checked, leaf last.
// cppcheck-suppress constParameterCallback
static int timing_verify_cb(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)data;
    (void)crt;
    s_verify_done_ms = to_ms_since_boot(get_absolute_time());
    if (depth + 1 > s_chain_length)
        s_chain_length = depth + 1;
    s_verify_flags |= *flags;
    return 0;
}

uint32_t tls_timing_verify_done_ms(void) { return s_verify_done_ms; }
int tls_timing_chain_length(void) { return s_chain_length; }
uint32_t tls_timing_verify_flags(void) { return s_verify_flags; }
#endif

void tls_apply_sni(struct altcp_pcb *pcb, const char *hostname) {
    if (!pcb || !hostname || hostname[0] == '\0')
        return;
    void *ctx = altcp_tls_context(pcb);
    if (ctx) {
        mbedtls_ssl_set_hostname((mbedtls_ssl_context *)ctx, hostname);
#if INKI_DEBUG_TIMING
        s_verify_done_ms = 0;
        s_chain_length = 0;
        s_verify_flags = 0;
        mbedtls_ssl_set_verify((mbedtls_ssl_context *)ctx, timing_verify_cb, NULL);
#endif
    }
}

#else // !TRUST_STORE_HEADER — stubs for use cases that don't need TLS

void tls_init(void) {}
struct altcp_tls_config *tls_get_client_config(void) { return NULL; }
void tls_apply_sni(struct altcp_pcb *pcb, const char *hostname) {
    (void)pcb;
    (void)hostname;
}

#if INKI_DEBUG_TIMING
uint32_t tls_timing_verify_done_ms(void) { return 0; }
int tls_timing_chain_length(void) { return 0; }
uint32_t tls_timing_verify_flags(void) { return 0; }
#endif

#endif // TRUST_STORE_HEADER
