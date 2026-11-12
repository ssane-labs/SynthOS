#include <synth/codepage.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

/* Notes: plain-text notes in ~/Notes, newest first, the first line is the title. Saved a moment after typing stops,
   when another note is picked and when the window closes. */

#define MAX_NOTES 64
#define NOTE_MAX (32 * 1024)
#define SIDE 230
#define PAD 24

struct note {
    char file[40], title[48], preview[64];
    uint32_t mtime;
};

struct line {
    int start, len;
};

struct notes {
    struct ui_state st;
    struct ui_text find;
    struct note list[MAX_NOTES];
    int count, current, scroll, list_scroll;
    char *text;
    int len, cursor, goal_x;
    bool dirty;
    struct line *lines;
    int line_count, line_cap;
};

static struct window *open_win;

static void dir(char *out, size_t cap)
{
    path_join(user_home(), "Notes", out, cap);
}

static void note_path(const struct note *n, char *out, size_t cap)
{
    char d[PATH_MAX_LEN];
    dir(d, sizeof(d));
    path_join(d, n->file, out, cap);
}

static void describe(struct note *n, const char *text, size_t len)
{
    size_t i = 0, k;
    while (i < len && (text[i] == '\n' || text[i] == ' '))
        i++;
    for (k = 0; i < len && text[i] != '\n' && k + 1 < sizeof(n->title); i++)
        n->title[k++] = text[i];
    n->title[k] = 0;
    if (!k)
        str_copy(n->title, "New Note", sizeof(n->title));
    while (i < len && text[i] != '\n')
        i++;
    for (k = 0; i < len && k + 1 < sizeof(n->preview); i++)
        if (text[i] != '\n' || (k && n->preview[k - 1] != ' '))
            n->preview[k++] = text[i] == '\n' ? ' ' : text[i];
    n->preview[k] = 0;
    if (!k)
        str_copy(n->preview, "No additional text", sizeof(n->preview));
}

static void scan(struct notes *s)
{
    char d[PATH_MAX_LEN], path[PATH_MAX_LEN];
    dir(d, sizeof(d));
    s->count = 0;
    struct vdir it;
    if (vfs_opendir(d, &it) != 0)
        return;
    struct vfs_dirent e;
    while (s->count < MAX_NOTES && vfs_readdir(&it, &e) == 0) {
        if (!str_ends(e.name, ".txt"))
            continue;
        struct note *n = &s->list[s->count];
        str_copy(n->file, e.name, sizeof(n->file));
        path_join(d, e.name, path, sizeof(path));
        struct vstat st;
        n->mtime = vfs_stat(path, &st) == 0 ? st.mtime : 0;
        char *data;
        size_t size;
        if (vfs_read_all(path, &data, &size) == 0) {
            describe(n, data, size);
            kfree(data);
        } else {
            describe(n, "", 0);
        }
        s->count++;
    }
    vfs_closedir(&it);
    for (int i = 1; i < s->count; i++) /* newest first */
        for (int j = i; j > 0 && s->list[j].mtime > s->list[j - 1].mtime; j--) {
            struct note t = s->list[j];
            s->list[j] = s->list[j - 1];
            s->list[j - 1] = t;
        }
}

static void save(struct notes *s)
{
    if (!s->dirty || s->current < 0 || s->current >= s->count)
        return;
    char path[PATH_MAX_LEN];
    note_path(&s->list[s->current], path, sizeof(path));
    vfs_write_all(path, s->text, (size_t)s->len);
    describe(&s->list[s->current], s->text, (size_t)s->len);
    s->list[s->current].mtime = time_now();
    s->dirty = false;
}

static void open_note(struct notes *s, int index)
{
    save(s);
    s->current = index;
    s->len = s->cursor = s->scroll = 0;
    s->text[0] = 0;
    if (index < 0 || index >= s->count)
        return;
    char path[PATH_MAX_LEN], *data;
    size_t size;
    note_path(&s->list[index], path, sizeof(path));
    if (vfs_read_all(path, &data, &size) == 0) {
        s->len = (int)MIN(size, (size_t)NOTE_MAX - 1);
        memcpy(s->text, data, (size_t)s->len);
        s->text[s->len] = 0;
        kfree(data);
    }
    s->cursor = s->len;
}

static void new_note(struct notes *s)
{
    save(s);
    if (s->count == MAX_NOTES)
        return;
    char d[PATH_MAX_LEN];
    dir(d, sizeof(d));
    vfs_mkdirs(d);
    struct note n = {0};
    for (uint32_t k = time_now();; k++) {
        char path[PATH_MAX_LEN];
        ksnprintf(n.file, sizeof(n.file), "note-%u.txt", k);
        path_join(d, n.file, path, sizeof(path));
        if (!vfs_exists(path)) {
            vfs_write_all(path, "", 0);
            break;
        }
    }
    describe(&n, "", 0);
    n.mtime = time_now();
    memmove(&s->list[1], &s->list[0], sizeof(s->list[0]) * (size_t)s->count);
    s->list[0] = n;
    s->count++;
    s->current = -1;
    open_note(s, 0);
    ui_text_set(&s->find, "");
}

