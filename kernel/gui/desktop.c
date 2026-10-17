#include <synth/config.h>
#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/lnk.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/theme.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "image.h"
#include "internal.h"

SYNTH_TABLE_DECLARE(synth_gui_apps, struct gui_app);

#define CELL_W 100
#define CELL_H 96
#define MAX_ICONS 24
#define NOTE_W 340
#define NOTE_H 72

struct gui_style style;

static struct canvas wallpapers[MAX_SCREENS];
static bool active;
static int quit_action = -1;
static struct shell *session;
static char wallpaper_setting[PATH_MAX_LEN] = "auto";
static char appearance_setting[8] = "auto";
static char accent_setting[12] = "auto";
static char glass_setting[10] = "liquid";
static bool autostart = true;
static char clipboard[4096];
static int last_minute = -1;
static uint64_t last_scan;
static bool autostart_pending;

struct desktop_icon {
    char label[64];
    char path[PATH_MAX_LEN];
    enum icon_id icon;
    bool volume;
};

static struct volume known_volumes[8];
static int known_count = -1;

static struct desktop_icon icons[MAX_ICONS];
static int icon_count;
static int selected_icon = -1;
static int drag_icon = -1, drag_x, drag_y;
static bool icon_dragging;
static int lock_minutes;
static uint64_t last_input;
static int want_w[MAX_SCREENS], want_h[MAX_SCREENS];
static char arrange[8] = "right";
static int display_hal[MAX_SCREENS];
static int display_count;

static void scan_desktop(void);
static const struct image *program_icon(const char *path);

int gui_display_hal(int screen)
{
    return screen >= 0 && screen < display_count ? display_hal[screen] : -1;
}

static int open_displays(struct framebuffer *fbs, struct rect *origins)
{
    int available = MIN(hal_screen_count(), MAX_SCREENS);
    if (!strcmp(arrange, "off"))
        available = MIN(available, 1);
    int n = 0;
    for (int i = 0; i < available; i++)
        if (hal_screen_open(i, want_w[i], want_h[i], &fbs[n]))
            display_hal[n++] = i;
    for (int i = available; i < hal_screen_count(); i++)
        hal_screen_close(i);
    display_count = n;
    int w0 = (int)fbs[0].width / (fbs[0].width >= 2400 ? 2 : 1);
    int h0 = (int)fbs[0].height / (fbs[0].width >= 2400 ? 2 : 1);
    origins[0] = (struct rect){0, 0, w0, h0};
    int right = w0, bottom = h0;
    for (int i = 1; i < n; i++) {
        int w = (int)fbs[i].width / (fbs[i].width >= 2400 ? 2 : 1), h = (int)fbs[i].height / (fbs[i].width >= 2400 ? 2 : 1);
        if (!strcmp(arrange, "left"))
            origins[i] = (struct rect){-w * i, 0, w, h};
        else if (!strcmp(arrange, "above"))
            origins[i] = (struct rect){0, -h * i, w, h};
        else if (!strcmp(arrange, "below")) {
            origins[i] = (struct rect){0, bottom, w, h};
            bottom += h;
        } else {
            origins[i] = (struct rect){right, 0, w, h};
            right += w;
        }
    }
    int min_x = 0, min_y = 0;
    for (int i = 0; i < n; i++) {
        min_x = MIN(min_x, origins[i].x);
        min_y = MIN(min_y, origins[i].y);
    }
    for (int i = 0; i < n; i++) {
        origins[i].x -= min_x;
        origins[i].y -= min_y;
    }
    return n;
}

void gui_displays_changed(void)
{
    struct framebuffer fbs[MAX_SCREENS];
    struct rect origins[MAX_SCREENS];
    int n = open_displays(fbs, origins);
    if (!n)
        return;
    chrome_stop();
    wm_reconfigure(fbs, origins, n);
    gui_wallpaper_refresh();
    chrome_start();
    icon_count = -1;
    scan_desktop();
}

void desktop_dir(char *out, size_t cap)
{
    path_join(user_home(), "Desktop", out, cap);
}

const struct canvas *desktop_wallpaper(int screen)
{
    return screen >= 0 && screen < MAX_SCREENS && wallpapers[screen].px ? &wallpapers[screen] : NULL;
}

static const struct {
    const char *name;
    uint32_t light, dark;
} accents[] = {
    {"blue", RGB(0, 122, 255), RGB(10, 132, 255)},       {"purple", RGB(149, 61, 150), RGB(191, 90, 242)},
    {"pink", RGB(247, 79, 158), RGB(255, 55, 140)},      {"red", RGB(224, 56, 62), RGB(255, 69, 58)},
    {"orange", RGB(247, 130, 27), RGB(255, 159, 10)},    {"yellow", RGB(230, 168, 0), RGB(255, 204, 0)},
    {"green", RGB(98, 186, 70), RGB(48, 209, 88)},       {"graphite", RGB(140, 140, 140), RGB(152, 152, 157)},
};

bool gui_active(void)
{
    return active;
}

static uint32_t palette_color(int index)
{
    return 0xff000000u | theme_current()->palette[index & 15];
}

static bool want_dark(void)
{
    if (!strcmp(appearance_setting, "dark"))
        return true;
    if (!strcmp(appearance_setting, "light"))
        return false;
    return look_current()->dark;
}

