#include <synth/config.h>
#if CONFIG_GUI
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "w32gui.h"

#define QUEUE_SIZE 512
#define MAX_TIMERS 64
#define MAX_MENU_ITEMS 64
#define MAX_MENUS 8
#define DEFAULT_W 640
#define DEFAULT_H 480
#define MENU_MAGIC 0x554e454du

enum {
    MK_LBUTTON = 0x01,
    MK_RBUTTON = 0x02,
    MK_SHIFT = 0x04,
    MK_CONTROL = 0x08,
    MK_MBUTTON = 0x10,
};

enum {
    SW_HIDE = 0,
    SW_SHOWMINIMIZED = 2,
    SW_SHOWMAXIMIZED = 3,
    SW_MINIMIZE = 6,
    SW_SHOWNOACTIVATE = 4,
    SW_SHOWMINNOACTIVE = 7,
    SW_SHOWNA = 8,
    SW_FORCEMINIMIZE = 11,
};

enum {
    MF_GRAYED = 0x0001,
    MF_DISABLED = 0x0002,
    MF_CHECKED = 0x0008,
    MF_POPUP = 0x0010,
    MF_SEPARATOR = 0x0800,
};

struct queued {
    MSG msg;
    uint32_t thread;
    unsigned mods;
};

struct timer {
    struct w32_window *win;
    uintptr_t id;
    uint32_t interval;
    uint64_t due;
    void *proc;
    bool used;
};

struct w32_menu_item {
    char text[64];
    uintptr_t id;
    struct w32_menu *sub;
    unsigned flags;
};

struct w32_menu {
    uint32_t magic;
    int count;
    struct w32_menu_item items[MAX_MENU_ITEMS];
};

struct menu_bar {
    struct app_menu menus[MAX_MENUS];
    struct menu_item items[MAX_MENUS][MAX_MENU_ITEMS];
    char labels[MAX_MENUS][MAX_MENU_ITEMS][64];
    char shortcuts[MAX_MENUS][MAX_MENU_ITEMS][24];
};

static W32_STATE struct w32_class *classes;
static W32_STATE struct w32_window *windows;
static W32_STATE struct queued queue[QUEUE_SIZE];
static W32_STATE unsigned queue_head, queue_tail;
static W32_STATE struct timer timers[MAX_TIMERS];
static W32_STATE uint32_t quit_thread;
static W32_STATE int quit_code;
static W32_STATE bool quit_posted;
static W32_STATE struct w32_window *capture;
static W32_STATE struct w32_window *active;
static W32_STATE uint16_t next_atom;
static W32_STATE int mouse_x, mouse_y;
static W32_STATE unsigned key_mods;
static W32_STATE unsigned mouse_buttons;
static W32_STATE struct w32_window *hover;
static W32_STATE struct gui_app *process_app;

struct process_app {
    struct gui_app app;
    char name[48];
    struct canvas picture;
};

static const struct gui_app *app_of_process(void)
{
    if (process_app)
        return process_app;
    struct process_app *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    str_copy(p->name, path_basename(w32->ctx->path), sizeof(p->name));
    char *dot = strrchr(p->name, '.');
    if (dot && dot != p->name)
        *dot = 0;
    p->app = (struct gui_app){p->name, "Windows program", ICON_APP, NULL, NULL, NULL};
    if (gui_program_icon(w32->ctx->path, 64, &p->picture))
        p->app.picture = &p->picture;
    process_app = &p->app;
    return process_app;
}

static const struct window_ops bridge_ops;

static struct w32_class *find_class(const char *name);

struct w32_window *w32_windows(void)
{
    return windows;
}

struct w32_class *w32_find_class(const char *name)
{
    return find_class(name);
}

struct w32_window *w32_win(HANDLE h)
{
    struct w32_window *w = h;
    for (struct w32_window *it = windows; it; it = it->next_all)
        if (it == w)
            return w->magic == W32_WINDOW_MAGIC && !w->destroyed ? w : NULL;
    return NULL;
}

struct w32_window *w32_top(struct w32_window *w)
{
    while (w && w->parent && (w->style & WS_CHILD))
        w = w->parent;
    return w;
}

static bool is_child(const struct w32_window *w)
{
    return w->parent && (w->style & WS_CHILD);
}

struct rect w32_area(struct w32_window *w)
{
    if (!is_child(w))
        return w->rect;
    struct rect p = w32_area(w->parent);
    return (struct rect){p.x + w->rect.x, p.y + w->rect.y, w->rect.w, w->rect.h};
}

struct rect w32_visible_area(struct w32_window *w)
{
    struct rect r = w32_area(w);
    for (struct w32_window *p = w; is_child(p); p = p->parent)
        r = rect_intersect(r, w32_area(p->parent));
    return r;
}

static bool shown(struct w32_window *w)
{
    for (; w; w = is_child(w) ? w->parent : NULL)
        if (!w->visible)
            return false;
    return true;
}

/* Programs that draw their own minimize/maximize/close buttons get them covered: the title bar has the real ones. */
static void cover_caption(struct w32_window *top)
{
    struct rect b = top->caption;
    struct canvas *c = &top->wm->surface;
    if (rect_empty(b) || b.x < 1 || b.x + b.w > c->w || b.y + b.h > c->h)
        return;
    struct rect clip = c->clip;
    c->clip = (struct rect){0, 0, c->w, c->h};
    for (int y = b.y; y < b.y + b.h; y++)
        gfx_fill(c, (struct rect){b.x, y, b.w, 1}, c->px[y * c->stride + b.x - 1]);
    c->clip = clip;
}

void w32_damage(struct w32_window *w, struct rect r)
{
    struct w32_window *top = w32_top(w);
    if (!top || !top->wm || rect_empty(r))
        return;
    struct rect cr = wm_client_rect(top->wm);
    r = rect_intersect(r, (struct rect){0, 0, cr.w, cr.h});
    if (!rect_empty(rect_intersect(r, top->caption)))
        cover_caption(top);
    if (!rect_empty(r))
        wm_damage_client(top->wm, r);
}

static uint32_t message_time(void)
{
    return (uint32_t)hal_ticks_ms();
}

static void enqueue(HANDLE hwnd, uint32_t thread, UINT msg, WPARAM wp, LPARAM lp)
{
    w32_signal();
    if (queue_tail - queue_head >= QUEUE_SIZE)
        return;
    struct queued *q = &queue[queue_tail++ % QUEUE_SIZE];
    q->msg = (MSG){hwnd, msg, wp, lp, message_time(), {mouse_x, mouse_y}};
    q->thread = thread;
    q->mods = key_mods;
}

void w32_post(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (w)
        enqueue(w, w->thread, msg, wp, lp);
}

LRESULT w32_send(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w || w->destroyed || !w->proc)
        return 0;
    return w->proc(w, msg, wp, lp);
}

void w32_invalidate(struct w32_window *w, const struct rect *r, bool erase)
{
    struct rect full = {0, 0, w->rect.w, w->rect.h};
    struct rect add = r ? rect_intersect(*r, full) : full;
    if (rect_empty(add))
        return;
    w->update = w->need_paint ? rect_union(w->update, add) : add;
    w->need_paint = true;
    w->erase |= erase;
    struct rect area = w32_area(w);
    struct rect abs = {area.x + add.x, area.y + add.y, add.w, add.h};
    for (struct w32_window *c = w->child; c; c = c->sibling) {
        struct rect ca = w32_area(c);
        struct rect o = rect_intersect(abs, ca);
        if (!rect_empty(o) && c->visible) {
            struct rect local = {o.x - ca.x, o.y - ca.y, o.w, o.h};
            w32_invalidate(c, &local, true);
        }
    }
}

void w32_set_text(struct w32_window *w, const char *text)
{
    char *copy = w32_strdup(text ? text : "");
    if (!copy)
        return;
    w32_free(w->text);
    w->text = copy;
    if (w->wm)
        wm_set_title(w->wm, copy);
}

#define WS_EX_NOACTIVATE 0x08000000u

struct w32_window *w32_focus(void)
{
    struct w32_window *top = active;
    if (!top)
        return NULL;
    return top->focus && w32_win(top->focus) ? top->focus : top;
}

void w32_set_focus(struct w32_window *w)
{
    struct w32_window *old = w32_focus();
    if (old == w)
        return;
    struct w32_window *top = w32_top(w);
    if (top)
        top->focus = w;
    if (old)
        w32_send(old, WM_KILLFOCUS, (WPARAM)w, 0);
    if (w)
        w32_send(w, WM_SETFOCUS, (WPARAM)old, 0);
    if (top && top->wm && wm_focused() != top->wm)
        wm_focus(top->wm);
}

static struct w32_class *find_class(const char *name)
{
    if ((uintptr_t)name < 0x10000) {
        for (struct w32_class *c = classes; c; c = c->next)
            if (c->atom == (uint16_t)(uintptr_t)name)
                return c;
        return NULL;
    }
    for (struct w32_class *c = classes; c; c = c->next)
        if (!str_casecmp(c->name, name))
            return c;
    return NULL;
}

static struct w32_class *add_class(const char *name, WNDPROC proc, uint32_t style, HANDLE background, int class_extra,
                                   int window_extra, bool unicode, HANDLE instance)
{
    if (find_class(name)) {
        w32_set_error(1410);
        return NULL;
    }
    struct w32_class *c = w32_zalloc(sizeof(*c));
    if (!c)
        return NULL;
    str_copy(c->name, name, sizeof(c->name));
    c->proc = proc;
    c->style = style;
    c->background = background;
    c->class_extra = class_extra;
    c->window_extra = window_extra;
    c->unicode = unicode;
    c->instance = instance;
    c->atom = (uint16_t)(0xc000 + next_atom++);
    c->next = classes;
    classes = c;
    return c;
}

static void builtin_classes(void)
{
    static const struct {
        const char *name;
        enum control_kind kind;
        int background;
    } list[] = {{"Button", CK_BUTTON, 16}, {"Static", CK_STATIC, 16}, {"Edit", CK_EDIT, 6}};
    if (!find_class("#32770")) {
        struct w32_class *c = add_class("#32770", w32_dialog_proc, 8, (HANDLE)16, 0, 0, false, NULL);
        if (c)
            c->dialog = true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(list); i++)
        if (!find_class(list[i].name)) {
            struct w32_class *c = add_class(list[i].name, w32_control_proc, 8, (HANDLE)(intptr_t)list[i].background,
                                            0, 0, false, NULL);
            if (c)
                c->control = list[i].kind;
        }
}

typedef struct {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HANDLE hInstance;
    HANDLE hIcon;
    HANDLE hCursor;
    HANDLE hbrBackground;
    const void *lpszMenuName;
    const void *lpszClassName;
    HANDLE hIconSm;
} WNDCLASSEX;

static uint16_t register_class(const WNDCLASSEX *wc, bool unicode)
{
    builtin_classes();
    char *name = NULL;
    char atom_name[16];
    if ((uintptr_t)wc->lpszClassName < 0x10000) {
        ksnprintf(atom_name, sizeof(atom_name), "#%u", (unsigned)(uintptr_t)wc->lpszClassName);
    } else if (unicode) {
        name = w32_narrow(wc->lpszClassName);
    }
    const char *n = name ? name : (uintptr_t)wc->lpszClassName < 0x10000 ? atom_name : wc->lpszClassName;
    struct w32_class *c = add_class(n, wc->lpfnWndProc, wc->style, wc->hbrBackground, wc->cbClsExtra, wc->cbWndExtra,
                                    unicode, wc->hInstance);
    w32_free(name);
    if (!c)
        return 0;
    c->icon = wc->hIcon;
    c->cursor = wc->hCursor;
    return c->atom;
}

static uint16_t WINAPI RegisterClassExA(const WNDCLASSEX *wc)
{
    return register_class(wc, false);
}

static uint16_t WINAPI RegisterClassExW(const WNDCLASSEX *wc)
{
    return register_class(wc, true);
}

static uint16_t WINAPI RegisterClassA(const UINT *wc)
{
    WNDCLASSEX ex;
    memset(&ex, 0, sizeof(ex));
    ex.cbSize = sizeof(ex);
    memcpy(&ex.style, wc, 40);
    return register_class(&ex, false);
}

