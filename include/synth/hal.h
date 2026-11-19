#ifndef SYNTH_HAL_H
#define SYNTH_HAL_H

#include <synth/types.h>

struct cell {
    uint8_t ch;
    uint8_t attr;
};

enum cursor_shape {
    CURSOR_BLOCK,
    CURSOR_UNDERLINE,
    CURSOR_BAR,
};

enum {
    DISPLAY_GRID = 1u << 0,
    DISPLAY_PALETTE = 1u << 1,
    DISPLAY_CURSOR_SHAPE = 1u << 2,
    DISPLAY_UNICODE = 1u << 3,
};

struct display {
    const char *name;
    int cols;
    int rows;
    unsigned caps;
    void (*present)(const struct cell *cells, int cols, int y0, int y1);
    void (*cursor)(int x, int y, bool visible);
    void (*scroll)(int top, int bottom, int lines);
    void (*palette)(const uint32_t rgb[16]);
    void (*cursor_shape)(enum cursor_shape shape);
    void (*write)(const char *s, size_t n);
};

struct rtc_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

struct boot_info {
    void *heap;
    size_t heap_size;
    size_t mem_total;
    const void *initrd;
    size_t initrd_size;
    const char *cmdline;
};

struct pointer_event {
    int dx;
    int dy;
    int wheel;
    unsigned buttons;
    bool absolute;
    bool placed;
    int x, y;
};

struct framebuffer;

struct display *hal_display(void);
bool hal_pointer_read(struct pointer_event *ev);
bool hal_in_interrupt(void);
bool hal_pointer_present(void);
/* The hook sees every pointer event in interrupt context before it is queued; it may set x, y and placed (the
   pointer position in screen pixels), which then travel with the event. */
void hal_pointer_hook(void (*hook)(struct pointer_event *ev));
bool hal_input_pending(void);
uint32_t hal_irq_save(void);
void hal_irq_restore(uint32_t flags);
bool hal_graphics_enter(struct framebuffer *fb);
void hal_graphics_leave(void);
/* the built-in panel's backlight in percent; false when the kernel cannot drive it (dim in software then) */
bool hal_backlight(int percent);

struct display_mode {
    uint16_t width, height;
};

int hal_screen_count(void);
const char *hal_screen_name(int index);
int hal_screen_modes(int index, struct display_mode *out, int max);
bool hal_screen_open(int index, int width, int height, struct framebuffer *fb);
void hal_screen_present(const struct framebuffer *fb, int x, int y, int w, int h);
/* The first screen of virtio-gpu gpu shows its 3D resource id (w x h) instead of its framebuffer, or the
   framebuffer again with id 0; false when no screen of that card is open */
bool hal_screen_show_host(int gpu, uint32_t id, int w, int h);
/* a pointer the display hardware draws over the screen (virtio-gpu): its picture and hot spot, then where it is;
   false: this screen has none, the pointer is drawn into the picture */
bool hal_cursor_image(const struct framebuffer *fb, const uint32_t *argb, int w, int h, int hot_x, int hot_y, int x,
                      int y);
bool hal_cursor_move(const struct framebuffer *fb, int x, int y, bool show);
void hal_screen_close(int index);
int hal_key_read(int timeout_ms);
int hal_key_modifiers(void);
void hal_key_repeat(int delay_ms, int rate);

struct usb_info {
    char name[48];
    char driver[16];
    char host[12];
    uint16_t vendor, product;
    int speed;
    int port;
    int depth;
};

int hal_usb_devices(struct usb_info *out, int max);
void hal_net_init(void);
/* network controllers found without a driver ("8086:15f3 Ethernet, ..."), empty when there are none */
const char *hal_net_unsupported(void);
void hal_debug_write(const char *s, size_t n);
void hal_set_idle(void (*fn)(void));
uint64_t hal_ticks_ms(void);
void hal_profile_start(uint32_t *buf, uint32_t cap);
uint32_t hal_profile_stop(void);
void hal_profile_callers(bool on);
/* samples only while `since` (a time in ms) is at least ms old; NULL samples always */
void hal_profile_gate(volatile const uint64_t *since, uint32_t ms);
void hal_sleep_ms(uint32_t ms);
void hal_rtc(struct rtc_time *t);
void hal_beep(uint32_t hz, uint32_t ms);
int hal_audio_open(int rate);
int hal_audio_space(void);
int hal_audio_write(const int16_t *frames, int count);
void hal_audio_close(void);
const char *hal_audio_name(void);

/* the vector instructions this processor has and the kernel has turned on, for code that picks its fastest path */
#define HAL_SIMD_SSE2 0x001u
#define HAL_SIMD_SSSE3 0x002u
#define HAL_SIMD_SSE41 0x004u
#define HAL_SIMD_AVX 0x008u
#define HAL_SIMD_AVX2 0x010u
#define HAL_SIMD_FMA 0x020u
#define HAL_SIMD_F16C 0x040u
#define HAL_SIMD_AVXVNNI 0x080u
#define HAL_SIMD_AVX512 0x100u /* F, BW */
#define HAL_SIMD_AVX512VNNI 0x200u
uint32_t hal_simd(void);
int hal_audio_record_open(void);
int hal_audio_record_read(int16_t *frames, int max);
void hal_audio_record_close(void);
uint32_t hal_entropy(void);
void hal_fs_init(void);
struct blockdev;
struct blockdev *hal_boot_disk(void);
uint32_t hal_system_sectors(void);
struct blockdev *hal_system_area(void);
uint32_t hal_bundle_sectors(void);
bool hal_installed(void);
bool hal_uefi(void);
void hal_set_codepage(int codepage);
const char *hal_arch(void);
const char *hal_machine(void);
int hal_start_cpus(void);
bool hal_safe_mode(void);
uint32_t hal_boot_nonce(void);
void hal_delay_ms(uint32_t ms);
/* runs fn; a CPU fault in it returns the vector, an app stuck in its own code for seconds returns HAL_GUARD_HUNG */
#define HAL_GUARD_HUNG 256
int hal_guard(void (*fn)(void *ctx), void *ctx);
int hal_cpu_count(void);
/* runs fn over [0, count) split into ranges of at least grain items on every processor; fn must only touch memory
   all address spaces share and call nothing that locks, sleeps or allocates */
void hal_parallel(void (*fn)(void *ctx, int from, int to), void *ctx, int count, int grain);
int hal_parallel_cpus(void);
NORETURN void hal_reboot(void);
NORETURN void hal_poweroff(void);

NORETURN void kmain(const struct boot_info *bi);

#endif
