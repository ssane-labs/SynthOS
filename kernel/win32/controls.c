#include <synth/config.h>
#if CONFIG_GUI
#include <synth/hal.h>
#include <synth/string.h>

#include "w32gui.h"

enum {
    BS_DEFPUSHBUTTON = 1,
    BS_CHECKBOX = 2,
    BS_AUTOCHECKBOX = 3,
    BS_RADIOBUTTON = 4,
    BS_3STATE = 5,
    BS_AUTO3STATE = 6,
    BS_GROUPBOX = 7,
    BS_AUTORADIOBUTTON = 9,
    BM_GETCHECK = 0xf0,
    BM_SETCHECK = 0xf1,
    BM_GETSTATE = 0xf2,
    BM_CLICK = 0xf5,
    ES_CENTER = 0x1,
    ES_RIGHT = 0x2,
    ES_MULTILINE = 0x4,
    ES_PASSWORD = 0x20,
    ES_READONLY = 0x800,
    ES_NUMBER = 0x2000,
    EM_GETSEL = 0xb0,
    EM_SETSEL = 0xb1,
    EM_REPLACESEL = 0xc2,
    EM_LIMITTEXT = 0xc5,
    EM_SETREADONLY = 0xcf,
    EN_CHANGE = 0x300,
    SS_CENTER = 0x1,
    SS_RIGHT = 0x2,
    SS_BLACKRECT = 0x4,
    SS_GRAYRECT = 0x5,
    SS_WHITERECT = 0x6,
    SS_ETCHEDHORZ = 0x10,
    PBS_MARQUEE = 0x8,
    PBM_SETRANGE = 0x401,
    PBM_SETPOS = 0x402,
    PBM_DELTAPOS = 0x403,
    PBM_SETSTEP = 0x404,
    PBM_STEPIT = 0x405,
    PBM_SETRANGE32 = 0x406,
    PBM_GETRANGE = 0x407,
    PBM_GETPOS = 0x408,
    PBM_SETMARQUEE = 0x40a,
};

static void notify(struct w32_window *w, unsigned code)
{
    if (w->parent)
        w32_send(w->parent, WM_COMMAND, (WPARAM)((uint32_t)(uint16_t)w->id | code << 16), (LPARAM)w);
}

static const struct ui_font *font_for(struct w32_window *w)
{
    return w->font ? w32_font_of(w->font) : &font_sans;
}

struct paint_ctx {
    HANDLE dc;
    struct canvas *c;
    struct rect r;
    COLORREF text;
    bool custom_text;
};

static bool begin(struct w32_window *w, struct paint_ctx *p, UINT color_msg)
{
    PAINTSTRUCT ps;
    extern HANDLE WINAPI w32_begin_paint(HANDLE, PAINTSTRUCT *);
    extern BOOL WINAPI w32_end_paint(HANDLE, const PAINTSTRUCT *);
    p->dc = w32_begin_paint(w, &ps);
    if (!p->dc)
        return false;
    int ox, oy;
    p->c = w32_dc_canvas(p->dc, &ox, &oy);
    if (!p->c) {
        w32_end_paint(w, &ps);
        return false;
    }
    p->r = (struct rect){ox, oy, w->rect.w, w->rect.h};
    p->custom_text = false;
    if (w->parent && color_msg) {
        HANDLE brush = (HANDLE)w32_send(w->parent, color_msg, (WPARAM)p->dc, (LPARAM)w);
        if (brush && color_msg == WM_CTLCOLORSTATIC) {
            RECT r = {0, 0, w->rect.w, w->rect.h};
            w32_fill_brush(p->dc, &r, brush);
        }
        p->text = w32_dc_text_color(p->dc);
        p->custom_text = p->text != 0;
    }
    return true;
}

static void end(struct w32_window *w, struct paint_ctx *p)
{
    w32_dc_damage(p->dc, p->r);
    extern BOOL WINAPI w32_end_paint(HANDLE, const PAINTSTRUCT *);
    PAINTSTRUCT ps = {.hdc = p->dc};
    w32_end_paint(w, &ps);
}

static void strip(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    for (const char *s = in; *s && n + 1 < cap; s++) {
        if (*s == '&' && s[1] != '&')
            continue;
        if (*s == '&')
            s++;
        out[n++] = *s;
    }
    out[n] = 0;
}

