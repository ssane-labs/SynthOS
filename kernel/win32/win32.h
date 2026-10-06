#ifndef SYNTH_WIN32_INTERNAL_H
#define SYNTH_WIN32_INTERNAL_H

#include <synth/arch_win32.h>
#include <synth/exec.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/vfs.h>

#define WINAPI __attribute__((stdcall))
#define WINAPIV __attribute__((cdecl))

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int32_t BOOL;
typedef uint32_t UINT;
typedef int32_t INT;
typedef uint16_t WCHAR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef uint32_t SIZE_T;
typedef uint32_t ULONG_PTR;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef char *LPSTR;
typedef const char *LPCSTR;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define W32_PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define STATUS_ACCESS_VIOLATION 0xc0000005u
#define STATUS_CONTROL_C_EXIT 0xc000013au
#define STATUS_DLL_NOT_FOUND 0xc0000135u
#define STATUS_ENTRYPOINT_NOT_FOUND 0xc0000139u
#define W32_PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

enum {
    ERROR_SUCCESS = 0,
    ERROR_INVALID_FUNCTION = 1,
    ERROR_FILE_NOT_FOUND = 2,
    ERROR_PATH_NOT_FOUND = 3,
    ERROR_TOO_MANY_OPEN_FILES = 4,
    ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6,
    ERROR_NOT_ENOUGH_MEMORY = 8,
    ERROR_INVALID_DATA = 13,
    ERROR_OUTOFMEMORY = 14,
    ERROR_NO_MORE_FILES = 18,
    ERROR_WRITE_FAULT = 29,
    ERROR_HANDLE_EOF = 38,
    ERROR_NOT_SUPPORTED = 50,
    ERROR_FILE_EXISTS = 80,
    ERROR_INVALID_PARAMETER = 87,
    ERROR_BROKEN_PIPE = 109,
    ERROR_CALL_NOT_IMPLEMENTED = 120,
    ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_INVALID_NAME = 123,
    ERROR_MOD_NOT_FOUND = 126,
    ERROR_PROC_NOT_FOUND = 127,
    ERROR_DIR_NOT_EMPTY = 145,
    ERROR_ALREADY_EXISTS = 183,
    ERROR_ENVVAR_NOT_FOUND = 203,
    ERROR_NO_MORE_ITEMS = 259,
    ERROR_NO_UNICODE_TRANSLATION = 1113,
};

struct w32_export {
    const char *name;
    void *address;
};

struct w32_module_def {
    const char *name;
    const struct w32_export *exports;
    size_t count;
    void (*attach)(void);
};

#define W32_FN(fn) {#fn, (void *)fn}
#define W32_AS(name, fn) {name, (void *)fn}
#define W32_MODULE(object) SYNTH_REGISTER(synth_win32, struct w32_module_def, object)
#define W32_EXPORTS(id, dll, table, ...)                                                                          \
    static const struct w32_module_def id = {.name = dll, .exports = table, .count = ARRAY_SIZE(table), __VA_ARGS__}; \
    W32_MODULE(id)

enum w32_object_type {
    WH_FREE,
    WH_STDIN,
    WH_STDOUT,
    WH_STDERR,
    WH_FILE,
    WH_FIND,
    WH_EVENT,
    WH_MUTEX,
    WH_SEMAPHORE,
    WH_NULL,
};

struct w32_find {
    char dir[PATH_MAX_LEN];
    char pattern[NAME_MAX_LEN];
    uint32_t index;
};

struct w32_find_entry {
    char name[NAME_MAX_LEN];
    uint8_t type;
    uint32_t size;
    uint32_t mtime;
};

struct w32_object {
    enum w32_object_type type;
    int refs;
    struct file *file;
    struct w32_find *find;
    uint32_t value;
    bool append;
};

