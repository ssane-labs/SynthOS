#ifndef SYNTH_WIN32_INTERNAL_H
#define SYNTH_WIN32_INTERNAL_H

#include <synth/arch_win32.h>
#include <synth/exec.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/vfs.h>

#ifdef __x86_64__
/* on the 64-bit kernel this is x32 code (4-byte pointers, as 32-bit programs have them); their calls reach these
   functions through a gate that turns stack arguments into register ones */
#define GPTR
#define WINAPI
#define WINAPIV
#else
#define GPTR
#define WINAPI __attribute__((stdcall))
#define WINAPIV __attribute__((cdecl))
#endif

/* a function address in a program's memory (tables of them, callbacks stored by programs) */
typedef uint32_t GADDR;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int32_t BOOL;
typedef uint32_t UINT;
typedef int32_t INT;
typedef uint16_t WCHAR;
typedef void *GPTR HANDLE;
typedef void *GPTR HMODULE;
typedef uint32_t SIZE_T;
typedef uint32_t ULONG_PTR;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef WCHAR *GPTR LPWSTR;
typedef const WCHAR *GPTR LPCWSTR;
typedef char *GPTR LPSTR;
typedef const char *GPTR LPCSTR;

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
    const char *const *ordinals;
    size_t ordinal_count;
};

#define W32_FN(fn) {#fn, (void *)fn}
#define W32_AS(name, fn) {name, (void *)fn}
#define W32_STATE __attribute__((section("w32state")))
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
    WH_THREAD,
    WH_PROCESS,
    WH_TIMER,
    WH_PIPE,
    WH_PORT,
    WH_SECTION,
    WH_TOKEN,
    WH_KEY,
    WH_JOB,
};

enum w32_engine {
    W32_ENGINE_NONE,
    W32_ENGINE_CHROMIUM,
    W32_ENGINE_GECKO,
};

/* The folder's names as they were at FindFirstFile: deleting while enumerating skips nothing. */
struct w32_find {
    char dir[PATH_MAX_LEN];
    char pattern[NAME_MAX_LEN];
    uint32_t index, count;
    char **names;
};

struct w32_find_entry {
    char name[NAME_MAX_LEN];
    uint8_t type;
    uint32_t size;
    uint32_t mtime;
};

struct w32_thread;

struct w32_object {
    enum w32_object_type type;
    int refs;
    struct file *file;
    struct w32_find *find;
    uint32_t value;
    bool append;
    bool manual;
    bool delete_on_close;
    uint32_t owner;
    uint32_t max;
    struct w32_thread *thread;
    uint64_t due;
    char *path;
    bool inherit;
    bool skip_port;
    uint32_t access;
    HANDLE io_port;
    uintptr_t io_key;
    struct w32_pipe *pipe;
    struct w32_pipe *pipe_out;
    struct w32_npipe *npipe;
    struct w32_port *port;
    struct w32_section *section;
    struct w32_child *child;
    uint32_t name_serial;
    struct w32_sync *sync;
    struct reg_key *key;
};

/* the state of an event, mutex or semaphore once a handle to it is in more than one process */
struct w32_sync {
    int refs;
    uint32_t value, owner;
};

static inline uint32_t *w32_sync_value(struct w32_object *o)
{
    return o->sync ? &o->sync->value : &o->value;
}

static inline uint32_t *w32_sync_owner(struct w32_object *o)
{
    return o->sync ? &o->sync->owner : &o->owner;
}

struct w32_child {
    int refs;
    bool done;
    uint32_t exit_code;
    uint32_t pid;
    struct w32_process *proc;
};

/* What a blocked thread waits for, so the scheduler can tell without switching to it whether it can go on. */
enum { W32_WAIT_ANY, W32_WAIT_TIME, W32_WAIT_OBJECTS, W32_WAIT_WORD, W32_WAIT_PROCESS };

struct w32_object;

struct w32_wait {
    uint8_t kind, count;
    bool all, equal;
    uint64_t deadline;
    struct w32_object *objects[16];
    const volatile uint32_t *addr;
    uint32_t mask, value;
    bool net;
    bool (*ready)(const void *arg);
    const void *arg;
    uint32_t seen_net, seen_kick, site;
    uint64_t since;
    const volatile void *on;
};