static uint32_t text_color(struct w32_window *w, struct paint_ctx *p)
{
    if (!w->enabled)
        return style.tertiary;
    return p->custom_text ? w32_rgb(p->text) : style.text;
}

static void paint_button(struct w32_window *w)
{
    struct paint_ctx p;
    if (!begin(w, &p, WM_CTLCOLORBTN))
        return;
    unsigned kind = w->style & 0xf;
    char label[128];
    strip(label, sizeof(label), w->text);
    const struct ui_font *f = font_for(w);
    struct rect r = p.r;
    if (kind == BS_GROUPBOX) {
        gfx_round_frame(p.c, (struct rect){r.x, r.y + f->height / 2, r.w, r.h - f->height / 2}, 6, style.separator);
        int tw = gfx_text_width(f, label);
        if (tw) {
            gfx_clear(p.c, (struct rect){r.x + 8, r.y, tw + 8, f->height}, style.window);
            gfx_text(p.c, r.x + 12, r.y, f, label, text_color(w, &p));
        }
    } else if (kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX || kind == BS_3STATE || kind == BS_AUTO3STATE ||
               kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON) {
        bool radio = kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON;
        int size = 16, by = r.y + (r.h - size) / 2;
        struct rect box = {r.x, by, size, size};
        uint32_t fill = w->checked ? style.accent : (w->pressed ? style.control_pressed : style.field);
        gfx_round(p.c, box, radio ? size / 2 : 4, fill);
        if (!w->checked)
            gfx_round_frame(p.c, box, radio ? size / 2 : 4, style.control_border);
        else if (radio)
            gfx_circle(p.c, box.x + size / 2, box.y + size / 2, 3, style.accent_text);
        else
            gfx_symbol(p.c, box.x, box.y, SYM_CHECK, 16, style.accent_text);
        gfx_text(p.c, r.x + size + 6, r.y + (r.h - f->height) / 2, f, label, text_color(w, &p));
    } else {
        bool primary = kind == BS_DEFPUSHBUTTON;
        uint32_t bg = primary ? style.accent : w->pressed ? style.control_pressed : style.control;
        struct rect b = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
        gfx_round(p.c, (struct rect){b.x, b.y + 1, b.w, b.h}, 6, ALPHA(0, 40));
        gfx_round(p.c, b, 6, bg);
        if (!primary)
            gfx_round_frame(p.c, b, 6, style.control_border);
        if (w->pressed && primary)
            gfx_round(p.c, b, 6, ALPHA(0, 50));
        uint32_t color = primary ? style.accent_text : text_color(w, &p);
        gfx_text_box(p.c, b, f, label, w->enabled ? color : style.tertiary, ALIGN_CENTER);
    }
    end(w, &p);
}

static void paint_static(struct w32_window *w)
{
    struct paint_ctx p;
    if (!begin(w, &p, WM_CTLCOLORSTATIC))
        return;
    unsigned kind = w->style & 0x1f;
    if (kind == SS_BLACKRECT || kind == SS_GRAYRECT || kind == SS_WHITERECT) {
        gfx_fill(p.c, p.r, kind == SS_BLACKRECT ? style.text : kind == SS_GRAYRECT ? style.secondary : style.content);
    } else if (kind == SS_ETCHEDHORZ) {
        gfx_fill(p.c, (struct rect){p.r.x, p.r.y, p.r.w, 1}, style.separator);
    } else {
        const struct ui_font *f = font_for(w);
        char text[512];
        strip(text, sizeof(text), w->text);
        int y = p.r.y;
        char *line = text;
        while (line && y < p.r.y + p.r.h) {
            char *next = strchr(line, '\n');
            if (next)
                *next++ = 0;
            size_t n = strlen(line);
            if (n && line[n - 1] == '\r')
                line[--n] = 0;
            enum text_align align = kind == SS_CENTER ? ALIGN_CENTER : kind == SS_RIGHT ? ALIGN_RIGHT : ALIGN_LEFT;
            gfx_text_box(p.c, (struct rect){p.r.x, y, p.r.w, f->height}, f, line, text_color(w, &p), align);
            y += f->height;
            line = next;
        }
    }
    end(w, &p);
}

