#include <synth/block.h>
#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/install.h>
#include <synth/keys.h>
#include <synth/parttab.h>
#include <synth/string.h>

#include <synth/module.h>

#include "../internal.h"

static struct window *installer_win, *pill_win;
static void put_away(void);

/* The installer walks through: welcome, disk, where on the disk (free space, a partition to replace or the whole
   disk), how much space, the boot menu, a summary, the copy and the restart. Nothing is written before the
   summary's button. */

#define MAX_CHOICES 12
#define NAV_H 76

enum page {
    PAGE_WELCOME,
    PAGE_DISK,
    PAGE_WHERE,
    PAGE_SIZE,
    PAGE_STARTUP,
    PAGE_CONFIRM,
    PAGE_INSTALL,
    PAGE_DONE,
    PAGE_ERROR,
};

struct choice {
    bool whole, danger;
    int replace;
    uint32_t start, count;
    char title[64], detail[112], brief[40];
};

struct installer {
    struct ui_state st;
    enum page page;
    struct spring slide, bar;
    uint64_t tick;
    struct blockdev *disks[8];
    int disk_count, chosen;
    struct disk_map map;
    struct choice choices[MAX_CHOICES];
    int choice_count, choice;
    int scroll;
    bool shrink_hint;
    int size_mb, min_mb, max_mb;
    bool copy_home;
    bool has_windows;
    int boot_menu, boot_wait, boot_default;
    struct install_job job;
};

static void go(struct installer *in, enum page page)
{
    int dir = page > in->page ? 1 : -1;
    in->page = page;
    in->scroll = 0;
    spring_set(&in->slide, dir * 1024);
    in->slide.target = 0;
}

static void scan(struct installer *in)
{
    in->disk_count = install_targets(in->disks, 8);
    if (in->chosen >= in->disk_count)
        in->chosen = -1;
    if (in->disk_count == 1)
        in->chosen = 0;
}

/* Sizes the way disk makers write them: "476 GB", "1.8 TB", "650 MB". */
static void size_text(uint64_t sectors, char *buf, size_t cap)
{
    uint64_t mb = sectors / 2048;
    if (mb >= 1024 * 1024)
        ksnprintf(buf, cap, "%u.%u TB", (unsigned)(mb >> 20), (unsigned)((mb & 0xfffff) * 10 >> 20));
    else if (mb >= 10240)
        ksnprintf(buf, cap, "%u GB", (unsigned)(mb >> 10));
    else if (mb >= 1024)
        ksnprintf(buf, cap, "%u.%u GB", (unsigned)(mb >> 10), (unsigned)((mb & 1023) * 10 >> 10));
    else
        ksnprintf(buf, cap, "%u MB", (unsigned)mb);
}

static uint32_t min_sectors(void)
{
    return MAX(install_min_sectors(false), 2048u * 512);
}

static void add_choice(struct installer *in, const struct choice *c)
{
    if (in->choice_count < MAX_CHOICES)
        in->choices[in->choice_count++] = *c;
}

/* Free space first (nothing is lost), then partitions that could make way, then the whole disk. */
static void build_choices(struct installer *in)
{
    struct blockdev *d = in->disks[in->chosen];
    in->choice_count = 0;
    in->choice = -1;
    in->shrink_hint = false;
    in->has_windows = false;
    bool readable = disk_map_read(d, &in->map) == 0;
    bool editable = readable && in->map.scheme != SCHEME_NONE && !in->map.read_only;
    char size[16];
    bool has_windows = false;
    for (int i = 0; editable && i < in->map.count; i++) {
        const struct part *p = &in->map.parts[i];
        has_windows |= p->kind == PART_WINDOWS;
        in->has_windows |= p->kind == PART_WINDOWS;
        if (p->kind != PART_FREE)
            continue;
        uint32_t start = (p->start + PART_ALIGN - 1) / PART_ALIGN * PART_ALIGN;
        uint32_t count = p->start + p->count - start;
        if (count < min_sectors())
            continue;
        struct choice c = {.replace = -1, .start = start, .count = count};
        size_text(count, size, sizeof(size));
        ksnprintf(c.title, sizeof(c.title), "Next to the other systems, in %s of free space", size);
        str_copy(c.detail, "Nothing on the disk is changed or erased", sizeof(c.detail));
        str_copy(c.brief, "Free space", sizeof(c.brief));
        add_choice(in, &c);
    }
    in->shrink_hint = editable && has_windows && in->choice_count == 0;
    for (int i = 0; editable && i < in->map.count; i++) {
        const struct part *p = &in->map.parts[i];
        if (p->kind == PART_FREE || p->kind == PART_EFI || p->kind == PART_MSR || p->kind == PART_EXTENDED ||
            p->kind == PART_RECOVERY)
            continue;
        uint32_t start = (p->start + PART_ALIGN - 1) / PART_ALIGN * PART_ALIGN;
        if (p->start + p->count < start + min_sectors())
            continue;
        struct choice c = {.danger = true, .replace = p->slot, .start = start, .count = p->start + p->count - start};
        size_text(p->count, size, sizeof(size));
        ksnprintf(c.title, sizeof(c.title), "Instead of partition %d (%s, %s)", p->slot + 1, part_kind_name(p->kind),
                  size);
        ksnprintf(c.brief, sizeof(c.brief), "Instead of partition %d", p->slot + 1);
        if (p->label[0])
            ksnprintf(c.detail, sizeof(c.detail), "Everything on \"%s\" is erased; other partitions stay", p->label);
        else
            str_copy(c.detail, "Everything on this partition is erased; other partitions stay", sizeof(c.detail));
        add_choice(in, &c);
    }
    struct choice whole = {.whole = true, .danger = true, .replace = -1, .count = d->sectors};
    size_text(d->sectors, size, sizeof(size));
    ksnprintf(whole.title, sizeof(whole.title), "The whole disk (%s)", size);
    str_copy(whole.brief, "The whole disk", sizeof(whole.brief));
    str_copy(whole.detail, readable && in->map.count && in->map.scheme != SCHEME_NONE
                               ? "Everything on the disk is erased, all other systems too"
                               : "The disk looks empty; SynthOS takes all of it",
             sizeof(whole.detail));
    add_choice(in, &whole);
    if (in->choice_count && !in->choices[0].danger)
        in->choice = 0;
}