struct w32_thread {
    uint32_t esp;
    uint32_t id;
    uint8_t *teb;
    uint8_t *stack;
    uint32_t stack_size;
    void *start;
    void *param;
    bool done;
    bool waiting;
    int suspended;
    uint32_t wait_caller, wait_sp;
    uint32_t seen, seen_local, user_eip, late_site, late_caller;
    struct w32_wait wait;
    const char *recent[8];
    unsigned recent_pos;
    int irq_depth;
    uint32_t exit_code;
    uint8_t *teb64;
    void *fls[64];
    uint8_t fpu_area[528];
    struct w32_process *proc;
    bool boot;
    struct w32_fiber *fiber;
    uint8_t com_apartment;
    int com_inits;
    void *cxx_object, *cxx_type;
    struct w32_thread *next;
};

struct w32_image {
    char name[64];
    char path[PATH_MAX_LEN];
    uint32_t base;
    uint32_t size;
    uint32_t entry;
    void *memory;
    const struct w32_module_def *builtin;
    bool pe64;
    bool dll;
    bool initialized;
    int refs;
    struct image_cache *cache;
    struct w32_image *next;
};

#define W32_TLS_MODULES 32
#define INFINITE 0xffffffffu

struct w32_tls_module {
    uint32_t base, start, size, zero, callbacks;
};

#define W32_MAX_HANDLES 2048
#define W32_TLS_SLOTS 1088
#define W32_TLS_TEB_SLOTS 64
#define W32_ATEXIT_MAX 64

struct vmm_space;

struct w32_process {
    struct exec_ctx *ctx;
    bool kicked;
    uint32_t signal_gen, kick_gen, io_gen, io_seen, net_seen;
    bool net_watch;
    char tag[20];
    enum w32_engine engine;
    struct vmm_space *space;
    struct heap heap;
    void *arena;
    uint32_t *arena_frames;
    uint32_t arena_base;
    uint32_t arena_size;
    uint8_t *teb;
    uint8_t *peb;
    uint8_t *teb64;
    uint8_t *peb64;
    void *w64;
    void *w64_thunks;
    bool win64;
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
    struct w32_thread *threads;
    struct w32_thread *current;
    int live_threads;
    uint32_t next_tid;
    struct w32_tls_module tls_mods[W32_TLS_MODULES];
    int tls_mod_count;
    struct w32_vm *vm;
    struct w32_process *ring_next;
    struct w32_process *parent;
    uint32_t parent_pid;
    uint8_t *state;
    struct w32_child *child;
    uint8_t *boot_stack;
    struct w32_thread *boot;
    struct exec_ctx *spawn_ctx;
    unsigned own_streams;
    bool dead;
    bool frozen;
    bool detached;
};

extern struct w32_process *w32;

void w32_process_enter(struct w32_process *p);
struct w32_process *w32_ring_next(struct w32_process *p);
void w32_reap(void);
HANDLE w32_bitmap_from_dib(const void *dib, uint32_t size);
struct w32_child *w32_spawn_async(struct exec_ctx *ctx, unsigned own);
bool w32_is_host(const struct w32_process *p);
struct w32_thread *w32_host_thread(void);
void w32_child_release(struct w32_child *c);
void w32_terminate_child(struct w32_child *c, uint32_t code);
void w32_thread_frame(struct w32_thread *t, void (*entry)(void), uint8_t *stack_top);
void w32_switch_thread(struct w32_thread *t);
extern bool w32_trace;
void w32_trace_dump(void);
void w32_trace_note(const char *module, const char *name);
extern bool w32_headless;
bool w32_vm_committed(uintptr_t address);
bool w32_vm_overlaps(uint32_t base, uint32_t size);

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent; in 64-bit programs the first two and the event are
   8 bytes wide */
static inline void w32_ov_set(uint32_t *ov, uint32_t status, uint32_t bytes)
{
    ov[0] = status;
    if (w32->win64) {
        ov[1] = 0;
        ov[2] = bytes;
        ov[3] = 0;
    } else {
        ov[1] = bytes;
    }
}