static void paint_edit(struct w32_window *w)
{
    struct paint_ctx p;
    if (!begin(w, &p, WM_CTLCOLOREDIT))
        return;
    const struct ui_font *f = font_for(w);
    bool focused = w32_focus() == w;
    struct rect r = p.r;
    gfx_round(p.c, r, 5, w->enabled && !(w->style & ES_READONLY) ? style.field : style.window);
    gfx_round_frame(p.c, r, 5, focused ? style.accent : style.field_border);
    struct rect inner = {r.x + 6, r.y + 3, r.w - 12, r.h - 6};
    p.c->clip = rect_intersect(p.c->clip, inner);
    char shown[1024];
    size_t len = strlen(w->text);
    if (w->style & ES_PASSWORD) {
        size_t n = 0;
        for (size_t i = 0; i < len && n + 4 < sizeof(shown); i++) {
            memcpy(shown + n, "•", 3);
            n += 3;
        }
        shown[n] = 0;
    } else {
        str_copy(shown, w->text, sizeof(shown));
    }
    uint32_t color = text_color(w, &p);
    if (w->style & ES_MULTILINE) {
        int y = inner.y - w->scroll * f->height;
        char *line = shown;
        size_t offset = 0;
        int caret_x = inner.x, caret_y = inner.y;
        while (line) {
            char *next = strchr(line, '\n');
            size_t n = next ? (size_t)(next - line) : strlen(line);
            size_t visible = n && line[n - 1] == '\r' ? n - 1 : n;
            gfx_text_n(p.c, inner.x, y, f, line, visible, color);
            if (w->caret >= offset && w->caret <= offset + n) {
                caret_x = inner.x + gfx_text_width_n(f, line, MIN(w->caret - offset, visible));
                caret_y = y;
            }
            offset += n + (next ? 1 : 0);
            y += f->height;
            line = next ? next + 1 : NULL;
        }
        if (focused)
            gfx_fill(p.c, (struct rect){caret_x, caret_y, 1, f->height}, style.accent);
    } else {
        int y = inner.y + (inner.h - f->height) / 2;
        size_t caret = w->style & ES_PASSWORD ? w->caret * 3 : w->caret;
        int caret_px = gfx_text_width_n(f, shown, MIN(caret, strlen(shown)));
        int shift = MAX(0, caret_px - inner.w + 2);
        gfx_text(p.c, inner.x - shift, y, f, shown, color);
        if (focused)
            gfx_fill(p.c, (struct rect){inner.x + caret_px - shift, y, 1, f->height}, style.accent);
    }
    end(w, &p);
}

static void paint_progress(struct w32_window *w)
{
    struct paint_ctx p;
    if (!begin(w, &p, 0))
        return;
    struct rect r = p.r;
    int radius = MIN(r.h / 2, 4);
    gfx_round(p.c, r, radius, style.field);
    gfx_round_frame(p.c, r, radius, style.field_border);
    int span = w->range_max - w->range_min, done;
    if (w->style & PBS_MARQUEE) {
        int seg = r.w / 4, at = (int)(hal_ticks_ms() / 8 % (uint64_t)(r.w + seg)) - seg;
        struct rect bar = rect_intersect((struct rect){r.x + at, r.y, seg, r.h}, r);
        if (bar.w > 0)
            gfx_round(p.c, bar, radius, style.accent);
    } else if (span > 0 && (done = (int)((int64_t)(MIN(MAX(w->pos, w->range_min), w->range_max) - w->range_min) *
                                         r.w / span)) > 0) {
        gfx_round(p.c, (struct rect){r.x, r.y, done, r.h}, radius, style.accent);
    }
    end(w, &p);
}

static LRESULT set_pos(struct w32_window *w, int pos)
{
    int old = w->pos;
    w->pos = MIN(MAX(pos, w->range_min), w->range_max);
    if (w->pos != old)
        w32_invalidate(w, NULL, true);
    return old;
}

