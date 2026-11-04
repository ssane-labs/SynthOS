#ifndef SYNTH_SBSIGN_H
#define SYNTH_SBSIGN_H

#include <synth/types.h>

#define SB_KEY_DIR "/home/.secureboot"

int sb_cert(const uint8_t *efi, size_t len, const uint8_t **cert, size_t *cert_len);
bool sb_has_key(void);
bool sb_key_created(void);
bool sb_resign_needed(const char *installed, const uint8_t *efi, size_t len);
int sb_sign_file(const char *path);
int sb_save_cert(const char *path);
int sb_sign_esp(const char *root, bool windows, bool shim);

#endif
