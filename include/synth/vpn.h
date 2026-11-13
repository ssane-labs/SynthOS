#ifndef SYNTH_VPN_H
#define SYNTH_VPN_H

#include <synth/types.h>

struct vpn_status {
    char name[32];
    bool on, connected;
    uint32_t address, dns, endpoint;
    uint16_t port;
    uint32_t handshake_ago;
    uint64_t rx, tx;
    char host[128];
    char error[96];
};

/* Another kind of VPN (a module: OpenVPN) plugs its tunnel in here; one VPN is up at a time. routes gets IPv4
   destinations off the local network; routes6 global IPv6 ones; output takes whole IP packets; tick runs with the
   network's timers. Packets out of the tunnel go to vpn_tunnel_input. */
struct vpn_tunnel {
    K64(bool (*routes)(uint32_t dst));
    K64(bool (*routes6)(const uint8_t *dst));
    K64(int (*output)(const uint8_t *packet, size_t n));
    K64(void (*tick)(uint64_t now));
    uint32_t address, dns;
    uint8_t address6[16];
    bool has6, block6;
    uint16_t mtu;
};

void vpn_tunnel_set(const struct vpn_tunnel *t);
void vpn_tunnel_input(const uint8_t *packet, size_t n);

int vpn_up(const char *name, char *why, size_t cap);
void vpn_down(void);
void vpn_status(struct vpn_status *st);
int vpn_list(char names[][32], int max);
int vpn_import(const char *file, const char *name, char *out_name, char *why, size_t cap);
int vpn_remove(const char *name);

#endif
