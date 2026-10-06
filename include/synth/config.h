#ifndef SYNTH_CONFIG_H
#define SYNTH_CONFIG_H

#define SYNTH_NAME "SynthOS"
#define SYNTH_VERSION "0.1.0"
#define SYNTH_CODENAME "Neon"

#ifndef CONFIG_DOS
#define CONFIG_DOS 1
#endif

#ifndef CONFIG_SXVM
#define CONFIG_SXVM 1
#endif

#ifndef CONFIG_EDITOR
#define CONFIG_EDITOR 1
#endif

#ifndef CONFIG_HISTORY
#define CONFIG_HISTORY 128
#endif

#ifndef CONFIG_LINE_MAX
#define CONFIG_LINE_MAX 1024
#endif

#ifndef CONFIG_MAX_COLS
#define CONFIG_MAX_COLS 320
#endif

#ifndef CONFIG_MAX_ROWS
#define CONFIG_MAX_ROWS 120
#endif

#ifndef CONFIG_LOG_SIZE
#define CONFIG_LOG_SIZE 4096
#endif

#ifndef CONFIG_HOSTNAME
#define CONFIG_HOSTNAME "synth"
#endif

#ifndef CONFIG_USER
#define CONFIG_USER "root"
#endif

#endif