static uint16_t WINAPI RegisterClassW(const UINT *wc)
{
    WNDCLASSEX ex;
    memset(&ex, 0, sizeof(ex));
    ex.cbSize = sizeof(ex);
    memcpy(&ex.style, wc, 40);
    return register_class(&ex, true);
}

static BOOL WINAPI UnregisterClassA(LPCSTR name, HANDLE instance)
{
    UNUSED(instance);
    for (struct w32_class **link = &classes; *link; link = &(*link)->next)
        if (*link == find_class(name)) {
            *link = (*link)->next;
            return TRUE;
        }
    return FALSE;
}

static BOOL WINAPI UnregisterClassW(LPCWSTR name, HANDLE instance)
{
    if ((uintptr_t)name < 0x10000)
        return UnregisterClassA((LPCSTR)name, instance);
    char *n = w32_narrow(name);
    BOOL ok = UnregisterClassA(n, instance);
    w32_free(n);
    return ok;
}

static BOOL get_class_info(const char *name, WNDCLASSEX *out)
{
    builtin_classes();
    struct w32_class *c = find_class(name);
    if (!c) {
        w32_set_error(1411);
        return FALSE;
    }
    out->style = c->style;
    out->lpfnWndProc = c->proc;
    out->cbClsExtra = c->class_extra;
    out->cbWndExtra = c->window_extra;
    out->hInstance = c->instance;
    out->hIcon = c->icon;
    out->hCursor = c->cursor;
    out->hbrBackground = c->background;
    out->lpszMenuName = NULL;
    out->lpszClassName = name;
    return TRUE;
}

static BOOL WINAPI GetClassInfoExA(HANDLE inst, LPCSTR name, WNDCLASSEX *out)
{
    UNUSED(inst);
    return get_class_info(name, out);
}

static BOOL WINAPI GetClassInfoExW(HANDLE inst, LPCWSTR name, WNDCLASSEX *out)
{
    UNUSED(inst);
    char *n = (uintptr_t)name < 0x10000 ? NULL : w32_narrow(name);
    BOOL ok = get_class_info(n ? n : (const char *)name, out);
    w32_free(n);
    return ok;
}

static void link_child(struct w32_window *parent, struct w32_window *w)
{
    struct w32_window **link = &parent->child;
    while (*link) {
        if (*link == w)
            return;
        link = &(*link)->sibling;
    }
    w->sibling = NULL;
    *link = w;
}

static void unlink_child(struct w32_window *w)
{
    if (!w->parent)
        return;
    for (struct w32_window **link = &w->parent->child; *link; link = &(*link)->sibling)
        if (*link == w) {
            *link = w->sibling;
            w->sibling = NULL;
            return;
        }
}

static uint32_t mods_to_mk(void)
{
    return (key_mods & KEY_MOD_SHIFT ? MK_SHIFT : 0) | (key_mods & KEY_MOD_CTRL ? MK_CONTROL : 0) |
           (mouse_buttons & BUTTON_LEFT ? MK_LBUTTON : 0) | (mouse_buttons & BUTTON_RIGHT ? MK_RBUTTON : 0) |
           (mouse_buttons & 4 ? MK_MBUTTON : 0);
}

typedef struct {
    void *lpCreateParams;
    HANDLE hInstance;
    HANDLE hMenu;
    HANDLE hwndParent;
    int cy, cx, y, x;
    LONG style;
    const void *lpszName;
    const void *lpszClass;
    DWORD dwExStyle;
} CREATESTRUCT;

static void apply_menu(struct w32_window *w);

static HANDLE create_window(DWORD ex, const char *class_name, const char *title, const void *raw_title,
                            const void *raw_class, DWORD style, int x, int y, int width, int height, HANDLE parent_h,
                            HANDLE menu, HANDLE instance, void *param, bool unicode)
{
    builtin_classes();
    struct w32_class *cls = find_class(class_name);
    if (!cls) {
        w32_set_error(1407);
        return NULL;
    }
    bool message_only = parent_h == (HANDLE)(intptr_t)-3;
    if (message_only)
        style &= ~(DWORD)(WS_CHILD | WS_VISIBLE);
    struct w32_window *parent = parent_h && !message_only ? w32_win(parent_h) : NULL;
    if ((style & WS_CHILD) && !parent) {
        w32_set_error(1406);
        return NULL;
    }
    bool headless = !(style & WS_CHILD) && !message_only && !gui_active();
    if (headless && !w32_headless) {
        w32_set_error(1400);
        return NULL;
    }
    struct w32_window *w = w32_zalloc(sizeof(*w));
    if (!w)
        return NULL;
    w->magic = W32_WINDOW_MAGIC;
    w->cls = cls;
    w->proc = cls->proc;
    w->unicode = cls->unicode;
    w->style = style;
    w->exstyle = ex;
    w->enabled = !(style & WS_DISABLED);
    w->thread = w32_thread_id();
    w->control = cls->control;
    w->text = w32_strdup(title ? title : "");
    w->extra = cls->window_extra > 0 ? w32_zalloc((size_t)cls->window_extra) : NULL;
    w->parent = parent;
    w->owner = (style & WS_CHILD) ? NULL : parent;
    w->next_all = windows;
    windows = w;

    if (style & WS_CHILD) {
        w->id = (intptr_t)menu;
        w->rect = (struct rect){x == W32_CW_USEDEFAULT ? 0 : x, y == W32_CW_USEDEFAULT ? 0 : y,
                                width == W32_CW_USEDEFAULT ? 100 : MAX(width, 0),
                                height == W32_CW_USEDEFAULT ? 30 : MAX(height, 0)};
        link_child(parent, w);
        w->font = parent->font;
    } else if (message_only || headless) {
        w->message_only = message_only;
        w->rect = (struct rect){0, 0, width == W32_CW_USEDEFAULT ? 0 : MAX(width, 0),
                                height == W32_CW_USEDEFAULT ? 0 : MAX(height, 0)};
    } else {
        struct rect work = wm_work_area(0);
        bool caption = (style & WS_CAPTION) == WS_CAPTION || !(style & WS_POPUP);
        int cw = width == W32_CW_USEDEFAULT ? MIN(DEFAULT_W, work.w * 3 / 4) : width;
        int ch = height == W32_CW_USEDEFAULT ? MIN(DEFAULT_H, work.h * 3 / 4) : height;
        if (caption && height != W32_CW_USEDEFAULT)
            ch -= TITLE_HEIGHT;
        cw = MAX(cw, 120);
        ch = MAX(ch, 40);
        unsigned flags = WIN_HIDDEN | ((style & WS_THICKFRAME) ? WIN_RESIZABLE : 0);
        if (!caption)
            flags |= WIN_FULLSIZE | WIN_NOTITLE | WIN_NOSHADOW;
        w->bridge = kzalloc(sizeof(*w->bridge));
        if (!w->bridge)
            return NULL;
        w->bridge->process = w32;
        w->bridge->win = w;
        w->wm = wm_open(w->text, ICON_APP, cw, ch, &bridge_ops, w->bridge, flags);
        if (!w->wm) {
            kfree(w->bridge);
            w->destroyed = true;
            return NULL;
        }
        w->wm->app = app_of_process();
        if (caption)
            w->caption_due = hal_ticks_ms() + 500;
        if (x != W32_CW_USEDEFAULT)
            wm_move(w->wm, x, MAX(y, wm_work_area(0).y));
        struct rect cr = wm_client_rect(w->wm);
        w->rect = (struct rect){0, 0, cr.w, cr.h};
        w->menu = menu;
    }

    CREATESTRUCT cs = {param, instance, menu, parent_h, height, width, y, x, (LONG)style, raw_title, raw_class, ex};
    if (!w32_send(w, WM_NCCREATE, 0, (LPARAM)&cs) || w32_send(w, WM_CREATE, 0, (LPARAM)&cs) == -1) {
        extern BOOL WINAPI w32_destroy_window(HANDLE);
        w32_destroy_window(w);
        return NULL;
    }
    if (w->destroyed)
        return NULL;
    w32_send(w, WM_SIZE, 0, (LPARAM)((uint32_t)w->rect.w | (uint32_t)w->rect.h << 16));
    w32_send(w, WM_MOVE, 0, (LPARAM)((uint32_t)(uint16_t)w->rect.x | (uint32_t)(uint16_t)w->rect.y << 16));
    if (w->menu)
        apply_menu(w);
    if (style & WS_VISIBLE) {
        w->visible = true;
        if (w->wm) {
            wm_show(w->wm);
            if (!(w->exstyle & WS_EX_NOACTIVATE))
                wm_focus(w->wm);
        }
        w32_invalidate(w, NULL, true);
    }
    return w;
}

HANDLE w32_create_window(DWORD ex, const char *cls, const char *title, DWORD style, int x, int y, int w, int h,
                         HANDLE parent, HANDLE menu, HANDLE inst, void *param)
{
    return create_window(ex, cls, title, title, cls, style, x, y, w, h, parent, menu, inst, param, false);
}

static HANDLE WINAPI CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                     HANDLE parent, HANDLE menu, HANDLE inst, void *param)
{
    return create_window(ex, cls, title, title, cls, style, x, y, w, h, parent, menu, inst, param, false);
}

static HANDLE WINAPI CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                                     HANDLE parent, HANDLE menu, HANDLE inst, void *param)
{
    char *c = (uintptr_t)cls < 0x10000 ? NULL : w32_narrow(cls);
    char *t = title ? w32_narrow(title) : NULL;
    HANDLE r = create_window(ex, c ? c : (const char *)cls, t, title, cls, style, x, y, w, h, parent, menu, inst,
                             param, true);
    w32_free(c);
    w32_free(t);
    return r;
}

static void kill_timers(struct w32_window *w)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used && timers[i].win == w)
            timers[i].used = false;
}

static void destroy_tree(struct w32_window *w)
{
    w32_send(w, WM_DESTROY, 0, 0);
    while (w->child) {
        struct w32_window *c = w->child;
        w->child = c->sibling;
        destroy_tree(c);
    }
    w32_send(w, WM_NCDESTROY, 0, 0);
    kill_timers(w);
    w->destroyed = true;
    w->visible = false;
    if (capture == w)
        capture = NULL;
    if (hover == w)
        hover = NULL;
    if (active == w)
        active = NULL;
    for (struct w32_window *it = windows; it; it = it->next_all)
        if (it->focus == w)
            it->focus = NULL;
}

BOOL WINAPI w32_destroy_window(HANDLE h)
{
    struct w32_window *w = h;
    bool known = false;
    for (struct w32_window *it = windows; it; it = it->next_all)
        known |= it == w;
    if (!known || w->magic != W32_WINDOW_MAGIC || w->destroyed) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    for (struct w32_window *it = windows; it; it = it->next_all)
        if (it->owner == w && !it->destroyed)
            w32_destroy_window(it);
    struct rect area = w32_area(w);
    struct w32_window *parent = is_child(w) ? w->parent : NULL;
    unlink_child(w);
    destroy_tree(w);
    if (w->wm) {
        struct window *wm = w->wm;
        w->wm = NULL;
        w->bridge->win = NULL;
        wm_destroy(wm);
    }
    if (parent) {
        struct rect pa = w32_area(parent);
        struct rect local = {area.x - pa.x, area.y - pa.y, area.w, area.h};
        w32_invalidate(parent, &local, true);
    }
    return TRUE;
}

static BOOL WINAPI DestroyWindow(HANDLE h)
{
    return w32_destroy_window(h);
}

static void show(struct w32_window *w, int cmd)
{
    bool visible = cmd != SW_HIDE;
    if (w->wm) {
        if (!visible) {
            wm_hide(w->wm);
        } else if (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE ||
                   cmd == SW_FORCEMINIMIZE) {
            wm_show(w->wm);
            wm_minimize(w->wm);
        } else {
            wm_show(w->wm);
            if (cmd == SW_SHOWMAXIMIZED && !(w->wm->flags & WIN_MAXIMIZED))
                wm_toggle_maximize(w->wm);
            if (cmd != SW_SHOWNOACTIVATE && cmd != SW_SHOWNA && !(w->exstyle & WS_EX_NOACTIVATE))
                wm_focus(w->wm);
        }
    }
    if (visible == w->visible)
        return;
    w->visible = visible;
    w32_send(w, WM_SHOWWINDOW, visible, 0);
    if (visible) {
        w32_invalidate(w, NULL, true);
    } else if (is_child(w)) {
        struct rect pa = w32_area(w->parent), a = w32_area(w);
        struct rect local = {a.x - pa.x, a.y - pa.y, a.w, a.h};
        w32_invalidate(w->parent, &local, true);
    }
}