void gui_style_refresh(void)
{
    const struct gui_look *look = look_current();
    bool dark = want_dark();
    uint32_t t = look->tint;
    style.dark = dark;
    style.tint = t;
    style.glass = !strcmp(glass_setting, "solid") ? GLASS_SOLID : !strcmp(glass_setting, "frosted") ? GLASS_FROSTED
                                                                                                      : GLASS_LIQUID;
    uint32_t accent = look->accent;
    for (size_t i = 0; i < ARRAY_SIZE(accents); i++)
        if (!strcmp(accent_setting, accents[i].name))
            accent = dark ? accents[i].dark : accents[i].light;
    for (int i = 0; i < 16; i++)
        style.palette[i] = palette_color(i);
    style.accent = accent;
    style.accent_text = color_luma(accent) > 186 ? RGB(30, 24, 20) : RGB(255, 255, 255);
    style.error = dark ? RGB(255, 92, 84) : RGB(232, 64, 56);
    style.ok = dark ? RGB(72, 214, 120) : RGB(48, 176, 86);
    style.warning = dark ? RGB(255, 176, 60) : RGB(232, 140, 20);
    if (dark) {
        style.window = color_mix(RGB(26, 26, 30), t, 18);
        style.content = color_mix(RGB(18, 18, 22), t, 12);
        style.sidebar = ALPHA(color_mix(RGB(34, 34, 40), t, 40), 196);
        style.titlebar = color_mix(RGB(30, 30, 35), t, 22);
        style.titlebar_inactive = color_mix(RGB(24, 24, 28), t, 14);
        style.separator = ALPHA(0xffffffffu, 26);
        style.text = color_mix(RGB(242, 240, 246), t, 10);
        style.secondary = color_mix(RGB(150, 150, 158), t, 34);
        style.tertiary = color_mix(RGB(98, 98, 106), t, 34);
        style.selection_inactive = color_mix(RGB(60, 60, 66), t, 40);
        style.hover = ALPHA(0xffffffffu, 18);
        style.control = color_mix(RGB(64, 64, 70), t, 30);
        style.control_pressed = color_mix(RGB(88, 88, 94), t, 30);
        style.control_border = ALPHA(0, 90);
        style.field = color_mix(RGB(16, 16, 20), t, 10);
        style.field_border = ALPHA(0xffffffffu, 34);
        style.zebra = color_mix(RGB(24, 24, 28), t, 12);
        style.group = color_mix(RGB(32, 32, 37), t, 20);
        style.menu = color_mix(RGB(36, 36, 42), t, 30);
    } else {
        style.window = color_mix(RGB(246, 246, 248), t, 14);
        style.content = color_mix(RGB(255, 255, 255), t, 6);
        style.sidebar = ALPHA(color_mix(RGB(238, 238, 242), t, 36), 200);
        style.titlebar = color_mix(RGB(242, 242, 245), t, 18);
        style.titlebar_inactive = color_mix(RGB(248, 248, 250), t, 10);
        style.separator = ALPHA(color_mix(RGB(0, 0, 0), t, 60), 26);
        style.text = color_mix(RGB(28, 28, 32), t, 46);
        style.secondary = color_mix(RGB(118, 118, 126), t, 50);
        style.tertiary = color_mix(RGB(176, 176, 182), t, 40);
        style.selection_inactive = color_mix(RGB(220, 220, 224), t, 30);
        style.hover = ALPHA(color_mix(RGB(0, 0, 0), t, 80), 16);
        style.control = color_mix(RGB(255, 255, 255), t, 8);
        style.control_pressed = color_mix(RGB(226, 226, 230), t, 30);
        style.control_border = ALPHA(0, 40);
        style.field = color_mix(RGB(255, 255, 255), t, 4);
        style.field_border = ALPHA(0, 44);
        style.zebra = color_mix(RGB(246, 246, 247), t, 10);
        style.group = color_mix(RGB(255, 255, 255), t, 6);
        style.menu = color_mix(RGB(248, 248, 250), t, 12);
    }
    style.selection = accent;
    style.desktop = style.window;
    style.panel = style.window;
    style.border = style.separator;
    style.muted = style.secondary;
    style.button = style.control;
    style.button_hover = style.control_pressed;
    style.accent2 = color_mix(accent, dark ? 0xff000000u : 0xffffffffu, 60);
    const char *console = theme_current()->name;
    if (!strcmp(console, "mirage") || !strcmp(console, "mirage-light")) {
        const char *want = dark ? "mirage" : "mirage-light";
        if (strcmp(console, want))
            theme_load(want);
    }
}

void gui_wallpaper_refresh(void)
{
    for (int i = 0; i < wm_screen_count(); i++) {
        struct rect a = wm_screen_rect(i);
        if (wallpapers[i].px && (wallpapers[i].w != a.w || wallpapers[i].h != a.h))
            canvas_free(&wallpapers[i]);
        if (!wallpapers[i].px && !canvas_alloc(&wallpapers[i], a.w, a.h))
            continue;
        wallpaper_paint(&wallpapers[i], strcmp(wallpaper_setting, "auto") ? wallpaper_setting : look_current()->name,
                        style.dark);
    }
    wm_damage_all();
}

static void free_wallpapers(void)
{
    for (int i = 0; i < MAX_SCREENS; i++)
        canvas_free(&wallpapers[i]);
}

static enum icon_id icon_for(const char *name, bool dir)
{
    if (dir)
        return ICON_FOLDER;
    const char *ext = path_ext(name);
    if (!ext)
        return ICON_FILE;
    if (picture_is_image(name))
        return ICON_IMAGE;
    if (!str_casecmp(ext, ".exe") || !str_casecmp(ext, ".lnk"))
        return ICON_APP;
    if (!str_casecmp(ext, ".dll"))
        return ICON_DLL;
    if (!str_casecmp(ext, ".com"))
        return ICON_DOS;
    if (!str_casecmp(ext, ".sx"))
        return ICON_SX;
    if (!str_casecmp(ext, ".sh") || !str_casecmp(ext, ".bat"))
        return ICON_SCRIPT;
    return ICON_TEXT;
}

/* A desktop shortcut to a built-in app is a small "Name.app" file holding the app's name. */
static const struct gui_app *app_shortcut(const char *path)
{
    const char *ext = path_ext(path);
    char *data;
    size_t size;
    if (!ext || str_casecmp(ext, ".app") || vfs_read_all(path, &data, &size) != 0)
        return NULL;
    char name[64];
    str_copy(name, data, MIN(size + 1, sizeof(name)));
    kfree(data);
    const struct gui_app *app = gui_app_find(str_trim(name));
    return app && !strcmp(app->name, str_trim(name)) ? app : NULL;
}