/* The installer covers the whole screen; its pages sit in a column in the middle. */
static struct rect panel(struct ui *ui)
{
    int W = ui->win->surface.w, H = ui->win->surface.h;
    int pw = MIN(W, 760), ph = MIN(H - 60, 620);
    return (struct rect){(W - pw) / 2, MAX(60, (H - ph) / 2), pw, ph};
}

/* Everything above the buttons; on a small screen the margins shrink first. */
static struct rect content(struct ui *ui, struct installer *in)
{
    struct rect p = panel(ui);
    int pad = p.w < 600 ? 24 : 40, top = p.h < 480 ? 18 : 30;
    int dx = in->slide.pos * p.w / 1024 / 3;
    return (struct rect){p.x + pad + dx, p.y + top, p.w - 2 * pad, MAX(p.h - top - NAV_H, 40)};
}

/* A list of cards that scrolls when the window is too short for all of them. */
struct card_list {
    struct rect area;
    struct canvas *saved, clipped;
    int total, width;
};

static void list_begin(struct ui *ui, struct installer *in, struct card_list *l, struct rect area, int total)
{
    l->area = area;
    l->total = total;
    bool scrolls = total > area.h;
    l->width = area.w - (scrolls ? 14 : 0);
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(area, ui->e->x, ui->e->y)) {
        in->scroll += ui->e->wheel * 40;
        ui->changed = true;
        ui->consumed = true;
    }
    in->scroll = CLAMP(in->scroll, 0, MAX(0, total - area.h));
    l->saved = ui->c;
    if (ui->c) {
        l->clipped = *ui->c;
        l->clipped.clip = rect_intersect(ui->c->clip, area);
        ui->c = &l->clipped;
    }
}

static bool list_hit(struct ui *ui, struct card_list *l, struct rect card)
{
    return ui_hover(ui, card) && ui->e && rect_contains(l->area, ui->e->x, ui->e->y);
}

static void list_end(struct ui *ui, struct installer *in, struct card_list *l)
{
    ui->c = l->saved;
    if (l->total > l->area.h)
        ui_scrollbar(ui, (struct rect){l->area.x + l->area.w - 12, l->area.y, 12, l->area.h}, &in->scroll, l->total,
                     l->area.h);
}

/* Keeps the card picked with the keyboard in view. */
static void list_show(struct installer *in, struct card_list *l, int top, int h)
{
    if (top < in->scroll)
        in->scroll = top;
    else if (top + h > in->scroll + l->area.h)
        in->scroll = top + h - l->area.h;
}

static bool nav(struct ui *ui, const char *back, const char *next, bool next_enabled, bool *went_back)
{
    struct rect p = panel(ui);
    int W = p.x + p.w, H = p.y + p.h;
    *went_back = back && ui_button(ui, (struct rect){W - 40 - 290, H - 58, 120, 32}, back);
    if (!next)
        return false;
    struct rect r = {W - 40 - 160, H - 58, 160, 32};
    if (!next_enabled) {
        if (ui->c) {
            gfx_round(ui->c, r, 16, style.control);
            gfx_text_box(ui->c, r, &font_bold, next, style.tertiary, ALIGN_CENTER);
        }
        return false;
    }
    int key = KEY_BASE(ui_key(ui));
    if (key == KEY_ENTER || key == '\r') {
        ui->consumed = true;
        return true;
    }
    return ui_button_primary(ui, r, next);
}