static BOOL WINAPI ShowWindow(HANDLE h, int cmd)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    bool was = w->visible;
    show(w, cmd);
    return was;
}

static BOOL WINAPI UpdateWindow(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    if (w && w->need_paint && shown(w))
        w32_send(w, WM_PAINT, 0, 0);
    return w != NULL;
}

static BOOL WINAPI IsWindow(HANDLE h)
{
    return w32_win(h) != NULL;
}

static BOOL WINAPI IsWindowVisible(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w && shown(w);
}

static BOOL WINAPI IsWindowEnabled(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w && w->enabled;
}

static BOOL WINAPI EnableWindow(HANDLE h, BOOL enable)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    bool was_disabled = !w->enabled;
    if (w->enabled != !!enable) {
        w->enabled = enable;
        w32_send(w, WM_ENABLE, enable, 0);
        w32_invalidate(w, NULL, true);
    }
    return was_disabled;
}

static BOOL WINAPI IsIconic(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w && w->wm && (w->wm->flags & WIN_MINIMIZED);
}

static BOOL WINAPI IsZoomed(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w && w->wm && (w->wm->flags & WIN_MAXIMIZED);
}

static void to_rect(RECT *out, struct rect r)
{
    *out = (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
}

static struct rect from_rect(const RECT *r)
{
    return (struct rect){r->left, r->top, r->right - r->left, r->bottom - r->top};
}

static BOOL WINAPI GetClientRect(HANDLE h, RECT *out)
{
    struct w32_window *w = w32_win(h);
    if (!w || !out)
        return FALSE;
    to_rect(out, (struct rect){0, 0, w->rect.w, w->rect.h});
    return TRUE;
}

static struct rect screen_rect(struct w32_window *w)
{
    struct w32_window *top = w32_top(w);
    if (!top->wm)
        return w32_area(w);
    struct rect cr = wm_client_rect(top->wm), a = w32_area(w);
    if (w == top)
        return top->wm->frame;
    return (struct rect){cr.x + a.x, cr.y + a.y, a.w, a.h};
}

static BOOL WINAPI GetWindowRect(HANDLE h, RECT *out)
{
    struct w32_window *w = w32_win(h);
    if (!w || !out)
        return FALSE;
    to_rect(out, screen_rect(w));
    return TRUE;
}

static void client_origin(struct w32_window *w, int *x, int *y)
{
    struct w32_window *top = w32_top(w);
    struct rect a = w32_area(w);
    struct rect cr = top->wm ? wm_client_rect(top->wm) : (struct rect){0, 0, 0, 0};
    *x = cr.x + a.x;
    *y = cr.y + a.y;
}

static BOOL WINAPI ClientToScreen(HANDLE h, POINT *p)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    int x, y;
    client_origin(w, &x, &y);
    p->x += x;
    p->y += y;
    return TRUE;
}

static BOOL WINAPI ScreenToClient(HANDLE h, POINT *p)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    int x, y;
    client_origin(w, &x, &y);
    p->x -= x;
    p->y -= y;
    return TRUE;
}

static int WINAPI MapWindowPoints(HANDLE from, HANDLE to, POINT *points, UINT count)
{
    int fx = 0, fy = 0, tx = 0, ty = 0;
    struct w32_window *f = w32_win(from), *t = w32_win(to);
    if (f)
        client_origin(f, &fx, &fy);
    if (t)
        client_origin(t, &tx, &ty);
    for (UINT i = 0; i < count; i++) {
        points[i].x += fx - tx;
        points[i].y += fy - ty;
    }
    return (int)((uint32_t)(uint16_t)(fx - tx) | (uint32_t)(uint16_t)(fy - ty) << 16);
}

static BOOL WINAPI AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex)
{
    UNUSED(menu);
    UNUSED(ex);
    if ((style & WS_CAPTION) == WS_CAPTION || !(style & (WS_POPUP | WS_CHILD)))
        r->top -= TITLE_HEIGHT;
    return TRUE;
}

static BOOL WINAPI AdjustWindowRect(RECT *r, DWORD style, BOOL menu)
{
    return AdjustWindowRectEx(r, style, menu, 0);
}

static void send_pos_changed(struct w32_window *w, bool move, bool size)
{
    struct rect r = screen_rect(w);
    struct {
        HANDLE hwnd, after;
        int x, y, cx, cy;
        UINT flags;
    } pos = {w, NULL, r.x, r.y, r.w, r.h, 0x4 | 0x10 | (move ? 0 : 0x2) | (size ? 0 : 0x1)};
    w32_send(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&pos);
}

static void move_window(struct w32_window *w, int x, int y, int width, int height, bool move, bool size)
{
    if (w->wm) {
        if (move)
            wm_move(w->wm, x, MAX(y, wm_work_area(0).y));
        if (size) {
            int ch = height - (w->wm->flags & WIN_FULLSIZE ? 0 : TITLE_HEIGHT);
            wm_resize_client(w->wm, MAX(width, 120), MAX(ch, 40));
            struct rect cr = wm_client_rect(w->wm);
            w->rect.w = cr.w;
            w->rect.h = cr.h;
        }
    } else {
        struct rect old = w32_area(w);
        if (move) {
            w->rect.x = x;
            w->rect.y = y;
        }
        if (size) {
            w->rect.w = MAX(width, 0);
            w->rect.h = MAX(height, 0);
        }
        if (w->parent) {
            struct rect pa = w32_area(w->parent);
            struct rect local = {old.x - pa.x, old.y - pa.y, old.w, old.h};
            w32_invalidate(w->parent, &local, true);
        }
    }
    send_pos_changed(w, move, size);
    if (size)
        w32_send(w, WM_SIZE, 0, (LPARAM)((uint32_t)w->rect.w | (uint32_t)w->rect.h << 16));
    if (move)
        w32_send(w, WM_MOVE, 0, (LPARAM)((uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16));
    w32_invalidate(w, NULL, true);
}

static BOOL WINAPI MoveWindow(HANDLE h, int x, int y, int width, int height, BOOL repaint)
{
    UNUSED(repaint);
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    move_window(w, x, y, width, height, true, true);
    return TRUE;
}

static BOOL WINAPI SetWindowPos(HANDLE h, HANDLE after, int x, int y, int cx, int cy, UINT flags)
{
    UNUSED(after);
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    bool move = !(flags & 0x2), size = !(flags & 0x1);
    if (move || size) {
        struct rect r = screen_rect(w);
        if (!move && w->wm) {
            x = r.x;
            y = r.y;
        }
        move_window(w, x, y, cx, cy, move, size);
    }
    if (flags & 0x40)
        show(w, flags & 0x10 ? SW_SHOWNA : 5);
    if (flags & 0x80)
        show(w, SW_HIDE);
    return TRUE;
}

static BOOL WINAPI BringWindowToTop(HANDLE h)
{
    struct w32_window *w = w32_top(w32_win(h));
    if (w && w->wm)
        wm_focus(w->wm);
    return w != NULL;
}

static BOOL WINAPI SetForegroundWindow(HANDLE h)
{
    return BringWindowToTop(h);
}

static HANDLE WINAPI GetForegroundWindow(void)
{
    return active;
}

static HANDLE WINAPI SetActiveWindow(HANDLE h)
{
    HANDLE old = active;
    BringWindowToTop(h);
    return old;
}

static HANDLE WINAPI GetParent(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w ? (is_child(w) ? w->parent : w->owner) : NULL;
}

static HANDLE WINAPI SetParent(HANDLE h, HANDLE parent_h)
{
    struct w32_window *w = w32_win(h), *p = w32_win(parent_h);
    if (!w || !p || !is_child(w))
        return NULL;
    for (struct w32_window *a = p; a; a = a->parent)
        if (a == w) {
            w32_set_error(ERROR_INVALID_PARAMETER);
            return NULL;
        }
    HANDLE old = w->parent;
    unlink_child(w);
    w->parent = p;
    link_child(p, w);
    w32_invalidate(p, NULL, true);
    return old;
}

static HANDLE WINAPI GetDlgItem(HANDLE h, int id)
{
    struct w32_window *w = w32_win(h);
    for (struct w32_window *c = w ? w->child : NULL; c; c = c->sibling)
        if (c->id == id)
            return c;
    return NULL;
}

static int WINAPI GetDlgCtrlID(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w ? (int)w->id : 0;
}

static HANDLE WINAPI GetWindow(HANDLE h, UINT cmd)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return NULL;
    switch (cmd) {
    case 2: return w->sibling;
    case 4: return w->owner;
    case 5: return w->child;
    case 0:
        return is_child(w) ? w->parent->child : NULL;
    default: return NULL;
    }
}

static HANDLE WINAPI GetDesktopWindow(void)
{
    return (HANDLE)(uintptr_t)0x10010;
}

static BOOL WINAPI EnumChildWindows(HANDLE parent, void *fn, LPARAM lp)
{
    struct w32_window *p = w32_win(parent);
    typedef BOOL WINAPI (*enum_fn)(HANDLE, LPARAM);
    for (struct w32_window *c = p ? p->child : NULL; c; c = c->sibling)
        if (!((enum_fn)fn)(c, lp))
            break;
    return TRUE;
}

static BOOL WINAPI EnumWindows(void *fn, LPARAM lp)
{
    typedef BOOL WINAPI (*enum_fn)(HANDLE, LPARAM);
    for (struct w32_window *w = windows; w; w = w->next_all)
        if (!w->destroyed && !is_child(w) && !((enum_fn)fn)(w, lp))
            break;
    return TRUE;
}

static HANDLE find_window(const char *cls, const char *title)
{
    for (struct w32_window *w = windows; w; w = w->next_all) {
        if (w->destroyed || is_child(w))
            continue;
        if (cls && (uintptr_t)cls >= 0x10000 && str_casecmp(w->cls->name, cls))
            continue;
        if (title && strcmp(w->text, title))
            continue;
        return w;
    }
    return NULL;
}

static HANDLE WINAPI FindWindowA(LPCSTR cls, LPCSTR title)
{
    return find_window(cls, title);
}

static HANDLE WINAPI FindWindowW(LPCWSTR cls, LPCWSTR title)
{
    char *c = cls && (uintptr_t)cls >= 0x10000 ? w32_narrow(cls) : NULL;
    char *t = title ? w32_narrow(title) : NULL;
    HANDLE r = find_window(c, t);
    w32_free(c);
    w32_free(t);
    return r;
}

static int text_length(struct w32_window *w, bool unicode)
{
    if (!unicode)
        return (int)strlen(w->text);
    WCHAR *wide = w32_widen(w->text);
    int n = wide ? (int)w32_wcslen(wide) : 0;
    w32_free(wide);
    return n;
}

static int copy_text(struct w32_window *w, void *out, int cap, bool unicode)
{
    if (cap <= 0 || !out)
        return 0;
    if (!unicode) {
        str_copy(out, w->text, (size_t)cap);
        return (int)strlen(out);
    }
    size_t n = w32_cp_to_wide(w->text, (int)strlen(w->text), out, (size_t)cap - 1, 65001);
    n = MIN(n, (size_t)cap - 1);
    ((WCHAR *)out)[n] = 0;
    return (int)n;
}

static LRESULT text_message(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp, bool unicode)
{
    if (msg == WM_SETTEXT) {
        if (unicode) {
            char *n = lp ? w32_narrow((LPCWSTR)lp) : NULL;
            w32_set_text(w, n ? n : "");
            w32_free(n);
        } else {
            w32_set_text(w, (const char *)lp);
        }
        if (is_child(w))
            w32_invalidate(w, NULL, true);
        return TRUE;
    }
    if (msg == WM_GETTEXT)
        return copy_text(w, (void *)lp, (int)wp, unicode);
    return text_length(w, unicode);
}

