#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "../internal.h"

#define SAMPLES 90
#define TAB_COUNT 3

struct monitor {
    struct ui_state st;
    int tab;
    uint32_t used[SAMPLES];
    int count;
    struct ui_list tasks;
    struct window *task_list[32];
    int task_count;
    char log[4096];
};

static void sample(struct monitor *m)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    if (m->count == SAMPLES) {
        memmove(m->used, m->used + 1, sizeof(m->used[0]) * (SAMPLES - 1));
        m->count--;
    }
    m->used[m->count++] = (uint32_t)hs.used;
}

static void bar(struct canvas *c, struct rect r, uint32_t permille, uint32_t color)
{
    gfx_round(c, r, 4, style.field);
    gfx_round(c, (struct rect){r.x, r.y, (int)(r.w * (int)MIN(permille, 1000u) / 1000), r.h}, 4, color);
}

static void draw_graph(struct canvas *c, struct rect r, struct monitor *m, uint64_t total)
{
    gfx_round(c, r, 6, style.field);
    for (int i = 1; i < 4; i++)
        gfx_fill(c, (struct rect){r.x, r.y + r.h * i / 4, r.w, 1}, ALPHA(style.border, 120));
    uint64_t top = total / 16;
    for (int i = 0; i < m->count; i++)
        top = MAX(top, (uint64_t)m->used[i] * 4 / 3);
    top = MIN(MAX(top, 1), total ? total : top);
    char size[16];
    fmt_size(size, sizeof(size), top);
    gfx_text_box(c, (struct rect){r.x + r.w - 88, r.y + 4, 80, 16}, &font_sans, size, style.muted, ALIGN_RIGHT);
    if (m->count < 2)
        return;
    struct canvas in = *c;
    in.clip = rect_intersect(c->clip, rect_inset(r, 2));
    int step = MAX(1, (r.w - 4) / (SAMPLES - 1));
    int x0 = r.x + r.w - 3 - (m->count - 1) * step;
    int h = r.h - 6, base = r.y + r.h - 3;
    for (int i = 0; i < m->count; i++) {
        int x = x0 + i * step;
        int y = base - (int)((uint64_t)m->used[i] * (uint64_t)h / top);
        if (i) {
            int px = x - step;
            int py = base - (int)((uint64_t)m->used[i - 1] * (uint64_t)h / top);
            for (int k = 0; k < step; k++) {
                int yy = py + (y - py) * k / step;
                gfx_fill(&in, (struct rect){px + k, yy, 1, base - yy}, ALPHA(style.accent, 60));
            }
            gfx_thick_line(&in, px, py, x, y, 2, style.accent);
        }
    }
}

static void draw_task(struct canvas *c, struct rect row, int index, bool sel, void *ctx)
{
    UNUSED(sel);
    struct monitor *m = ctx;
    struct window *w = m->task_list[index];
    if (!wm_window_alive(w))
        return;
    gfx_icon(c, row.x + 8, row.y + (row.h - 16) / 2, w->icon, 16);
    gfx_text_box(c, (struct rect){row.x + 32, row.y, row.w - 160, row.h}, &font_sans, w->title, style.text, ALIGN_LEFT);
    char size[32];
    fmt_size(size, sizeof(size), (uint64_t)w->surface.w * (uint64_t)w->surface.h * 4);
    gfx_text_box(c, (struct rect){row.x + row.w - 120, row.y, 110, row.h}, &font_sans, size, style.muted, ALIGN_RIGHT);
}

static void refresh_tasks(struct monitor *m)
{
    m->task_count = wm_window_list(m->task_list, 32);
    m->tasks.count = m->task_count;
    if (m->tasks.selected >= m->task_count)
        m->tasks.selected = m->task_count - 1;
}