static uint32_t kind_color(enum part_kind kind)
{
    switch (kind) {
    case PART_FREE: return style.control;
    case PART_SYNTHOS: return style.accent;
    case PART_WINDOWS: return style.palette[12];
    case PART_EFI: return style.palette[11];
    case PART_LINUX: return style.palette[10];
    case PART_RECOVERY: return style.palette[13];
    default: return style.tertiary;
    }
}

/* A bar with every partition in proportion; `mark` paints the space SynthOS would take. */
static void draw_map(struct ui *ui, struct installer *in, struct rect r, const struct choice *mark, uint32_t mark_count)
{
    if (!ui->c)
        return;
    uint64_t total = in->disks[in->chosen]->sectors;
    gfx_round(ui->c, r, 10, style.control);
    struct canvas clip = *ui->c;
    for (int i = 0; i < in->map.count; i++) {
        const struct part *p = &in->map.parts[i];
        int x0 = r.x + (int)((uint64_t)p->start * r.w / total), x1 = r.x + (int)(((uint64_t)p->start + p->count) * r.w / total);
        if (x1 - x0 < 3)
            x1 = x0 + 3;
        if (p->kind != PART_FREE)
            gfx_fill(&clip, (struct rect){x0, r.y, x1 - x0 - 1, r.h}, kind_color(p->kind));
    }
    if (mark) {
        uint64_t start = mark->whole ? 0 : mark->start, count = mark->whole ? total : mark_count;
        int x0 = r.x + (int)(start * r.w / total), x1 = r.x + (int)((start + count) * r.w / total);
        gfx_fill(&clip, (struct rect){x0, r.y, MAX(x1 - x0, 4), r.h}, style.accent);
        if (x1 - x0 > 70)
            gfx_text_box(&clip, (struct rect){x0, r.y, x1 - x0, r.h}, &font_bold, "SynthOS", style.accent_text,
                         ALIGN_CENTER);
    }
    gfx_round_frame(ui->c, r, 10, style.separator);
}

static int draw_legend(struct ui *ui, struct installer *in, int x, int y, int w)
{
    int cx = x, y0 = y;
    for (int i = 0; i < in->map.count; i++) {
        const struct part *p = &in->map.parts[i];
        char size[16], text[80];
        size_text(p->count, size, sizeof(size));
        if (p->kind == PART_FREE)
            ksnprintf(text, sizeof(text), "Free %s", size);
        else
            ksnprintf(text, sizeof(text), "%d  %s  %s", p->slot + 1, p->label[0] ? p->label : part_kind_name(p->kind),
                      size);
        int tw = MIN(gfx_text_width(&font_small, text) + 26, w);
        if (cx > x && cx + tw > x + w) {
            cx = x;
            y += 18;
        }
        if (ui->c) {
            gfx_circle(ui->c, cx + 5, y + 8, 5, kind_color(p->kind));
            gfx_text_box(ui->c, (struct rect){cx + 14, y, tw - 20, 17}, &font_small, text, style.secondary, ALIGN_LEFT);
        }
        cx += tw;
    }
    return in->map.count ? y + 18 - y0 : 0;
}

static void welcome(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    static const char about[] =
        "Install SynthOS on a disk of this computer: next to Windows or another system in free space, "
        "instead of a partition you no longer need, or on a whole disk. Nothing is written until you "
        "confirm the summary.";
    int text_h = ui_wrap_text(NULL, (struct rect){0, 0, r.w - 80, 200}, &font_sans, about, 0);
    r.y += MAX(0, (r.h - 214 - text_h) / 2) - 20;
    if (ui->c) {
        struct rect logo = {r.x + (r.w - 96) / 2, r.y + 20, 96, 96};
        gfx_icon(ui->c, logo.x, logo.y, ICON_LOGO, 96);
    }
    ui_label(ui, (struct rect){r.x, r.y + 134, r.w, 34}, "SynthOS " SYNTH_VERSION, &font_large, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 172, r.w, 20}, "\"" SYNTH_CODENAME "\"", &font_title, style.accent, ALIGN_CENTER);
    ui_wrap_text(ui->c, (struct rect){r.x + 40, r.y + 214, r.w - 80, text_h}, &font_sans, about, style.secondary);
    bool back;
    if (nav(ui, NULL, "Continue", true, &back)) {
        scan(in);
        go(in, PAGE_DISK);
    }
}