static void delete_note(struct window *owner, const char *value)
{
    struct notes *s = owner && wm_window_alive(owner) ? owner->data : NULL;
    if (!s || !value || s->current < 0 || s->current >= s->count)
        return;
    char path[PATH_MAX_LEN];
    note_path(&s->list[s->current], path, sizeof(path));
    vfs_unlink(path);
    memmove(&s->list[s->current], &s->list[s->current + 1],
            sizeof(s->list[0]) * (size_t)(s->count - s->current - 1));
    s->count--;
    s->dirty = false;
    int next = MIN(s->current, s->count - 1);
    s->current = -1;
    open_note(s, next);
    wm_invalidate(owner);
}

static void insert(struct notes *s, const char *t, int n)
{
    n = MIN(n, NOTE_MAX - 1 - s->len);
    if (n <= 0)
        return;
    memmove(s->text + s->cursor + n, s->text + s->cursor, (size_t)(s->len - s->cursor + 1));
    memcpy(s->text + s->cursor, t, (size_t)n);
    s->len += n;
    s->cursor += n;
    s->dirty = true;
}

static void erase(struct notes *s, int from, int to)
{
    if (from >= to)
        return;
    memmove(s->text + from, s->text + to, (size_t)(s->len - to + 1));
    s->len -= to - from;
    s->cursor = from;
    s->dirty = true;
}

static int prev_char(const char *t, int i)
{
    while (i > 0 && ((uint8_t)t[--i] & 0xc0) == 0x80)
        ;
    return i;
}

static int next_char(const char *t, int i, int len)
{
    if (i < len)
        i++;
    while (i < len && ((uint8_t)t[i] & 0xc0) == 0x80)
        i++;
    return i;
}

/* wraps the text at word boundaries into lines no wider than width */
static void layout(struct notes *s, const struct ui_font *f, int width)
{
    s->line_count = 0;
    int i = 0;
    for (;;) {
        if (s->line_count == s->line_cap) {
            int cap = s->line_cap ? s->line_cap * 2 : 128;
            struct line *grown = krealloc(s->lines, sizeof(*grown) * (size_t)cap);
            if (!grown)
                return;
            s->lines = grown, s->line_cap = cap;
        }
        int start = i, end = i, w = 0, last_space = -1;
        while (end < s->len && s->text[end] != '\n') {
            int next = next_char(s->text, end, s->len);
            int cw = gfx_text_width_n(f, s->text + end, (size_t)(next - end));
            if (w + cw > width && end > start) {
                if (last_space > start)
                    end = last_space + 1;
                break;
            }
            if (s->text[end] == ' ')
                last_space = end;
            w += cw;
            end = next;
        }
        s->lines[s->line_count++] = (struct line){start, end - start};
        if (end >= s->len)
            break;
        i = s->text[end] == '\n' ? end + 1 : end;
    }
}

static int line_of(const struct notes *s, int pos)
{
    for (int i = s->line_count - 1; i >= 0; i--)
        if (pos >= s->lines[i].start)
            return i;
    return 0;
}

static int pos_at_x(const struct notes *s, const struct ui_font *f, int line, int x)
{
    const struct line *l = &s->lines[line];
    int i = l->start, end = l->start + l->len;
    if (end > l->start && end <= s->len && line + 1 < s->line_count && s->text[end - 1] == ' ' &&
        s->lines[line + 1].start == end)
        end--;
    while (i < end) {
        int next = next_char(s->text, i, end);
        int a = gfx_text_width_n(f, s->text + l->start, (size_t)(i - l->start)),
            b = gfx_text_width_n(f, s->text + l->start, (size_t)(next - l->start));
        if (x < (a + b) / 2)
            return i;
        i = next;
    }
    return end;
}