struct w32_image {
    char name[64];
    char path[PATH_MAX_LEN];
    uint32_t base;
    uint32_t size;
    uint32_t entry;
    void *memory;
    const struct w32_module_def *builtin;
    bool dll;
    bool initialized;
    int refs;
    struct w32_image *next;
};

#define W32_MAX_HANDLES 256
#define W32_TLS_SLOTS 64
#define W32_ATEXIT_MAX 64

struct w32_process {
    struct exec_ctx *ctx;
    struct heap heap;
    void *arena;
    uint32_t arena_base;
    uint32_t arena_size;
    uint8_t *teb;
    uint8_t *peb;
    uint8_t *stack;
    uint32_t stack_size;
    struct w32_image *images;
    struct w32_image *main;
    struct w32_object handles[W32_MAX_HANDLES];
    struct w32_jump jump;
    bool exiting;
    uint32_t exit_code;
    char cwd[PATH_MAX_LEN];
    char *cmdline;
    WCHAR *cmdline_w;
    char *env_block;
    WCHAR *env_block_w;
    int argc;
    char **argv;
    WCHAR **wargv;
    char **environ;
    WCHAR **wenviron;
    bool tls_used[W32_TLS_SLOTS];
    void (*atexit_fns[W32_ATEXIT_MAX])(void);
    int atexit_count;
    uint8_t console_attr;
    uint8_t default_attr;
    int output_cp;
    int input_cp;
    DWORD input_mode;
    DWORD output_mode;
    char message[160];
};

extern struct w32_process *w32;

void w32_set_error(DWORD error);
DWORD w32_error(void);
DWORD w32_error_from_vfs(int err);

void *w32_alloc(size_t size);
void *w32_zalloc(size_t size);
void *w32_realloc(void *ptr, size_t size);
void w32_free(void *ptr);
size_t w32_msize(void *ptr);
char *w32_strdup(const char *s);

HANDLE w32_handle_new(enum w32_object_type type);
struct w32_object *w32_handle_get(HANDLE h);
HANDLE w32_std_handle(int which);
bool w32_handle_close(HANDLE h);

int w32_path(const char *win, char *out, size_t cap);
void w32_path_to_win(const char *vfs_path, char *out, size_t cap);

int w32_acp(void);
size_t w32_wide_to_cp(const WCHAR *w, int wlen, char *out, size_t cap, int codepage);
size_t w32_cp_to_wide(const char *s, int len, WCHAR *out, size_t cap, int codepage);
char *w32_narrow(const WCHAR *w);
WCHAR *w32_widen(const char *s);
size_t w32_wcslen(const WCHAR *w);

HANDLE w32_create_file(const char *name, DWORD access, DWORD disposition);
HANDLE w32_find_first(const char *spec, struct w32_find_entry *e);
bool w32_find_next(HANDLE h, struct w32_find_entry *e);
long w32_handle_read(HANDLE h, void *buf, size_t n);
long w32_handle_write(HANDLE h, const void *buf, size_t n);

void w32_console_write(int handle_kind, const char *s, size_t n);
void w32_console_write_raw(int handle_kind, const char *s, size_t n);
int w32_console_getkey(bool wait);
bool w32_key_info(int key, WORD *vk, WORD *scan, WCHAR *ch, DWORD *state);
const char *w32_getenv(const char *name);
int w32_putenv(const char *name, const char *value);
WCHAR w32_towupper(WCHAR c);
WCHAR w32_towlower(WCHAR c);
long w32_console_read(char *buf, size_t n);

struct w32_image *w32_find_module(const char *name);
struct w32_image *w32_load(const char *name, const char *search_dir, int *error);
void *w32_get_proc(struct w32_image *image, const char *name, uint32_t ordinal);
int w32_init_modules(void);
void w32_unload_all(void);

NORETURN void w32_exit(uint32_t code);
void w32_sleep(uint32_t ms);
void w32_run_atexit(void);
uint32_t w32_call(void *fn, int argc, const uint32_t *args);

#endif