static LRESULT def_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp, bool unicode)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    switch (msg) {
    case WM_NCCREATE: {
        const CREATESTRUCT *cs = (const CREATESTRUCT *)lp;
        if (cs && cs->lpszName && (uintptr_t)cs->lpszName >= 0x10000)
            text_message(w, WM_SETTEXT, 0, (LPARAM)cs->lpszName, unicode);
        return TRUE;
    }
    case WM_CLOSE: w32_destroy_window(w); return 0;
    case WM_PAINT: {
        extern HANDLE WINAPI w32_begin_paint(HANDLE, PAINTSTRUCT *);
        extern BOOL WINAPI w32_end_paint(HANDLE, const PAINTSTRUCT *);
        PAINTSTRUCT ps;
        w32_begin_paint(w, &ps);
        w32_end_paint(w, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        HANDLE brush = w->cls->background;
        if (!brush)
            return 0;
        RECT r = {0, 0, w->rect.w, w->rect.h};
        w32_fill_brush((HANDLE)wp, &r, brush);
        return 1;
    }
    case WM_SETTEXT:
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH: return text_message(w, msg, wp, lp, unicode);
    case WM_SETFONT:
        w->font = (HANDLE)wp;
        if (lp)
            w32_invalidate(w, NULL, true);
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_NCHITTEST: return 1;
    case WM_MOUSEACTIVATE: return 1;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == 0xf060)
            w32_post(w, WM_CLOSE, 0, 0);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: return (LRESULT)w32_syscolor_brush(15);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: return (LRESULT)w32_syscolor_brush(5);
    default: return 0;
    }
}

static LRESULT WINAPI DefWindowProcA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return def_proc(h, msg, wp, lp, false);
}

static LRESULT WINAPI DefWindowProcW(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return def_proc(h, msg, wp, lp, true);
}

static LRESULT WINAPI CallWindowProcA(WNDPROC proc, HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return proc ? proc(h, msg, wp, lp) : 0;
}

/* comctl32 window subclassing: the newest subclass runs first, DefSubclassProc passes to the next one. */
#define MAX_SUBCLASSES 8

typedef LRESULT(WINAPI *SUBCLASSPROC)(HANDLE, UINT, WPARAM, LPARAM, uintptr_t, uintptr_t);

struct subclasses {
    WNDPROC original;
    int count, level;
    struct {
        SUBCLASSPROC proc;
        uintptr_t id, data;
    } entry[MAX_SUBCLASSES];
};

static LRESULT subclass_chain(struct w32_window *w, int i, UINT msg, WPARAM wp, LPARAM lp)
{
    struct subclasses *s = w->subs;
    if (i >= s->count)
        return s->original ? s->original(w, msg, wp, lp) : 0;
    s->level = i + 1;
    int at = s->count - 1 - i;
    return s->entry[at].proc(w, msg, wp, lp, s->entry[at].id, s->entry[at].data);
}

static LRESULT WINAPI subclass_thunk(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w || !w->subs)
        return 0;
    int saved = w->subs->level;
    LRESULT r = subclass_chain(w, 0, msg, wp, lp);
    if (w->subs)
        w->subs->level = saved;
    return r;
}

static LRESULT WINAPI DefSubclassProc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    if (!w->subs)
        return w->proc ? w->proc(h, msg, wp, lp) : 0;
    int saved = w->subs->level;
    LRESULT r = subclass_chain(w, saved, msg, wp, lp);
    w->subs->level = saved;
    return r;
}

static BOOL WINAPI SetWindowSubclass(HANDLE h, SUBCLASSPROC proc, uintptr_t id, uintptr_t data)
{
    struct w32_window *w = w32_win(h);
    if (!w || !proc)
        return FALSE;
    if (!w->subs) {
        w->subs = w32_zalloc(sizeof(*w->subs));
        if (!w->subs)
            return FALSE;
        w->subs->original = w->proc;
        w->proc = subclass_thunk;
    }
    struct subclasses *s = w->subs;
    for (int i = 0; i < s->count; i++)
        if (s->entry[i].proc == proc && s->entry[i].id == id) {
            s->entry[i].data = data;
            return TRUE;
        }
    if (s->count >= MAX_SUBCLASSES)
        return FALSE;
    s->entry[s->count].proc = proc;
    s->entry[s->count].id = id;
    s->entry[s->count].data = data;
    s->count++;
    return TRUE;
}

static BOOL WINAPI GetWindowSubclass(HANDLE h, SUBCLASSPROC proc, uintptr_t id, uintptr_t *data)
{
    struct w32_window *w = w32_win(h);
    for (int i = 0; w && w->subs && i < w->subs->count; i++)
        if (w->subs->entry[i].proc == proc && w->subs->entry[i].id == id) {
            if (data)
                *data = w->subs->entry[i].data;
            return TRUE;
        }
    return FALSE;
}

static BOOL WINAPI RemoveWindowSubclass(HANDLE h, SUBCLASSPROC proc, uintptr_t id)
{
    struct w32_window *w = w32_win(h);
    if (!w || !w->subs)
        return FALSE;
    struct subclasses *s = w->subs;
    for (int i = 0; i < s->count; i++)
        if (s->entry[i].proc == proc && s->entry[i].id == id) {
            memmove(&s->entry[i], &s->entry[i + 1], (size_t)(s->count - i - 1) * sizeof(s->entry[0]));
            s->count--;
            if (s->level > i)
                s->level--;
            return TRUE;
        }
    return FALSE;
}

LRESULT WINAPI w32_def_window_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return def_proc(h, msg, wp, lp, false);
}

static LRESULT send_converted(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp, bool unicode)
{
    if (!w)
        return 0;
    if (w->unicode == unicode || (msg != WM_SETTEXT && msg != WM_GETTEXT))
        return w32_send(w, msg, wp, lp);
    if (msg == WM_SETTEXT) {
        if (unicode) {
            char *n = lp ? w32_narrow((LPCWSTR)lp) : NULL;
            LRESULT r = w32_send(w, msg, wp, (LPARAM)n);
            w32_free(n);
            return r;
        }
        WCHAR *wide = lp ? w32_widen((LPCSTR)lp) : NULL;
        LRESULT r = w32_send(w, msg, wp, (LPARAM)wide);
        w32_free(wide);
        return r;
    }
    int cap = (int)wp;
    if (cap <= 0)
        return 0;
    if (unicode) {
        char *tmp = w32_zalloc((size_t)cap * 3 + 1);
        w32_send(w, msg, (WPARAM)cap * 3, (LPARAM)tmp);
        size_t n = w32_cp_to_wide(tmp, (int)strlen(tmp), (WCHAR *)lp, (size_t)cap - 1, 65001);
        n = MIN(n, (size_t)cap - 1);
        ((WCHAR *)lp)[n] = 0;
        w32_free(tmp);
        return (LRESULT)n;
    }
    WCHAR *tmp = w32_zalloc(((size_t)cap + 1) * 2);
    w32_send(w, msg, (WPARAM)cap, (LPARAM)tmp);
    char *n = w32_narrow(tmp);
    str_copy((char *)lp, n ? n : "", (size_t)cap);
    w32_free(n);
    w32_free(tmp);
    return (LRESULT)strlen((char *)lp);
}

static LRESULT WINAPI SendMessageA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return send_converted(w32_win(h), msg, wp, lp, false);
}

static LRESULT WINAPI SendMessageW(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return send_converted(w32_win(h), msg, wp, lp, true);
}

static BOOL WINAPI PostMessageA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!h) {
        enqueue(NULL, w32_thread_id(), msg, wp, lp);
        return TRUE;
    }
    struct w32_window *w = w32_win(h);
    if (!w) {
        w32_set_error(1400);
        return FALSE;
    }
    w32_post(w, msg, wp, lp);
    return TRUE;
}

static BOOL WINAPI PostThreadMessageA(DWORD thread, UINT msg, WPARAM wp, LPARAM lp)
{
    enqueue(NULL, thread, msg, wp, lp);
    return TRUE;
}

static void WINAPI PostQuitMessage(int code)
{
    quit_posted = true;
    quit_thread = w32_thread_id();
    quit_code = code;
}

static BOOL WINAPI SetWindowTextA(HANDLE h, LPCSTR text)
{
    return (BOOL)send_converted(w32_win(h), WM_SETTEXT, 0, (LPARAM)text, false);
}

static BOOL WINAPI SetWindowTextW(HANDLE h, LPCWSTR text)
{
    return (BOOL)send_converted(w32_win(h), WM_SETTEXT, 0, (LPARAM)text, true);
}

static int WINAPI GetWindowTextA(HANDLE h, LPSTR out, int cap)
{
    struct w32_window *w = w32_win(h);
    return w ? copy_text(w, out, cap, false) : 0;
}

static int WINAPI GetWindowTextW(HANDLE h, LPWSTR out, int cap)
{
    struct w32_window *w = w32_win(h);
    return w ? copy_text(w, out, cap, true) : 0;
}

static int WINAPI GetWindowTextLengthA(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w ? text_length(w, false) : 0;
}

static int WINAPI GetWindowTextLengthW(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w ? text_length(w, true) : 0;
}

static BOOL WINAPI SetDlgItemTextA(HANDLE dlg, int id, LPCSTR text)
{
    return SetWindowTextA(GetDlgItem(dlg, id), text);
}

static BOOL WINAPI SetDlgItemTextW(HANDLE dlg, int id, LPCWSTR text)
{
    return SetWindowTextW(GetDlgItem(dlg, id), text);
}

static UINT WINAPI GetDlgItemTextA(HANDLE dlg, int id, LPSTR out, int cap)
{
    return (UINT)GetWindowTextA(GetDlgItem(dlg, id), out, cap);
}

static UINT WINAPI GetDlgItemTextW(HANDLE dlg, int id, LPWSTR out, int cap)
{
    return (UINT)GetWindowTextW(GetDlgItem(dlg, id), out, cap);
}

static UINT WINAPI GetDlgItemInt(HANDLE dlg, int id, BOOL *ok, BOOL is_signed)
{
    struct w32_window *w = w32_win(GetDlgItem(dlg, id));
    if (ok)
        *ok = w != NULL;
    if (!w)
        return 0;
    long v = 0;
    const char *p = w->text;
    bool neg = is_signed && *p == '-';
    if (neg)
        p++;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return (UINT)(neg ? -v : v);
}

static BOOL WINAPI SetDlgItemInt(HANDLE dlg, int id, UINT value, BOOL is_signed)
{
    char buf[16];
    ksnprintf(buf, sizeof(buf), is_signed ? "%d" : "%u", value);
    return SetDlgItemTextA(dlg, id, buf);
}

static LRESULT WINAPI SendDlgItemMessageA(HANDLE dlg, int id, UINT msg, WPARAM wp, LPARAM lp)
{
    return SendMessageA(GetDlgItem(dlg, id), msg, wp, lp);
}

static LRESULT WINAPI SendDlgItemMessageW(HANDLE dlg, int id, UINT msg, WPARAM wp, LPARAM lp)
{
    return SendMessageW(GetDlgItem(dlg, id), msg, wp, lp);
}

static BOOL WINAPI CheckDlgButton(HANDLE dlg, int id, UINT check)
{
    return SendMessageA(GetDlgItem(dlg, id), 0xf1, check, 0) == 0;
}

static UINT WINAPI IsDlgButtonChecked(HANDLE dlg, int id)
{
    return (UINT)SendMessageA(GetDlgItem(dlg, id), 0xf0, 0, 0);
}

static intptr_t get_long(struct w32_window *w, int index)
{
    if (index >= 0)
        return w->extra && index + 4 <= w->cls->window_extra ? *(int32_t *)(w->extra + index) : 0;
    switch (index) {
    case -4: return (intptr_t)w->proc;
    case -6: return (intptr_t)w->cls->instance;
    case -8: return (intptr_t)w->parent;
    case -12: return (intptr_t)w->id;
    case -16: return (intptr_t)(w->style | (w->visible ? WS_VISIBLE : 0));
    case -20: return (intptr_t)w->exstyle;
    case -21: return (intptr_t)w->userdata;
    default: return 0;
    }
}

static intptr_t set_long(struct w32_window *w, int index, intptr_t value)
{
    intptr_t old = get_long(w, index);
    if (index >= 0) {
        if (w->extra && index + 4 <= w->cls->window_extra)
            *(int32_t *)(w->extra + index) = (int32_t)value;
        return old;
    }
    switch (index) {
    case -4: w->proc = (WNDPROC)value; break;
    case -12: w->id = value; break;
    case -16: w->style = (uint32_t)value & ~WS_VISIBLE; break;
    case -20: w->exstyle = (uint32_t)value; break;
    case -21: w->userdata = value; break;
    }
    return old;
}