void gui_draw_file_icon(struct canvas *c, const char *path, struct rect r, int alpha)
{
    const struct gui_app *app = app_shortcut(path);
    const struct image *img = app ? NULL : program_icon(path);
    if (app && app->picture)
        gfx_scale(c, rect_inset(r, r.w / 16), app->picture, alpha);
    else if (img)
        gfx_scale(c, rect_inset(r, r.w / 16), &(struct canvas){img->px, img->w, img->h, img->w, {0, 0, img->w, img->h}},
                  alpha);
    else
        gfx_icon_alpha(c, r.x, r.y, app ? app->icon : icon_for(path_basename(path), vfs_is_dir(path)), r.w, alpha);
}

void gui_file_label(const char *path, char *out, size_t cap)
{
    str_copy(out, path_basename(path), cap);
    const char *ext = path_ext(out);
    if (ext && (!str_casecmp(ext, ".lnk") || !str_casecmp(ext, ".app")))
        out[strlen(out) - 4] = 0;
}

void gui_make_app_shortcut(const char *name)
{
    char dir[PATH_MAX_LEN], file[96], path[PATH_MAX_LEN];
    desktop_dir(dir, sizeof(dir));
    vfs_mkdirs(dir);
    ksnprintf(file, sizeof(file), "%s.app", name);
    path_join(dir, file, path, sizeof(path));
    if (!vfs_exists(path))
        vfs_write_all(path, name, strlen(name));
    scan_desktop();
}

static void scan_desktop(void)
{
    struct desktop_icon fresh[MAX_ICONS];
    char dir[PATH_MAX_LEN];
    desktop_dir(dir, sizeof(dir));
    int n = 0;
    str_copy(fresh[n].label, "SynthOS HD", sizeof(fresh[n].label));
    str_copy(fresh[n].path, "/", sizeof(fresh[n].path));
    fresh[n].volume = false;
    fresh[n++].icon = ICON_COMPUTER;
    struct volume volumes[8];
    int volume_count = vfs_volumes(volumes, (int)ARRAY_SIZE(volumes));
    for (int i = 0; i < volume_count && n < MAX_ICONS; i++, n++) {
        memset(&fresh[n], 0, sizeof(fresh[n]));
        str_copy(fresh[n].label, volumes[i].label, sizeof(fresh[n].label));
        str_copy(fresh[n].path, volumes[i].path, sizeof(fresh[n].path));
        fresh[n].icon = ICON_USB;
        fresh[n].volume = true;
    }
    if (known_count >= 0) {
        for (int i = 0; i < volume_count; i++) {
            bool seen = false;
            for (int k = 0; k < known_count; k++)
                seen |= !strcmp(volumes[i].path, known_volumes[k].path);
            if (!seen)
                gui_notify(volumes[i].label, "The drive is connected and ready to use.");
        }
    }
    memcpy(known_volumes, volumes, sizeof(volumes[0]) * (size_t)volume_count);
    known_count = volume_count;
    struct vdir d;
    if (vfs_opendir(dir, &d) == 0) {
        struct vfs_dirent ent;
        while (n < MAX_ICONS && vfs_readdir(&d, &ent) == 0) {
            if (ent.name[0] == '.')
                continue;
            memset(&fresh[n], 0, sizeof(fresh[n]));
            str_copy(fresh[n].label, ent.name, sizeof(fresh[n].label));
            if (str_ends(fresh[n].label, ".lnk") || str_ends(fresh[n].label, ".LNK") || str_ends(fresh[n].label, ".app"))
                fresh[n].label[strlen(fresh[n].label) - 4] = 0;
            path_join(dir, ent.name, fresh[n].path, sizeof(fresh[n].path));
            const struct gui_app *app = app_shortcut(fresh[n].path);
            fresh[n].icon = app ? app->icon : icon_for(ent.name, ent.type == VN_DIR);
            n++;
        }
        vfs_closedir(&d);
    }
    if (n == icon_count && !memcmp(fresh, icons, sizeof(fresh[0]) * (size_t)n))
        return;
    memcpy(icons, fresh, sizeof(fresh[0]) * (size_t)n);
    icon_count = n;
    if (selected_icon >= n)
        selected_icon = -1;
    struct rect s = wm_screen_rect(0);
    wm_damage_z((struct rect){s.x + s.w - 4 * CELL_W - widgets_right_reserved(), s.y, 4 * CELL_W + widgets_right_reserved(), s.h},
                Z_DESKTOP);
}

static struct rect icon_cell(int i)
{
    struct rect s = wm_work_area(0);
    int top = MAX(s.y + 10, status_bottom() + 10);
    int per_column = MAX(1, (s.y + s.h - top - 6) / CELL_H);
    int right = s.x + s.w - 12 - widgets_right_reserved();
    return (struct rect){right - (i / per_column + 1) * CELL_W, top + (i % per_column) * CELL_H, CELL_W, CELL_H - 6};
}

static void icon_label(struct canvas *c, struct rect cell, const char *label, bool selected)
{
    int w = MIN(gfx_text_width(&font_bold, label) + 10, cell.w);
    struct rect r = {cell.x + (cell.w - w) / 2, cell.y + 70, w, 18};
    if (selected)
        gfx_round(c, r, 5, style.accent);
    else
        gfx_text_box(c, (struct rect){r.x + 1, r.y + 1, r.w, r.h}, &font_bold, label, ALPHA(0, 150), ALIGN_CENTER);
    gfx_text_box(c, r, &font_bold, label, RGB(255, 255, 255), ALIGN_CENTER);
}

