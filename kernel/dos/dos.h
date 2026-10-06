#ifndef SYNTH_DOS_DOS_H
#define SYNTH_DOS_DOS_H

#include <synth/exec.h>
#include <synth/vfs.h>

#include "cpu.h"

#define DOS_MAX_HANDLES 32
#define DOS_MAX_DEPTH 8
#define DOS_MAX_FINDS 8
#define DOS_FIRST_MCB 0x0100
#define DOS_MEM_TOP 0xa000
#define DOS_BIOS_SEG 0xf000
#define DOS_TIMER_STUB 0x0400
#define DOS_VIDEO_SEG 0xb800
#define DOS_COLS 80
#define DOS_ROWS 25
#define DOS_ENV_SIZE 0x20

enum dos_handle_kind {
    H_FREE,
    H_STDIN,
    H_STDOUT,
    H_STDERR,
    H_NUL,
    H_FILE,
};

struct dos_handle {
    enum dos_handle_kind kind;
    struct file *file;
    uint16_t owner;
};

struct dos_saved {
    uint16_t r[8];
    uint16_t s[4];
    uint16_t ip;
    uint16_t flags;
    uint16_t psp;
    uint16_t dta_seg;
    uint16_t dta_off;
};

struct dos_find {
    bool used;
    char dir[PATH_MAX_LEN];
    char pattern[16];
    uint8_t attr;
    uint32_t index;
};

struct dos {
    struct cpu cpu;
    struct exec_ctx *ctx;
    uint8_t *mem;
    bool running;
    uint16_t psp;
    uint16_t dta_seg;
    uint16_t dta_off;
    struct dos_handle handles[DOS_MAX_HANDLES];
    struct dos_saved stack[DOS_MAX_DEPTH];
    int depth;
    int exit_code;
    uint16_t return_code;
    uint16_t last_error;
    char cwd[PATH_MAX_LEN];
    struct dos_find finds[DOS_MAX_FINDS];
    bool screen;
    int ox, oy;
    uint8_t shadow[DOS_COLS * DOS_ROWS * 2];
    uint8_t baseline[DOS_COLS * DOS_ROWS * 2];
    uint64_t last_sync;
    uint8_t video_mode;
    bool graphics_warned;
    uint64_t start_ms;
    uint32_t base_ticks;
    uint32_t delivered;
    uint8_t last_scancode;
    uint8_t crtc_index;
    uint16_t keys[32];
    unsigned khead;
    unsigned ktail;
    int pending_scan;
    char inbuf[256];
    int inlen;
    int inpos;
    unsigned retrace;
    char message[96];
};

void dos_int21(struct dos *d);
void dos_bios(struct dos *d, uint8_t n);
void dos_video_init(struct dos *d);
void dos_video_sync(struct dos *d, bool force);
void dos_enter_screen(struct dos *d, bool clear);
void dos_video_finish(struct dos *d);
void dos_putc(struct dos *d, uint8_t ch);
void dos_puts(struct dos *d, const char *s);
int dos_read_key(struct dos *d, bool wait, bool peek);
void dos_flush_keys(struct dos *d);
int dos_line_input(struct dos *d, char *buf, int max);
void dos_out(struct dos *d, int handle_kind, uint8_t ch);
uint16_t dos_key_code(int key);
void dos_idle(struct dos *d);

void dos_set_cf(struct dos *d, bool on);
void dos_set_zf(struct dos *d, bool on);
void dos_fail(struct dos *d, uint16_t error);
void dos_ok(struct dos *d);

uint16_t mcb_alloc(struct dos *d, uint16_t paragraphs, uint16_t *largest);
bool mcb_free(struct dos *d, uint16_t seg);
bool mcb_resize(struct dos *d, uint16_t seg, uint16_t paragraphs, uint16_t *largest);
void mcb_free_owner(struct dos *d, uint16_t owner);

int dos_exec(struct dos *d, const char *path, const char *tail, uint16_t env, bool child);
void dos_terminate(struct dos *d, uint8_t code, uint8_t type);

int dos_resolve(struct dos *d, const char *dos_path, char *out, size_t cap);
void dos_read_string(struct dos *d, uint16_t seg, uint16_t off, char *out, size_t cap);
void dos_handles_init(struct dos *d);
void dos_handles_close_owner(struct dos *d, uint16_t owner);
void dos_handle_release(struct dos *d, int index);
int dos_load_overlay(struct dos *d, const uint8_t *data, size_t size, uint16_t seg, uint16_t reloc);
uint32_t dos_ticks(struct dos *d);

#endif