static void edit_key(struct notes *s, const struct ui_font *f, int key)
{
    int base = KEY_BASE(key), line = line_of(s, s->cursor);
    bool vertical = base == KEY_UP || base == KEY_DOWN || base == KEY_PGUP || base == KEY_PGDN;
    if (!vertical)
        s->goal_x = -1;
    if (s->goal_x < 0)
        s->goal_x = gfx_text_width_n(f, s->text + s->lines[line].start, (size_t)(s->cursor - s->lines[line].start));
    if (KEY_IS_TEXT(key)) {
        char enc[4];
        insert(s, enc, (int)utf8_encode((uint32_t)KEY_TEXT_CP(key), enc));
    } else if (key == KEY_ENTER || key == '\r') {
        insert(s, "\n", 1);
    } else if (key == KEY_TAB) {
        insert(s, "    ", 4);
    } else if (key == KEY_BACKSPACE) {
        erase(s, prev_char(s->text, s->cursor), s->cursor);
    } else if (base == KEY_DELETE) {
        erase(s, s->cursor, next_char(s->text, s->cursor, s->len));
    } else if (base == KEY_LEFT) {
        s->cursor = prev_char(s->text, s->cursor);
    } else if (base == KEY_RIGHT) {
        s->cursor = next_char(s->text, s->cursor, s->len);
    } else if (base == KEY_HOME) {
        s->cursor = s->lines[line].start;
    } else if (base == KEY_END) {
        int end = s->lines[line].start + s->lines[line].len;
        s->cursor = line + 1 < s->line_count && end > 0 && s->text[end - 1] == ' ' ? end - 1 : end;
    } else if (vertical) {
        int step = base == KEY_UP ? -1 : base == KEY_DOWN ? 1 : base == KEY_PGUP ? -12 : 12;
        int to = CLAMP(line + step, 0, s->line_count - 1);
        s->cursor = to == line ? (step < 0 ? 0 : s->len) : pos_at_x(s, f, to, s->goal_x);
    } else if (key == KEY_CTRL('v')) {
        const char *clip = gui_clipboard();
        insert(s, clip, (int)strlen(clip));
    } else if (key == KEY_CTRL('a')) {
        s->cursor = s->len;
    } else if (key >= 0x20 && key < 0x100 && key != 0x7f) {
        char enc[4];
        insert(s, enc, (int)utf8_encode((uint32_t)key, enc));
    }
}

static bool has(const char *text, const char *find)
{
    size_t n = strlen(find);
    for (; *text; text++)
        if (!str_ncasecmp(text, find, n))
            return true;
    return false;
}

static bool matches(const struct note *n, const char *find)
{
    return !*find || has(n->title, find) || has(n->preview, find);
}

