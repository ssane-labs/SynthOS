#ifndef SYNTH_UPDATE_H
#define SYNTH_UPDATE_H

#include <synth/types.h>

struct update_file {
    char name[56];
    uint32_t size;
    const uint8_t *data;
};

struct update_pkg {
    int arch, count;
    char build[49];
    struct update_file files[32];
};

struct update_status {
    bool available;
    char build[49];
    char sha256[72];
    size_t size;
    char message[128];
};

int update_parse(const uint8_t *data, size_t size, struct update_pkg *pkg, char *why, size_t cap);
const struct update_file *update_file(const struct update_pkg *pkg, const char *name);
int update_installed_kernel(uint32_t size, uint8_t out[32]);
int update_apply(const struct update_pkg *pkg, char *msg, size_t cap);
int update_install(const uint8_t *data, size_t size, char *msg, size_t cap);
int update_check(struct update_status *st, void (*progress)(void *, const char *, size_t, size_t), void *ctx);
int update_download(const struct update_status *st, uint8_t **data, size_t *size,
                    void (*progress)(void *, const char *, size_t, size_t), void *ctx, char *msg, size_t cap);

#endif
