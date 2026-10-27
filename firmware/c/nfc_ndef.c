#include "nfc_ndef.h"

#include "debug.h"
#include "st25_io.h"

#include <string.h>

#define T5T_MAGIC 0xE1u
#define T5T_VERSION_1_0_RW 0x40u
#define TLV_NDEF 0x03u
#define TLV_TERMINATOR 0xFEu
#define RECORD_SHORT_SINGLE_WELL_KNOWN 0xD1u // MB, ME, SR, TNF = well-known
#define URI_HTTP 0x03u
#define URI_HTTPS 0x04u

// The address must end before the request slot in the last 16 bytes of the smallest tag.
_Static_assert(NFC_NDEF_AREA_LEN + 16u <= ST25_SMALLEST_MEM_BYTES,
               "address area runs into the request slot");

size_t nfc_ndef_encode_url(const char *url, uint8_t *out, size_t out_size) {
    uint8_t prefix;
    const char *rest;
    if (strncmp(url, "https://", 8) == 0) {
        prefix = URI_HTTPS;
        rest = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        prefix = URI_HTTP;
        rest = url + 7;
    } else {
        return 0;
    }

    const size_t rest_len = strlen(rest);
    const size_t payload_len = 1 + rest_len;   // prefix code + remainder
    const size_t record_len = 4 + payload_len; // header, type length, payload length, type
    const size_t total = 4 + 2 + record_len + 1;
    if (rest_len == 0 || total > NFC_NDEF_AREA_LEN || total > out_size)
        return 0;

    uint8_t *p = out;
    *p++ = T5T_MAGIC;
    *p++ = T5T_VERSION_1_0_RW;
    *p++ = (uint8_t)((NFC_NDEF_AREA_LEN - 4u) / 8u); // size of the data area in 8-byte units
    *p++ = 0x00;                                     // no optional commands claimed
    *p++ = TLV_NDEF;
    *p++ = (uint8_t)record_len;
    *p++ = RECORD_SHORT_SINGLE_WELL_KNOWN;
    *p++ = 1;
    *p++ = (uint8_t)payload_len;
    *p++ = 'U';
    *p++ = prefix;
    memcpy(p, rest, rest_len);
    p += rest_len;
    *p++ = TLV_TERMINATOR;
    return (size_t)(p - out);
}

bool nfc_ndef_holds_url(const char *url) {
    uint8_t want[NFC_NDEF_AREA_LEN];
    uint8_t have[NFC_NDEF_AREA_LEN];
    const size_t len = nfc_ndef_encode_url(url, want, sizeof(want));
    return len != 0 && st25_read_bytes(0, have, (uint16_t)len) && memcmp(want, have, len) == 0;
}

nfc_ndef_result_t nfc_ndef_set_url(const char *url) {
    uint8_t want[NFC_NDEF_AREA_LEN];
    const size_t len = nfc_ndef_encode_url(url, want, sizeof(want));
    if (len == 0)
        return NFC_NDEF_FAILED;
    uint8_t have[NFC_NDEF_AREA_LEN];
    if (!st25_read_bytes(0, have, (uint16_t)len))
        return NFC_NDEF_NO_TAG;
    if (memcmp(want, have, len) == 0)
        return NFC_NDEF_UNCHANGED;
    return st25_write_bytes(0, want, (uint16_t)len) ? NFC_NDEF_WRITTEN : NFC_NDEF_FAILED;
}

bool nfc_ndef_present(void) {
    uint8_t head[7];
    return st25_read_bytes(0, head, sizeof(head)) && head[0] == T5T_MAGIC && head[4] == TLV_NDEF &&
           head[6] == RECORD_SHORT_SINGLE_WELL_KNOWN;
}

void nfc_ndef_offer_url(const char *url, int page) {
    if (!url)
        return;
    const nfc_ndef_result_t r = nfc_ndef_set_url(url);
    if (r == NFC_NDEF_WRITTEN)
        debug_status("OK", "NFC: address for page %d written\n", page);
    else if (r == NFC_NDEF_FAILED)
        debug_status("WARN", "NFC: address for page %d not written\n", page);
}
