#ifndef SYNTH_RECOVERY_H
#define SYNTH_RECOVERY_H

#include <synth/types.h>

bool safe_mode(void);
void recovery_init(const char *cmdline);
void recovery_boot_ok(void);
#define BOOT_NAME_MAX 40
int boot_systems(char (*names)[BOOT_NAME_MAX], int max);
bool boot_name_has(const char *name, const char *words);
int recovery_own_esp(int (*fn)(const char *root, void *ctx), void *ctx);

#endif
