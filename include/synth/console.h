#ifndef SYNTH_CONSOLE_H
#define SYNTH_CONSOLE_H

#include <synth/hal.h>
#include <synth/theme.h>

typedef void (*status_render_fn)(int cols);

void con_init(struct display *d);
struct display *con_display(void);
void con_resize(int cols, int rows);
bool con_is_grid(void);
int con_cols(void);
int con_rows(void);

void con_write(const char *s, size_t n);
void con_puts(const char *s);
void con_putc(char c);
int kprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
void con_markup(const char *s);
size_t markup_strip(const char *s, char *out, size_t cap);

void con_set_attr(uint8_t attr);
uint8_t con_attr(void);
void con_role(enum theme_role role);
void con_reset(void);

void con_goto(int x, int y);
void con_cursor(int *x, int *y);
void con_show_cursor(bool visible);
bool con_cursor_visible(void);
void con_clear(void);
void con_clear_line_from(int x, int y);
void con_clear_below(int y);
void con_fill(int x, int y, int w, int h, uint8_t ch, uint8_t attr);
void con_put_cell(int x, int y, uint8_t ch, uint8_t attr);
struct cell con_get_cell(int x, int y);
void con_put_text(int x, int y, const char *s, uint8_t attr);
uint32_t con_scroll_count(void);

void con_flush(void);
void con_hold(bool hold);
void con_redraw(void);
void con_apply_palette(void);
void con_set_cursor_shape(enum cursor_shape shape);
enum cursor_shape con_cursor_shape(void);

void con_set_statusbar(bool on);
bool con_statusbar(void);
void con_set_status_renderer(status_render_fn fn);
void con_status_put(int x, const char *s, uint8_t attr);
void con_update_status(void);

#endif