static inline uint32_t w32_ov_bytes(const uint32_t *ov)
{
    return w32->win64 ? ov[2] : ov[1];
}

static inline uint32_t w32_ov_offset(const uint32_t *ov)
{
    return w32->win64 ? ov[4] : ov[2];
}

static inline uint32_t w32_ov_event(const uint32_t *ov)
{
    return w32->win64 ? ov[6] : ov[4];
}
uint32_t w32_vm_reserve(uint32_t size);
void w32_vm_set_committed(uint32_t start, uint32_t size, bool on);
uint32_t w32_vm_map_shared(uint8_t *const *pages, uint32_t count, uint32_t at);
bool w32_vm_unmap_shared(uint32_t base);
void w32_vm_cleanup(void);
NORETURN void w32_leave_forever(void);

struct w32_pipe;
struct w32_npipe;
struct w32_fiber;
struct image_cache;
struct stream;
void w32_pipe_release(struct w32_object *o);
bool w32_pipe_clone(struct w32_object *o);
void w32_section_retain(struct w32_section *s);
void w32_object_release(struct w32_object *o);
bool w32_object_clone(struct w32_object *dst, struct w32_object *src);

struct w32_inherit {
    int count;
    struct {
        uint32_t index;
        struct w32_object object;
    } items[];
};

struct w32_inherit *w32_inherit_collect(const HANDLE *list, int count);
void w32_inherit_free(struct w32_inherit *in);
HANDLE w32_handle_put(struct w32_process *p, struct w32_object *o);
const HANDLE *w32_attribute_handles(const void *attributes, int *count);
HANDLE w32_inheritable(HANDLE h, const void *sa);
struct w32_object *w32_handle_get_in(struct w32_process *p, HANDLE h);
struct w32_process *w32_process_by_pid(uint32_t pid);
uint32_t w32_current_pid(void);
HANDLE w32_npipe_open(const char *name);
bool w32_is_pipe_name(const char *name);
void w32_section_release(struct w32_section *s);
void *w32_map_section(HANDLE h, uint32_t offset, uint32_t bytes);
bool w32_is_file_dialog(const uint8_t *clsid);
uint32_t w32_new_file_dialog(const uint8_t *clsid, const uint8_t *iid, void **out);
void w32_unmap_section(const void *p);
long w32_pipe_write(struct w32_pipe *p, const void *data, size_t n);
long w32_pipe_read(struct w32_pipe *p, void *out, size_t n, bool wait);
size_t w32_pipe_available(struct w32_pipe *p);
struct stream *w32_handle_stream(HANDLE h);
void w32_port_post(HANDLE port, DWORD bytes, uintptr_t key, void *ov);
void w32_winsock_poll(void);
void w32_io_poll(void);
void w32_io_complete(struct w32_object *o, uint32_t *ov, uint32_t status, uint32_t bytes, bool sync);
int w32_pipe_overlapped(HANDLE h, struct w32_object *o, void *buf, DWORD n, DWORD *done, uint32_t *ov, bool write);
bool w32_io_cancel(HANDLE h, const uint32_t *ov);
bool w32_winsock_cancel(UINT s, uint32_t *ov);
bool w32_socket_associate(uintptr_t s, HANDLE port, uintptr_t key);
void w32_tls_register(struct w32_image *img);
void w32_tls_callbacks(uint32_t base, DWORD reason);
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
struct file_cache;
struct file_cache *w32_fcache_get(const char *path, uint32_t size);
void w32_fcache_put(struct file_cache *c);
void *w32_fcache_page(struct file_cache *c, uint32_t index);
void w32_fcache_write(const char *path, uint32_t offset, const void *data, uint32_t len);
void w32_fcache_truncate(const char *path, uint32_t size);
void w32_fcache_forget(const char *path);
void w32_image_detach(const char *path);
void w32_find_free(struct w32_find *f);
bool w32_wow64(void);
uint32_t w32_vm_map_file(struct file_cache *c, uint32_t first_page, uint32_t count, uint32_t at);
bool w32_vm_fault(uintptr_t address);
bool w32_image_fault(uint32_t address);

