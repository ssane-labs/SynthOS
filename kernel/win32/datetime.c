#include <synth/config.h>
#if CONFIG_GUI
#include <synth/fmt.h>
#include <synth/gui.h>
#include <synth/string.h>
#include <synth/time.h>

#include "w32gui.h"

/* SysDateTimePick32: a date (or time) shown as fields that the arrow keys change, with the optional check box */
enum {
    DTS_UPDOWN = 0x1,
    DTS_SHOWNONE = 0x2,
    DTS_TIMEFORMAT = 0x9,
    DTM_GETSYSTEMTIME = 0x1001,
    DTM_SETSYSTEMTIME = 0x1002,
    DTM_GETRANGE = 0x1003,
    DTM_SETRANGE = 0x1004,
    DTM_SETFORMATA = 0x1005,
    DTM_SETMCCOLOR = 0x1006,
    DTM_GETMCCOLOR = 0x1007,
    DTM_GETMONTHCAL = 0x1008,
    DTM_SETMCFONT = 0x1009,
    DTM_GETMCFONT = 0x100a,
    DTM_SETFORMATW = 0x1032,
    GDT_VALID = 0,
    GDT_NONE = 1,
    DTN_DATETIMECHANGE = -759,
    WM_NCDESTROY_ = 0x82,
    WM_SETFOCUS_ = 0x07,
    WM_KILLFOCUS_ = 0x08,
    WM_GETDLGCODE_ = 0x87,
    VK_LEFT_ = 0x25,
    VK_UP_ = 0x26,
    VK_RIGHT_ = 0x27,
    VK_DOWN_ = 0x28,
    VK_SPACE_ = 0x20,
};

struct dtp {
    uint16_t v[8]; /* SYSTEMTIME: year, month, weekday, day, hour, minute, second, ms */
    bool unchecked;
    int field;
};

static bool is_time(struct w32_window *w)
{
    return (w->style & DTS_TIMEFORMAT) == DTS_TIMEFORMAT;
}

static void weekday(struct dtp *d)
{
    struct rtc_time t = {d->v[0], (uint8_t)d->v[1], (uint8_t)d->v[3], 0, 0, 0};
    d->v[2] = (uint16_t)time_weekday(&t);
}

static struct dtp *dtp_of(struct w32_window *w)
{
    if (!w->list) {
        struct dtp *d = w32_zalloc(sizeof(*d));
        if (!d)
            return NULL;
        struct rtc_time t;
        time_local(&t);
        d->v[0] = t.year;
        d->v[1] = t.month;
        d->v[3] = t.day;
        d->v[4] = t.hour;
        d->v[5] = t.minute;
        d->v[6] = t.second;
        weekday(d);
        w->list = d;
    }
    return w->list;
}

static int month_days(int year, int month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[(month - 1) % 12];
}

/* field 0..2 is year/month/day for dates and hour/minute/second for times */
static void step(struct w32_window *w, struct dtp *d, int delta)
{
    int slot = is_time(w) ? 4 + d->field : (int[]){0, 1, 3}[d->field];
    int lo = 0, hi;
    switch (slot) {
    case 0: lo = 1601; hi = 9999; break;
    case 1: lo = 1; hi = 12; break;
    case 3: lo = 1; hi = month_days(d->v[0], d->v[1]); break;
    case 4: hi = 23; break;
    default: hi = 59; break;
    }
    int v = d->v[slot] + delta;
    d->v[slot] = (uint16_t)(v < lo ? hi : v > hi ? lo : v);
    d->v[3] = (uint16_t)MIN(d->v[3], month_days(d->v[0], d->v[1]));
    weekday(d);
}

static void changed(struct w32_window *w, struct dtp *d)
{
    w32_invalidate(w, NULL, true);
    if (!w->parent)
        return;
    int head = w32->win64 ? 24 : 12;
    uint8_t nm[48] = {0};
    uint64_t from = (uintptr_t)w, id = (uint64_t)w->id;
    int code = DTN_DATETIMECHANGE;
    uint32_t flags = d->unchecked ? GDT_NONE : GDT_VALID;
    memcpy(nm, &from, w32->win64 ? 8 : 4);
    memcpy(nm + head / 3, &id, w32->win64 ? 8 : 4);
    memcpy(nm + 2 * (head / 3), &code, 4);
    memcpy(nm + head, &flags, 4);
    memcpy(nm + head + 4, d->v, sizeof(d->v));
    w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
}

static void field_text(struct w32_window *w, struct dtp *d, int i, char *out, size_t cap)
{
    if (is_time(w))
        ksnprintf(out, cap, "%02u", d->v[4 + i]);
    else if (i == 0)
        ksnprintf(out, cap, "%04u", d->v[0]);
    else
        ksnprintf(out, cap, "%02u", d->v[i == 1 ? 1 : 3]);
}