void desktop_paint(struct canvas *c, int screen, struct rect area)
{
    if (screen < MAX_SCREENS && wallpapers[screen].px) {
        struct rect s = wm_screen_rect(screen);
        gfx_blit(c, s.x, s.y, &wallpapers[screen], (struct rect){0, 0, wallpapers[screen].w, wallpapers[screen].h});
    } else {
        gfx_fill(c, area, style.window);
    }
    if (screen != 0)
        return;
    widgets_paint(c, area);
    for (int i = 0; i < icon_count; i++) {
        struct rect cell = icon_cell(i);
        if (rect_empty(rect_intersect(cell, c->clip)))
            continue;
        if (i == drag_icon && icon_dragging) {
            icon_label(c, cell, icons[i].label, false);
            continue;
        }
        if (i == selected_icon)
            gfx_round(c, (struct rect){cell.x + 14, cell.y - 2, cell.w - 28, 70}, 8, ALPHA(0, 70));
        const struct image *img = program_icon(icons[i].path);
        if (img)
            gfx_scale(c, (struct rect){cell.x + (cell.w - 56) / 2, cell.y + 6, 56, 56},
                      &(struct canvas){img->px, img->w, img->h, img->w, {0, 0, img->w, img->h}}, 255);
        else
            gfx_icon(c, cell.x + (cell.w - 64) / 2, cell.y + 2, icons[i].icon, 64);
        icon_label(c, cell, icons[i].label, i == selected_icon);
    }
}

/* Windows programs and shortcuts to them show the program's own icon. */
#define PROGRAM_ICONS 16

static struct program_icon {
    char path[PATH_MAX_LEN];
    struct image img;
} program_icons[PROGRAM_ICONS];
static int program_icon_next;

static const struct image *program_icon(const char *path)
{
    const char *ext = path_ext(path);
    if (!ext || (str_casecmp(ext, ".exe") && str_casecmp(ext, ".lnk")))
        return NULL;
    for (int i = 0; i < PROGRAM_ICONS; i++)
        if (!strcmp(program_icons[i].path, path))
            return program_icons[i].img.px ? &program_icons[i].img : NULL;
    struct program_icon *p = &program_icons[program_icon_next++ % PROGRAM_ICONS];
    image_free(&p->img);
    str_copy(p->path, path, sizeof(p->path));
    char target[PATH_MAX_LEN];
    str_copy(target, path, sizeof(target));
    struct lnk *l = str_casecmp(ext, ".lnk") ? NULL : kmalloc(sizeof(*l));
    if (l && (lnk_read(path, l) != 0 || winpath_to_vfs("/", l->icon[0] ? l->icon : l->target, target, sizeof(target))))
        target[0] = 0;
    kfree(l);
    if (!target[0] || !image_load_exe_icon(target, 64, &p->img))
        p->img = (struct image){0};
    return p->img.px ? &p->img : NULL;
}

static void desktop_pick(struct window *owner, int index);
static void volume_pick(struct window *owner, int index);

/* A desktop icon dragged onto the Dock is pinned there (an app shortcut moves into the Dock). */
static void icon_entry(int i, char *out, size_t cap)
{
    const struct gui_app *app = app_shortcut(icons[i].path);
    str_copy(out, app ? app->name : icons[i].path, cap);
}

bool desktop_icon_drag(int x, int y)
{
    if (drag_icon < 0 || drag_icon >= icon_count)
        return false;
    int dx = x - drag_x, dy = y - drag_y;
    if (!icon_dragging && dx * dx + dy * dy <= 64)
        return true;
    if (!icon_dragging) {
        icon_dragging = true;
        wm_damage_z(icon_cell(drag_icon), Z_DESKTOP);
    }
    char entry[PATH_MAX_LEN];
    icon_entry(drag_icon, entry, sizeof(entry));
    dock_drag_move(entry, x, y);
    return true;
}

void desktop_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    if (!down && drag_icon >= 0) {
        int i = drag_icon;
        bool was = icon_dragging;
        drag_icon = -1;
        icon_dragging = false;
        if (was) {
            wm_damage_z(icon_cell(i), Z_DESKTOP);
            if (dock_drag_drop(x, y) && i < icon_count && app_shortcut(icons[i].path)) {
                vfs_unlink(icons[i].path);
                scan_desktop();
            }
            return;
        }
    }
    if (widgets_mouse(x, y, button, down, clicks) || !down)
        return;
    int hit = -1;
    for (int i = 0; i < icon_count; i++)
        if (rect_contains(icon_cell(i), x, y))
            hit = i;
    if (hit != selected_icon) {
        if (selected_icon >= 0)
            wm_damage_z(icon_cell(selected_icon), Z_DESKTOP);
        if (hit >= 0)
            wm_damage_z(icon_cell(hit), Z_DESKTOP);
        selected_icon = hit;
    }
    if (hit >= 0 && button == BUTTON_LEFT && clicks >= 2)
        gui_open_file(icons[hit].path);
    else if (hit >= 0 && button == BUTTON_LEFT && !icons[hit].volume && strcmp(icons[hit].path, "/")) {
        drag_icon = hit;
        drag_x = x;
        drag_y = y;
    }
    if (button == BUTTON_RIGHT && hit >= 0 && icons[hit].volume) {
        static const char *const items[] = {"Open", "", "Eject"};
        ui_menu(NULL, x, y, items, ARRAY_SIZE(items), volume_pick);
    } else if (button == BUTTON_RIGHT) {
        static const char *const items[] = {"New Folder", "Open Desktop in Files", "", "Change Wallpaper…",
                                            "Edit Widgets…", "Open Terminal"};
        ui_menu(NULL, x, y, items, ARRAY_SIZE(items), desktop_pick);
    }
}

bool gui_eject(const char *path)
{
    char label[48] = "";
    for (int i = 0; i < known_count; i++)
        if (!strcmp(known_volumes[i].path, path))
            str_copy(label, known_volumes[i].label, sizeof(label));
    int err = vfs_eject(path);
    if (err) {
        gui_notify("Eject failed", err_str(err));
        return false;
    }
    gui_notify(label[0] ? label : "Drive", "Ejected. You can now remove the drive safely.");
    scan_desktop();
    return true;
}

static void volume_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (selected_icon < 0 || selected_icon >= icon_count || !icons[selected_icon].volume)
        return;
    char path[PATH_MAX_LEN];
    str_copy(path, icons[selected_icon].path, sizeof(path));
    if (index == 0)
        gui_open_file(path);
    else if (index == 2)
        gui_eject(path);
}