static void disk_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, "Which disk?", &font_title, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){r.x, r.y + 30, r.w, 18}, "Next you choose where on it SynthOS goes.", &font_sans,
             style.secondary, ALIGN_LEFT);
    if (!in->disk_count) {
        ui_wrap_text(ui->c, (struct rect){r.x, r.y + 80, r.w, 60}, &font_sans,
                     "No other disk was found. Connect a disk, or check that the disk controller is in AHCI mode in "
                     "the firmware settings.",
                     style.text);
        if (ui_button(ui, (struct rect){r.x, r.y + 140, 120, 30}, "Look again"))
            scan(in);
    }
    struct card_list l;
    list_begin(ui, in, &l, (struct rect){r.x, r.y + 64, r.w, r.h - 64}, in->disk_count * 66 - 8);
    for (int i = 0; i < in->disk_count; i++) {
        struct rect card = {r.x, r.y + 64 + i * 66 - in->scroll, l.width, 58};
        bool sel = in->chosen == i, hot = list_hit(ui, &l, card);
        if (ui->c) {
            gfx_round(ui->c, card, 16, sel ? ALPHA(style.accent, 50) : hot ? style.hover : style.group);
            gfx_round_frame(ui->c, card, 16, sel ? style.accent : style.separator);
            gfx_icon(ui->c, card.x + 14, card.y + 9, strncmp(in->disks[i]->name, "usb", 3) ? ICON_COMPUTER : ICON_USB, 40);
            char size[16], title[80];
            size_text(in->disks[i]->sectors, size, sizeof(size));
            ksnprintf(title, sizeof(title), "%s  ·  %s", in->disks[i]->model[0] ? in->disks[i]->model : "Disk", size);
            gfx_text_box(ui->c, (struct rect){card.x + 70, card.y + 9, card.w - 90, 20}, &font_bold, title, style.text,
                         ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){card.x + 70, card.y + 30, card.w - 90, 18}, &font_small, in->disks[i]->name,
                         style.secondary, ALIGN_LEFT);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
            ui->consumed = true;
            in->chosen = i;
        }
    }
    list_end(ui, in, &l);
    int key = KEY_BASE(ui_key(ui));
    if ((key == KEY_UP || key == KEY_DOWN) && in->disk_count) {
        ui->consumed = true;
        in->chosen = CLAMP(in->chosen + (key == KEY_UP ? -1 : 1), 0, in->disk_count - 1);
        list_show(in, &l, in->chosen * 66, 58);
    }
    bool back;
    if (nav(ui, "Back", "Continue", in->chosen >= 0, &back)) {
        build_choices(in);
        go(in, PAGE_WHERE);
    }
    if (back)
        go(in, PAGE_WELCOME);
}

static void where_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, "Where on the disk?", &font_title, style.text, ALIGN_LEFT);
    const struct choice *sel = in->choice >= 0 ? &in->choices[in->choice] : NULL;
    draw_map(ui, in, (struct rect){r.x, r.y + 40, r.w, 30}, sel, sel ? sel->count : 0);
    int y = r.y + 82 + draw_legend(ui, in, r.x, r.y + 80, r.w);
    if (in->shrink_hint) {
        char need[16], hint[256];
        size_text(min_sectors(), need, sizeof(need));
        ksnprintf(hint, sizeof(hint),
                  "Not enough free space next to Windows (SynthOS needs %s or more). To keep Windows, make room "
                  "first: in Windows open Disk Management, right-click the C: partition, choose Shrink Volume, then "
                  "start this installer again.",
                  need);
        y += 6 + ui_wrap_text(ui->c, (struct rect){r.x, y, r.w, 54}, &font_small, hint, style.warning);
    }
    struct card_list l;
    list_begin(ui, in, &l, (struct rect){r.x, y, r.w, r.y + r.h - y}, in->choice_count * 56 - 6);
    y -= in->scroll;
    for (int i = 0; i < in->choice_count; i++) {
        const struct choice *c = &in->choices[i];
        struct rect card = {r.x, y, l.width, 50};
        bool on = in->choice == i, hot = list_hit(ui, &l, card);
        if (ui->c) {
            gfx_round(ui->c, card, 14, on ? ALPHA(c->danger ? style.error : style.accent, 40) : hot ? style.hover : style.group);
            gfx_round_frame(ui->c, card, 14, on ? (c->danger ? style.error : style.accent) : style.separator);
            gfx_ring(ui->c, card.x + 22, card.y + 25, 8, on ? style.accent : style.tertiary);
            if (on)
                gfx_circle(ui->c, card.x + 22, card.y + 25, 4, style.accent);
            gfx_text_box(ui->c, (struct rect){card.x + 42, card.y + 7, card.w - 54, 20}, &font_bold, c->title, style.text,
                         ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){card.x + 42, card.y + 27, card.w - 54, 18}, &font_small, c->detail,
                         c->danger ? style.error : style.secondary, ALIGN_LEFT);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
            ui->consumed = true;
            in->choice = i;
        }
        y += 56;
    }
    list_end(ui, in, &l);
    int key = KEY_BASE(ui_key(ui));
    if ((key == KEY_UP || key == KEY_DOWN) && in->choice_count) {
        ui->consumed = true;
        in->choice = CLAMP(in->choice + (key == KEY_UP ? -1 : 1), 0, in->choice_count - 1);
        list_show(in, &l, in->choice * 56, 50);
    }
    bool back;
    if (nav(ui, "Back", "Continue", in->choice >= 0, &back)) {
        const struct choice *c = &in->choices[in->choice];
        in->min_mb = (int)((min_sectors() + 2047) / 2048);
        in->max_mb = (int)(c->count / 2048);
        in->size_mb = c->whole ? in->max_mb : MIN(in->max_mb, MAX(in->min_mb, in->max_mb / 2 < 32768 ? in->max_mb : 32768));
        go(in, c->whole || in->max_mb <= in->min_mb ? PAGE_STARTUP : PAGE_SIZE);
    }
    if (back)
        go(in, PAGE_DISK);
}