struct dw_point {
    int32_t x, y;
    uint8_t on, end;
};
int dwrite_glyph_outline(int face, uint16_t glyph, struct dw_point *pts, int cap, int *advance, int *units);
uint32_t dwrite_raster(int face, uint32_t em, uint32_t ppd, const uint32_t *matrix, uint32_t ox, uint32_t oy,
                       const uint16_t *glyphs, const uint32_t *advances, const uint32_t *offsets, UINT count,
                       int32_t rect[4], uint8_t **out);
uint32_t dwrite_outline(int face, uint32_t em, const uint16_t *glyphs, const uint32_t *advances, const void *offsets,
                        UINT count, BOOL rtl, void *sink);
void w32_dump_status(void);
void w32_pipe_dump(void);
void w32_kick(struct w32_process *p);
void w32_kick_all(void);
void w32_signal_local(void);
void w32_wait_done(void);
bool w32_thread_ready(const struct w32_thread *t);
void w32_signal_tick(void);
void w32_wait_process(uint64_t deadline, bool net, bool (*ready)(const void *), const void *arg);
void w32_wait_word(const volatile uint32_t *addr, uint32_t mask, uint32_t value, bool equal, uint64_t deadline);
bool w32_logging_errors(void);
void w32_io_forget(struct w32_process *p);
void w32_io_drop(HANDLE h);
void w32_signal(void);
void w32_sock_poll(void);
void w32_logfont(void *out, bool wide, LONG height, LONG weight, BYTE italic, BYTE underline, const char *face);
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
DWORD w32_file_access(DWORD access);
HANDLE w32_find_first(const char *spec, struct w32_find_entry *e);
bool w32_find_next(HANDLE h, struct w32_find_entry *e);
long w32_handle_read(HANDLE h, void *buf, size_t n);
long w32_handle_write(HANDLE h, const void *buf, size_t n);

void w32_console_write(int handle_kind, const char *s, size_t n);
void w32_console_write_raw(int handle_kind, const char *s, size_t n);
int w32_console_getkey(bool wait);
bool w32_console_peek(void);
bool w32_key_info(int key, WORD *vk, WORD *scan, WCHAR *ch, DWORD *state);
const char *w32_getenv(const char *name);
int w32_putenv(const char *name, const char *value);
WCHAR w32_towupper(WCHAR c);
WCHAR w32_towlower(WCHAR c);
long w32_console_read(char *buf, size_t n);

struct w32_image *w32_find_module(const char *name);
bool w32_is_pe64(const uint8_t *file, size_t size);
void *w64_stub(const char *module, const char *symbol, uint32_t ordinal);
void *w64_lookup(const char *module, const char *symbol, uint32_t ordinal, bool create_missing);
void *w32_builtin_lookup(const char *module, const char *symbol);

/* What a 32-bit program may call: a built-in function's address as the program sees it. On the 64-bit kernel that
   is a stub into the call gate (w32gate.c), described by the generated signature tables; otherwise the function. */
struct w32_sig {
    const void *fn;
    uint16_t pop;
    uint8_t ret;
    uint8_t nargs;
    const char *args;
} __attribute__((aligned(8)));
#ifdef __x86_64__
void *w32_guest_addr(const void *fn);
bool w32_native_code(const void *fn);
void w32_gate_init(void);
/* the built-in COM objects' method tables: programs call through them, so they hold gate stubs (w32_gate_init) */
#define W32_VTABLE __attribute__((section("w32vtbl"), aligned(8)))
/* functions only assembly or the kernel hands to programs get a gate entry too */
#define W32_GATE(fn) static const void *const w32_gate_ref_##fn __attribute__((used)) = (const void *)fn
/* a 32-bit program's va_list: a pointer to its arguments in i386 layout; a variadic built-in gets it from the gate
   as its only native variadic argument */
