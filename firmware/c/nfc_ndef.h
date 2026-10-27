#ifndef NFC_NDEF_H
#define NFC_NDEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One web address at the start of the tag, in the form every phone opens by itself
// (NFC Forum Type 5: capability container, then one NDEF URI record).
//
// The area overlaps what the Android app writes (see the map in st25_io.h). The address is
// therefore put back whenever it is missing or damaged, see nfc_ndef_set_url().
//
// 4 + 31 x 8 bytes: an address of up to 248 characters, a Seatsurfing booking link for three
// spaces. Beyond that the one-byte lengths of a short record run out.
#define NFC_NDEF_AREA_LEN 252u

typedef enum {
    NFC_NDEF_UNCHANGED, // the tag already holds this address
    NFC_NDEF_WRITTEN,
    NFC_NDEF_NO_TAG, // nothing answers at the tag's address
    NFC_NDEF_FAILED, // address not http(s) or too long, or the write did not go through
} nfc_ndef_result_t;

// Encodes url into out. Returns the number of bytes, or 0 if the address does not start
// with http:// or https:// or does not fit into NFC_NDEF_AREA_LEN.
size_t nfc_ndef_encode_url(const char *url, uint8_t *out, size_t out_size);

// Makes the tag offer url. Reads first and writes only on a difference, so calling it on
// every wake costs no EEPROM cycles.
nfc_ndef_result_t nfc_ndef_set_url(const char *url);

// Keeps the tag offering url (NULL: leave the tag as it is) and logs the outcome; page only
// names the log line. The address itself stays out of the log: it may carry a token.
void nfc_ndef_offer_url(const char *url, int page);

// True if the tag offers exactly this address.
bool nfc_ndef_holds_url(const char *url);

// True if the tag starts with an address record of the kind written here. A phone that reads
// it wakes the sign without leaving a request; this tells such a tap from stray RF.
bool nfc_ndef_present(void);

#endif
