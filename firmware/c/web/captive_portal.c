#include "captive_portal.h"
#include "debug.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"

static const uint8_t s_dhcp_offer[] = {
    0x02, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     0x00,      0x00,      0x00,      0,
    0,    0,    0,    192,  168,  4,    100,  192,  168,      4,         1,         0x00,      0x00,
    0x00, 0x00, 0,    0,    0,    0,    0,    0,    0,        0,         0,         0,         0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,        0,         0,         0,         0,
    0,    0,    0,    0,    0,    0,    0,    0,    [44] = 0, [107] = 0, [108] = 0, [235] = 0, 99,
    130,  83,   99,   53,   1,    2,    54,   4,    192,      168,       4,         1,         51,
    4,    0x00, 0x01, 0x51, 0x80, 58,   4,    0x00, 0x00,     0x01,      0x2C,      59,        4,
    0x00, 0x00, 0x01, 0xE0, 1,    4,    255,  255,  255,      0,         3,         4,         192,
    168,  4,    1,    6,    4,    192,  168,  4,    1,        15,        4,         'i',       'n',
    'k',  'i',  119,  5,    4,    'i',  'n',  'k',  'i',      0,         255};

static const uint8_t s_dhcp_ack[] = {
    0x02, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     0x00,      0x00,      0x00,      0,
    0,    0,    0,    192,  168,  4,    100,  192,  168,      4,         1,         0x00,      0x00,
    0x00, 0x00, 0,    0,    0,    0,    0,    0,    0,        0,         0,         0,         0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,        0,         0,         0,         0,
    0,    0,    0,    0,    0,    0,    0,    0,    [44] = 0, [107] = 0, [108] = 0, [235] = 0, 99,
    130,  83,   99,   53,   1,    5,    54,   4,    192,      168,       4,         1,         51,
    4,    0x00, 0x01, 0x51, 0x80, 58,   4,    0x00, 0x00,     0x01,      0x2C,      59,        4,
    0x00, 0x00, 0x01, 0xE0, 1,    4,    255,  255,  255,      0,         3,         4,         192,
    168,  4,    1,    6,    4,    192,  168,  4,    1,        15,        4,         'i',       'n',
    'k',  'i',  119,  5,    4,    'i',  'n',  'k',  'i',      0,         255};

// Each device keeps its own address, 192.168.4.100 to .109, for as long as the setup runs.
// More than ten devices reuse the oldest addresses.
#define DHCP_FIRST_HOST 100
#define DHCP_HOSTS 10
static uint8_t s_lease_mac[DHCP_HOSTS][6];
static int s_leases_given;

static uint8_t dhcp_host_for(const uint8_t *mac) {
    const int known = s_leases_given < DHCP_HOSTS ? s_leases_given : DHCP_HOSTS;
    for (int i = 0; i < known; i++)
        if (memcmp(s_lease_mac[i], mac, 6) == 0)
            return (uint8_t)(DHCP_FIRST_HOST + i);
    const int i = s_leases_given++ % DHCP_HOSTS;
    memcpy(s_lease_mac[i], mac, 6);
    return (uint8_t)(DHCP_FIRST_HOST + i);
}

static char s_client_name[33];
static bool s_client_known;

const char *captive_client_name(void) { return s_client_known ? s_client_name : NULL; }
void captive_forget_client(void) { s_client_known = false; }

// Walks the options after the magic cookie. Returns the message type (option 53) and copies
// the host name (option 12) into name, keeping printable ASCII only.
static uint8_t dhcp_read_options(const uint8_t *pkt, size_t len, char *name, size_t name_size) {
    uint8_t msg_type = 0;
    name[0] = '\0';
    size_t i = 240;
    while (i < len) {
        const uint8_t code = pkt[i++];
        if (code == 0)
            continue;
        if (code == 255 || i >= len)
            break;
        const uint8_t olen = pkt[i++];
        if (i + olen > len)
            break;
        if (code == 53 && olen == 1) {
            msg_type = pkt[i];
        } else if (code == 12) {
            size_t n = 0;
            for (uint8_t k = 0; k < olen && n + 1 < name_size; k++)
                if (pkt[i + k] >= 0x20 && pkt[i + k] <= 0x7e)
                    name[n++] = (char)pkt[i + k];
            name[n] = '\0';
        }
        i += olen;
    }
    return msg_type;
}

