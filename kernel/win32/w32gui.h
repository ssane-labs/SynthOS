#ifndef SYNTH_W32GUI_H
#define SYNTH_W32GUI_H

#include <synth/gui.h>

#include "win32.h"

typedef uint32_t COLORREF;
typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef LRESULT WINAPI (*WNDPROC)(HANDLE, UINT, WPARAM, LPARAM);

typedef struct {
    LONG left, top, right, bottom;
} RECT;

typedef struct {
    LONG x, y;
} POINT;

typedef struct {
    LONG cx, cy;
} SIZE;

typedef struct {
    HANDLE hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG;

typedef struct {
    HANDLE hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT;

enum {
    WM_NULL = 0x0000,
    WM_CREATE = 0x0001,
    WM_DESTROY = 0x0002,
    WM_MOVE = 0x0003,
    WM_SIZE = 0x0005,
    WM_ACTIVATE = 0x0006,
    WM_SETFOCUS = 0x0007,
    WM_KILLFOCUS = 0x0008,
    WM_ENABLE = 0x000a,
    WM_SETTEXT = 0x000c,
    WM_GETTEXT = 0x000d,
    WM_GETTEXTLENGTH = 0x000e,
    WM_PAINT = 0x000f,
    WM_CLOSE = 0x0010,
    WM_QUIT = 0x0012,
    WM_ERASEBKGND = 0x0014,
    WM_SHOWWINDOW = 0x0018,
    WM_ACTIVATEAPP = 0x001c,
    WM_SETCURSOR = 0x0020,
    WM_MOUSEACTIVATE = 0x0021,
    WM_GETMINMAXINFO = 0x0024,
    WM_SETFONT = 0x0030,
    WM_GETFONT = 0x0031,
    WM_WINDOWPOSCHANGED = 0x0047,
    WM_NOTIFY = 0x004e,
    WM_NCCREATE = 0x0081,
    WM_NCDESTROY = 0x0082,
    WM_NCCALCSIZE = 0x0083,
    WM_NCHITTEST = 0x0084,
    WM_NCPAINT = 0x0085,
    WM_NCACTIVATE = 0x0086,
    WM_GETDLGCODE = 0x0087,
    WM_KEYDOWN = 0x0100,
    WM_KEYUP = 0x0101,
    WM_CHAR = 0x0102,
    WM_SYSKEYDOWN = 0x0104,
    WM_SYSKEYUP = 0x0105,
    WM_SYSCHAR = 0x0106,
    WM_INITDIALOG = 0x0110,
    WM_COMMAND = 0x0111,
    WM_SYSCOMMAND = 0x0112,
    WM_TIMER = 0x0113,
    WM_HSCROLL = 0x0114,
    WM_VSCROLL = 0x0115,
    WM_INITMENU = 0x0116,
    WM_INITMENUPOPUP = 0x0117,
    WM_CTLCOLOREDIT = 0x0133,
    WM_CTLCOLORLISTBOX = 0x0134,
    WM_CTLCOLORBTN = 0x0135,
    WM_CTLCOLORDLG = 0x0136,
    WM_CTLCOLORSTATIC = 0x0138,
    WM_MOUSEMOVE = 0x0200,
    WM_LBUTTONDOWN = 0x0201,
    WM_LBUTTONUP = 0x0202,
    WM_LBUTTONDBLCLK = 0x0203,
    WM_RBUTTONDOWN = 0x0204,
    WM_RBUTTONUP = 0x0205,
    WM_RBUTTONDBLCLK = 0x0206,
    WM_MBUTTONDOWN = 0x0207,
    WM_MBUTTONUP = 0x0208,
    WM_MOUSEWHEEL = 0x020a,
    WM_MOUSELEAVE = 0x02a3,
    WM_USER = 0x0400,
};

enum {
    WS_OVERLAPPED = 0x00000000u,
    WS_TABSTOP = 0x00010000u,
    WS_GROUP = 0x00020000u,
    WS_THICKFRAME = 0x00040000u,
    WS_SYSMENU = 0x00080000u,
    WS_HSCROLL = 0x00100000u,
    WS_VSCROLL = 0x00200000u,
    WS_DLGFRAME = 0x00400000u,
    WS_BORDER = 0x00800000u,
    WS_CAPTION = 0x00c00000u,
    WS_MAXIMIZE = 0x01000000u,
    WS_CLIPCHILDREN = 0x02000000u,
    WS_CLIPSIBLINGS = 0x04000000u,
    WS_DISABLED = 0x08000000u,
    WS_VISIBLE = 0x10000000u,
    WS_MINIMIZE = 0x20000000u,
    WS_CHILD = 0x40000000u,
    WS_POPUP = 0x80000000u,
};

#ifndef ABS
#define ABS(x) ((x) < 0 ? -(x) : (x))
#endif

#define W32_CW_USEDEFAULT ((int)0x80000000)
#define W32_WINDOW_MAGIC 0x57444e57u
#define W32_GDI_MAGIC 0x49444757u
#define W32_DC_MAGIC 0x43444457u

enum control_kind {
    CK_NONE,
    CK_BUTTON,
    CK_STATIC,
    CK_EDIT,
};

struct w32_class {
    char name[64];
    WNDPROC proc;
    uint32_t style;
    HANDLE background;
    HANDLE icon, cursor;
    int class_extra, window_extra;
    bool unicode;
    HANDLE instance;
    uint16_t atom;
    enum control_kind control;
    uintptr_t extra[8];
    struct w32_class *next;
};

struct w32_bridge {
    struct w32_process *process;
    struct w32_window *win;
};

struct w32_window {
    uint32_t magic;
    struct w32_class *cls;
    WNDPROC proc;
    bool unicode;
    uint32_t style, exstyle;
    char *text;
    struct w32_window *parent, *owner;
    struct w32_window *child, *sibling;
    struct w32_window *next_all;
    struct rect rect;
    struct window *wm;
    struct w32_bridge *bridge;
    intptr_t id;
    intptr_t userdata;
    uint8_t *extra;
    bool visible, enabled, destroyed;
    bool need_paint, erase;
    struct rect update;
    uint32_t thread;
    HANDLE font;
    HANDLE menu;
    struct w32_window *focus;
    enum control_kind control;
    bool pressed, hot, checked;
    char *edit_buf;
    size_t edit_cap, edit_len, caret;
    int scroll;
    void *bar;
};

struct w32_window *w32_win(HANDLE h);
LRESULT w32_send(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp);
void w32_post(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp);
struct rect w32_area(struct w32_window *w);
struct rect w32_visible_area(struct w32_window *w);
struct w32_window *w32_top(struct w32_window *w);
void w32_damage(struct w32_window *w, struct rect surface_rect);
void w32_invalidate(struct w32_window *w, const struct rect *r, bool erase);
void w32_set_text(struct w32_window *w, const char *text);
void w32_set_focus(struct w32_window *w);
struct w32_window *w32_focus(void);
void w32_gui_cleanup(void);

HANDLE w32_window_dc(struct w32_window *w, struct rect clip);
void w32_free_dc(HANDLE dc);
uint32_t w32_rgb(COLORREF c);
COLORREF w32_syscolor(int index);
HANDLE w32_syscolor_brush(int index);
const struct ui_font *w32_font_of(HANDLE font);
HANDLE w32_stock(int index);
void w32_fill_brush(HANDLE dc, const RECT *r, HANDLE brush);
struct canvas *w32_dc_canvas(HANDLE dc, int *ox, int *oy);
void w32_dc_damage(HANDLE dc, struct rect r);
HANDLE w32_dc_font(HANDLE dc);
COLORREF w32_dc_text_color(HANDLE dc);

LRESULT WINAPI w32_control_proc(HANDLE hwnd, UINT msg, WPARAM wp, LPARAM lp);

#endif