static void desktop_pick(struct window *owner, int index)
{
    UNUSED(owner);
    char dir[PATH_MAX_LEN];
    desktop_dir(dir, sizeof(dir));
    if (index == 0) {
        char path[PATH_MAX_LEN];
        for (int i = 0; i < 100; i++) {
            if (i)
                ksnprintf(path, sizeof(path), "%s/untitled folder %d", dir, i + 1);
            else
                ksnprintf(path, sizeof(path), "%s/untitled folder", dir);
            if (!vfs_exists(path)) {
                vfs_mkdir(path);
                break;
            }
        }
        scan_desktop();
    } else if (index == 1) {
        gui_open_file(dir);
    } else if (index == 3) {
        const struct gui_app *settings = gui_app_find("System Settings");
        gui_app_launch(settings, "wallpaper");
    } else if (index == 4) {
        widgets_edit(true);
    } else if (index == 5) {
        gui_terminal_show();
    }
}

static bool word_prefix(const char *name, const char *prefix)
{
    size_t n = strlen(prefix);
    for (const char *w = name; *w; w++)
        if ((w == name || w[-1] == ' ') && str_ncasecmp(w, prefix, n) == 0)
            return true;
    return false;
}

const struct gui_app *gui_app_find(const char *name)
{
    if (!name || !*name)
        return NULL;
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        if (str_casecmp(app->name, name) == 0)
            return app;
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        if (word_prefix(app->name, name))
            return app;
    return NULL;
}

void gui_app_foreach(void (*fn)(const struct gui_app *app, void *ctx), void *ctx)
{
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        fn(app, ctx);
}

void gui_app_launch(const struct gui_app *app, const char *arg)
{
    if (!app || !app->launch)
        return;
    wm_set_app(app);
    app->launch(arg);
    wm_set_app(NULL);
}

static bool extension_listed(const char *list, const char *ext)
{
    if (!list || !ext)
        return false;
    size_t n = strlen(ext);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && str_ncasecmp(p, ext, n) == 0)
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}

#if CONFIG_WIN32
static int split_command_line(char *s, char **argv, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        char *out = s;
        argv[n++] = out;
        bool quoted = false;
        for (; *s && (quoted || (*s != ' ' && *s != '\t')); s++) {
            if (*s == '\\' && s[1] == '"')
                *out++ = *++s;
            else if (*s == '"')
                quoted = !quoted;
            else
                *out++ = *s;
        }
        if (*s)
            s++;
        *out = 0;
    }
    return n;
}

static bool start_program(const char *path, char *args, const char *dir)
{
    char *argv[32] = {(char *)path};
    int argc = 1 + (args ? split_command_line(args, argv + 1, (int)ARRAY_SIZE(argv) - 1) : 0);
    char cwd[PATH_MAX_LEN];
    if (dir && dir[0])
        str_copy(cwd, dir, sizeof(cwd));
    else
        path_dirname(path, cwd, sizeof(cwd));
    return session && w32_start(session, path, argc, argv, cwd) == 0;
}

static bool open_shortcut(const char *file)
{
    struct lnk *l = kmalloc(sizeof(*l));
    char path[PATH_MAX_LEN], dir[PATH_MAX_LEN] = "";
    bool ok = l && lnk_read(file, l) == 0 && winpath_to_vfs("/", l->target, path, sizeof(path)) == 0;
    if (ok && l->dir[0] && winpath_to_vfs("/", l->dir, dir, sizeof(dir)) != 0)
        dir[0] = 0;
    if (ok && !vfs_exists(path)) {
        gui_message("Cannot open the shortcut", "The program it points to is no longer installed.");
    } else if (ok) {
        if (!start_program(path, l->args, dir))
            gui_open_file(path);
    } else {
        gui_message("Cannot open the shortcut", "The file is not a valid Windows shortcut.");
    }
    kfree(l);
    return true;
}
#endif

bool gui_open_file(const char *path)
{
    if (vfs_is_dir(path)) {
        gui_app_launch(gui_app_find("Files"), path);
        return true;
    }
    const struct gui_app *shortcut = app_shortcut(path);
    if (shortcut) {
        gui_app_launch(shortcut, NULL);
        return true;
    }
    const char *ext = path_ext(path);
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        if (extension_listed(app->extensions, ext)) {
            gui_app_launch(app, path);
            return true;
        }
#if CONFIG_WIN32
    if (ext && !str_casecmp(ext, ".lnk"))
        return open_shortcut(path);
    if (ext && !str_casecmp(ext, ".exe") && start_program(path, NULL, NULL))
        return true;
#endif
    if (exec_is_executable_name(path)) {
        char line[PATH_MAX_LEN + 8];
        ksnprintf(line, sizeof(line), "'%s'", path);
        gui_terminal_run(line);
        return true;
    }
    gui_app_launch(gui_app_find(viewer_is_binary(path) ? "Viewer" : "Notepad"), path);
    return true;
}

const char *gui_clipboard(void)
{
    return clipboard;
}

void gui_set_clipboard(const char *text, size_t n)
{
    n = MIN(n, sizeof(clipboard) - 1);
    memcpy(clipboard, text, n);
    clipboard[n] = 0;
}

const char *gui_user(void)
{
    return user_current()->name;
}

static void close_app_window(struct window *w, void *ctx)
{
    UNUSED(ctx);
    if (wm_window_alive(w) && !(w->flags & (WIN_KEEP | WIN_POPUP | WIN_OVERLAY)))
        wm_destroy(w);
}

void gui_session_login(const char *name)
{
    user_switch(session, name);
    look_user_changed();
    gui_look_changed();
    widgets_user_changed();
    icon_count = -1;
    scan_desktop();
    chrome_changed();
    terminal_retitle();
    last_input = hal_ticks_ms();
    if (session && session->idle)
        gui_terminal_run("clear");
    autostart_pending = true;
}

void gui_terminal_show(void)
{
    struct window *t = terminal_window();
    if (t)
        wm_show(t);
}

