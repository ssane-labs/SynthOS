#include <synth/command.h>
#include <synth/module.h>
#include <synth/vfs.h>

#include "systool.h"

/* Task Scheduler: the rules of /home/.rules shown as tasks — a command that runs at a time (every day or on some
   days), every few minutes, at login, at the start or when an app opens. Tasks are added and removed through the
   'rules' command, so the running rules always match the list; "Run Now" runs one at once and shows what it
   printed. When each task last ran comes from the log. */

#define MAX_TASKS 32

struct task {
    char when[64], cmd[192];
};

enum { T_DAILY, T_DAYS, T_EVERY, T_LOGIN, T_START, T_OPEN, T_KINDS };

struct sched {
    struct ui_state st;
    struct task tasks[MAX_TASKS];
    int count, selected, kind;
    bool days[7];
    struct ui_text time, minutes, app, cmd;
    char status[160];
};

static struct window *open_win;
static const char *const day_names[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

static void load(struct sched *s)
{
    s->count = 0;
    char *data;
    size_t size;
    if (vfs_read_all("/home/.rules", &data, &size) == 0) {
        for (char *line = data; line < data + size && s->count < MAX_TASKS;) {
            char *end = memchr(line, '\n', (size_t)(data + size - line));
            if (end)
                *end = 0;
            char *tab = strchr(line, '\t');
            if (tab) {
                *tab = 0;
                str_copy(s->tasks[s->count].when, line, sizeof(s->tasks[0].when));
                str_copy(s->tasks[s->count].cmd, tab + 1, sizeof(s->tasks[0].cmd));
                s->count++;
            }
            line = end ? end + 1 : data + size;
        }
        kfree(data);
    }
    s->selected = CLAMP(s->selected, -1, s->count - 1);
}

/* "at 09:00 mon,fri" -> "Mondays and Fridays at 09:00" */
static void describe(const char *when, char *out, size_t cap)
{
    static const char *const full[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (!strncmp(when, "at ", 3) && strlen(when) > 9) {
        char days[48] = "";
        for (int d = 0; d < 7; d++)
            if (strstr(when + 9, day_names[d])) {
                if (days[0])
                    str_append(days, ", ", sizeof(days));
                str_append(days, full[d], sizeof(days));
            }
        ksnprintf(out, cap, "%s at %.5s", days, when + 3);
    } else if (!strncmp(when, "at ", 3)) {
        ksnprintf(out, cap, "Every day at %s", when + 3);
    } else if (!strncmp(when, "every ", 6)) {
        ksnprintf(out, cap, !strcmp(when + 6, "1") ? "Every minute" : "Every %s minutes", when + 6);
    } else if (!strcmp(when, "login")) {
        str_copy(out, "At login", cap);
    } else if (!strcmp(when, "start")) {
        str_copy(out, "When the computer starts", cap);
    } else if (!strncmp(when, "open ", 5)) {
        ksnprintf(out, cap, "When %s opens", when + 5);
    } else if (!strncmp(when, "save ", 5)) {
        ksnprintf(out, cap, "When a file in %s is saved", when + 5);
    } else {
        str_copy(out, when, cap);
    }
}

/* the time of the latest "rules: <when> -> <cmd>" line in this start's log */
static void last_run(const struct task *t, char *out, size_t cap)
{
    str_copy(out, "Not run since the start", cap);
    char *log;
    size_t size;
    if (vfs_read_all("/home/.logs/current.log", &log, &size) != 0)
        return;
    char want[280];
    ksnprintf(want, sizeof(want), "] rules: %s -> %s", t->when, t->cmd);
    const char *hit = NULL;
    for (const char *p = log; (p = strstr(p, want)) != NULL; p++)
        hit = p;
    if (hit) {
        const char *open = hit;
        while (open > log && *open != '[')
            open--;
        long secs = str_to_long(open + 1, NULL, 10);
        ksnprintf(out, cap, "Last ran %ld:%02ld:%02ld after the start", secs / 3600, secs / 60 % 60, secs % 60);
    }
    kfree(log);
}

static void build_when(struct sched *s, char *out, size_t cap)
{
    switch (s->kind) {
    case T_DAILY: ksnprintf(out, cap, "at %s", s->time.buf); break;
    case T_DAYS: {
        ksnprintf(out, cap, "at %s ", s->time.buf);
        bool first = true;
        for (int d = 0; d < 7; d++)
            if (s->days[d]) {
                if (!first)
                    str_append(out, ",", cap);
                str_append(out, day_names[d], cap);
                first = false;
            }
        break;
    }
    case T_EVERY: ksnprintf(out, cap, "every %s", s->minutes.buf); break;
    case T_LOGIN: str_copy(out, "login", cap); break;
    case T_START: str_copy(out, "start", cap); break;
    default: ksnprintf(out, cap, "open %s", s->app.buf); break;
    }
}

static void set_status(struct sched *s, char *text)
{
    tool_clean(text);
    char *nl = strchr(text, '\n');
    if (nl && !nl[1])
        *nl = 0;
    str_copy(s->status, text[0] ? text : "Done, nothing printed", sizeof(s->status));
    for (char *p = s->status; *p; p++)
        if (*p == '\n')
            *p = ' ';
    kfree(text);
}

static void frame(struct ui *ui, void *data)
{
    struct sched *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    static const struct tool_item items[] = {{SYM_CLOCK, "Tasks", "Task Scheduler"}};
    int page = 0;
    tool_sidebar(ui, H, items, 1, &page);
    char sub[32];
    ksnprintf(sub, sizeof(sub), "%d tasks", s->count);
    struct rect a = tool_page(ui, W, H, "Tasks", sub);
    int x = a.x + 20, w = a.w - 40, y = a.y + 14;
    for (int i = 0; i < s->count && i < 7; i++) { /* the list */
        struct rect r = {x, y + i * 44, w, 42};
        bool sel = s->selected == i;
        if (ui->c)
            gfx_round(ui->c, r, 8, sel ? style.selection : style.group);
        char when[96], last[64];
        describe(s->tasks[i].when, when, sizeof(when));
        ui_label(ui, (struct rect){r.x + 12, r.y + 3, r.w / 2, 20}, when, &font_bold, ui_row_text(sel), ALIGN_LEFT);
        ui_label(ui, (struct rect){r.x + 12, r.y + 21, r.w - 24, 18}, s->tasks[i].cmd, &font_small,
                 sel ? ui_row_text(true) : style.secondary, ALIGN_LEFT);
        if (sel) {
            last_run(&s->tasks[i], last, sizeof(last));
            ui_label(ui, (struct rect){r.x + r.w / 2, r.y + 3, r.w / 2 - 12, 20}, last, &font_small, ui_row_text(true),
                     ALIGN_RIGHT);
        }
        if (ui_hover(ui, r) && ui->e && ui->e->type == EV_MOUSE_DOWN)
            s->selected = i;
    }
    if (!s->count)
        ui_label(ui, (struct rect){x, y + 40, w, 20}, "No tasks yet: make one below", &font_sans, style.tertiary,
                 ALIGN_CENTER);
    int fy = H - 170;
    if (ui->c)
        gfx_fill(ui->c, (struct rect){a.x, fy - 12, a.w, 1}, style.separator);
    static const char *const kinds[T_KINDS] = {"Daily", "On Days", "Every", "At Login", "At Start", "App Opens"};
    ui_segmented(ui, (struct rect){x, fy, MIN(w, 520), 26}, kinds, T_KINDS, &s->kind);
    fy += 36;
    int fx = x;
    if (s->kind <= T_DAYS) {
        ui_label(ui, (struct rect){fx, fy, 40, 28}, "Time", &font_sans, style.secondary, ALIGN_LEFT);
        ui_textbox_hint(ui, (struct rect){fx + 44, fy, 70, 28}, &s->time, "09:00");
        fx += 130;
    }
    if (s->kind == T_DAYS)
        for (int d = 0; d < 7; d++) {
            static const char *const letters[7] = {"S", "M", "T", "W", "T", "F", "S"};
            struct rect r = {fx + d * 34, fy, 30, 28};
            if (s->days[d] ? ui_button_primary(ui, r, letters[d]) : ui_button(ui, r, letters[d]))
                s->days[d] = !s->days[d];
        }
    if (s->kind == T_EVERY) {
        ui_textbox_hint(ui, (struct rect){fx, fy, 70, 28}, &s->minutes, "15");
        ui_label(ui, (struct rect){fx + 78, fy, 80, 28}, "minutes", &font_sans, style.secondary, ALIGN_LEFT);
    }
    if (s->kind == T_OPEN)
        ui_textbox_hint(ui, (struct rect){fx, fy, 220, 28}, &s->app, "App name, e.g. Notepad");
    fy += 38;
    ui_label(ui, (struct rect){x, fy, 70, 28}, "Command", &font_sans, style.secondary, ALIGN_LEFT);
    ui_textbox_hint(ui, (struct rect){x + 74, fy, w - 74, 28}, &s->cmd, "e.g. backup /home/Documents");
    fy += 40;
    if (ui_button_primary(ui, (struct rect){x + w - 110, fy, 110, 28}, "Add Task") && s->cmd.buf[0]) {
        char when[96], line[320];
        build_when(s, when, sizeof(when));
        ksnprintf(line, sizeof(line), "rules add %s -- %s", when, s->cmd.buf);
        set_status(s, tool_run(line));
        load(s);
    }
    if (s->selected >= 0 && s->selected < s->count) {
        if (ui_button(ui, (struct rect){x, fy, 100, 28}, "Run Now"))
            set_status(s, tool_run(s->tasks[s->selected].cmd));
        if (ui_button(ui, (struct rect){x + 108, fy, 100, 28}, "Remove")) {
            char line[32];
            ksnprintf(line, sizeof(line), "rules remove %d", s->selected + 1);
            set_status(s, tool_run(line));
            s->selected = -1;
            load(s);
        }
    }
    ui_label(ui, (struct rect){x + 220, fy, w - 340, 28}, s->status, &font_small, style.secondary, ALIGN_LEFT);
}

static void paint(struct window *win, struct canvas *c)
{
    struct sched *s = win->data;
    ui_run(win, &s->st, c, NULL, frame, s);
}

static void event(struct window *win, struct gui_event *e)
{
    struct sched *s = win->data;
    ui_run(win, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *win)
{
    kfree(win->data);
    if (open_win == win)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct sched *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    s->selected = -1;
    ui_text_set(&s->time, "09:00");
    ui_text_set(&s->minutes, "15");
    s->days[1] = s->days[2] = s->days[3] = s->days[4] = s->days[5] = true;
    load(s);
    open_win = tool_open("Task Scheduler", ICON_CLOCK, 820, 600, &ops, s);
    if (!open_win)
        kfree(s);
}

static const struct gui_app scheduler_app = {.name = "Task Scheduler",
                                             .description = "Commands that run by themselves at set times or events",
                                             .icon = ICON_CLOCK,
                                             .launch = launch};
GUI_APP(scheduler_app);
