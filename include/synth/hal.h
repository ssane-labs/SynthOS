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

struct display_mode {
    uint16_t width, height;
};

int hal_screen_count(void);
const char *hal_screen_name(int index);
int hal_screen_modes(int index, struct display_mode *out, int max);
bool hal_screen_open(int index, int width, int height, struct framebuffer *fb);
void hal_screen_close(int index);
int hal_key_read(int timeout_ms);

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
void hal_debug_write(const char *s, size_t n);
void hal_set_idle(void (*fn)(void));
uint64_t hal_ticks_ms(void);
void hal_profile_start(uint32_t *buf, uint32_t cap);
uint32_t hal_profile_stop(void);
void hal_sleep_ms(uint32_t ms);
void hal_rtc(struct rtc_time *t);
void hal_beep(uint32_t hz, uint32_t ms);
int hal_audio_open(int rate);
int hal_audio_space(void);
int hal_audio_write(const int16_t *frames, int count);
void hal_audio_close(void);
const char *hal_audio_name(void);
uint32_t hal_entropy(void);
void hal_fs_init(void);
struct blockdev;
struct blockdev *hal_boot_disk(void);
uint32_t hal_system_sectors(void);
void hal_set_codepage(int codepage);
const char *hal_arch(void);
const char *hal_machine(void);
NORETURN void hal_reboot(void);
NORETURN void hal_poweroff(void);

NORETURN void kmain(const struct boot_info *bi);

#endif
