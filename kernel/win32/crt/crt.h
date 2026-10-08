#ifndef SYNTH_WIN32_CRT_H
#define SYNTH_WIN32_CRT_H

#include "../win32.h"

#define CRT_EXPORTS(id, table) W32_EXPORTS(id, "msvcrt.dll", table)

enum {
    CRT_EPERM = 1,
    CRT_ENOENT = 2,
    CRT_EINTR = 4,
    CRT_EIO = 5,
    CRT_EBADF = 9,
    CRT_ENOMEM = 12,
    CRT_EACCES = 13,
    CRT_EEXIST = 17,
    CRT_EXDEV = 18,
    CRT_ENOTDIR = 20,
    CRT_EISDIR = 21,
    CRT_EINVAL = 22,
    CRT_EMFILE = 24,
    CRT_ENOSPC = 28,
    CRT_ESPIPE = 29,
    CRT_EDOM = 33,
    CRT_ERANGE = 34,
    CRT_ENOTEMPTY = 41,
    CRT_ENAMETOOLONG = 38,
    CRT_ENOSYS = 40,
};

enum {
    CRT_O_RDONLY = 0x0,
    CRT_O_WRONLY = 0x1,
    CRT_O_RDWR = 0x2,
    CRT_O_APPEND = 0x8,
    CRT_O_CREAT = 0x100,
    CRT_O_TRUNC = 0x200,
    CRT_O_EXCL = 0x400,
    CRT_O_TEXT = 0x4000,
    CRT_O_BINARY = 0x8000,
    CRT_O_WTEXT = 0x10000,
    CRT_O_U16TEXT = 0x20000,
    CRT_O_U8TEXT = 0x40000,
};

typedef struct crt_file {
    char *ptr;
    int cnt;
    char *base;
    int flag;
    int file;
    int charbuf;
    int bufsiz;
    char *tmpfname;
} CRT_FILE;

typedef void (*crt_sink)(void *ctx, const char *s, size_t n);
typedef void (*crt_wsink)(void *ctx, const WCHAR *s, size_t n);

extern int crt_errno_value;
extern int crt_fmode;
extern int crt_slots64;
extern CRT_FILE crt_iob[3];

void crt_set_errno(int e);
int crt_errno_from_vfs(int err);
void crt_stdio_reset(void);
void crt_flush_all(void);
void crt_close_all(void);

int crt_isatty(int fd);
long crt_fd_write(int fd, const void *buf, size_t n);
long crt_fd_read(int fd, void *buf, size_t n);
int crt_fputs_raw(CRT_FILE *f, const char *s, size_t n);
int crt_fputc(int c, CRT_FILE *f);
int crt_fgetc(CRT_FILE *f);
int crt_ungetc(int c, CRT_FILE *f);
int crt_fflush(CRT_FILE *f);
void crt_stream_sink(void *ctx, const char *s, size_t n);
void crt_stream_done(CRT_FILE *f);
void crt_lock_file(CRT_FILE *f);
void crt_unlock_file(CRT_FILE *f);

int crt_vformat(crt_sink sink, void *ctx, const char *fmt, va_list ap);
int crt_vwformat(crt_wsink sink, void *ctx, const WCHAR *fmt, va_list ap);
int crt_vscan(int (*get)(void *), void (*unget)(int, void *), void *ctx, const char *fmt, va_list ap);

long crt_strtol(const char *s, char **end, int base);
unsigned long crt_strtoul(const char *s, char **end, int base);
int64_t crt_strtoi64(const char *s, char **end, int base);
uint64_t crt_strtoui64(const char *s, char **end, int base);

void crt_exit(int code);
void crt_time_reset(void);

#endif