static void size_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    const struct choice *c = &in->choices[in->choice];
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, "How much space for SynthOS?", &font_title, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){r.x, r.y + 30, r.w, 18}, "Programs, games and your files live in this space.",
             &font_sans, style.secondary, ALIGN_LEFT);
    draw_map(ui, in, (struct rect){r.x, r.y + 64, r.w, 30}, c, (uint32_t)in->size_mb * 2048);
    char size[16], rest[16], text[96];
    size_text((uint64_t)in->size_mb * 2048, size, sizeof(size));
    ui_label(ui, (struct rect){r.x, r.y + 120, r.w, 34}, size, &font_large, style.text, ALIGN_CENTER);
    ui_slider(ui, (struct rect){r.x + 30, r.y + 170, r.w - 60, 24}, &in->size_mb, in->min_mb, in->max_mb);
    size_text((uint64_t)(in->max_mb - in->size_mb) * 2048, rest, sizeof(rest));
    ksnprintf(text, sizeof(text), "%s stays free for other systems", rest);
    ui_label(ui, (struct rect){r.x, r.y + 206, r.w, 18}, in->size_mb < in->max_mb ? text : "SynthOS takes all of it",
             &font_small, style.secondary, ALIGN_CENTER);
    static const char *const presets[] = {"Smallest", "16 GB", "64 GB", "All"};
    int pick = -1;
    if (ui_segmented(ui, (struct rect){r.x + (r.w - 360) / 2, r.y + 240, 360, 28}, presets, 4, &pick)) {
        int want[] = {in->min_mb, 16384, 65536, in->max_mb};
        in->size_mb = CLAMP(want[pick], in->min_mb, in->max_mb);
    }
    int key = KEY_BASE(ui_key(ui));
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        ui->consumed = true;
        in->size_mb = CLAMP(in->size_mb + (key == KEY_LEFT ? -1024 : 1024), in->min_mb, in->max_mb);
    }
    bool back;
    if (nav(ui, "Back", "Continue", true, &back))
        go(in, PAGE_STARTUP);
    if (back)
        go(in, PAGE_WHERE);
}

static const char *const menu_labels[] = {"When needed", "Every time", "Never"};
static const char *const menu_values[] = {"auto", "always", "never"};
static const char *const wait_labels[] = {"1 s", "3 s", "5 s", "10 s", "30 s"};
static const int wait_values[] = {1, 3, 5, 10, 30};
static const char *const default_labels[] = {"SynthOS", "Windows", "Other system"};
static const char *const default_values[] = {"synthos", "windows", "other"};