static LRESULT progress_msg(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w->range_max && !w->range_min && !w->step) {
        w->range_max = 100;
        w->step = 10;
    }
    LRESULT old_range = (LRESULT)((uint32_t)(uint16_t)w->range_min | (uint32_t)(uint16_t)w->range_max << 16);
    switch (msg) {
    case PBM_SETRANGE:
        w->range_min = (int16_t)(lp & 0xffff);
        w->range_max = (int16_t)((uint32_t)lp >> 16);
        set_pos(w, w->pos);
        w32_invalidate(w, NULL, true);
        return old_range;
    case PBM_SETRANGE32:
        w->range_min = (int)wp;
        w->range_max = (int)lp;
        set_pos(w, w->pos);
        w32_invalidate(w, NULL, true);
        return old_range;
    case PBM_GETRANGE:
        if (lp) {
            ((int *)lp)[0] = w->range_min;
            ((int *)lp)[1] = w->range_max;
        }
        return wp ? w->range_min : w->range_max;
    case PBM_SETPOS: return set_pos(w, (int)wp);
    case PBM_DELTAPOS: return set_pos(w, w->pos + (int)wp);
    case PBM_SETSTEP: {
        int old = w->step;
        w->step = (int)wp;
        return old;
    }
    case PBM_STEPIT: {
        int old = w->pos, next = w->pos + w->step;
        if (next > w->range_max)
            next = w->range_min + (next - w->range_max);
        w->pos = MIN(MAX(next, w->range_min), w->range_max);
        w32_invalidate(w, NULL, true);
        return old;
    }
    case PBM_GETPOS: return w->pos;
    case PBM_SETMARQUEE:
        w32_invalidate(w, NULL, true);
        return TRUE;
    default: return 0;
    }
}

static void edit_insert(struct w32_window *w, const char *s)
{
    size_t len = strlen(w->text), add = strlen(s);
    size_t limit = w->edit_cap ? w->edit_cap : 32767;
    if (len + add > limit)
        add = limit > len ? limit - len : 0;
    if (!add)
        return;
    char *text = w32_alloc(len + add + 1);
    if (!text)
        return;
    size_t at = MIN(w->caret, len);
    memcpy(text, w->text, at);
    memcpy(text + at, s, add);
    memcpy(text + at + add, w->text + at, len - at + 1);
    w32_free(w->text);
    w->text = text;
    w->caret = at + add;
}

static void edit_changed(struct w32_window *w)
{
    w32_invalidate(w, NULL, true);
    notify(w, EN_CHANGE);
}

static void edit_key(struct w32_window *w, WPARAM vk)
{
    size_t len = strlen(w->text);
    switch (vk) {
    case 0x25:
        if (w->caret)
            w->caret--;
        break;
    case 0x27:
        if (w->caret < len)
            w->caret++;
        break;
    case 0x24: w->caret = 0; break;
    case 0x23: w->caret = len; break;
    case 0x2e:
        if (w->caret < len && !(w->style & ES_READONLY)) {
            memmove(w->text + w->caret, w->text + w->caret + 1, len - w->caret);
            edit_changed(w);
            return;
        }
        break;
    default: return;
    }
    w32_invalidate(w, NULL, true);
}

static void edit_char(struct w32_window *w, WPARAM ch)
{
    if (w->style & ES_READONLY)
        return;
    size_t len = strlen(w->text);
    if (ch == 8) {
        if (w->caret) {
            size_t from = w->caret - 1;
            while (from && ((uint8_t)w->text[from] & 0xc0) == 0x80)
                from--;
            memmove(w->text + from, w->text + w->caret, len - w->caret + 1);
            w->caret = from;
            edit_changed(w);
        }
        return;
    }
    if (ch == '\r') {
        if (w->style & ES_MULTILINE) {
            edit_insert(w, "\r\n");
            edit_changed(w);
        }
        return;
    }
    if (ch < 32 || ((w->style & ES_NUMBER) && (ch < '0' || ch > '9')))
        return;
    char buf[4] = {0};
    if (ch < 0x80) {
        buf[0] = (char)ch;
    } else if (ch < 0x800) {
        buf[0] = (char)(0xc0 | ch >> 6);
        buf[1] = (char)(0x80 | (ch & 0x3f));
    } else {
        buf[0] = (char)(0xe0 | ch >> 12);
        buf[1] = (char)(0x80 | (ch >> 6 & 0x3f));
        buf[2] = (char)(0x80 | (ch & 0x3f));
    }
    edit_insert(w, buf);
    edit_changed(w);
}

static bool toggles(unsigned kind)
{
    return kind == BS_AUTOCHECKBOX || kind == BS_AUTO3STATE || kind == BS_AUTORADIOBUTTON;
}

