#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>

#include "w32gui.h"

/* The SCROLLBAR control: range, page and position live in the SB_CTL bar; arrows step a line, the track a page, the
   thumb drags, and each step goes to the parent as WM_HSCROLL/WM_VSCROLL with the control in lParam */
enum {
    SBS_VERT = 1,
    SBM_SETPOS = 0xe0,
    SBM_GETPOS = 0xe1,
    SBM_SETRANGE = 0xe2,
    SBM_GETRANGE = 0xe3,
    SBM_ENABLE_ARROWS = 0xe4,
    SBM_SETRANGEREDRAW = 0xe6,
    SBM_SETSCROLLINFO = 0xe9,
    SBM_GETSCROLLINFO = 0xea,
    WM_HSCROLL_ = 0x114,
    WM_LBUTTONUP_ = 0x202,
    WM_CAPTURECHANGED_ = 0x215,
};

struct sb_drag {
    int grab, start_pos, track;
    bool dragging;
};

static bool vertical(struct w32_window *w)
{
    return w->style & SBS_VERT;
}

static int length(struct w32_window *w)
{
    return vertical(w) ? w->rect.h : w->rect.w;
}

static int arrow(struct w32_window *w)
{
    int thick = vertical(w) ? w->rect.w : w->rect.h;
    return MIN(thick, length(w) / 3);
}

static int span(const struct w32_scroll *b)
{
    return MAX(b->max - b->min + 1, 1);
}

/* the thumb's offset and size along the track */
static void thumb(struct w32_window *w, const struct w32_scroll *b, int pos, int *at, int *size)
{
    int track = MAX(length(w) - 2 * arrow(w), 0);
    *size = b->page ? MAX(track * (int)b->page / span(b), 10) : MAX(arrow(w), 10);
    *size = MIN(*size, track);
    int range = span(b) - MAX((int)b->page, 1);
    *at = arrow(w) + (range > 0 ? (track - *size) * (pos - b->min) / range : 0);
}

static int clamp(const struct w32_scroll *b, int pos)
{
    int top = b->max - MAX((int)b->page - 1, 0);
    return MAX(MIN(pos, top), b->min);
}

static void tell(struct w32_window *w, int code, int pos)
{
    if (w->parent)
        w32_send(w->parent, vertical(w) ? WM_VSCROLL : WM_HSCROLL_, (WPARAM)((uint32_t)code | (uint32_t)pos << 16),
                 (LPARAM)(uintptr_t)w);
}

static void draw_arrow(struct canvas *c, struct rect r, int dir, uint32_t color)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int k = 0; k < 4; k++) {
        switch (dir) {
        case 0: gfx_fill(c, (struct rect){cx - k, cy - 2 + k, 2 * k + 1, 1}, color); break;
        case 1: gfx_fill(c, (struct rect){cx - 3 + k, cy - 2 + k, 7 - 2 * k, 1}, color); break;
        case 2: gfx_fill(c, (struct rect){cx - 2 + k, cy - k, 1, 2 * k + 1}, color); break;
        default: gfx_fill(c, (struct rect){cx - 2 + k, cy - 3 + k, 1, 7 - 2 * k}, color); break;
        }
    }
}

static void paint(struct w32_window *w)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    struct w32_scroll *b = &w->bars[2];
    struct sb_drag *d = w->list;
    gfx_fill(p.c, p.r, style.window);
    int a = arrow(w), at, size;
    thumb(w, b, d && d->dragging ? d->track : b->pos, &at, &size);
    bool v = vertical(w);
    uint32_t ink = b->disabled || !w->enabled ? style.tertiary : style.secondary;
    struct rect first = v ? (struct rect){p.r.x, p.r.y, p.r.w, a} : (struct rect){p.r.x, p.r.y, a, p.r.h};
    struct rect last = v ? (struct rect){p.r.x, p.r.y + p.r.h - a, p.r.w, a} : (struct rect){p.r.x + p.r.w - a, p.r.y, a, p.r.h};
    draw_arrow(p.c, first, v ? 0 : 2, ink);
    draw_arrow(p.c, last, v ? 1 : 3, ink);
    if (size > 0 && !b->disabled) {
        struct rect t = v ? (struct rect){p.r.x + 3, p.r.y + at, p.r.w - 6, size} : (struct rect){p.r.x + at, p.r.y + 3, size, p.r.h - 6};
        gfx_round(p.c, t, MIN(t.w, t.h) / 2, style.tertiary);
    }
    w32_ctl_end(w, &p);
}