void gui_terminal_run(const char *command)
{
    gui_terminal_show();
    if (!session || !session->idle) {
        gui_message("Terminal is busy", "A program is still running in the terminal. Wait for it to finish or "
                                        "press Ctrl+C there.");
        return;
    }
    input_inject(KEY_CTRL('u'));
    for (const char *p = command; *p; p++)
        input_inject((uint8_t)*p);
    input_inject(KEY_ENTER);
}

void gui_quit(int action)
{
    if (!session)
        return;
    if (action == QUIT_LOGOUT) {
        wm_close_popups();
        wm_foreach(close_app_window, NULL);
        user_logout();
        gui_login();
        return;
    }
    if (!session->idle) {
        gui_message("Cannot leave yet", "A program is still running in the terminal.");
        gui_terminal_show();
        return;
    }
    quit_action = action;
    session->ctl = CTL_EXIT;
    input_inject(KEY_EOF);
}

struct note {
    char title[64];
    char text[160];
    uint64_t until;
};

static void note_paint(struct window *w, struct canvas *c)
{
    struct note *n = w->data;
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    gfx_icon(c, 12, (c->h - 36) / 2, ICON_LOGO, 36);
    gfx_text_box(c, (struct rect){58, 12, c->w - 70, 18}, &font_bold, n->title, style.text, ALIGN_LEFT);
    ui_wrap_text(c, (struct rect){58, 32, c->w - 70, c->h - 36}, &font_sans, n->text, style.text);
}

static void note_event(struct window *w, struct gui_event *e)
{
    struct note *n = w->data;
    if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
        wm_destroy(w);
    } else if (e->type == EV_TIMER && hal_ticks_ms() >= n->until) {
        wm_destroy(w);
    }
}

static void note_destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops note_ops = {note_paint, note_event, NULL, note_destroy};

static void note_below(struct window *w, void *ctx)
{
    int *y = ctx;
    if (w->ops == &note_ops)
        *y = MAX(*y, w->frame.y + w->frame.h + 10);
}

void gui_notify(const char *title, const char *text)
{
    if (!active)
        return;
    struct note *n = kzalloc(sizeof(*n));
    if (!n)
        return;
    str_copy(n->title, title, sizeof(n->title));
    str_copy(n->text, text, sizeof(n->text));
    n->until = hal_ticks_ms() + 6000;
    struct rect s = wm_screen_rect(0);
    int y = s.y + menubar_height() + 10;
    wm_foreach(note_below, &y);
    struct window *w = wm_popup(s.x + s.w - NOTE_W - 12, y, NOTE_W, NOTE_H, &note_ops, n, NULL);
    if (!w) {
        kfree(n);
        return;
    }
    w->radius = 18;
    w->flags |= WIN_PASSIVE;
    wm_set_timer(w, 500);
}

void notify_tick(void)
{
}

static void cmd_key(int key)
{
    struct window *f = wm_focused();
    int base = KEY_BASE(key);
    int mods = key & KEY_MOD_MASK & ~KEY_MOD_SUPER;
    if (base == 'q' || base == 'Q') {
        if (f && f->app) {
            for (struct window *w = wm_app_window(f->app); w; w = wm_app_window(f->app)) {
                wm_close(w);
                if (wm_window_alive(w))
                    break;
            }
        } else if (f) {
            wm_close(f);
        }
        return;
    }
    if (base == 'w' || base == 'W') {
        if (f)
            wm_close(f);
        return;
    }
    if (base == 'm' || base == 'M') {
        if (f)
            wm_minimize(f);
        return;
    }
    if (base == ' ') {
        spotlight_toggle();
        return;
    }
    if (base == KEY_TAB) {
        wm_cycle();
        return;
    }
    if (base == ',') {
        gui_app_launch(gui_app_find("System Settings"), NULL);
        return;
    }
    if (base == 'l' || base == 'L') {
        gui_lock();
        return;
    }
    if ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z')) {
        wm_key(KEY_CTRL(base) | (mods & KEY_MOD_SHIFT));
        return;
    }
    wm_key(base | mods);
}

static void handle_key(int key)
{
    int base = KEY_BASE(key);
    if (gui_locked()) {
        struct window *lock = wm_top_popup();
        if (lock && lock->ops && lock->ops->event) {
            struct gui_event e = {.type = EV_KEY, .key = key};
            lock->ops->event(lock, &e);
        }
        return;
    }
    if (key == KEY_SUPER || key == (KEY_ESC | KEY_MOD_CTRL)) {
        launchpad_toggle();
        return;
    }
    if (key == (' ' | KEY_MOD_CTRL) || key == KEY_CTRL(' ')) {
        spotlight_toggle();
        return;
    }
    if (key == (KEY_TAB | KEY_MOD_ALT)) {
        wm_close_popups();
        wm_cycle();
        return;
    }
    if (key == (KEY_F4 | KEY_MOD_ALT)) {
        struct window *f = wm_focused();
        if (f)
            wm_close(f);
        return;
    }
    struct window *popup = wm_top_popup();
    if (popup && popup->ops && popup->ops->event) {
        struct gui_event e = {.type = EV_KEY, .key = key};
        popup->ops->event(popup, &e);
        return;
    }
    if ((key & KEY_MOD_SUPER) && base != KEY_SUPER) {
        cmd_key(key);
        return;
    }
    wm_key(key);
}

static bool pumping;