static void frame(struct ui *ui, void *data)
{
    struct notes *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    const struct ui_font *f = &font_sans;
    if (ui->e && ui->e->type == EV_TIMER) {
        save(s);
        ui->consumed = true;
    }
    int key = ui_key(ui);
    if (key == KEY_CTRL('n') || key == KEY_COMMAND('n')) {
        new_note(s);
        ui->consumed = true;
    }
    ui_sidebar_panel(ui, SIDE, H, style.sidebar);
    if (ui_search(ui, (struct rect){10, 10, SIDE - 52, 26}, &s->find, "Search"))
        s->list_scroll = 0;
    if (ui_symbol_button(ui, (struct rect){SIDE - 38, 10, 28, 26}, SYM_PENCIL, false))
        new_note(s);
    int y = 46 - s->list_scroll, shown = 0;
    for (int i = 0; i < s->count; i++) {
        const struct note *n = &s->list[i];
        if (!matches(n, s->find.buf))
            continue;
        shown++;
        struct rect row = {8, y, SIDE - 16, 54};
        y += 58;
        if (row.y + row.h < 40 || row.y > H)
            continue;
        if (ui->c && i == s->current)
            gfx_round(ui->c, row, 8, ALPHA(style.accent, 110));
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(row, ui->e->x, ui->e->y) && ui->e->y > 40 &&
            i != s->current) {
            open_note(s, i);
            ui->consumed = true;
        }
        struct rtc_time t;
        time_to_rtc(n->mtime + (uint32_t)(time_zone() * 60), &t);
        char date[24];
        ksnprintf(date, sizeof(date), "%02d.%02d.%04d", t.day, t.month, t.year);
        ui_label(ui, (struct rect){row.x + 10, row.y + 6, row.w - 20, 20}, n->title, &font_bold, style.text,
                 ALIGN_LEFT);
        int dw = gfx_text_width(&font_small, date);
        ui_label(ui, (struct rect){row.x + 10, row.y + 28, dw, 18}, date, &font_small, style.text, ALIGN_LEFT);
        ui_label(ui, (struct rect){row.x + 18 + dw, row.y + 28, row.w - 28 - dw, 18}, n->preview, &font_small,
                 style.secondary, ALIGN_LEFT);
    }
    if (ui->e && ui->e->type == EV_WHEEL && ui->e->x < SIDE) {
        s->list_scroll = CLAMP(s->list_scroll - ui->e->wheel * 40, 0, MAX(0, shown * 58 - (H - 50)));
        ui->consumed = true;
    }
    if (!shown)
        ui_label(ui, (struct rect){10, 60, SIDE - 20, 20}, s->count ? "No matches" : "No notes", &font_sans,
                 style.secondary, ALIGN_CENTER);
    struct rect page = {SIDE, 0, W - SIDE, H};
    ui_panel(ui, page, style.content);
    if (s->current < 0) {
        ui_label(ui, page, "Press the pencil to write a note", &font_sans, style.secondary, ALIGN_CENTER);
        return;
    }
    if (ui_button(ui, (struct rect){W - 90, 10, 76, 26}, "Delete"))
        ui_confirm(ui->win, "Delete this note?", s->list[s->current].title, delete_note);
    struct rtc_time t;
    time_to_rtc(s->list[s->current].mtime + (uint32_t)(time_zone() * 60), &t);
    char text[48];
    ksnprintf(text, sizeof(text), "%d %s %d at %02d:%02d", t.day, time_month_name(t.month), t.year, t.hour, t.minute);
    ui_label(ui, (struct rect){SIDE, 14, W - SIDE, 18}, text, &font_small, style.tertiary, ALIGN_CENTER);
    struct rect area = {SIDE + PAD, 46, W - SIDE - 2 * PAD, H - 56};
    int lh = f->height + 4, before = s->cursor;
    if (key != KEY_NONE && !ui->consumed && !(key & (KEY_MOD_SUPER | KEY_MOD_ALT))) {
        layout(s, f, area.w);
        edit_key(s, f, key);
        ui->consumed = true;
    }
    layout(s, f, area.w);
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(page, ui->e->x, ui->e->y) && ui->e->y >= area.y) {
        int line = CLAMP((ui->e->y - area.y + s->scroll) / lh, 0, s->line_count - 1);
        s->cursor = pos_at_x(s, f, line, ui->e->x - area.x);
        s->goal_x = -1;
    }
    if (ui->e && ui->e->type == EV_WHEEL && ui->e->x >= SIDE) {
        s->scroll -= ui->e->wheel * lh * 3;
        ui->consumed = true;
    }
    int cl = line_of(s, s->cursor);
    if (s->cursor != before || s->dirty) { /* keep the caret in view */
        if (cl * lh < s->scroll)
            s->scroll = cl * lh;
        if ((cl + 1) * lh > s->scroll + area.h)
            s->scroll = (cl + 1) * lh - area.h;
    }
    s->scroll = CLAMP(s->scroll, 0, MAX(0, s->line_count * lh - area.h));
    if (s->dirty)
        wm_set_timer(ui->win, 1500);
    if (!ui->c)
        return;
    struct canvas clip = *ui->c;
    clip.clip = rect_intersect(ui->c->clip, area);
    for (int i = s->scroll / lh; i < s->line_count; i++) {
        int ly = area.y + i * lh - s->scroll;
        if (ly > area.y + area.h)
            break;
        const struct line *l = &s->lines[i];
        bool title = i == 0 || (i > 0 && s->lines[0].start + s->lines[0].len == l->start && s->text[l->start - 1] != '\n');
        gfx_text_n(&clip, area.x, ly, title ? &font_bold : f, s->text + l->start, (size_t)l->len, style.text);
    }
    if (ui->win == wm_focused()) {
        const struct line *l = &s->lines[cl];
        int cx = area.x + gfx_text_width_n(f, s->text + l->start, (size_t)(s->cursor - l->start));
        gfx_fill(&clip, (struct rect){cx, area.y + cl * lh - s->scroll, 2, f->height + 2}, style.accent);
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct notes *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct notes *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    struct notes *s = w->data;
    save(s);
    if (open_win == w)
        open_win = NULL;
    kfree(s->lines);
    kfree(s->text);
    kfree(s);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct notes *s = kzalloc(sizeof(*s));
    if (!s || !(s->text = kmalloc(NOTE_MAX))) {
        kfree(s);
        return;
    }
    s->text[0] = 0;
    s->current = -1;
    s->goal_x = -1;
    scan(s);
    if (!s->count) {
        static const char first[] = "Welcome to Notes\nNotes are kept in the Notes folder of your home as plain text. "
                                    "The first line is the title.\n\nPress the pencil or Ctrl+N for a new note.";
        new_note(s);
        insert(s, first, (int)sizeof(first) - 1);
        save(s);
    } else {
        open_note(s, 0);
    }
    open_win = wm_open("Notes", ICON_NOTES, 820, 560, &ops, s, WIN_RESIZABLE);
    if (!open_win) {
        kfree(s->lines);
        kfree(s->text);
        kfree(s);
    }
}

static const struct gui_app notes_app = {
    .name = "Notes", .description = "Quick plain-text notes", .icon = ICON_NOTES, .launch = launch};
GUI_APP(notes_app);