static void captive_dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                              u16_t port) {
    if (!p || p->len < 240) {
        if (p)
            pbuf_free(p);
        return;
    }
    const uint8_t *req = (const uint8_t *)p->payload;
    char name[sizeof(s_client_name)];
    const uint8_t msg_type = dhcp_read_options(req, p->len, name, sizeof(name));
    const uint8_t *tmpl = NULL;
    size_t tmpl_len = 0;
    if (msg_type == 1) {
        tmpl = s_dhcp_offer;
        tmpl_len = sizeof(s_dhcp_offer);
    } else if (msg_type == 3) {
        tmpl = s_dhcp_ack;
        tmpl_len = sizeof(s_dhcp_ack);
        const uint8_t *mac = req + 28;
        if (!s_client_known || strcmp(name, s_client_name) != 0)
            debug_status("OK", "DHCP: \"%s\" (%02X:%02X:%02X:%02X:%02X:%02X) joined as .%u\n",
                         name[0] ? name : "no name", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                         dhcp_host_for(mac));
        memcpy(s_client_name, name, sizeof(s_client_name));
        s_client_known = true;
    } else {
        pbuf_free(p);
        return;
    }
    uint8_t resp[300] = {0};
    memcpy(resp, tmpl, tmpl_len);
    memcpy(resp + 4, req + 4, 4);
    memcpy(resp + 28, req + 28, 16);
    resp[19] = dhcp_host_for(req + 28); // yiaddr, last byte
    struct pbuf *rb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)tmpl_len, PBUF_RAM);
    if (!rb) {
        pbuf_free(p);
        return;
    }
    memcpy(rb->payload, resp, tmpl_len);
    udp_sendto(pcb, rb, addr, port);
    pbuf_free(rb);
    pbuf_free(p);
}

void captive_start_dhcp(void) {
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb)
        return;
    if (udp_bind(pcb, IP_ADDR_ANY, 67) != ERR_OK) {
        udp_remove(pcb);
        return;
    }
    udp_recv(pcb, captive_dhcp_recv, NULL);
}

static bool dns_qname_lower(const uint8_t *pkt, size_t len, size_t *pos, char *out, size_t olen) {
    size_t in = *pos, oi = 0;
    while (in < len) {
        uint8_t ll = pkt[in++];
        if (ll == 0) {
            out[oi] = '\0';
            *pos = in;
            return true;
        }
        if ((ll & 0xC0) || in + ll > len)
            return false;
        if (oi > 0) {
            if (oi + 1 >= olen)
                return false;
            out[oi++] = '.';
        }
        for (uint8_t i = 0; i < ll; i++) {
            if (oi + 1 >= olen)
                return false;
            char c = (char)pkt[in++];
            out[oi++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    return false;
}

static void captive_dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                             u16_t port) {
    (void)arg;
    if (!p || p->tot_len < 12 || p->tot_len > 512) {
        if (p)
            pbuf_free(p);
        return;
    }
    uint8_t req[512];
    pbuf_copy_partial(p, req, p->tot_len, 0);
    size_t req_len = p->tot_len;
    uint16_t flags = (uint16_t)((req[2] << 8) | req[3]);
    uint16_t qdcount = (uint16_t)((req[4] << 8) | req[5]);
    if ((flags & 0x8000u) || !qdcount) {
        pbuf_free(p);
        return;
    }
    size_t qpos = 12;
    char qname[128];
    if (!dns_qname_lower(req, req_len, &qpos, qname, sizeof(qname)) || qpos + 4 > req_len) {
        pbuf_free(p);
        return;
    }
    uint16_t qtype = (uint16_t)((req[qpos] << 8) | req[qpos + 1]);
    uint16_t qclass = (uint16_t)((req[qpos + 2] << 8) | req[qpos + 3]);
    size_t qend = qpos + 4;
    bool match = (strcmp(qname, "inki-setup") == 0 || strcmp(qname, "inki-setup.local") == 0 ||
                  (strncmp(qname, "inki-setup.", 11) == 0 && qname[11] != '\0'));
    bool add_ans = match && (qtype == 1 || qtype == 255) && (qclass == 1 || qclass == 255);
    uint16_t rcode = match ? 0u : 3u;
    uint8_t resp[512];
    memcpy(resp, req, qend);
    uint16_t rf = (uint16_t)(0x8400u | (flags & 0x0100u) | (rcode & 0x000Fu));
    resp[2] = (uint8_t)(rf >> 8);
    resp[3] = (uint8_t)rf;
    resp[4] = 0;
    resp[5] = 1;
    resp[6] = 0;
    resp[7] = add_ans ? 1 : 0;
    resp[8] = 0;
    resp[9] = 0;
    resp[10] = 0;
    resp[11] = 0;
    size_t off = qend;
    if (add_ans) {
        resp[off++] = 0xC0;
        resp[off++] = 0x0C;
        resp[off++] = 0x00;
        resp[off++] = 0x01;
        resp[off++] = 0x00;
        resp[off++] = 0x01;
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        resp[off++] = 0x3C;
        resp[off++] = 0x00;
        resp[off++] = 0x04;
        resp[off++] = 192;
        resp[off++] = 168;
        resp[off++] = 4;
        resp[off++] = 1;
    }
    struct pbuf *rb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)off, PBUF_RAM);
    if (!rb) {
        pbuf_free(p);
        return;
    }
    memcpy(rb->payload, resp, off);
    udp_sendto(pcb, rb, addr, port);
    pbuf_free(rb);
    pbuf_free(p);
}

void captive_start_dns(void) {
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb)
        return;
    if (udp_bind(pcb, IP_ADDR_ANY, 53) != ERR_OK) {
        udp_remove(pcb);
        return;
    }
    udp_recv(pcb, captive_dns_recv, NULL);
}