static void step(struct w32_window *w, int code)
{
    struct w32_scroll *b = &w->bars[2];
    int page = MAX((int)b->page, 1), to = b->pos;
    switch (code) {
    case 0: to -= 1; break;
    case 1: to += 1; break;
    case 2: to -= page; break;
    case 3: to += page; break;
    }
    tell(w, code, clamp(b, to));
    tell(w, 8, 0);
}

static LRESULT mouse(struct w32_window *w, UINT msg, LPARAM lp)
{
    struct w32_scroll *b = &w->bars[2];
    struct sb_drag *d = w->list;
    if (!d && !(d = w->list = w32_zalloc(sizeof(*d))))
        return 0;
    int x = (int16_t)lp, y = (int16_t)(lp >> 16), along = vertical(w) ? y : x, at, size;
    thumb(w, b, b->pos, &at, &size);
    if (msg == WM_LBUTTONDOWN) {
        if (b->disabled || !w->enabled)
            return 0;
        if (along < arrow(w))
            step(w, 0);
        else if (along >= length(w) - arrow(w))
            step(w, 1);
        else if (along < at)
            step(w, 2);
        else if (along >= at + size)
            step(w, 3);
        else {
            d->dragging = true;
            d->grab = along;
            d->start_pos = d->track = b->pos;
            w32_set_capture_internal(w);
        }
        return 0;
    }
    if (!d->dragging)
        return 0;
    if (msg == WM_MOUSEMOVE) {
        int track = MAX(length(w) - 2 * arrow(w) - size, 1), range = span(b) - MAX((int)b->page, 1);
        int to = clamp(b, d->start_pos + (along - d->grab) * range / track);
        if (to != d->track) {
            d->track = to;
            tell(w, 5, to);
            w32_invalidate(w, NULL, true);
        }
        return 0;
    }
    d->dragging = false;
    w32_set_capture_internal(NULL);
    tell(w, 4, d->track);
    tell(w, 8, 0);
    w32_invalidate(w, NULL, true);
    return 0;
}

LRESULT WINAPI w32_scrollbar_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct w32_scroll *b = &w->bars[2];
    switch (msg) {
    case SBM_SETPOS: {
        int old = b->pos;
        b->pos = clamp(b, (int)wp);
        if (lp)
            w32_invalidate(w, NULL, true);
        return old;
    }
    case SBM_GETPOS: return b->pos;
    case SBM_SETRANGE:
    case SBM_SETRANGEREDRAW:
        b->min = (int)wp;
        b->max = (int)lp;
        b->pos = clamp(b, b->pos);
        w32_invalidate(w, NULL, true);
        return b->pos;
    case SBM_GETRANGE:
        if (wp)
            *(int *)(uintptr_t)(uint32_t)wp = b->min;
        if (lp)
            *(int *)(uintptr_t)(uint32_t)lp = b->max;
        return 0;
    case SBM_ENABLE_ARROWS:
        b->disabled = wp == 3;
        w32_invalidate(w, NULL, true);
        return TRUE;
    case SBM_SETSCROLLINFO:
    case SBM_GETSCROLLINFO: {
        uint32_t *si = (uint32_t *)(uintptr_t)(uint32_t)lp;
        if (!si)
            return 0;
        uint32_t mask = si[1];
        if (msg == SBM_GETSCROLLINFO) {
            struct sb_drag *d = w->list;
            if (mask & 1) {
                si[2] = (uint32_t)b->min;
                si[3] = (uint32_t)b->max;
            }
            if (mask & 2)
                si[4] = b->page;
            if (mask & 4)
                si[5] = (uint32_t)b->pos;
            if (mask & 0x10)
                si[6] = (uint32_t)(d && d->dragging ? d->track : b->pos);
            return TRUE;
        }
        if (mask & 1) {
            b->min = (int)si[2];
            b->max = (int)si[3];
        }
        if (mask & 2)
            b->page = MIN(si[4], (UINT)span(b));
        if (mask & 4)
            b->pos = (int)si[5];
        b->pos = clamp(b, b->pos);
        if (wp)
            w32_invalidate(w, NULL, true);
        return b->pos;
    }
    case WM_PAINT: paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: return mouse(w, WM_LBUTTONDOWN, lp);
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP_: return mouse(w, msg, lp);
    case WM_CAPTURECHANGED_:
        if (w->list && ((struct sb_drag *)w->list)->dragging && (HANDLE)lp != h)
            mouse(w, WM_LBUTTONUP_, 0);
        return 0;
    case WM_ENABLE: w32_invalidate(w, NULL, true); return 0;
    case 0x82: /* WM_NCDESTROY */
        w32_free(w->list);
        w->list = NULL;
        break;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}
#endif