static void click(struct w32_window *w)
{
    unsigned kind = w->style & 0xf;
    if (kind == BS_AUTORADIOBUTTON && w->parent) {
        for (struct w32_window *s = w->parent->child; s; s = s->sibling)
            if (s != w && s->control == CK_BUTTON && (s->style & 0xf) == BS_AUTORADIOBUTTON && s->checked) {
                s->checked = false;
                w32_invalidate(s, NULL, true);
            }
        w->checked = true;
    } else if (toggles(kind)) {
        w->checked = !w->checked;
    }
    w32_invalidate(w, NULL, true);
    notify(w, 0);
}

LRESULT WINAPI w32_control_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    extern LRESULT WINAPI w32_def_window_proc(HANDLE, UINT, WPARAM, LPARAM);
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    switch (msg) {
    case WM_PAINT:
        if (w->control == CK_BUTTON)
            paint_button(w);
        else if (w->control == CK_STATIC)
            paint_static(w);
        else if (w->control == CK_PROGRESS)
            paint_progress(w);
        else
            paint_edit(w);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETTEXT: {
        LRESULT r = w32_def_window_proc(h, msg, wp, lp);
        w->caret = strlen(w->text);
        w32_invalidate(w, NULL, true);
        return r;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (w->control == CK_BUTTON && (w->style & 0xf) != BS_GROUPBOX) {
            w->pressed = true;
            extern HANDLE WINAPI w32_set_capture_internal(struct w32_window *);
            w32_set_capture_internal(w);
            w32_invalidate(w, NULL, true);
        } else if (w->control == CK_EDIT) {
            w32_set_focus(w);
            w->caret = strlen(w->text);
            w32_invalidate(w, NULL, true);
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->control == CK_BUTTON && w->pressed) {
            extern HANDLE WINAPI w32_set_capture_internal(struct w32_window *);
            w32_set_capture_internal(NULL);
            w->pressed = false;
            int x = (int16_t)(lp & 0xffff), y = (int16_t)(lp >> 16);
            w32_invalidate(w, NULL, true);
            if (x >= 0 && y >= 0 && x < w->rect.w && y < w->rect.h)
                click(w);
        } else if (w->control == CK_STATIC && (w->style & 0x100)) {
            notify(w, 0);
        }
        return 0;
    case WM_CHAR:
        if (w->control == CK_EDIT)
            edit_char(w, wp);
        else if (w->control == CK_BUTTON && (wp == ' ' || wp == '\r'))
            click(w);
        return 0;
    case WM_KEYDOWN:
        if (w->control == CK_EDIT)
            edit_key(w, wp);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (w->control == CK_EDIT)
            w32_invalidate(w, NULL, true);
        return 0;
    case WM_ENABLE: w32_invalidate(w, NULL, true); return 0;
    case BM_GETCHECK: return w->checked;
    case BM_SETCHECK:
        w->checked = wp != 0;
        w32_invalidate(w, NULL, true);
        return 0;
    case BM_GETSTATE: return (w->checked ? 1 : 0) | (w->pressed ? 4 : 0);
    case BM_CLICK: click(w); return 0;
    case EM_GETSEL: {
        DWORD c = (DWORD)w->caret;
        if (wp)
            *(DWORD *)wp = c;
        if (lp)
            *(DWORD *)lp = c;
        return (LRESULT)(c | c << 16);
    }
    case EM_SETSEL:
        w->caret = MIN((size_t)(lp == -1 ? strlen(w->text) : (size_t)lp), strlen(w->text));
        w32_invalidate(w, NULL, true);
        return 0;
    case EM_REPLACESEL:
        if (lp) {
            edit_insert(w, (const char *)lp);
            edit_changed(w);
        }
        return 0;
    case EM_LIMITTEXT: w->edit_cap = wp; return 0;
    case EM_SETREADONLY:
        w->style = wp ? w->style | ES_READONLY : w->style & ~(uint32_t)ES_READONLY;
        w32_invalidate(w, NULL, true);
        return TRUE;
    case WM_GETDLGCODE: return w->control == CK_EDIT ? 0x85 : 0;
    default:
        if (w->control == CK_PROGRESS && msg >= PBM_SETRANGE && msg <= 0x410)
            return progress_msg(w, msg, wp, lp);
        return w32_def_window_proc(h, msg, wp, lp);
    }
}
#endif
