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

int vpn_up(const char *name, char *why, size_t cap);
void vpn_down(void);
void vpn_status(struct vpn_status *st);
int vpn_list(char names[][32], int max);
int vpn_import(const char *file, const char *name, char *out_name, char *why, size_t cap);
int vpn_remove(const char *name);

#endif