static void startup_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    const struct choice *c = &in->choices[in->choice];
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, "Starting up", &font_title, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){r.x, r.y + 30, r.w, 18}, "With several systems on this computer, a menu lets you pick one.",
             &font_sans, style.secondary, ALIGN_LEFT);
    int y = r.y + 70, lw = 170;
    ui_label(ui, (struct rect){r.x, y + 6, lw, 20}, "Show the boot menu", &font_sans, style.text, ALIGN_LEFT);
    ui_segmented(ui, (struct rect){r.x + lw, y, r.w - lw, 30}, menu_labels, 3, &in->boot_menu);
    static const char *const menu_help[] = {
        "Only when other systems are found on the disks, or when the last starts failed.",
        "At every start, also to reach safe mode quickly.",
        "SynthOS starts straight away; the menu comes back only after failed starts."};
    ui_label(ui, (struct rect){r.x + lw, y + 34, r.w - lw, 16}, menu_help[in->boot_menu], &font_small, style.tertiary,
             ALIGN_LEFT);
    y += 66;
    bool menu = in->boot_menu != 2;
    if (menu) {
        ui_label(ui, (struct rect){r.x, y + 6, lw, 20}, "Wait before starting", &font_sans, style.text, ALIGN_LEFT);
        ui_segmented(ui, (struct rect){r.x + lw, y, r.w - lw, 30}, wait_labels, 5, &in->boot_wait);
        y += 46;
        ui_label(ui, (struct rect){r.x, y + 6, lw, 20}, "Start by itself", &font_sans, style.text, ALIGN_LEFT);
        ui_segmented(ui, (struct rect){r.x + lw, y, r.w - lw, 30}, default_labels, 3, &in->boot_default);
        y += 46;
    }
    ui_wrap_text(ui->c, (struct rect){r.x, y + 4, r.w, 60}, &font_small,
                 "Arrows choose in the menu, Enter starts. Everything here, and which systems the menu lists, can be "
                 "changed later in System Settings > General > Startup.",
                 style.secondary);
    if (!hal_uefi())
        ui_wrap_text(ui->c, (struct rect){r.x, y + 44, r.w, 40}, &font_small,
                     "The boot menu is part of UEFI starts; in legacy BIOS mode the firmware's own boot menu is used.",
                     style.warning);
    bool back;
    if (nav(ui, "Back", "Continue", true, &back))
        go(in, PAGE_CONFIRM);
    if (back)
        go(in, c->whole || in->max_mb <= in->min_mb ? PAGE_WHERE : PAGE_SIZE);
}

static void plan_of(struct installer *in, struct install_plan *plan)
{
    const struct choice *c = &in->choices[in->choice];
    *plan = (struct install_plan){in->disks[in->chosen], c->whole, c->replace, c->start, c->count, in->copy_home,
                                  menu_values[in->boot_menu], default_values[in->boot_default],
                                  wait_values[in->boot_wait]};
    if (!c->whole)
        plan->count = MIN(c->count, (uint32_t)in->size_mb * 2048);
}

static void confirm_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    const struct choice *c = &in->choices[in->choice];
    struct blockdev *d = in->disks[in->chosen];
    struct install_plan plan;
    plan_of(in, &plan);
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, c->danger ? "Ready to install. This erases data." : "Ready to install",
             &font_title, c->danger ? style.error : style.text, ALIGN_LEFT);
    char disk[80], size[16], where[112];
    size_text(d->sectors, size, sizeof(size));
    ksnprintf(disk, sizeof(disk), "%s, %s (%s)", d->model[0] ? d->model : "Disk", size, d->name);
    size_text(c->whole ? d->sectors : plan.count, size, sizeof(size));
    int row = r.h < 380 ? 34 : 42;
    char start[80];
    if (in->boot_menu == 2)
        str_copy(start, "SynthOS, no menu", sizeof(start));
    else
        ksnprintf(start, sizeof(start), "Menu %s, %s, then %s", in->boot_menu ? "every time" : "when needed",
                  wait_labels[in->boot_wait], default_labels[in->boot_default]);
    struct rect g = ui_group(ui, r.x, r.y + 40, r.w, 5, row);
    ui_group_label(ui, (struct rect){g.x, g.y, g.w, row}, "Disk", disk, false);
    ui_group_label(ui, (struct rect){g.x, g.y + row, g.w, row}, "Where", c->brief, false);
    ui_group_label(ui, (struct rect){g.x, g.y + 2 * row, g.w, row}, "SynthOS gets", size, false);
    ui_group_label(ui, (struct rect){g.x, g.y + 3 * row, g.w, row}, "Start-up", start, false);
    ui_group_label(ui, (struct rect){g.x, g.y + 4 * row, g.w, row}, "Bring my files and settings", "", true);
    ui_switch(ui, (struct rect){g.x + g.w - 50, g.y + 4 * row + (row - 22) / 2, 36, 22}, &in->copy_home);
    ksnprintf(where, sizeof(where), "%s.", c->detail);
    int y = r.y + 40 + 5 * row + 12;
    y += ui_wrap_text(ui->c, (struct rect){r.x, y, r.w, 40}, &font_small, where, c->danger ? style.error : style.secondary);
    if (!hal_uefi() && !c->whole)
        ui_wrap_text(ui->c, (struct rect){r.x, y + 6, r.w, 40}, &font_small,
                     "This computer started in legacy BIOS mode. SynthOS next to other systems starts through UEFI: "
                     "turn UEFI on in the firmware settings before you restart.",
                     style.warning);
    bool back;
    if (nav(ui, "Back", c->danger ? "Erase and Install" : "Install", true, &back)) {
        plan_of(in, &plan);
        if (install_begin(&in->job, &plan) == 0)
            go(in, PAGE_INSTALL);
        else
            go(in, PAGE_ERROR);
    }
    if (back)
        go(in, PAGE_STARTUP);
}

