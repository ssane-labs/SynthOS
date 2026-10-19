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
};

struct w32_child {
    int refs;
    bool done;
    uint32_t exit_code;
    uint32_t pid;
    struct w32_process *proc;
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
    uint32_t seen, user_eip;
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
    char tag[20];
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
uint32_t w32_vm_map_shared(uint8_t *const *pages, uint32_t count);
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
bool w32_object_clone(struct w32_object *dst, const struct w32_object *src);

struct w32_inherit {
    int count;
    struct {
        uint32_t index;
        struct w32_object object;
    } items[];
};

struct w32_inherit *w32_inherit_collect(const HANDLE *list, int count);
void w32_inherit_free(struct w32_inherit *in);
HANDLE w32_handle_put(struct w32_process *p, const struct w32_object *o);
const HANDLE *w32_attribute_handles(const void *attributes, int *count);
HANDLE w32_inheritable(HANDLE h, const void *sa);
struct w32_object *w32_handle_get_in(struct w32_process *p, HANDLE h);
struct w32_process *w32_process_by_pid(uint32_t pid);
uint32_t w32_current_pid(void);
HANDLE w32_npipe_open(const char *name);
bool w32_is_pipe_name(const char *name);
void w32_section_release(struct w32_section *s);
void *w32_map_section(HANDLE h, uint32_t offset, uint32_t bytes);
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
uint32_t w32_vm_map_file(struct file_cache *c, uint32_t first_page, uint32_t count);
bool w32_vm_fault(uintptr_t address);

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
bool w32_is_system_dll(const char *name);
const char *w32_builtin_ordinal_name(const char *module, uint32_t ordinal);
bool w64_setup(void);
void w64_set_image(uint32_t base);
void w64_setup_tls(struct w32_image *img);
void w64_tls_callbacks(uint32_t reason);
uint8_t *w64_make_teb(uint32_t stack, uint32_t size, uint32_t tid);
uint32_t w64_run_main(uint32_t entry);
void w64_cleanup(void);
struct w32_image *w32_load(const char *name, const char *search_dir, int *error);
void *w32_get_proc(struct w32_image *image, const char *name, uint32_t ordinal);
int w32_init_modules(void);
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
