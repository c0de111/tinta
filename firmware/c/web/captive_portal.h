#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

void captive_start_dhcp(void);

// The device that got an address last: its name as it announced it (DHCP option 12,
// printable ASCII only, empty if it sent none). NULL until a device has, and again after
// captive_forget_client().
const char *captive_client_name(void);
void captive_forget_client(void);
void captive_start_dns(void);

#endif