static void pump(int timeout_ms)
{
    if (pumping)
        return;
    pumping = true;
    bool busy = false;
    for (int i = 0; i < 64; i++) {
        int key = hal_key_read(0);
        if (key == KEY_NONE)
            break;
        handle_key(key);
        busy = true;
    }
    struct pointer_event ev;
    for (int i = 0; i < 64 && hal_pointer_read(&ev); i++) {
        wm_pointer(&ev);
        busy = true;
    }
    uint64_t now = hal_ticks_ms();
    if (busy)
        last_input = now;
    else if (lock_minutes && now - last_input > (uint64_t)lock_minutes * 60000 && !gui_locked() &&
             user_current()->has_password) {
        gui_lock();
        last_input = now;
    }
    wm_timers();
    struct rtc_time t;
    hal_rtc(&t);
    if (t.minute != last_minute) {
        last_minute = t.minute;
        chrome_changed();
    }
    if (now - last_scan > 2000) {
        last_scan = now;
        scan_desktop();
    }
    terminal_flush();
    wm_render();
    pumping = false;
    if (autostart_pending && session && session->idle && !gui_locked()) {
        autostart_pending = false;
        autostart_run();
    }
    wallpaper_fetch_run();
#if CONFIG_WIN32
    if (timeout_ms != 0)
        w32_background();
#endif
    if (!busy && timeout_ms != 0)
        hal_sleep_ms((uint32_t)(timeout_ms > 0 ? MIN(timeout_ms, 8) : 8));
}

static void copy_samples(const char *dir, const char *ext)
{
    struct vdir d;
    if (vfs_opendir("/system/samples", &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (str_casecmp(path_ext(ent.name), ext))
            continue;
        char from[PATH_MAX_LEN], to[PATH_MAX_LEN], *data;
        size_t size;
        path_join("/system/samples", ent.name, from, sizeof(from));
        path_join(dir, ent.name, to, sizeof(to));
        if (vfs_read_all(from, &data, &size) == 0) {
            vfs_write_all(to, data, size);
            kfree(data);
        }
    }
    vfs_closedir(&d);
}

static void prepare_home(void)
{
    static const char *const dirs[] = {"Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos"};
    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
        char path[PATH_MAX_LEN];
        path_join(user_home(), dirs[i], path, sizeof(path));
        if (vfs_exists(path))
            continue;
        vfs_mkdirs(path);
        if (!strcmp(dirs[i], "Pictures"))
            copy_samples(path, ".jpg");
        else if (!strcmp(dirs[i], "Videos"))
            copy_samples(path, ".avi");
    }
}

int gui_run(struct shell *sh)
{
    if (active)
        return sh_error(sh, "desktop", "the desktop is already running");
    struct framebuffer fbs[MAX_SCREENS];
    struct rect origins[MAX_SCREENS];
    int screens = hal_screen_count() ? open_displays(fbs, origins) : 0;
    if (!screens)
        return sh_error(sh, "desktop", "no graphics mode available on this display");
    if (!wm_start(fbs, origins, screens)) {
        hal_graphics_leave();
        return sh_error(sh, "desktop", "not enough memory for the desktop");
    }
    struct display *text_display = con_display();
    active = true;
    session = sh;
    quit_action = -1;
    last_minute = -1;
    selected_icon = -1;
    icon_count = 0;
    prepare_home();
    gui_style_refresh();
    gui_wallpaper_refresh();
    chrome_start();
    widgets_start();
    scan_desktop();
    wm_set_app(gui_app_find("Terminal"));
    struct window *term = terminal_open();
    wm_set_app(NULL);
    if (!term) {
        chrome_stop();
        active = false;
        wm_stop();
        free_wallpapers();
        hal_graphics_leave();
        con_init(text_display);
        return sh_error(sh, "desktop", "cannot open the terminal window");
    }
    input_set_pump(pump);
    last_input = hal_ticks_ms();
    if (!user_logged_in())
        gui_login();
    else
        autostart_pending = true;
    con_reset();

    shell_repl(sh);

    sh->ctl = CTL_NONE;
    input_set_pump(NULL);
    media_stop();
    widgets_stop();
    chrome_stop();
    terminal_close();
    wm_stop();
    free_wallpapers();
    active = false;
    session = NULL;
    hal_graphics_leave();
    con_init(text_display);
    if (quit_action == QUIT_REBOOT)
        hal_reboot();
    if (quit_action == QUIT_POWEROFF)
        hal_poweroff();
    return 0;
}

static void get_wallpaper(char *buf, size_t cap)
{
    str_copy(buf, wallpaper_setting, cap);
}

static int set_wallpaper(struct shell *sh, const char *value)
{
    UNUSED(sh);
    str_copy(wallpaper_setting, value, sizeof(wallpaper_setting));
    if (active)
        gui_wallpaper_refresh();
    return 0;
}

void gui_wallpaper_use(const char *path)
{
    set_wallpaper(NULL, path);
}

static const struct setting s_wallpaper = {"wallpaper", "auto|dune|mist|night|sunset|waves|hills|aurora|synthwave|solid|<picture file>",
                                           "desktop background", get_wallpaper, set_wallpaper};
SETTING(s_wallpaper);

static void invalidate_one(struct window *w, void *ctx)
{
    UNUSED(ctx);
    wm_invalidate(w);
}

static void refresh_all(void)
{
    if (!active)
        return;
    wm_crossfade();
    gui_style_refresh();
    gui_wallpaper_refresh();
    wm_foreach(invalidate_one, NULL);
    chrome_changed();
}

void gui_desktop_changed(void)
{
    if (!active)
        return;
    icon_count = -1;
    scan_desktop();
    wm_damage_all();
}

void gui_look_changed(void)
{
    if (!active)
        return;
    wm_crossfade();
    chrome_stop();
    widgets_stop();
    gui_style_refresh();
    chrome_start();
    widgets_start();
    gui_wallpaper_refresh();
    wm_foreach(invalidate_one, NULL);
    wm_reflow();
    icon_count = -1;
    scan_desktop();
    chrome_changed();
}

static void get_appearance(char *buf, size_t cap)
{
    str_copy(buf, appearance_setting, cap);
}

static int set_appearance(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "light") && strcmp(value, "dark") && strcmp(value, "auto"))
        return E_INVAL;
    str_copy(appearance_setting, value, sizeof(appearance_setting));
    refresh_all();
    return 0;
}

static const struct setting s_appearance = {"appearance", "auto|light|dark", "light or dark mode (auto follows the look)",
                                            get_appearance, set_appearance};
SETTING(s_appearance);

static void get_accent(char *buf, size_t cap)
{
    str_copy(buf, accent_setting, cap);
}

