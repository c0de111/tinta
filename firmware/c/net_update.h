#ifndef NET_UPDATE_H
#define NET_UPDATE_H

#include <stdbool.h>

// Firmware update over the network, started by buttons 3 + 4 (development builds with
// --exp-net-update, ID 206). Fetches the image for the slot that is not running from
// NET_UPDATE_HOST, writes it in pieces, checks it like a web upload and shuts down; the new
// firmware runs from the next wake. Every outcome is shown on the panel. Does not return.
void net_update_run(bool st25_present);

#endif