static void paint(struct w32_window *w, struct dtp *d)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    const struct ui_font *f = w32_ctl_font(w);
    bool focused = w32_focus() == w;
    struct rect r = p.r;
    gfx_round(p.c, r, 5, w->enabled ? style.field : style.window);
    gfx_round_frame(p.c, r, 5, focused ? style.accent : style.field_border);
    int x = r.x + 6, y = r.y + (r.h - f->height) / 2;
    uint32_t color = d->unchecked || !w->enabled ? style.tertiary : style.text;
    if (w->style & DTS_SHOWNONE) {
        int s = MIN(14, r.h - 6);
        struct rect box = {x, r.y + (r.h - s) / 2, s, s};
        gfx_round(p.c, box, 3, d->unchecked ? style.field : style.accent);
        gfx_round_frame(p.c, box, 3, style.field_border);
        if (!d->unchecked)
            gfx_text(p.c, box.x + (s - gfx_text_width(f, "x")) / 2, box.y + (s - f->height) / 2, f, "x", style.window);
        x += s + 6;
    }
    for (int i = 0; i < 3; i++) {
        char t[8];
        field_text(w, d, i, t, sizeof(t));
        int tw = gfx_text_width(f, t);
        if (focused && i == d->field && !d->unchecked) {
            gfx_round(p.c, (struct rect){x - 1, y, tw + 2, f->height}, 2, style.accent);
            gfx_text(p.c, x, y, f, t, style.window);
        } else {
            gfx_text(p.c, x, y, f, t, color);
        }
        x += tw;
        if (i < 2) {
            const char *sep = is_time(w) ? ":" : "-";
            gfx_text(p.c, x, y, f, sep, color);
            x += gfx_text_width(f, sep);
        }
    }
    int ax = r.x + r.w - 14;
    gfx_text(p.c, ax, y, f, (w->style & DTS_UPDOWN) || is_time(w) ? "^" : "v", style.secondary);
    w32_ctl_end(w, &p);
}

LRESULT WINAPI w32_datetime_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct dtp *d = dtp_of(w);
    if (!d)
        return w32_def_window_proc(h, msg, wp, lp);
    uint16_t *st = (uint16_t *)(uintptr_t)(uint32_t)lp;
    switch (msg) {
    case DTM_GETSYSTEMTIME:
        if (d->unchecked)
            return GDT_NONE;
        if (st)
            memcpy(st, d->v, sizeof(d->v));
        return GDT_VALID;
    case DTM_SETSYSTEMTIME:
        if (wp == GDT_NONE) {
            if (!(w->style & DTS_SHOWNONE))
                return FALSE;
            d->unchecked = true;
        } else {
            if (!st || st[1] < 1 || st[1] > 12 || st[3] < 1 || st[3] > month_days(st[0], st[1]))
                return FALSE;
            memcpy(d->v, st, sizeof(d->v));
            weekday(d);
            d->unchecked = false;
        }
        w32_invalidate(w, NULL, true);
        return TRUE;
    case DTM_GETRANGE: return 0;
    case DTM_SETRANGE:
    case DTM_SETFORMATA:
    case DTM_SETFORMATW: return TRUE;
    case DTM_SETMCCOLOR:
    case DTM_GETMCCOLOR: return -1;
    case DTM_GETMONTHCAL:
    case DTM_GETMCFONT: return 0;
    case DTM_SETMCFONT: return 0;
    case WM_GETDLGCODE_: return 0x1; /* arrow keys */
    case WM_SETFOCUS_:
    case WM_KILLFOCUS_:
        w32_invalidate(w, NULL, true);
        break;
    case WM_LBUTTONDOWN: {
        w32_set_focus(w);
        int x = (int16_t)(lp & 0xffff);
        if ((w->style & DTS_SHOWNONE) && x < 6 + MIN(14, w->rect.h - 6) + 3) {
            d->unchecked = !d->unchecked;
            changed(w, d);
            return 0;
        }
        const struct ui_font *f = w32_ctl_font(w);
        int at = 6 + ((w->style & DTS_SHOWNONE) ? MIN(14, w->rect.h - 6) + 6 : 0);
        for (int i = 0; i < 3; i++) {
            char t[8];
            field_text(w, d, i, t, sizeof(t));
            at += gfx_text_width(f, t) + gfx_text_width(f, ":");
            if (x < at || i == 2) {
                d->field = i;
                break;
            }
        }
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (!d->unchecked) {
            step(w, d, (int16_t)(wp >> 16) > 0 ? 1 : -1);
            changed(w, d);
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT_: d->field = (d->field + 2) % 3; break;
        case VK_RIGHT_: d->field = (d->field + 1) % 3; break;
        case VK_UP_:
        case VK_DOWN_:
            if (d->unchecked)
                return 0;
            step(w, d, wp == VK_UP_ ? 1 : -1);
            changed(w, d);
            return 0;
        case VK_SPACE_:
            if (w->style & DTS_SHOWNONE) {
                d->unchecked = !d->unchecked;
                changed(w, d);
            }
            return 0;
        default: return w32_def_window_proc(h, msg, wp, lp);
        }
        w32_invalidate(w, NULL, true);
        return 0;
    case WM_PAINT:
        paint(w, d);
        return 0;
    case WM_NCDESTROY_:
        w32_free(d);
        w->list = NULL;
        break;
    default:
        if (msg >= 0x1000 && msg < 0x1100)
            return 0;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}
#endif
