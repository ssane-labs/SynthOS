#ifndef SYNTH_GUI_INTERNAL_H
#define SYNTH_GUI_INTERNAL_H

#include <synth/gui.h>
#include <synth/hal.h>

#define TASKBAR_HEIGHT 40

struct framebuffer;

extern int mouse_x, mouse_y;

bool wm_start(const struct framebuffer *fb);
void wm_stop(void);
void wm_render(void);
void wm_pointer(const struct pointer_event *ev);
void wm_key(int key);
void wm_cycle(void);
void wm_timers(void);
void wm_close_popups(void);
struct window *wm_top_popup(void);
void wm_damage_all(void);
void wm_set_cursor_visible(bool on);
int wm_window_list(struct window **out, int max);

void desktop_paint(struct canvas *c);
void desktop_mouse(int x, int y, unsigned button, bool down, int clicks);
void taskbar_paint(struct canvas *c);
bool taskbar_hit(int x, int y);
void taskbar_mouse(int x, int y, unsigned button, bool down);
void taskbar_hover(int x, int y);
void taskbar_changed(void);

struct window *terminal_open(void);
void terminal_close(void);
void terminal_flush(void);
struct window *terminal_window(void);

void start_menu_toggle(void);

#endif