static void performance(struct ui *ui, struct monitor *m, struct rect area)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    char used[16], total[16], peak[16], line[96];
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(total, sizeof(total), hs.total);
    fmt_size(peak, sizeof(peak), hs.peak);
    int x = area.x + 16, y = area.y + 12;
    ui_label(ui, (struct rect){x, y, area.w - 32, 26}, "Memory", &font_title, style.text, ALIGN_LEFT);
    ksnprintf(line, sizeof(line), "%s of %s in use   ·   peak %s   ·   %u blocks", used, total, peak, (unsigned)hs.blocks);
    ui_label(ui, (struct rect){x, y + 28, area.w - 32, 20}, line, &font_sans, style.muted, ALIGN_LEFT);
    if (ui->c)
        draw_graph(ui->c, (struct rect){x, y + 54, area.w - 32, 120}, m, hs.total);
    y += 190;
    ui_label(ui, (struct rect){x, y, area.w - 32, 26}, "Processor", &font_title, style.text, ALIGN_LEFT);
    uint64_t up = hal_ticks_ms() / 1000;
    ksnprintf(line, sizeof(line), "%s   ·   %s   ·   up %uh %02um %02us", hal_machine(), hal_arch(),
              (unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60));
    ui_label(ui, (struct rect){x, y + 28, area.w - 32, 20}, line, &font_sans, style.muted, ALIGN_LEFT);
    y += 64;
    ui_label(ui, (struct rect){x, y, area.w - 32, 26}, "Storage", &font_title, style.text, ALIGN_LEFT);
    y += 32;
    for (struct mount *mt = vfs_mounts(); mt && y < area.y + area.h - 30; mt = mt->next) {
        uint64_t t = 0, f = 0;
        if (!mt->fs->statfs || mt->fs->statfs(mt->root, &t, &f) != 0 || !t)
            continue;
        char ts[16], fs[16];
        fmt_size(ts, sizeof(ts), t);
        fmt_size(fs, sizeof(fs), f);
        ksnprintf(line, sizeof(line), "%s  (%s)   %s free of %s", mt->path, mt->fs->name, fs, ts);
        ui_label(ui, (struct rect){x, y, area.w - 32, 20}, line, &font_sans, style.text, ALIGN_LEFT);
        if (ui->c)
            bar(ui->c, (struct rect){x, y + 22, area.w - 32, 8}, (uint32_t)((t - f) * 1000 / t), style.accent2);
        y += 40;
    }
}

static void tasks(struct ui *ui, struct monitor *m, struct rect area)
{
    refresh_tasks(m);
    m->tasks.row_h = 28;
    struct rect list = {area.x + 12, area.y + 12, area.w - 24, area.h - 70};
    int r = ui_list(ui, list, &m->tasks, draw_task, m);
    struct window *sel = m->tasks.selected >= 0 && m->tasks.selected < m->task_count ? m->task_list[m->tasks.selected] : NULL;
    if (r == LIST_ACTIVATE && sel)
        wm_show(sel);
    if (ui_button(ui, (struct rect){area.x + area.w - 132, area.y + area.h - 48, 120, 34}, "End task") && sel &&
        sel != ui->win)
        wm_close(sel);
    if (ui_button(ui, (struct rect){area.x + area.w - 264, area.y + area.h - 48, 120, 34}, "Switch to") && sel)
        wm_show(sel);
}

static void log_tab(struct ui *ui, struct monitor *m, struct rect area)
{
    if (!ui->c)
        return;
    log_snapshot(m->log, sizeof(m->log));
    gfx_round(ui->c, rect_inset(area, 10), 6, style.field);
    int lines = 0;
    for (const char *p = m->log; *p; p++)
        lines += *p == '\n';
    int visible = (area.h - 30) / (font_mono.height + 1);
    const char *p = m->log;
    for (int skip = lines - visible; skip > 0 && *p; p++)
        if (*p == '\n')
            skip--;
    int y = area.y + 16;
    struct canvas inner = *ui->c;
    inner.clip = rect_intersect(ui->c->clip, rect_inset(area, 12));
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        gfx_text_n(&inner, area.x + 20, y, &font_mono, p, n, style.text);
        y += font_mono.height + 1;
        p += n + (end ? 1 : 0);
    }
}

static const char *const tab_names[TAB_COUNT] = {"Performance", "Windows", "Kernel log"};

static void frame(struct ui *ui, void *data)
{
    struct monitor *m = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    ui_panel(ui, (struct rect){0, 0, W, H}, style.window);
    ui_panel(ui, (struct rect){0, 0, W, 42}, style.panel);
    for (int i = 0; i < TAB_COUNT; i++)
        if (ui_tool(ui, (struct rect){8 + i * 124, 6, 118, 30}, i == 0 ? ICON_MONITOR : i == 1 ? ICON_APP : ICON_TEXT,
                    tab_names[i], m->tab == i))
            m->tab = i;
    struct rect area = {0, 42, W, H - 42};
    if (m->tab == 0)
        performance(ui, m, area);
    else if (m->tab == 1)
        tasks(ui, m, area);
    else
        log_tab(ui, m, area);
    if (ui->e && ui->e->type == EV_TIMER)
        sample(m);
}

static void paint(struct window *w, struct canvas *c)
{
    struct monitor *m = w->data;
    ui_run(w, &m->st, c, NULL, frame, m);
}

static void event(struct window *w, struct gui_event *e)
{
    struct monitor *m = w->data;
    ui_run(w, &m->st, NULL, e, frame, m);
}

static void destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    struct monitor *m = kzalloc(sizeof(*m));
    if (!m)
        return;
    m->tasks.selected = -1;
    sample(m);
    struct window *w = wm_open("System Monitor", ICON_MONITOR, 560, 520, &ops, m, WIN_RESIZABLE);
    if (!w) {
        kfree(m);
        return;
    }
    wm_set_timer(w, 1000);
}

static const struct gui_app monitor_app = {"System Monitor", "Memory, storage, windows and kernel log",
                                           ICON_MONITOR, launch, NULL};
GUI_APP(monitor_app);