static int set_accent(struct shell *sh, const char *value)
{
    UNUSED(sh);
    bool known = !strcmp(value, "auto");
    for (size_t i = 0; i < ARRAY_SIZE(accents); i++)
        known |= !strcmp(value, accents[i].name);
    if (!known)
        return E_INVAL;
    str_copy(accent_setting, value, sizeof(accent_setting));
    refresh_all();
    return 0;
}

static const struct setting s_accent = {"accent", "auto|blue|purple|pink|red|orange|yellow|green|graphite",
                                        "desktop accent color", get_accent, set_accent};
SETTING(s_accent);

static void get_glass(char *buf, size_t cap)
{
    str_copy(buf, glass_setting, cap);
}

static int set_glass(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "liquid") && strcmp(value, "frosted") && strcmp(value, "solid"))
        return E_INVAL;
    str_copy(glass_setting, value, sizeof(glass_setting));
    refresh_all();
    return 0;
}

static const struct setting s_glass = {"glass", "liquid|frosted|solid", "transparency of menus, dock and sidebars",
                                       get_glass, set_glass};
SETTING(s_glass);

static void get_autostart(char *buf, size_t cap)
{
    str_copy(buf, autostart ? "on" : "off", cap);
}

static int set_autostart(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    autostart = strcmp(value, "on") == 0;
    return 0;
}

static const struct setting s_autostart = {"desktop", "on|off", "start the graphical desktop at boot",
                                           get_autostart, set_autostart};
SETTING(s_autostart);

static void get_resolution(int i, char *buf, size_t cap)
{
    if (want_w[i])
        ksnprintf(buf, cap, "%dx%d", want_w[i], want_h[i]);
    else
        str_copy(buf, "auto", cap);
}

static int set_resolution(int i, const char *value)
{
    if (!strcmp(value, "auto")) {
        want_w[i] = want_h[i] = 0;
    } else {
        const char *x = strchr(value, 'x');
        long w, h;
        char wbuf[8];
        if (!x || x - value >= (long)sizeof(wbuf))
            return E_INVAL;
        memcpy(wbuf, value, (size_t)(x - value));
        wbuf[x - value] = 0;
        if (!str_parse_long(wbuf, &w) || !str_parse_long(x + 1, &h) || w < 320 || h < 200 || w > 4096 || h > 4096)
            return E_INVAL;
        want_w[i] = (int)w;
        want_h[i] = (int)h;
    }
    if (active)
        gui_displays_changed();
    return 0;
}

static void get_res1(char *buf, size_t cap)
{
    get_resolution(0, buf, cap);
}

static int set_res1(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return set_resolution(0, value);
}

static void get_res2(char *buf, size_t cap)
{
    get_resolution(1, buf, cap);
}

static int set_res2(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return set_resolution(1, value);
}

static const struct setting s_res1 = {"resolution", "auto|<width>x<height>", "resolution of the main display",
                                      get_res1, set_res1};
SETTING(s_res1);
static const struct setting s_res2 = {"resolution2", "auto|<width>x<height>", "resolution of the second display",
                                      get_res2, set_res2};
SETTING(s_res2);

static void get_arrange(char *buf, size_t cap)
{
    str_copy(buf, arrange, cap);
}

static int set_arrange(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "right") && strcmp(value, "left") && strcmp(value, "above") && strcmp(value, "below") &&
        strcmp(value, "off"))
        return E_INVAL;
    str_copy(arrange, value, sizeof(arrange));
    if (active)
        gui_displays_changed();
    return 0;
}

static const struct setting s_arrange = {"arrange", "right|left|above|below|off",
                                         "where the second display is, or off to use one display", get_arrange,
                                         set_arrange};
SETTING(s_arrange);

static void get_locktime(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", lock_minutes);
}

static int set_locktime(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 0 || v > 1440)
        return E_INVAL;
    lock_minutes = (int)v;
    return 0;
}

static const struct setting s_locktime = {"locktime", "<minutes>|0", "lock the desktop after this many idle minutes",
                                          get_locktime, set_locktime};
SETTING(s_locktime);

bool gui_autostart(void)
{
    return autostart;
}

COMMAND(desktop, "startx,gui", "system", "desktop", "start the graphical desktop")
{
    UNUSED(argc);
    UNUSED(argv);
    return gui_run(sh);
}

static void list_app(const struct gui_app *app, void *ctx)
{
    struct shell *sh = ctx;
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "  %-16s", app->name);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, " %s\n", app->description);
    sh_role(sh, ROLE_TEXT);
}

COMMAND(launch, "xopen,open", "system", "launch [app|file] [file]", "start a desktop app or open a file in one")
{
    if (argc < 2) {
        gui_app_foreach(list_app, sh);
        return 0;
    }
    if (!active)
        return sh_error(sh, "launch", "the desktop is not running (start it with 'desktop')");
    char path[PATH_MAX_LEN];
    const struct gui_app *app = gui_app_find(argv[1]);
    if (app) {
        if (argc > 2 && strstr(argv[2], "://"))
            str_copy(path, argv[2], sizeof(path));
        else if (argc > 2 && sh_resolve(sh, argv[2], path, sizeof(path)) != 0)
            return sh_error(sh, "launch", "bad path: %s", argv[2]);
        gui_app_launch(app, argc > 2 ? path : NULL);
        return 0;
    }
    if (sh_resolve(sh, argv[1], path, sizeof(path)) != 0 || !vfs_exists(path))
        return sh_error(sh, "launch", "no such app or file: %s", argv[1]);
    return gui_open_file(path) ? 0 : sh_error(sh, "launch", "nothing can open %s", argv[1]);
}

COMMAND(notify, NULL, "system", "notify <title> [text]", "show a desktop notification")
{
    if (argc < 2)
        return sh_error(sh, "notify", "usage: notify <title> [text]");
    if (!active)
        return sh_error(sh, "notify", "the desktop is not running");
    gui_notify(argv[1], argc > 2 ? argv[2] : "");
    return 0;
}