static intptr_t WINAPI GetWindowLongA(HANDLE h, int index)
{
    struct w32_window *w = w32_win(h);
    return w ? get_long(w, index) : 0;
}

static intptr_t WINAPI SetWindowLongA(HANDLE h, int index, intptr_t value)
{
    struct w32_window *w = w32_win(h);
    return w ? set_long(w, index, value) : 0;
}

static intptr_t WINAPI GetWindowLongW(HANDLE h, int index)
{
    return GetWindowLongA(h, index);
}

static intptr_t WINAPI SetWindowLongW(HANDLE h, int index, intptr_t value)
{
    struct w32_window *w = w32_win(h);
    if (w && index == -4)
        w->unicode = true;
    return SetWindowLongA(h, index, value);
}

static uintptr_t WINAPI GetClassLongA(HANDLE h, int index)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    switch (index) {
    case -10: return (uintptr_t)w->cls->background;
    case -12: return (uintptr_t)w->cls->cursor;
    case -14: return (uintptr_t)w->cls->icon;
    case -24: return (uintptr_t)w->cls->proc;
    case -26: return w->cls->style;
    case -32: return w->cls->atom;
    default: return index >= 0 && index / 4 < 8 ? w->cls->extra[index / 4] : 0;
    }
}

static uintptr_t WINAPI SetClassLongA(HANDLE h, int index, uintptr_t value)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    uintptr_t old = GetClassLongA(h, index);
    switch (index) {
    case -10: w->cls->background = (HANDLE)value; break;
    case -12: w->cls->cursor = (HANDLE)value; break;
    case -14: w->cls->icon = (HANDLE)value; break;
    case -26: w->cls->style = (uint32_t)value; break;
    default:
        if (index >= 0 && index / 4 < 8)
            w->cls->extra[index / 4] = value;
    }
    return old;
}

static int WINAPI GetClassNameA(HANDLE h, LPSTR out, int cap)
{
    struct w32_window *w = w32_win(h);
    if (!w || cap <= 0)
        return 0;
    str_copy(out, w->cls->name, (size_t)cap);
    return (int)strlen(out);
}

static BOOL WINAPI InvalidateRect(HANDLE h, const RECT *r, BOOL erase)
{
    struct w32_window *w = w32_win(h);
    if (!w) {
        for (struct w32_window *it = windows; it; it = it->next_all)
            if (!it->destroyed && !is_child(it))
                w32_invalidate(it, NULL, true);
        return TRUE;
    }
    struct rect area = r ? from_rect(r) : (struct rect){0, 0, w->rect.w, w->rect.h};
    w32_invalidate(w, &area, erase);
    return TRUE;
}

static BOOL WINAPI InvalidateRgn(HANDLE h, HANDLE rgn, BOOL erase)
{
    UNUSED(rgn);
    return InvalidateRect(h, NULL, erase);
}

static BOOL WINAPI ValidateRect(HANDLE h, const RECT *r)
{
    UNUSED(r);
    struct w32_window *w = w32_win(h);
    if (w)
        w->need_paint = false;
    return w != NULL;
}

static BOOL WINAPI GetUpdateRect(HANDLE h, RECT *out, BOOL erase)
{
    UNUSED(erase);
    struct w32_window *w = w32_win(h);
    if (!w || !w->need_paint) {
        if (out)
            *out = (RECT){0, 0, 0, 0};
        return FALSE;
    }
    if (out)
        to_rect(out, w->update);
    return TRUE;
}

static BOOL WINAPI RedrawWindow(HANDLE h, const RECT *r, HANDLE rgn, UINT flags)
{
    UNUSED(rgn);
    InvalidateRect(h, r, (flags & 4) != 0);
    if (flags & 0x100)
        UpdateWindow(h);
    return TRUE;
}

HANDLE WINAPI w32_begin_paint(HANDLE h, PAINTSTRUCT *ps)
{
    struct w32_window *w = w32_win(h);
    memset(ps, 0, sizeof(*ps));
    if (!w)
        return NULL;
    struct rect update = w->need_paint ? w->update : (struct rect){0, 0, 0, 0};
    struct rect area = w32_area(w);
    struct rect clip = rect_intersect(w32_visible_area(w), (struct rect){area.x + update.x, area.y + update.y,
                                                                          update.w, update.h});
    bool erase = w->erase;
    w->need_paint = false;
    w->erase = false;
    ps->hdc = w32_window_dc(w, clip);
    to_rect(&ps->rcPaint, update);
    ps->fErase = !erase;
    if (erase && ps->hdc)
        ps->fErase = !w32_send(w, WM_ERASEBKGND, (WPARAM)ps->hdc, 0);
    return ps->hdc;
}

BOOL WINAPI w32_end_paint(HANDLE h, const PAINTSTRUCT *ps)
{
    UNUSED(h);
    if (ps->hdc)
        w32_free_dc(ps->hdc);
    return TRUE;
}

static HANDLE WINAPI GetDC(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    if (!w) {
        extern HANDLE w32_screen_dc(void);
        return w32_screen_dc();
    }
    return w32_window_dc(w, w32_visible_area(w));
}

static HANDLE WINAPI GetDCEx(HANDLE h, HANDLE rgn, DWORD flags)
{
    UNUSED(rgn);
    UNUSED(flags);
    return GetDC(h);
}

static int WINAPI ReleaseDC(HANDLE h, HANDLE dc)
{
    UNUSED(h);
    w32_free_dc(dc);
    return 1;
}

static uintptr_t WINAPI SetTimer(HANDLE h, uintptr_t id, UINT ms, void *proc)
{
    struct w32_window *w = h ? w32_win(h) : NULL;
    if (h && !w)
        return 0;
    if (!w)
        id = 0;
    struct timer *slot = NULL;
    for (int i = 0; i < MAX_TIMERS && !slot; i++)
        if (timers[i].used && timers[i].win == w && w && timers[i].id == id)
            slot = &timers[i];
    for (int i = 0; i < MAX_TIMERS && !slot; i++)
        if (!timers[i].used)
            slot = &timers[i];
    if (!slot)
        return 0;
    if (!w && !id)
        id = 0x7000 + (uintptr_t)(slot - timers);
    *slot = (struct timer){w, id, MAX(ms, 10u), hal_ticks_ms() + MAX(ms, 10u), proc, true};
    return id;
}

static BOOL WINAPI KillTimer(HANDLE h, uintptr_t id)
{
    struct w32_window *w = h ? w32_win(h) : NULL;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used && timers[i].win == w && timers[i].id == id) {
            timers[i].used = false;
            return TRUE;
        }
    return FALSE;
}

bool w32_next_message(MSG *msg);

static bool in_filter(const MSG *m, HANDLE hwnd, UINT lo, UINT hi)
{
    if (hwnd && hwnd != (HANDLE)-1 && m->hwnd != hwnd) {
        struct w32_window *target = w32_win(m->hwnd), *want = w32_win(hwnd);
        bool descendant = false;
        for (struct w32_window *p = target; p && !descendant; p = is_child(p) ? p->parent : NULL)
            descendant = p == want;
        if (!descendant)
            return false;
    }
    if (hwnd == (HANDLE)-1 && m->hwnd)
        return false;
    return (!lo && !hi) || (m->message >= lo && m->message <= hi);
}

static struct w32_window *paint_candidate(struct w32_window *w, uint32_t thread)
{
    if (w->destroyed || !w->visible)
        return NULL;
    if (w->need_paint && w->thread == thread)
        return w;
    for (struct w32_window *c = w->child; c; c = c->sibling) {
        struct w32_window *p = paint_candidate(c, thread);
        if (p)
            return p;
    }
    return NULL;
}

static void gui_replay(void);

enum { HT_MINBUTTON = 8, HT_MAXBUTTON = 9, HT_CLOSE = 20 };

static bool caption_hit(struct w32_window *w, int x, int y)
{
    int sx, sy;
    client_origin(w, &sx, &sy);
    LRESULT r = w32_send(w, WM_NCHITTEST, 0, (LPARAM)((uint32_t)(uint16_t)(sx + x) | (uint32_t)(uint16_t)(sy + y) << 16));
    return r == HT_MINBUTTON || r == HT_MAXBUTTON || r == HT_CLOSE;
}

static bool probe_caption(struct w32_window *w)
{
    int cw = w->rect.w, y = 10, right = -1;
    for (int x = cw - 2; x > cw - 12 && x > 0 && right < 0; x -= 2)
        if (caption_hit(w, x, y))
            right = x;
    if (right < 0)
        return false;
    int left = right, bottom = y;
    while (left > 4 && caption_hit(w, left - 4, y))
        left -= 4;
    while (left > 1 && caption_hit(w, left - 1, y))
        left--;
    while (bottom < 80 && caption_hit(w, right, bottom + 4))
        bottom += 4;
    while (bottom < 84 && caption_hit(w, right, bottom + 1))
        bottom++;
    w->caption = (struct rect){left, 0, cw - left, bottom + 1};
    return true;
}

static void probe_captions(uint32_t me)
{
    uint64_t now = hal_ticks_ms();
    for (struct w32_window *w = windows; w; w = w->next_all) {
        if (!w->caption_due || now < w->caption_due || w->thread != me || !w->wm || !w->visible || w->destroyed)
            continue;
        w->caption = (struct rect){0, 0, 0, 0};
        if (probe_caption(w)) {
            w->caption_due = 0;
            cover_caption(w);
            w32_damage(w, w->caption);
        } else {
            w->caption_due = ++w->caption_tries < 120 ? now + 1000 : 0;
        }
    }
}

static bool next_message(MSG *msg, HANDLE hwnd, UINT lo, UINT hi, bool remove)
{
    gui_replay();
    uint32_t me = w32_thread_id();
    probe_captions(me);
    for (unsigned i = queue_head; i != queue_tail; i++) {
        struct queued *q = &queue[i % QUEUE_SIZE];
        if (q->thread != me || !in_filter(&q->msg, hwnd, lo, hi))
            continue;
        *msg = q->msg;
        if (remove) {
            key_mods = q->mods;
            for (unsigned k = i; k != queue_head; k--)
                queue[k % QUEUE_SIZE] = queue[(k - 1) % QUEUE_SIZE];
            queue_head++;
        }
        return true;
    }
    if (quit_posted && quit_thread == me && (!lo || WM_QUIT >= lo)) {
        *msg = (MSG){NULL, WM_QUIT, (WPARAM)quit_code, 0, message_time(), {mouse_x, mouse_y}};
        if (remove)
            quit_posted = false;
        return true;
    }
    if ((!lo && !hi) || (WM_PAINT >= lo && WM_PAINT <= hi))
        for (struct w32_window *w = windows; w; w = w->next_all) {
            if (is_child(w))
                continue;
            struct w32_window *p = paint_candidate(w, me);
            if (p && in_filter(&(MSG){p, WM_PAINT, 0, 0, 0, {0, 0}}, hwnd, lo, hi)) {
                *msg = (MSG){p, WM_PAINT, 0, 0, message_time(), {mouse_x, mouse_y}};
                return true;
            }
        }
    if ((!lo && !hi) || (WM_TIMER >= lo && WM_TIMER <= hi)) {
        uint64_t now = hal_ticks_ms();
        for (int i = 0; i < MAX_TIMERS; i++) {
            struct timer *t = &timers[i];
            if (!t->used || now < t->due || (t->win && t->win->thread != me))
                continue;
            MSG m = {t->win, WM_TIMER, t->id, (LPARAM)t->proc, (DWORD)now, {mouse_x, mouse_y}};
            if (!in_filter(&m, hwnd, lo, hi))
                continue;
            if (remove)
                t->due = now + t->interval;
            *msg = m;
            return true;
        }
    }
    return false;
}

bool w32_next_message(MSG *msg)
{
    return next_message(msg, NULL, 0, 0, true);
}

static BOOL get_message(MSG *msg, HANDLE hwnd, UINT lo, UINT hi)
{
    for (;;) {
        if (next_message(msg, hwnd, lo, hi, true))
            return msg->message != WM_QUIT;
        w32_block();
    }
}

static BOOL WINAPI GetMessageA(MSG *msg, HANDLE hwnd, UINT lo, UINT hi)
{
    return get_message(msg, hwnd, lo, hi);
}