static void install_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    const struct settings_preview *pv = module_service ? module_service("settings-preview") : NULL;
    if (pv && r.h >= 440) { /* while it copies: what is coming, one scene after another */
        static const char *const tour[] = {"appearance", "dock", "ai", "apps", "lock", "sound", "language", "screenshot"};
        uint32_t ms = (uint32_t)hal_ticks_ms();
        if (ui->c)
            pv->hero(ui->c, (struct rect){r.x, r.y, r.w, 230}, tour[ms / 6000 % 8], ms);
        r.y += 150;
    } else if (ui->c) {
        struct rect logo = {r.x + (r.w - 72) / 2, r.y + 30, 72, 72};
        gfx_icon(ui->c, logo.x, logo.y, ICON_LOGO, 72);
    }
    ui_label(ui, (struct rect){r.x, r.y + 120, r.w, 26}, "Installing SynthOS", &font_title, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 150, r.w, 18}, in->job.message, &font_sans, style.secondary, ALIGN_CENTER);
    struct rect bar = {r.x + 40, r.y + 190, r.w - 80, 8};
    if (ui->c) {
        gfx_round(ui->c, bar, 4, ALPHA(style.text, 30));
        int fill = bar.w * CLAMP(in->bar.pos, 0, 1024) / 1024;
        gfx_round(ui->c, (struct rect){bar.x, bar.y, fill, bar.h}, 4, style.accent);
    }
    char pct[8];
    ksnprintf(pct, sizeof(pct), "%d%%", in->job.percent);
    ui_label(ui, (struct rect){r.x, r.y + 206, r.w, 18}, pct, &font_small, style.tertiary, ALIGN_CENTER);
}

static void done_page(struct ui *ui, struct installer *in, bool ok)
{
    struct rect r = content(ui, in);
    if (ui->c) {
        int cx = r.x + r.w / 2, cy = r.y + 60;
        gfx_circle(ui->c, cx, cy, 34, ok ? style.ok : style.error);
        gfx_symbol(ui->c, cx - 16, cy - 16, ok ? SYM_CHECK : SYM_CLOSE, 32, RGB(255, 255, 255));
    }
    ui_label(ui, (struct rect){r.x, r.y + 114, r.w, 26}, ok ? "SynthOS is installed" : "The installation stopped",
             &font_title, style.text, ALIGN_CENTER);
    bool whole = in->choice >= 0 && in->choices[in->choice].whole;
    const char *text = !ok ? in->job.message
                       : !hal_uefi() && !whole
                           ? "SynthOS next to other systems starts through UEFI. Restart, open the firmware settings "
                             "(often Del or F2 right after power on), switch the boot mode to UEFI and start from the "
                             "USB stick once more: it adds SynthOS to the boot menu and starts it."
                       : hal_uefi()
                           ? "Restart with the USB stick still in: SynthOS adds itself to the computer's boot menu and "
                             "starts from the disk; then you can remove the stick. If other systems are on the "
                             "computer, a menu lets you choose one at every start."
                           : "Remove the USB stick and restart. If the computer starts another disk, choose this one in "
                             "the firmware boot menu (usually F12, F11, F8 or Esc right after power on).";
    ui_wrap_text(ui->c, (struct rect){r.x + 30, r.y + 150, r.w - 60, 100}, &font_sans, text, style.secondary);
    bool back;
    if (ok) {
        if (nav(ui, "Close", "Restart", true, &back))
            shell_power_menu(true);
        if (back)
            wm_close(ui->win);
    } else if (nav(ui, NULL, "Back", true, &back)) {
        scan(in);
        go(in, PAGE_DISK);
    }
}

static void frame(struct ui *ui, void *data)
{
    struct installer *in = data;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, ui->win->surface.w, ui->win->surface.h}, style.content);
    switch (in->page) {
    case PAGE_WELCOME: welcome(ui, in); break;
    case PAGE_DISK: disk_page(ui, in); break;
    case PAGE_WHERE: where_page(ui, in); break;
    case PAGE_SIZE: size_page(ui, in); break;
    case PAGE_STARTUP: startup_page(ui, in); break;
    case PAGE_CONFIRM: confirm_page(ui, in); break;
    case PAGE_INSTALL: install_page(ui, in); break;
    case PAGE_DONE: done_page(ui, in, true); break;
    default: done_page(ui, in, false); break;
    }
    /* no window buttons: "Try SynthOS" puts the installer away, the Install widget brings it back */
    if (in->page != PAGE_INSTALL && ui->win) {
        int W = ui->win->surface.w;
        if (ui_button(ui, (struct rect){W - 196, 18, 176, 32}, hal_installed() ? "Hide" : "Try SynthOS"))
            put_away();
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct installer *in = w->data;
    ui_run(w, &in->st, c, NULL, frame, in);
}