typedef uint32_t w32_va_list;
#define W32_VA_SLOT(T) ((sizeof(T) + 3u) & ~3u)
#define w32_va_arg(ap, T) (*(T *)(uintptr_t)(((ap) += W32_VA_SLOT(T)) - W32_VA_SLOT(T)))
#define w32_va_copy(d, s) ((d) = (s))
#define w32_va_end(ap) ((void)(ap))
#define w32_va_start(ap, last)                                                                                     \
    do {                                                                                                           \
        va_list native_;                                                                                           \
        va_start(native_, last);                                                                                   \
        (ap) = va_arg(native_, uint32_t);                                                                          \
        va_end(native_);                                                                                           \
    } while (0)
/* A call to a function pointer that may hold a program's code: the program's code runs in compatibility mode with
   its arguments on its stack (32-bit values; edx:eax back), a built-in function (or its gate stub) is called as is. */
bool w32_guest_code(const void *fn);
const void *w32_native_fn(const void *fn);
uint64_t w32_call_guest(const void *fn, int argc, const uint64_t *args, const uint8_t *sizes);
#define W32_ARG_(x) ((uint64_t)(x))
#define W32_SIZE_(x) ((uint8_t)sizeof(x))
#define W32_MAP_(m, ...) W32_MAPN_(W32_NARGS(__VA_ARGS__), m, __VA_ARGS__)
#define W32_MAPN_(n, m, ...) W32_MAPX_(n, m, __VA_ARGS__)
#define W32_MAPX_(n, m, ...) W32_MAP##n##_(m, __VA_ARGS__)
#define W32_MAP0_(m, ...)
#define W32_MAP1_(m, a) m(a)
#define W32_MAP2_(m, a, ...) m(a), W32_MAP1_(m, __VA_ARGS__)
#define W32_MAP3_(m, a, ...) m(a), W32_MAP2_(m, __VA_ARGS__)
#define W32_MAP4_(m, a, ...) m(a), W32_MAP3_(m, __VA_ARGS__)
#define W32_MAP5_(m, a, ...) m(a), W32_MAP4_(m, __VA_ARGS__)
#define W32_MAP6_(m, a, ...) m(a), W32_MAP5_(m, __VA_ARGS__)
#define W32_MAP7_(m, a, ...) m(a), W32_MAP6_(m, __VA_ARGS__)
#define W32_MAP8_(m, a, ...) m(a), W32_MAP7_(m, __VA_ARGS__)
#define W32_MAP9_(m, a, ...) m(a), W32_MAP8_(m, __VA_ARGS__)
#define W32_MAP10_(m, a, ...) m(a), W32_MAP9_(m, __VA_ARGS__)
#define W32_MAP11_(m, a, ...) m(a), W32_MAP10_(m, __VA_ARGS__)
#define W32_MAP12_(m, a, ...) m(a), W32_MAP11_(m, __VA_ARGS__)
#define W32_MAP13_(m, a, ...) m(a), W32_MAP12_(m, __VA_ARGS__)
#define W32_MAP14_(m, a, ...) m(a), W32_MAP13_(m, __VA_ARGS__)
#define W32_MAP15_(m, a, ...) m(a), W32_MAP14_(m, __VA_ARGS__)
#define W32_MAP16_(m, a, ...) m(a), W32_MAP15_(m, __VA_ARGS__)
#define W32_NARGS(...) W32_NARGS_(0, ##__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define W32_NARGS_(z, a, b, c, d, e, f, g, h, i, j, k, l, m, n_, o, p, n, ...) n
#define W32_CALL(type, fn, ...)                                                                                    \
    (w32_guest_code((const void *)(uintptr_t)(fn))                                                                \
         ? (__typeof__(((type)0)(__VA_ARGS__)))w32_call_guest(                                                    \
               (const void *)(uintptr_t)(fn), W32_NARGS(__VA_ARGS__), (const uint64_t[]){W32_MAP_(W32_ARG_, ##__VA_ARGS__)}, \
               (const uint8_t[]){W32_MAP_(W32_SIZE_, ##__VA_ARGS__)})                                              \
         : ((type)w32_native_fn((const void *)(uintptr_t)(fn)))(__VA_ARGS__))
#else
#define W32_VTABLE
#define W32_CALL(type, fn, ...) (((type)(uintptr_t)(fn))(__VA_ARGS__))
#define w32_guest_addr(fn) ((void *)(fn))
#define w32_native_code(fn) false
#define W32_GATE(fn) _Static_assert(1, "")
typedef va_list w32_va_list;
#define w32_va_arg va_arg
#define w32_va_copy va_copy
#define w32_va_end va_end
#define w32_va_start va_start
#endif
bool w32_is_system_dll(const char *name);
const char *w32_builtin_ordinal_name(const char *module, uint32_t ordinal);


enum w32_engine w32_browser_engine(const char *path);

struct reg_key;
void w32_reg_key_ref(struct reg_key *k);
void w32_reg_key_unref(struct reg_key *k);
void w32_registry_flush(void);
bool w32_reg_read(const char *path, const char *name, char *out, size_t cap);
bool w32_reg_write(const char *path, const char *name, const char *text);
bool w32_reg_set(const char *path, const char *name, uint32_t type, const void *data, uint32_t size);
bool w32_reg_delete_value(const char *path, const char *name);
bool w32_reg_delete_key(const char *path);
int msiexec_main(struct shell *sh, int argc, char **argv);
int w32_split_args(const char *line, char *buf, char **argv, int max);
char **w32_crt_errlist(int *count);
int w32_reg_subkeys(const char *path, void (*fn)(const char *name, void *ctx), void *ctx);
int w32_reg_values(const char *path, void (*fn)(const char *name, void *ctx), void *ctx);
const char *w32_engine_environment(enum w32_engine e);
bool w64_setup(void);
void w64_set_image(uint32_t base);
void w64_setup_tls(struct w32_image *img);
void w64_tls_callbacks(uint32_t reason);
void w64_tls_register(struct w32_image *img);
void w64_tls_attach(uint32_t base);
uint8_t *w64_make_teb(uint32_t stack, uint32_t size, uint32_t tid);
uint32_t w64_run_main(uint32_t entry);
void w64_cleanup(void);
struct w32_image *w32_load(const char *name, const char *search_dir, int *error);
void *w32_get_proc(struct w32_image *image, const char *name, uint32_t ordinal);
int w32_init_modules(void);
int w32_init_mark(void);
int w32_init_modules_from(int mark);
void w32_unload_all(void);

NORETURN void w32_exit(uint32_t code);
void w32_sleep(uint32_t ms);
bool w32_threads_start(void);
void w32_threads_end(void);
void w32_block(void);
void w32_yield(void);
void w32_preempt(void);
void w32_input_boost(void);
uint32_t w32_thread_id(void);
void w32_idle(int ms);
void w32_gui_cleanup(void);
void w32_winsock_cleanup(void);
void w32_inet_cleanup(void);
HANDLE w32_create_thread(SIZE_T stack, void *start, void *param, DWORD flags, DWORD *id);
void w32_run_atexit(void);
uint32_t w32_call(void *fn, int argc, const uint32_t *args);
const uint32_t *dw_params_values(void *self);
float WINAPI dw_params_gamma(void *self);
float WINAPI dw_params_contrast(void *self);
float WINAPI dw_params_cleartype(void *self);
float WINAPI dw_params_gray(void *self);
BOOL w32_verify_version(uint32_t major, uint32_t minor, uint32_t build, uint32_t platform, uint32_t sp_major,
                        uint32_t sp_minor, uint32_t product, DWORD mask, uint64_t cond);
BOOL w32_create_process(const char *cmdline, const char *dir, uint32_t *info);
void w32_seh_raise(uint32_t code, uint32_t flags, uint32_t argc, const uint32_t *args);
void w32_seh_unwind(void *target, uint8_t *rec);
uint32_t w32_kernel_call(uint32_t (*fn)(void *), void *arg);

uint32_t WINAPI w32_zero0(void);
uint32_t WINAPI w32_zero1(uint32_t);
uint32_t WINAPI w32_zero2(uint32_t, uint32_t);
uint32_t WINAPI w32_zero3(uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_zero4(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_zero5(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_zero6(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_one0(void);
uint32_t WINAPI w32_one1(uint32_t);
uint32_t WINAPI w32_one2(uint32_t, uint32_t);
uint32_t WINAPI w32_one3(uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_one4(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_one5(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_one6(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t WINAPI w32_zero7();
uint32_t WINAPI w32_zero8();
uint32_t WINAPI w32_zero9();
uint32_t WINAPI w32_zero10();
uint32_t WINAPI w32_zero11();
uint32_t WINAPI w32_zero12();
uint32_t WINAPI w32_zero13();
uint32_t WINAPI w32_zero14();
uint32_t WINAPI w32_one7();
uint32_t WINAPI w32_one8();
uint32_t WINAPI w32_one9();
uint32_t WINAPI w32_one10();
uint32_t WINAPI w32_one11();
uint32_t WINAPI w32_one12();
uint32_t WINAPI w32_one13();
uint32_t WINAPI w32_one14();
uint32_t WINAPI w32_fail0();
uint32_t WINAPI w32_fail1();
uint32_t WINAPI w32_fail2();
uint32_t WINAPI w32_fail3();
uint32_t WINAPI w32_fail4();
uint32_t WINAPI w32_fail5();
uint32_t WINAPI w32_fail6();
uint32_t WINAPI w32_fail7();
uint32_t WINAPI w32_fail8();
uint32_t WINAPI w32_fail9();
uint32_t WINAPI w32_fail10();
uint32_t WINAPI w32_fail11();
uint32_t WINAPI w32_fail12();
uint32_t WINAPI w32_fail13();
uint32_t WINAPI w32_fail14();
uint32_t WINAPI w32_status0();
uint32_t WINAPI w32_status1();
uint32_t WINAPI w32_status2();
uint32_t WINAPI w32_status3();
uint32_t WINAPI w32_status4();
uint32_t WINAPI w32_status5();
uint32_t WINAPI w32_status6();
uint32_t WINAPI w32_status7();
uint32_t WINAPI w32_status8();
uint32_t WINAPI w32_status9();
uint32_t WINAPI w32_status10();
uint32_t WINAPI w32_status11();
uint32_t WINAPI w32_status12();
uint32_t WINAPI w32_status13();
uint32_t WINAPI w32_status14();
uint32_t WINAPI w32_notimpl0();
uint32_t WINAPI w32_notimpl1();
uint32_t WINAPI w32_notimpl2();
uint32_t WINAPI w32_notimpl3();
uint32_t WINAPI w32_notimpl4();
uint32_t WINAPI w32_notimpl5();
uint32_t WINAPI w32_notimpl6();
uint32_t WINAPI w32_notimpl7();
uint32_t WINAPI w32_notimpl8();
uint32_t WINAPI w32_notimpl9();
uint32_t WINAPI w32_notimpl10();
uint32_t WINAPI w32_notimpl11();
uint32_t WINAPI w32_notimpl12();
uint32_t WINAPI w32_notimpl13();
uint32_t WINAPI w32_notimpl14();
uint32_t WINAPI w32_neg0();
uint32_t WINAPI w32_neg1();
uint32_t WINAPI w32_neg2();
uint32_t WINAPI w32_neg3();
uint32_t WINAPI w32_neg4();
uint32_t WINAPI w32_neg5();
uint32_t WINAPI w32_neg6();
uint32_t WINAPI w32_neg7();
uint32_t WINAPI w32_neg8();
uint32_t WINAPI w32_neg9();
uint32_t WINAPI w32_neg10();
uint32_t WINAPI w32_neg11();
uint32_t WINAPI w32_neg12();
uint32_t WINAPI w32_neg13();
uint32_t WINAPI w32_neg14();
uint32_t WINAPI w32_secerr0();
uint32_t WINAPI w32_secerr1();
uint32_t WINAPI w32_secerr2();
uint32_t WINAPI w32_secerr3();
uint32_t WINAPI w32_secerr4();
uint32_t WINAPI w32_secerr5();
uint32_t WINAPI w32_secerr6();
uint32_t WINAPI w32_secerr7();
uint32_t WINAPI w32_secerr8();
uint32_t WINAPI w32_secerr9();
uint32_t WINAPI w32_secerr10();
uint32_t WINAPI w32_secerr11();
uint32_t WINAPI w32_secerr12();
uint32_t WINAPI w32_secerr13();
uint32_t WINAPI w32_secerr14();

#endif