static BOOL WINAPI PeekMessageA(MSG *msg, HANDLE hwnd, UINT lo, UINT hi, UINT flags)
{
    if (next_message(msg, hwnd, lo, hi, flags & 1))
        return TRUE;
    w32_idle(0);
    w32_yield();
    return next_message(msg, hwnd, lo, hi, flags & 1);
}

static BOOL WINAPI WaitMessage(void)
{
    MSG m;
    while (!next_message(&m, NULL, 0, 0, false))
        w32_block();
    return TRUE;
}

static DWORD WINAPI MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms, DWORD mask)
{
    UNUSED(mask);
    uint64_t deadline = hal_ticks_ms() + ms;
    for (;;) {
        MSG m;
        if (next_message(&m, NULL, 0, 0, false))
            return count;
        if (count) {
            typedef DWORD WINAPI (*wait_fn)(DWORD, const HANDLE *, BOOL, DWORD);
            struct w32_image *k = w32_find_module("kernel32.dll");
            wait_fn wait = k ? (wait_fn)w32_get_proc(k, "WaitForMultipleObjects", 0) : NULL;
            DWORD r = wait ? wait(count, handles, all, 0) : 0x102;
            if (r != 0x102)
                return r;
        }
        if (ms != 0xffffffffu && hal_ticks_ms() >= deadline)
            return 0x102;
        w32_block();
    }
}

static BOOL WINAPI TranslateMessage(const MSG *msg)
{
    UNUSED(msg);
    return FALSE;
}

LRESULT WINAPI w32_dispatch(const MSG *msg)
{
    if (msg->message == WM_TIMER && msg->lParam) {
        typedef void WINAPI (*timer_fn)(HANDLE, UINT, uintptr_t, DWORD);
        ((timer_fn)msg->lParam)(msg->hwnd, WM_TIMER, msg->wParam, msg->time);
        return 0;
    }
    struct w32_window *w = w32_win(msg->hwnd);
    if (!w)
        return 0;
    LRESULT r = w32_send(w, msg->message, msg->wParam, msg->lParam);
    if (msg->message == WM_PAINT && w32_win(msg->hwnd) && w->need_paint) {
        w->need_paint = false;
        w->erase = false;
    }
    return r;
}



static HANDLE WINAPI SetCapture(HANDLE h)
{
    HANDLE old = capture;
    capture = w32_win(h);
    return old;
}

HANDLE WINAPI w32_set_capture_internal(struct w32_window *w)
{
    HANDLE old = capture;
    capture = w;
    return old;
}

static BOOL WINAPI ReleaseCapture(void)
{
    capture = NULL;
    return TRUE;
}

static HANDLE WINAPI GetCapture(void)
{
    return capture;
}

static HANDLE WINAPI SetFocus(HANDLE h)
{
    HANDLE old = w32_focus();
    struct w32_window *w = w32_win(h);
    if (w)
        w32_set_focus(w);
    return old;
}

static HANDLE WINAPI GetFocus(void)
{
    return w32_focus();
}

static HANDLE WINAPI GetActiveWindow(void)
{
    return active;
}

static BOOL WINAPI GetCursorPos(POINT *p)
{
    p->x = mouse_x;
    p->y = mouse_y;
    return TRUE;
}

static BOOL WINAPI SetCursorPos(int x, int y)
{
    UNUSED(x);
    UNUSED(y);
    return TRUE;
}

static short WINAPI GetKeyState(int vk)
{
    bool down = false;
    switch (vk) {
    case 0x10: down = key_mods & KEY_MOD_SHIFT; break;
    case 0x11: down = key_mods & KEY_MOD_CTRL; break;
    case 0x12: down = key_mods & KEY_MOD_ALT; break;
    case 0x01: down = mouse_buttons & BUTTON_LEFT; break;
    case 0x02: down = mouse_buttons & BUTTON_RIGHT; break;
    case 0x04: down = mouse_buttons & 4; break;
    }
    return down ? (short)0x8000 : 0;
}

static int WINAPI GetSystemMetrics(int index)
{
    int sw = 1024, sh = 768;
    if (gui_active()) {
        struct rect s = wm_screen_rect(0);
        sw = s.w;
        sh = s.h;
    }
    switch (index) {
    case 0:
    case 16:
    case 78: return sw;
    case 1:
    case 79: return sh;
    case 17: return wm_work_area(0).h - TITLE_HEIGHT;
    case 2:
    case 3:
    case 20:
    case 21: return 15;
    case 4: return TITLE_HEIGHT;
    case 5:
    case 6: return 1;
    case 7:
    case 8:
    case 32:
    case 33: return 0;
    case 11:
    case 12:
    case 13:
    case 14: return 32;
    case 15: return 0;
    case 28:
    case 29: return 120;
    case 36:
    case 37: return 4;
    case 43: return 3;
    case 49:
    case 50: return 16;
    case 80: return gui_active() ? wm_screen_count() : 1;
    default: return 0;
    }
}

static void ui_logfont(uint8_t *lf, bool wide, LONG weight)
{
    w32_logfont(lf, wide, -12, weight, 0, 0, "Segoe UI");
}

static BOOL system_parameters(UINT action, UINT param, void *out, bool wide)
{
    UNUSED(param);
    uint8_t *b = out;
    DWORD *d = out;
    int lf = wide ? 92 : 60;
    if (!out)
        return FALSE;
    switch (action) {
    case 0x30: {
        struct rect a = gui_active() ? wm_work_area(0) : (struct rect){0, 0, 1024, 768};
        to_rect(out, a);
        return TRUE;
    }
    case 0x29: {
        DWORD size = d[0] ? d[0] : (DWORD)(wide ? 504 : 344);
        memset(b, 0, size);
        d[0] = size;
        LONG head[5] = {1, 17, 17, 22, 22};
        memcpy(b + 4, head, sizeof(head));
        uint8_t *p = b + 24;
        ui_logfont(p, wide, 400);
        p += lf;
        LONG sm[2] = {17, 17};
        memcpy(p, sm, 8);
        ui_logfont(p + 8, wide, 400);
        p += 8 + lf;
        LONG menu[2] = {19, 19};
        memcpy(p, menu, 8);
        p += 8;
        for (int i = 0; i < 3; i++, p += lf)
            ui_logfont(p, wide, 400);
        if ((DWORD)(p - b) + 4 <= size)
            memset(p, 0, 4);
        return TRUE;
    }
    case 0x1f: ui_logfont(b, wide, 400); return TRUE;
    case 0x42: d[1] = 0; if (d[0] >= 12) d[2] = 0; return TRUE;
    case 0x48: if (d[0] >= 8) d[1] = 1; return TRUE;
    case 0x4a: *d = 1; return TRUE;
    case 0x200a: *d = 2; return TRUE;
    case 0x200c: *d = 1400; return TRUE;
    case 0x2006:
    case 0x200e:
    case 0x2010: *d = 1; return TRUE;
    case 0x68:
    case 0x6c: *d = 3; return TRUE;
    case 0x62:
    case 0x64: *d = 4; return TRUE;
    case 0x66: *d = 400; return TRUE;
    case 0x6a: *d = 400; return TRUE;
    case 0x16: *d = 1; return TRUE;
    case 0x0a: *d = 31; return TRUE;
    case 0x46:
    case 0x5f: *d = 0; return TRUE;
    case 0x0d:
    case 0x18: *d = 75; return TRUE;
    case 0x73: if (wide) *(WCHAR *)b = 0; else *b = 0; return TRUE;
    default:
        if (action >= 0x1000 && action < 0x2000 && !(action & 1)) {
            *d = action == 0x1002 || action == 0x1004 || action == 0x1042 || action == 0x1024 || action == 0x1012 ||
                 action == 0x101a || action == 0x1022;
            return TRUE;
        }
        return FALSE;
    }
}

static BOOL WINAPI SystemParametersInfoA(UINT action, UINT param, void *out, UINT ini)
{
    UNUSED(ini);
    return system_parameters(action, param, out, false);
}

static BOOL WINAPI SystemParametersInfoW(UINT action, UINT param, void *out, UINT ini)
{
    UNUSED(ini);
    return system_parameters(action, param, out, true);
}

static BOOL WINAPI SystemParametersInfoForDpi(UINT action, UINT param, void *out, UINT ini, UINT dpi)
{
    UNUSED(ini);
    UNUSED(dpi);
    return system_parameters(action, param, out, true);
}

static HANDLE WINAPI LoadCursorA(HANDLE inst, LPCSTR name)
{
    UNUSED(inst);
    return (HANDLE)(0x20000u | ((uintptr_t)name & 0xffff));
}

static HANDLE WINAPI LoadIconA(HANDLE inst, LPCSTR name)
{
    UNUSED(inst);
    return (HANDLE)(0x30000u | ((uintptr_t)name & 0xffff));
}

static HANDLE load_bitmap(HANDLE inst, const void *name)
{
    uint32_t size = 0;
    const void *dib = w32_find_resource(inst, (const void *)2, name, &size);
    HANDLE h = dib ? w32_bitmap_from_dib(dib, size) : NULL;
    if (!h)
        w32_set_error(1814);
    return h;
}

static HANDLE WINAPI LoadImageA(HANDLE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags)
{
    UNUSED(cx);
    UNUSED(cy);
    if (type == 0)
        return (flags & 0x10) || (uintptr_t)name >= 0x10000 ? NULL : load_bitmap(inst, name);
    return (HANDLE)(0x30000u | ((uintptr_t)name & 0xffff));
}

static HANDLE WINAPI LoadBitmapA(HANDLE inst, LPCSTR name)
{
    return (uintptr_t)name < 0x10000 ? load_bitmap(inst, name) : NULL;
}

static HANDLE WINAPI SetCursor(HANDLE cursor)
{
    return cursor;
}

static int WINAPI ShowCursor(BOOL show)
{
    return show ? 1 : 0;
}

static BOOL WINAPI DestroyIcon(HANDLE icon)
{
    UNUSED(icon);
    return TRUE;
}

static struct w32_menu *menu_of(HANDLE h)
{
    struct w32_menu *m = h;
    return m && m->magic == MENU_MAGIC ? m : NULL;
}

static HANDLE WINAPI CreateMenu(void)
{
    struct w32_menu *m = w32_zalloc(sizeof(*m));
    if (m)
        m->magic = MENU_MAGIC;
    return m;
}

static BOOL WINAPI DestroyMenu(HANDLE h)
{
    struct w32_menu *m = menu_of(h);
    if (m)
        m->magic = 0;
    return m != NULL;
}

static BOOL insert_item(struct w32_menu *m, int at, UINT flags, uintptr_t id, const char *text)
{
    if (!m || m->count >= MAX_MENU_ITEMS)
        return FALSE;
    if (at < 0 || at > m->count)
        at = m->count;
    memmove(&m->items[at + 1], &m->items[at], sizeof(m->items[0]) * (size_t)(m->count - at));
    struct w32_menu_item *it = &m->items[at];
    memset(it, 0, sizeof(*it));
    it->flags = flags;
    if (flags & MF_POPUP)
        it->sub = menu_of((HANDLE)id);
    else
        it->id = id;
    if (!(flags & MF_SEPARATOR) && text && (uintptr_t)text >= 0x10000)
        str_copy(it->text, text, sizeof(it->text));
    m->count++;
    return TRUE;
}

HANDLE w32_menu_new(void)
{
    struct w32_menu *m = w32_zalloc(sizeof(*m));
    if (m)
        m->magic = MENU_MAGIC;
    return m;
}

bool w32_menu_append(HANDLE h, UINT flags, uintptr_t id, const char *text)
{
    return insert_item(menu_of(h), -1, flags, id, text);
}

static BOOL WINAPI AppendMenuA(HANDLE h, UINT flags, uintptr_t id, LPCSTR text)
{
    return insert_item(menu_of(h), -1, flags, id, text);
}

static BOOL WINAPI AppendMenuW(HANDLE h, UINT flags, uintptr_t id, LPCWSTR text)
{
    char *n = text && (uintptr_t)text >= 0x10000 ? w32_narrow(text) : NULL;
    BOOL ok = insert_item(menu_of(h), -1, flags, id, n);
    w32_free(n);
    return ok;
}