static void event(struct window *w, struct gui_event *e)
{
    struct installer *in = w->data;
    if (e->type == EV_TIMER) {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - in->tick, 64u);
        in->tick = now;
        bool busy = spring_step(&in->slide, 300, 30, dt);
        if (in->page == PAGE_INSTALL) {
            uint64_t until = now + 40;
            int r = 1;
            while (r > 0 && hal_ticks_ms() < until)
                r = install_step(&in->job);
            in->bar.target = in->job.percent * 1024 / 100;
            if (r < 0)
                go(in, PAGE_ERROR);
            else if (r == 0)
                go(in, PAGE_DONE);
            busy = true;
        }
        busy |= spring_step(&in->bar, 200, 26, dt);
        if (busy)
            wm_invalidate(w);
        return;
    }
    ui_run(w, &in->st, NULL, e, frame, in);
}

static bool on_close(struct window *w)
{
    struct installer *in = w->data;
    return in->page != PAGE_INSTALL;
}

static void destroy(struct window *w)
{
    kfree(w->data);
    if (w == installer_win)
        installer_win = NULL;
    if (pill_win) {
        wm_destroy(pill_win);
        pill_win = NULL;
    }
}

static const struct window_ops ops = {paint, event, on_close, destroy};

/* The Install widget: a glass pill in the top right corner while the installer is put away. */
#define PILL_W 196
#define PILL_H 44

static void pill_paint(struct window *w, struct canvas *c)
{
    struct rect r = {0, 0, w->surface.w, w->surface.h};
    gfx_clear(c, r, 0);
    gfx_round(c, r, PILL_H / 2, ALPHA(style.accent, 230));
    gfx_icon(c, 10, (PILL_H - 28) / 2, ICON_INSTALLER, 28);
    gfx_text_box(c, (struct rect){46, 0, PILL_W - 56, PILL_H}, &font_bold, "Install SynthOS", RGB(255, 255, 255),
                 ALIGN_LEFT);
}

static void pill_event(struct window *w, struct gui_event *e)
{
    UNUSED(w);
    if (e->type == EV_MOUSE_DOWN && installer_win) {
        wm_hide(pill_win);
        wm_show(installer_win);
    }
}

static const struct window_ops pill_ops = {pill_paint, pill_event, NULL, NULL};

static void put_away(void)
{
    if (!installer_win)
        return;
    wm_hide(installer_win);
    if (!pill_win) {
        struct rect sr = wm_screen_rect(0);
        pill_win = wm_popup(sr.x + sr.w - PILL_W - 16, sr.y + 44, PILL_W, PILL_H, &pill_ops, NULL, NULL);
        if (!pill_win)
            return;
        pill_win->flags &= ~WIN_POPUP;
        pill_win->flags |= WIN_OVERLAY;
        pill_win->radius = PILL_H / 2;
    }
    wm_show(pill_win);
    wm_invalidate(pill_win);
}

/* "launch 'Install SynthOS' /dev/sda" opens straight on the choices for that disk. */
static void launch(const char *arg)
{
    if (installer_win) {
        if (pill_win)
            wm_hide(pill_win);
        wm_show(installer_win);
        return;
    }
    struct installer *in = kzalloc(sizeof(*in));
    if (!in)
        return;
    in->chosen = -1;
    in->boot_wait = 1;
    in->copy_home = !hal_installed();
    in->tick = hal_ticks_ms();
    wm_close_popups();
    struct rect sr = wm_screen_rect(0);
    struct window *w = wm_popup(sr.x, sr.y, sr.w, sr.h, &ops, in, NULL);
    if (!w) {
        kfree(in);
        return;
    }
    installer_win = w;
    w->flags &= ~WIN_POPUP;
    w->flags |= WIN_OVERLAY | WIN_NOSHADOW;
    if (w->glass) {
        glass_unregister(w->glass);
        kfree(w->glass);
        w->glass = NULL;
    }
    w->radius = 0;
    wm_move(w, sr.x, sr.y);
    wm_set_timer(w, 16);
    if (arg && !strcmp(arg, "hidden")) { /* "Try SynthOS" at the first start: only the widget shows */
        put_away();
        return;
    }
    if (arg && str_starts(arg, "/dev/")) {
        scan(in);
        for (int i = 0; i < in->disk_count; i++)
            if (!strcmp(in->disks[i]->name, arg + 5))
                in->chosen = i;
        if (in->chosen >= 0) {
            build_choices(in);
            in->page = PAGE_WHERE;
        }
    }
}

static const struct gui_app installer_app = {"Install SynthOS", "Put SynthOS on a disk of this computer", ICON_INSTALLER,
                                             launch, NULL, NULL};
GUI_APP(installer_app);
