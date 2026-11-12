#ifndef SYNTH_MODULE_H
#define SYNTH_MODULE_H

#include <synth/types.h>

struct command;
struct picture;

/* What a module (/system/apps/<name>.sxm) adds besides an app: shell commands and decoders for file formats the
   kernel does not know. Each one is optional. */
struct module_ext {
    const struct command *const *commands;
    int command_count;
    const char *picture_exts; /* ".webp .tif .tiff" */
    int (*picture_decode)(const uint8_t *data, size_t size, struct picture *out);
    const char *audio_exts;
    /* interleaved 16-bit stereo, kmalloc'd */
    int (*audio_decode)(const uint8_t *data, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate);
};

/* weak: absent from builds without modules */
__attribute__((weak)) const struct command *module_command_find(const char *name);
__attribute__((weak)) void module_command_foreach(void (*fn)(const struct command *cmd, void *ctx), void *ctx);
__attribute__((weak)) int module_picture_decode(const uint8_t *data, size_t size, struct picture *out);
__attribute__((weak)) int module_audio_decode(const uint8_t *data, size_t size, int16_t **frames, uint32_t *count,
                                              uint32_t *rate);
__attribute__((weak)) bool module_handles(const char *name, bool audio);

#endif