static BOOL WINAPI InsertMenuA(HANDLE h, UINT pos, UINT flags, uintptr_t id, LPCSTR text)
{
    return insert_item(menu_of(h), (flags & 0x400) ? (int)pos : -1, flags & ~0x400u, id, text);
}

static void label_of(const char *text, char *label, size_t cap, char *shortcut, size_t scap)
{
    size_t n = 0;
    shortcut[0] = 0;
    for (const char *p = text; *p && n + 1 < cap; p++) {
        if (*p == '\t') {
            str_copy(shortcut, p + 1, scap);
            break;
        }
        if (*p == '&' && p[1] != '&')
            continue;
        label[n++] = *p;
    }
    label[n] = 0;
}

static void apply_menu(struct w32_window *w)
{
    struct w32_menu *m = menu_of(w->menu);
    if (!w->wm)
        return;
    if (!m) {
        wm_set_menus(w->wm, NULL, 0);
        return;
    }
    if (!w->bar && !(w->bar = w32_zalloc(sizeof(struct menu_bar))))
        return;
    struct menu_bar *bar = w->bar;
    int count = 0;
    for (int i = 0; i < m->count && count < MAX_MENUS; i++) {
        struct w32_menu_item *top = &m->items[i];
        if (!top->sub)
            continue;
        int n = 0;
        for (int k = 0; k < top->sub->count && n < MAX_MENU_ITEMS; k++, n++) {
            struct w32_menu_item *it = &top->sub->items[k];
            struct menu_item *out = &bar->items[count][n];
            if (it->flags & MF_SEPARATOR) {
                *out = (struct menu_item){"", 0, NULL};
                continue;
            }
            label_of(it->text, bar->labels[count][n], sizeof(bar->labels[count][n]), bar->shortcuts[count][n],
                     sizeof(bar->shortcuts[count][n]));
            out->label = bar->labels[count][n];
            out->key = (it->flags & (MF_GRAYED | MF_DISABLED)) || it->sub ? -1 : KEY_COMMAND((int)(it->id & 0xffff));
            out->shortcut = bar->shortcuts[count][n][0] ? bar->shortcuts[count][n] : NULL;
        }
        char dummy[4];
        label_of(top->text, bar->labels[count][MAX_MENU_ITEMS - 1], sizeof(bar->labels[0][0]), dummy, sizeof(dummy));
        bar->menus[count] = (struct app_menu){bar->labels[count][MAX_MENU_ITEMS - 1], bar->items[count], n};
        count++;
    }
    wm_set_menus(w->wm, bar->menus, count);
}

static BOOL WINAPI SetMenu(HANDLE h, HANDLE menu)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    w->menu = menu;
    apply_menu(w);
    return TRUE;
}

static HANDLE WINAPI GetMenu(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    return w ? w->menu : NULL;
}

static BOOL WINAPI DrawMenuBar(HANDLE h)
{
    struct w32_window *w = w32_win(h);
    if (w)
        apply_menu(w);
    return w != NULL;
}

static HANDLE WINAPI GetSubMenu(HANDLE h, int pos)
{
    struct w32_menu *m = menu_of(h);
    return m && pos >= 0 && pos < m->count ? m->items[pos].sub : NULL;
}

static struct w32_menu_item *find_item(struct w32_menu *m, UINT id, UINT flags)
{
    if (!m)
        return NULL;
    if (flags & 0x400)
        return id < (UINT)m->count ? &m->items[id] : NULL;
    for (int i = 0; i < m->count; i++) {
        if (!m->items[i].sub && m->items[i].id == id)
            return &m->items[i];
        struct w32_menu_item *it = m->items[i].sub ? find_item(m->items[i].sub, id, flags) : NULL;
        if (it)
            return it;
    }
    return NULL;
}

static DWORD WINAPI CheckMenuItem(HANDLE h, UINT id, UINT flags)
{
    struct w32_menu_item *it = find_item(menu_of(h), id, flags);
    if (!it)
        return 0xffffffffu;
    DWORD old = it->flags & MF_CHECKED;
    it->flags = (it->flags & ~(unsigned)MF_CHECKED) | (flags & MF_CHECKED);
    return old;
}

static BOOL WINAPI EnableMenuItem(HANDLE h, UINT id, UINT flags)
{
    struct w32_menu_item *it = find_item(menu_of(h), id, flags);
    if (!it)
        return -1;
    BOOL old = (BOOL)(it->flags & (MF_GRAYED | MF_DISABLED));
    it->flags = (it->flags & ~(unsigned)(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED));
    for (struct w32_window *w = windows; w; w = w->next_all)
        if (!w->destroyed && w->menu == h)
            apply_menu(w);
    return old;
}

static UINT WINAPI GetMenuState(HANDLE h, UINT id, UINT flags)
{
    struct w32_menu_item *it = find_item(menu_of(h), id, flags);
    if (!it)
        return 0xffffffffu;
    return (it->flags & (MF_CHECKED | MF_GRAYED | MF_DISABLED)) | (it->sub ? 0x10 | (UINT)it->sub->count << 8 : 0);
}

static BOOL WINAPI IsMenu(HANDLE h)
{
    return menu_of(h) != NULL;
}

static HANDLE WINAPI CopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags)
{
    UNUSED(type);
    UNUSED(cx);
    UNUSED(cy);
    UNUSED(flags);
    return h;
}

static int WINAPI GetMenuItemCount(HANDLE h)
{
    struct w32_menu *m = menu_of(h);
    return m ? m->count : -1;
}

struct box_result {
    volatile bool done;
    volatile int choice;
};

static W32_STATE struct box_result *box;

static void box_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (box) {
        box->choice = index;
        box->done = true;
    }
}

int w32_gui_message_box(const char *text, const char *caption, UINT type)
{
    static const char *const sets[6][3] = {{"OK"},          {"Cancel", "OK"},        {"Abort", "Retry", "Ignore"},
                                           {"Cancel", "No", "Yes"}, {"No", "Yes"}, {"Cancel", "Retry"}};
    static const int ids[6][3] = {{1}, {2, 1}, {3, 4, 5}, {2, 7, 6}, {7, 6}, {2, 4}};
    static const int counts[6] = {1, 2, 3, 3, 2, 2};
    int set = (int)(type & 0xf);
    if (set > 5)
        set = 0;
    unsigned kind = type & 0xf0;
    enum icon_id icon = kind == 0x10 ? ICON_WARNING : kind == 0x30 ? ICON_WARNING : ICON_INFO;
    struct box_result result = {false, -1};
    struct box_result *saved = box;
    box = &result;
    struct window *owner = active && active->wm ? active->wm : NULL;
    if (!ui_alert(owner, caption ? caption : "Message", text ? text : "", icon, sets[set], counts[set], box_pick)) {
        box = saved;
        return 0;
    }
    while (!result.done) {
        MSG m;
        if (!next_message(&m, NULL, 0, 0, true)) {
            w32_block();
            continue;
        }
        bool input = (m.message >= WM_KEYDOWN && m.message <= WM_SYSCHAR) ||
                     (m.message >= WM_MOUSEMOVE && m.message <= WM_MOUSEWHEEL) || m.message == WM_CLOSE;
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (!input) {
            extern LRESULT WINAPI w32_dispatch(const MSG *msg);
            w32_dispatch(&m);
        }
    }
    box = saved;
    return result.choice >= 0 ? ids[set][result.choice] : ids[set][0];
}

static struct w32_window *window_at(struct w32_window *w, int x, int y)
{
    for (struct w32_window *c = w->child; c; c = c->sibling) {
        if (!c->visible || c->destroyed)
            continue;
        struct rect a = w32_area(c);
        if (rect_contains(a, x, y)) {
            struct w32_window *deeper = window_at(c, x, y);
            if (deeper)
                return deeper;
            if (c->control == CK_STATIC && !(c->style & 0x100))
                continue;
            return c;
        }
    }
    return NULL;
}

