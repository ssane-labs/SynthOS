#include <synth/block.h>
#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/install.h>
#include <synth/keys.h>
#include <synth/parttab.h>
#include <synth/string.h>

#include "../internal.h"

/* The installer walks through: welcome, disk, where on the disk (free space, a partition to replace or the whole
   disk), how much space, a summary, the copy and the restart. Nothing is written before the summary's button. */

#define MAX_CHOICES 6

enum page {
    PAGE_WELCOME,
    PAGE_DISK,
    PAGE_WHERE,
    PAGE_SIZE,
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
    bool shrink_hint;
    int size_mb, min_mb, max_mb;
    bool copy_home;
    struct install_job job;
};

static void go(struct installer *in, enum page page)
{
    int dir = page > in->page ? 1 : -1;
    in->page = page;
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

static void size_text(uint64_t sectors, char *buf, size_t cap)
{
    fmt_size(buf, cap, sectors * BLOCK_SIZE);
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
    bool readable = disk_map_read(d, &in->map) == 0;
    bool editable = readable && in->map.scheme != SCHEME_NONE && !in->map.read_only;
    char size[16];
    bool has_windows = false;
    for (int i = 0; editable && i < in->map.count; i++) {
        const struct part *p = &in->map.parts[i];
        has_windows |= p->kind == PART_WINDOWS;
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
        if (p->kind == PART_FREE || p->kind == PART_EFI || p->kind == PART_MSR || p->kind == PART_EXTENDED)
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

static struct rect content(struct ui *ui, struct installer *in)
{
    int W = ui->win->surface.w, H = ui->win->surface.h;
    int dx = in->slide.pos * W / 1024 / 3;
    return (struct rect){40 + dx, 30, W - 80, H - 100};
}

static bool nav(struct ui *ui, const char *back, const char *next, bool next_enabled, bool *went_back)
{
    int W = ui->win->surface.w, H = ui->win->surface.h;
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

static void draw_legend(struct ui *ui, struct installer *in, int x, int y, int w)
{
    if (!ui->c)
        return;
    int cx = x;
    for (int i = 0; i < in->map.count; i++) {
        const struct part *p = &in->map.parts[i];
        char size[16], text[80];
        size_text(p->count, size, sizeof(size));
        if (p->kind == PART_FREE)
            ksnprintf(text, sizeof(text), "Free %s", size);
        else
            ksnprintf(text, sizeof(text), "%d %s %s%s%s", p->slot + 1, part_kind_name(p->kind), size,
                      p->label[0] ? " " : "", p->label);
        int tw = gfx_text_width(&font_small, text) + 26;
        if (cx + tw > x + w) {
            cx = x;
            y += 18;
        }
        gfx_circle(ui->c, cx + 5, y + 8, 5, kind_color(p->kind));
        gfx_text(ui->c, cx + 14, y + 1, &font_small, text, style.secondary);
        cx += tw;
    }
}

static void welcome(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    if (ui->c) {
        struct rect logo = {r.x + (r.w - 96) / 2, r.y + 20, 96, 96};
        gfx_icon(ui->c, logo.x, logo.y, ICON_LOGO, 96);
    }
    ui_label(ui, (struct rect){r.x, r.y + 134, r.w, 34}, "SynthOS " SYNTH_VERSION, &font_large, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 172, r.w, 20}, "\"" SYNTH_CODENAME "\"", &font_title, style.accent, ALIGN_CENTER);
    ui_wrap_text(ui->c, (struct rect){r.x + 40, r.y + 214, r.w - 80, 100}, &font_sans,
                 "Install SynthOS on a disk of this computer: next to Windows or another system in free space, "
                 "instead of a partition you no longer need, or on a whole disk. Nothing is written until you "
                 "confirm the summary.",
                 style.secondary);
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
    for (int i = 0; i < in->disk_count; i++) {
        struct rect card = {r.x, r.y + 64 + i * 66, r.w, 58};
        bool sel = in->chosen == i, hot = ui_hover(ui, card);
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
    int key = KEY_BASE(ui_key(ui));
    if ((key == KEY_UP || key == KEY_DOWN) && in->disk_count) {
        ui->consumed = true;
        in->chosen = CLAMP(in->chosen + (key == KEY_UP ? -1 : 1), 0, in->disk_count - 1);
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
    draw_legend(ui, in, r.x, r.y + 78, r.w);
    int y = r.y + 122;
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
    for (int i = 0; i < in->choice_count; i++) {
        const struct choice *c = &in->choices[i];
        struct rect card = {r.x, y, r.w, 50};
        bool on = in->choice == i, hot = ui_hover(ui, card);
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
    int key = KEY_BASE(ui_key(ui));
    if ((key == KEY_UP || key == KEY_DOWN) && in->choice_count) {
        ui->consumed = true;
        in->choice = CLAMP(in->choice + (key == KEY_UP ? -1 : 1), 0, in->choice_count - 1);
    }
    bool back;
    if (nav(ui, "Back", "Continue", in->choice >= 0, &back)) {
        const struct choice *c = &in->choices[in->choice];
        in->min_mb = (int)((min_sectors() + 2047) / 2048);
        in->max_mb = (int)(c->count / 2048);
        in->size_mb = c->whole ? in->max_mb : MIN(in->max_mb, MAX(in->min_mb, in->max_mb / 2 < 32768 ? in->max_mb : 32768));
        go(in, c->whole || in->max_mb <= in->min_mb ? PAGE_CONFIRM : PAGE_SIZE);
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
        go(in, PAGE_CONFIRM);
    if (back)
        go(in, PAGE_WHERE);
}

static void plan_of(struct installer *in, struct install_plan *plan)
{
    const struct choice *c = &in->choices[in->choice];
    *plan = (struct install_plan){in->disks[in->chosen], c->whole, c->replace, c->start, c->count, in->copy_home};
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
    struct rect g = ui_group(ui, r.x, r.y + 40, r.w, 4, 44);
    ui_group_label(ui, (struct rect){g.x, g.y, g.w, 44}, "Disk", disk, false);
    ui_group_label(ui, (struct rect){g.x, g.y + 44, g.w, 44}, "Where", c->brief, false);
    ui_group_label(ui, (struct rect){g.x, g.y + 88, g.w, 44}, "SynthOS gets", size, false);
    ui_group_label(ui, (struct rect){g.x, g.y + 132, g.w, 44}, "Bring my files and settings", "", true);
    ui_switch(ui, (struct rect){g.x + g.w - 50, g.y + 132 + 11, 36, 22}, &in->copy_home);
    ksnprintf(where, sizeof(where), "%s.", c->detail);
    int y = r.y + 40 + 4 * 44 + 14;
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
        go(in, c->whole || in->max_mb <= in->min_mb ? PAGE_WHERE : PAGE_SIZE);
}

static void install_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    if (ui->c) {
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
    case PAGE_CONFIRM: confirm_page(ui, in); break;
    case PAGE_INSTALL: install_page(ui, in); break;
    case PAGE_DONE: done_page(ui, in, true); break;
    default: done_page(ui, in, false); break;
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
}

static const struct window_ops ops = {paint, event, on_close, destroy};

/* "launch 'Install SynthOS' /dev/sda" opens straight on the choices for that disk. */
static void launch(const char *arg)
{
    struct installer *in = kzalloc(sizeof(*in));
    if (!in)
        return;
    in->chosen = -1;
    in->copy_home = !hal_installed();
    in->tick = hal_ticks_ms();
    struct window *w = wm_open("Install SynthOS", ICON_INSTALLER, 680, 560, &ops, in, 0);
    if (!w) {
        kfree(in);
        return;
    }
    wm_set_timer(w, 16);
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
