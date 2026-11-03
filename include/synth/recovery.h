#ifndef SYNTH_RECOVERY_H
#define SYNTH_RECOVERY_H

#include <synth/types.h>

bool safe_mode(void);
void recovery_init(const char *cmdline);
void recovery_boot_ok(void);

#endif