static void mouse_message(struct w32_window *top, struct gui_event *e)
{
    struct w32_window *target = capture && w32_top(capture) == top ? capture : NULL;
    if (!target) {
        target = window_at(top, e->x, e->y);
        if (!target)
            target = top;
    }
    if (!target->enabled)
        return;
    struct rect a = w32_area(target);
    int x = e->x - a.x, y = e->y - a.y;
    LPARAM lp = (LPARAM)((uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16);
    UINT msg = 0;
    switch (e->type) {
    case EV_MOUSE_MOVE: msg = WM_MOUSEMOVE; break;
    case EV_MOUSE_DOWN: {
        bool dbl = e->clicks >= 2 && (target->cls->style & 8);
        msg = e->button == BUTTON_RIGHT ? (dbl ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN)
              : e->button == 4          ? WM_MBUTTONDOWN
                                        : (dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN);
        if (target->control == CK_EDIT || (target != top && (target->style & WS_TABSTOP)))
            w32_set_focus(target);
        break;
    }
    case EV_MOUSE_UP:
        msg = e->button == BUTTON_RIGHT ? WM_RBUTTONUP : e->button == 4 ? WM_MBUTTONUP : WM_LBUTTONUP;
        break;
    case EV_WHEEL: {
        int sx, sy;
        client_origin(top, &sx, &sy);
        lp = (LPARAM)((uint32_t)(uint16_t)(e->x + sx) | (uint32_t)(uint16_t)(e->y + sy) << 16);
        w32_post(w32_focus() ? w32_focus() : target, WM_MOUSEWHEEL,
                 (WPARAM)(mods_to_mk() | (uint32_t)(uint16_t)(-e->wheel * 120) << 16), lp);
        return;
    }
    default: return;
    }
    if (msg == WM_MOUSEMOVE && hover != target) {
        if (hover && w32_win(hover) && hover->control)
            w32_post(hover, WM_MOUSELEAVE, 0, 0);
        hover = target;
    }
    w32_post(target, msg, mods_to_mk(), lp);
}

static void key_message(struct w32_window *top, int key)
{
    struct w32_window *target = w32_focus();
    if (!target || w32_top(target) != top)
        target = top;
    if (key >= KEY_COMMAND(0) && key < KEY_COMMAND(0x10000)) {
        w32_post(top, WM_COMMAND, (WPARAM)(key - KEY_COMMAND(0)), 0);
        return;
    }
    WORD vk = 0, scan = 0;
    WCHAR ch = 0;
    DWORD state = 0;
    if (!w32_key_info(key, &vk, &scan, &ch, &state))
        return;
    if (top->cls->dialog && w32_dialog_key(top, target, key))
        return;
    key_mods = (unsigned)key & KEY_MOD_MASK;
    if ((key & ~KEY_MOD_MASK) < 0x20 && vk >= 'A' && vk <= 'Z')
        key_mods |= KEY_MOD_CTRL;
    bool alt = key & KEY_MOD_ALT;
    LPARAM lp = (LPARAM)(1u | (uint32_t)scan << 16 | (alt ? 1u << 29 : 0));
    w32_post(target, alt ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp);
    if (ch)
        w32_post(target, alt ? WM_SYSCHAR : WM_CHAR, ch, lp);
    w32_post(target, alt ? WM_SYSKEYUP : WM_KEYUP, vk, lp | (LPARAM)(3u << 30));
    key_mods = 0;
}

static struct w32_window *bridged(struct window *wm)
{
    struct w32_bridge *b = wm->data;
    return b && b->process == w32 && w32 ? b->win : NULL;
}

/* Desktop events can arrive while another process runs: they wait here for the window's own process. */
#define DEFER_PAINT (-1)
#define DEFER_CLOSE (-2)
#define DEFERRED_MAX 128

static struct deferred {
    struct window *wm;
    struct w32_process *proc;
    struct gui_event e;
} deferred[DEFERRED_MAX];
static unsigned deferred_count;

static bool defer(struct window *wm, const struct gui_event *e, int kind)
{
    struct w32_bridge *b = wm->data;
    if (!b || b->process == w32)
        return false;
    if (deferred_count < DEFERRED_MAX) {
        struct deferred *d = &deferred[deferred_count++];
        d->wm = wm;
        d->proc = b->process;
        d->e = e ? *e : (struct gui_event){0};
        d->e.type = kind ? (enum gui_event_type)kind : d->e.type;
        w32_kick(b->process);
    }
    return true;
}

static void forget_deferred(struct window *wm)
{
    for (unsigned i = 0; i < deferred_count;)
        if (deferred[i].wm == wm)
            deferred[i] = deferred[--deferred_count];
        else
            i++;
}

static void bridge_dispatch(struct window *wm, struct gui_event *e);

static void gui_replay(void)
{
    for (unsigned i = 0; i < deferred_count;) {
        if (deferred[i].proc != w32) {
            i++;
            continue;
        }
        struct deferred d = deferred[i];
        memmove(&deferred[i], &deferred[i + 1], (deferred_count - i - 1) * sizeof(deferred[0]));
        deferred_count--;
        struct w32_window *w = bridged(d.wm);
        if (!w)
            continue;
        if ((int)d.e.type == DEFER_PAINT)
            w32_invalidate(w, NULL, true);
        else if ((int)d.e.type == DEFER_CLOSE)
            w32_post(w, WM_CLOSE, 0, 0);
        else
            bridge_dispatch(d.wm, &d.e);
    }
}

static void bridge_paint(struct window *wm, struct canvas *c)
{
    UNUSED(c);
    if (defer(wm, NULL, DEFER_PAINT))
        return;
    struct w32_window *w = bridged(wm);
    if (w)
        w32_invalidate(w, NULL, true);
}

static void bridge_event(struct window *wm, struct gui_event *e)
{
    if (defer(wm, e, 0)) {
        e->handled = e->type == EV_MOUSE_DOWN;
        return;
    }
    bridge_dispatch(wm, e);
}

static void bridge_dispatch(struct window *wm, struct gui_event *e)
{
    struct w32_window *w = bridged(wm);
    if (!w)
        return;
    struct rect cr = wm_client_rect(wm);
    mouse_x = cr.x + e->x;
    mouse_y = cr.y + e->y;
    bool mouse = e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP || e->type == EV_MOUSE_MOVE;
    if (mouse && !capture && rect_contains(w->caption, e->x, e->y)) {
        if (e->type == EV_MOUSE_DOWN)
            mouse_buttons |= e->button;
        else if (e->type == EV_MOUSE_UP)
            mouse_buttons &= ~e->button;
        if (hover && w32_win(hover))
            w32_post(hover, WM_MOUSELEAVE, 0, 0);
        hover = NULL;
        e->handled = e->type == EV_MOUSE_DOWN;
        return;
    }
    switch (e->type) {
    case EV_MOUSE_DOWN:
        mouse_buttons |= e->button;
        mouse_message(w, e);
        e->handled = true;
        break;
    case EV_MOUSE_UP:
        mouse_buttons &= ~e->button;
        mouse_message(w, e);
        break;
    case EV_MOUSE_MOVE:
    case EV_WHEEL: mouse_message(w, e); break;
    case EV_KEY: key_message(w, e->key); break;
    case EV_FOCUS:
        active = w;
        w32_post(w, WM_ACTIVATE, 1, 0);
        w32_post(w, WM_SETFOCUS, 0, 0);
        break;
    case EV_BLUR:
        if (active == w)
            active = NULL;
        w32_post(w, WM_ACTIVATE, 0, 0);
        w32_post(w, WM_KILLFOCUS, 0, 0);
        break;
    case EV_RESIZE: {
        struct rect r = wm_client_rect(wm);
        w->rect.w = r.w;
        w->rect.h = r.h;
        w->caption = (struct rect){0, 0, 0, 0};
        if (!(wm->flags & WIN_NOTITLE)) {
            w->caption_due = hal_ticks_ms() + 200;
            w->caption_tries = 0;
        }
        send_pos_changed(w, false, true);
        w32_post(w, WM_SIZE, wm->flags & WIN_MAXIMIZED ? 2 : 0, (LPARAM)((uint32_t)r.w | (uint32_t)r.h << 16));
        w32_invalidate(w, NULL, true);
        break;
    }
    case EV_LEAVE:
        if (hover && w32_win(hover))
            w32_post(hover, WM_MOUSELEAVE, 0, 0);
        hover = NULL;
        break;
    default: break;
    }
}

static bool bridge_close(struct window *wm)
{
    if (defer(wm, NULL, DEFER_CLOSE))
        return false;
    struct w32_window *w = bridged(wm);
    if (w)
        w32_post(w, WM_CLOSE, 0, 0);
    return w == NULL;
}

static void bridge_destroy(struct window *wm)
{
    forget_deferred(wm);
    struct w32_bridge *b = wm->data;
    if (b && b->process == w32 && b->win) {
        b->win->wm = NULL;
        b->win->bridge = NULL;
    }
    kfree(b);
}

static const struct window_ops bridge_ops = {bridge_paint, bridge_event, bridge_close, bridge_destroy};

void w32_gdi_reset(void);

void w32_gui_cleanup(void)
{
    w32_gdi_reset();
    for (struct w32_window *w = windows; w; w = w->next_all)
        if (w->wm) {
            struct window *wm = w->wm;
            w->wm = NULL;
            if (w->bridge)
                w->bridge->win = NULL;
            wm_destroy(wm);
        }
    classes = NULL;
    windows = NULL;
    queue_head = queue_tail = 0;
    memset(timers, 0, sizeof(timers));
    quit_posted = false;
    capture = active = hover = NULL;
    next_atom = 0;
    box = NULL;
    if (process_app) {
        kfree(((struct process_app *)process_app)->picture.px);
        kfree(process_app);
    }
    process_app = NULL;
}

static void window_attach(void)
{
    w32_gui_cleanup();
}

static const struct w32_export window_exports[] = {
    W32_FN(RegisterClassA), W32_FN(RegisterClassW), W32_FN(RegisterClassExA), W32_FN(RegisterClassExW),
    W32_FN(UnregisterClassA), W32_FN(UnregisterClassW), W32_FN(GetClassInfoExA), W32_FN(GetClassInfoExW),
    W32_FN(CreateWindowExA), W32_FN(CreateWindowExW), W32_FN(DestroyWindow), W32_FN(ShowWindow),
    W32_FN(UpdateWindow), W32_FN(IsWindow), W32_FN(IsWindowVisible), W32_FN(IsWindowEnabled), W32_FN(EnableWindow),
    W32_FN(IsIconic), W32_FN(IsZoomed), W32_FN(GetClientRect), W32_FN(GetWindowRect), W32_FN(ClientToScreen),
    W32_FN(ScreenToClient), W32_FN(MapWindowPoints), W32_FN(AdjustWindowRect), W32_FN(AdjustWindowRectEx),
    W32_FN(MoveWindow), W32_FN(SetWindowPos), W32_FN(BringWindowToTop), W32_FN(SetForegroundWindow),
    W32_FN(GetForegroundWindow), W32_FN(SetActiveWindow), W32_FN(GetActiveWindow), W32_FN(GetParent),
    W32_FN(SetParent), W32_FN(GetDlgItem), W32_FN(GetDlgCtrlID), W32_FN(GetWindow), W32_FN(GetDesktopWindow),
    W32_FN(EnumChildWindows), W32_FN(EnumWindows), W32_FN(FindWindowA), W32_FN(FindWindowW),
    W32_FN(DefWindowProcA), W32_FN(DefWindowProcW), W32_FN(CallWindowProcA),
    W32_AS("CallWindowProcW", CallWindowProcA), W32_FN(SendMessageA), W32_FN(SendMessageW),
    W32_AS("SendNotifyMessageA", SendMessageA), W32_AS("SendNotifyMessageW", SendMessageW), W32_FN(PostMessageA),
    W32_AS("PostMessageW", PostMessageA), W32_FN(PostThreadMessageA), W32_AS("PostThreadMessageW", PostThreadMessageA),
    W32_FN(PostQuitMessage), W32_FN(SetWindowTextA), W32_FN(SetWindowTextW), W32_FN(GetWindowTextA),
    W32_FN(GetWindowTextW), W32_FN(GetWindowTextLengthA), W32_FN(GetWindowTextLengthW), W32_FN(SetDlgItemTextA),
    W32_FN(SetDlgItemTextW), W32_FN(GetDlgItemTextA), W32_FN(GetDlgItemTextW), W32_FN(GetDlgItemInt),
    W32_FN(SetDlgItemInt), W32_FN(SendDlgItemMessageA), W32_FN(SendDlgItemMessageW), W32_FN(CheckDlgButton),
    W32_FN(IsDlgButtonChecked), W32_FN(GetWindowLongA), W32_FN(SetWindowLongA), W32_FN(GetWindowLongW),
    W32_FN(SetWindowLongW), W32_AS("GetWindowLongPtrA", GetWindowLongA), W32_AS("SetWindowLongPtrA", SetWindowLongA),
    W32_AS("GetWindowLongPtrW", GetWindowLongW), W32_AS("SetWindowLongPtrW", SetWindowLongW), W32_FN(GetClassLongA),
    W32_AS("GetClassLongW", GetClassLongA), W32_AS("GetClassLongPtrA", GetClassLongA),
    W32_AS("GetClassLongPtrW", GetClassLongA), W32_FN(SetClassLongA), W32_AS("SetClassLongW", SetClassLongA),
    W32_AS("SetClassLongPtrA", SetClassLongA), W32_AS("SetClassLongPtrW", SetClassLongA), W32_FN(GetClassNameA),
    W32_FN(InvalidateRect), W32_FN(InvalidateRgn), W32_FN(ValidateRect), W32_FN(GetUpdateRect), W32_FN(RedrawWindow),
    W32_AS("BeginPaint", w32_begin_paint), W32_AS("EndPaint", w32_end_paint), W32_FN(GetDC), W32_FN(GetDCEx),
    W32_AS("GetWindowDC", GetDC), W32_FN(ReleaseDC), W32_FN(SetTimer), W32_FN(KillTimer), W32_FN(GetMessageA),
    W32_AS("GetMessageW", GetMessageA), W32_FN(PeekMessageA), W32_AS("PeekMessageW", PeekMessageA),
    W32_FN(WaitMessage), W32_FN(MsgWaitForMultipleObjects), W32_FN(TranslateMessage), W32_AS("DispatchMessageA", w32_dispatch),
    W32_AS("DispatchMessageW", w32_dispatch),
    W32_FN(SetCapture),
    W32_FN(ReleaseCapture), W32_FN(GetCapture), W32_FN(SetFocus), W32_FN(GetFocus), W32_FN(GetCursorPos),
    W32_FN(SetCursorPos), W32_FN(GetKeyState), W32_AS("GetAsyncKeyState", GetKeyState), W32_FN(GetSystemMetrics),
    W32_FN(SystemParametersInfoA), W32_FN(SystemParametersInfoW), W32_FN(SystemParametersInfoForDpi), W32_FN(LoadCursorA),
    W32_AS("LoadCursorW", LoadCursorA), W32_FN(LoadIconA), W32_AS("LoadIconW", LoadIconA), W32_FN(LoadImageA),
    W32_AS("LoadImageW", LoadImageA),
    W32_FN(LoadBitmapA), W32_AS("LoadBitmapW", LoadBitmapA), W32_FN(SetCursor), W32_FN(ShowCursor), W32_FN(DestroyIcon), W32_FN(CreateMenu),
    W32_AS("CreatePopupMenu", CreateMenu), W32_FN(DestroyMenu), W32_FN(AppendMenuA), W32_FN(AppendMenuW),
    W32_FN(InsertMenuA), W32_FN(SetMenu), W32_FN(GetMenu), W32_FN(DrawMenuBar), W32_FN(GetSubMenu),
    W32_FN(CheckMenuItem), W32_FN(EnableMenuItem), W32_FN(GetMenuItemCount), W32_FN(GetMenuState), W32_FN(IsMenu),
    W32_FN(CopyImage),
};

W32_EXPORTS(window_module, "user32.dll", window_exports, .attach = window_attach);

static const struct w32_export subclass_exports[] = {
    W32_FN(SetWindowSubclass), W32_FN(GetWindowSubclass), W32_FN(RemoveWindowSubclass), W32_FN(DefSubclassProc),
};

static const char *const subclass_ordinals[414] = {
    [410] = "SetWindowSubclass", [411] = "GetWindowSubclass", [412] = "RemoveWindowSubclass",
    [413] = "DefSubclassProc",
};

W32_EXPORTS(subclass_module, "comctl32.dll", subclass_exports, .ordinals = subclass_ordinals,
            .ordinal_count = ARRAY_SIZE(subclass_ordinals));
#endif
