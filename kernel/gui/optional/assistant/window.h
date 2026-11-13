/* The Assistant window: the conversation in bubbles, a box to write in, the card that asks before a tool changes
   something, and, without a model, the card that downloads one. The model is read in pieces and the answer is worked
   out a step at a time from the module's tick, so the desktop keeps moving while it thinks. */

#define WIN_W 460
#define WIN_H 640
#define GAP 14
#define BUBBLE_PAD 10
#define INPUT_H 40

static struct chat {
    struct window *win;
    struct ui_state st;
    struct ui_text input;
    int scroll, content_h;
    bool stick; /* follows the end of the conversation */
    char context_title[96];
} *chat;

static struct agent gui_agent;
static const struct gui_app assistant_app;
static struct shell *gui_shell;

/* reading a model in pieces from the tick */
static struct {
    struct file *f;
    uint8_t *buf;
    size_t got, size;
    char path[160], error[200];
    bool active;
    char *question; /* asked before the model was ready */
} reading;

static const char *const suggestions[] = {
    "What is in my Documents folder?",
    "Turn on Do Not Disturb",
    "Write a note with today's date",
    "How much disk space is free?",
};

static void chat_invalidate(void)
{
    if (chat && chat->win && wm_window_alive(chat->win))
        wm_invalidate(chat->win);
}

static void read_stop(void)
{
    if (reading.f)
        vfs_close(reading.f);
    reading.f = NULL;
    big_free(reading.buf);
    reading.buf = NULL;
    reading.active = false;
}

/* starts reading the default model; false when there is none */
static bool read_start(void)
{
    if (loaded || reading.active)
        return true;
    reading.error[0] = 0;
    if (!default_model(reading.path, sizeof(reading.path)))
        return false;
    struct vstat st;
    int err = vfs_stat(reading.path, &st);
    if (!err && !(reading.buf = big_alloc(st.size)))
        err = E_NOMEM;
    if (!err)
        err = vfs_open(reading.path, VFS_READ, &reading.f);
    if (err) {
        ksnprintf(reading.error, sizeof(reading.error), err == E_NOMEM ? "Not enough memory for the model" : "%s: %s",
                  reading.path, err_str(err));
        read_stop();
        return true;
    }
    reading.size = st.size;
    reading.got = 0;
    reading.active = true;
    return true;
}

static void gui_send(const char *text);

static void read_step(void)
{
    uint64_t start = hal_ticks_ms();
    while (reading.got < reading.size && hal_ticks_ms() - start < 40) {
        long n = vfs_read(reading.f, reading.buf + reading.got, MIN((size_t)(4u << 20), reading.size - reading.got));
        if (n <= 0) {
            ksnprintf(reading.error, sizeof(reading.error), "%s: %s", reading.path, err_str(n < 0 ? (int)n : E_IO));
            read_stop();
            return;
        }
        reading.got += (size_t)n;
    }
    if (reading.got < reading.size)
        return;
    vfs_close(reading.f);
    reading.f = NULL;
    if (model_load_fit(&model, reading.buf, reading.size, CTX_DEFAULT, reading.error, sizeof(reading.error))) {
        big_free(reading.buf);
    } else {
        str_copy(model.path, reading.path, sizeof(model.path));
        loaded = true;
    }
    reading.buf = NULL;
    reading.active = false;
    if (loaded && reading.question) {
        char *q = reading.question;
        reading.question = NULL;
        gui_send(q);
        kfree(q);
    }
}

static bool busy(void)
{
    return gui_agent.state != AG_IDLE;
}

static void gui_send(const char *text)
{
    if (!*text || (busy() && gui_agent.state != AG_APPROVE))
        return;
    if (!loaded) {
        kfree(reading.question);
        reading.question = kmalloc(strlen(text) + 1);
        if (reading.question)
            memcpy(reading.question, text, strlen(text) + 1);
        read_start();
        chat_invalidate();
        return;
    }
    if (!gui_shell)
        gui_shell = shell_create();
    gui_agent.sh = gui_shell;
    gui_agent.smp = (struct sampler){0.7f, 0.9f, 1.1f, hal_ticks_ms() | 1};
    agent_ask(&gui_agent, text);
    if (chat)
        chat->stick = true;
    chat_invalidate();
}

static void new_chat(void)
{
    if (gui_agent.state == AG_DECODE || gui_agent.state == AG_PREFILL)
        return;
    agent_clear(&gui_agent);
    if (chat) {
        chat->context_title[0] = 0;
        chat->scroll = 0;
    }
    chat_invalidate();
}

/* stops the answer where it is */
static void stop_answer(void)
{
    if (gui_agent.state == AG_PREFILL || gui_agent.state == AG_DECODE) {
        if (gui_agent.reply_len)
            agent_add(&gui_agent, R_ASSISTANT, gui_agent.reply);
        gui_agent.reply_len = 0;
        gui_agent.reply[0] = 0;
        gui_agent.state = AG_IDLE;
        gui_agent.status[0] = 0;
        model.n_past = MIN(model.n_past, gui_agent.fed);
    } else if (gui_agent.state == AG_APPROVE) {
        agent_finish_tool(&gui_agent, false);
    }
    chat_invalidate();
}

/* ---- drawing ---- */

static int bubble(struct ui *ui, int y, int W, const char *text, bool mine)
{
    const struct ui_font *f = &font_sans;
    int max = W * 3 / 4 - 2 * BUBBLE_PAD;
    int tw = gfx_text_width(f, text) + 2;
    bool one_line = !strchr(text, '\n') && tw <= max;
    int w = one_line ? tw : max;
    int h = ui_wrap_text(NULL, (struct rect){0, 0, w, 10000}, f, text, 0);
    struct rect r = {mine ? W - GAP - w - 2 * BUBBLE_PAD : GAP, y, w + 2 * BUBBLE_PAD, h + 2 * BUBBLE_PAD - 4};
    if (ui->c && r.y + r.h > 0 && r.y < ui->win->surface.h) {
        gfx_round(ui->c, r, 14, mine ? style.accent : style.control);
        ui_wrap_text(ui->c, (struct rect){r.x + BUBBLE_PAD, r.y + BUBBLE_PAD - 2, w, h}, f, text,
                     mine ? style.accent_text : style.text);
    }
    return r.h + 8;
}

static int note_line(struct ui *ui, int y, int W, enum symbol_id sym, const char *text, uint32_t color)
{
    if (ui->c && y + 20 > 0 && y < ui->win->surface.h) {
        gfx_symbol(ui->c, GAP + 4, y + 2, sym, 14, color);
        ui_label(ui, (struct rect){GAP + 24, y, W - 2 * GAP - 24, 18}, text, &font_small, color, ALIGN_LEFT);
    }
    return 24;
}

static int messages(struct ui *ui, int y, int W)
{
    char *vis = kmalloc(MAX_REPLY);
    if (!vis)
        return y;
    for (int i = 0; i < gui_agent.count; i++) {
        const struct msg *m = &gui_agent.msgs[i];
        if (m->role == R_USER) {
            y += bubble(ui, y, W, m->text, true);
        } else if (m->role == R_ASSISTANT) {
            visible_reply(m->text, vis, MAX_REPLY);
            if (*vis)
                y += bubble(ui, y, W, vis, false);
            char name[32], args[512], label[160];
            if (find_call(m->text, name, sizeof(name), args, sizeof(args))) {
                tool_label(name, args, label, sizeof(label));
                bool refused = i + 1 < gui_agent.count && gui_agent.msgs[i + 1].role == R_TOOL &&
                               !strcmp(gui_agent.msgs[i + 1].text, "The user did not allow this.");
                bool pending = i == gui_agent.count - 1;
                y += note_line(ui, y, W, refused ? SYM_CLOSE : pending ? SYM_MORE : SYM_CHECK, label,
                               refused ? style.error : style.secondary);
            }
        }
    }
    if (gui_agent.state == AG_PREFILL || gui_agent.state == AG_DECODE) {
        visible_reply(gui_agent.reply, vis, MAX_REPLY);
        if (*vis)
            y += bubble(ui, y, W, vis, false);
        char st[200];
        int pct = gui_agent.state == AG_PREFILL && gui_agent.ntok > gui_agent.fed
                      ? 100 * gui_agent.fed / MAX(1, gui_agent.ntok) : -1;
        if (pct >= 0)
            ksnprintf(st, sizeof(st), "%s %d%%", gui_agent.status, pct);
        else
            str_copy(st, gui_agent.status, sizeof(st));
        y += note_line(ui, y, W, SYM_SPARKLE, st, style.tertiary);
    }
    kfree(vis);
    return y;
}

static void empty_state(struct ui *ui, struct rect area)
{
    int cy = area.y + area.h / 2 - 150;
    if (ui->c) {
        gfx_icon(ui->c, area.x + area.w / 2 - 32, cy, ICON_ASSISTANT, 64);
        ui_label(ui, (struct rect){area.x, cy + 76, area.w, 28}, "How can I help?", &font_title, style.text,
                 ALIGN_CENTER);
        ui_label(ui, (struct rect){area.x, cy + 106, area.w, 20}, "Runs on this computer. Nothing leaves it.",
                 &font_small, style.secondary, ALIGN_CENTER);
    }
    int y = cy + 144;
    for (size_t i = 0; i < ARRAY_SIZE(suggestions); i++, y += 40) {
        struct rect r = {area.x + 40, y, area.w - 80, 32};
        if (ui_button(ui, r, suggestions[i]))
            gui_send(suggestions[i]);
    }
    bool on = wake_enabled();
    ui_label(ui, (struct rect){area.x + 40, y + 12, area.w - 130, 20}, "Listen for “Hey Synth”", &font_sans,
             style.secondary, ALIGN_LEFT);
    if (ui_switch(ui, (struct rect){area.x + area.w - 84, y + 12, 40, 22}, &on))
        voice_wake_set(on);
}

static void approval_card(struct ui *ui, struct rect r)
{
    if (ui->c) {
        gfx_round(ui->c, r, 14, style.group);
        gfx_symbol(ui->c, r.x + 14, r.y + 14, SYM_HAND, 18, style.warning);
        ui_label(ui, (struct rect){r.x + 42, r.y + 12, r.w - 56, 20}, "Allow the assistant to do this?", &font_bold,
                 style.text, ALIGN_LEFT);
        ui_wrap_text(ui->c, (struct rect){r.x + 42, r.y + 36, r.w - 56, 40}, &font_small, gui_agent.call_label,
                     style.secondary);
    }
    if (ui_button_primary(ui, (struct rect){r.x + r.w - 104, r.y + r.h - 40, 90, 28}, "Allow")) {
        agent_finish_tool(&gui_agent, true);
        chat->stick = true;
    }
    if (ui_button(ui, (struct rect){r.x + r.w - 236, r.y + r.h - 40, 124, 28}, "Don't Allow")) {
        agent_finish_tool(&gui_agent, false);
        chat->stick = true;
    }
}

static bool want_download;
static char download_error[200];

/* without a model: reading it, or getting it; returns true while the card takes the place of the conversation */
static bool model_card(struct ui *ui, struct rect area)
{
    if (!reading.active && !reading.error[0] && !want_download)
        read_start(); /* when there is a model, it is read now */
    int cx = area.x + area.w / 2, y = area.y + area.h / 2 - 140;
    struct rect text = {area.x + 36, y + 110, area.w - 72, 80};
    if (ui->c) {
        gfx_icon(ui->c, cx - 32, y, ICON_ASSISTANT, 64);
        ui_label(ui, (struct rect){area.x, y + 76, area.w, 28}, "Assistant", &font_title, style.text, ALIGN_CENTER);
    }
    if (reading.active) {
        char line[96];
        ksnprintf(line, sizeof(line), "Loading the model… %u%%",
                  (unsigned)((uint64_t)reading.got * 100 / MAX(reading.size, (size_t)1)));
        ui_label(ui, text, line, &font_sans, style.secondary, ALIGN_CENTER);
        if (ui->c) {
            struct rect bar = {area.x + 60, y + 150, area.w - 120, 6};
            gfx_round(ui->c, bar, 3, style.control);
            bar.w = (int)((uint64_t)bar.w * reading.got / MAX(reading.size, (size_t)1));
            gfx_round(ui->c, bar, 3, style.accent);
        }
        return true;
    }
    char about[240];
    const struct download *d = chat_choice();
    ksnprintf(about, sizeof(about),
              "The assistant needs a language model on this computer: %s, %u MB, downloaded once. After that it "
              "works offline and nothing you write leaves the computer.",
              d == &big_model ? "MiniCPM5 2B" : "Qwen2.5 0.5B Instruct", d->bytes >> 20);
    const char *msg = reading.error[0] ? reading.error : download_error[0] ? download_error : about;
    if (ui->c)
        ui_wrap_text(ui->c, text, &font_sans, msg, reading.error[0] || download_error[0] ? style.error : style.secondary);
    if (ui_button_primary(ui, (struct rect){cx - 90, y + 200, 180, 32},
                          reading.error[0] ? "Try Again" : "Download the Model")) {
        if (reading.error[0]) {
            reading.error[0] = 0;
            read_start();
        } else {
            want_download = true;
        }
    }
    if (ui->c)
        ui_label(ui, (struct rect){area.x, y + 242, area.w, 18}, "Or put any .gguf model in " MODELS_DIR, &font_small,
                 style.tertiary, ALIGN_CENTER);
    return true;
}

static void frame(struct ui *ui, void *data)
{
    UNUSED(data);
    int W = ui->win->surface.w, H = ui->win->surface.h;
    ui_panel(ui, (struct rect){0, 0, W, H}, style.content);
    ui_label(ui, (struct rect){0, 0, W, TOOLBAR_HEIGHT}, "Assistant", &font_bold, style.text, ALIGN_CENTER);
    if (ui_symbol_button(ui, (struct rect){W - 44, 12, 30, 28}, SYM_PENCIL, false))
        new_chat();
    int key = ui_key(ui);
    if (key == KEY_CTRL('n') || key == KEY_COMMAND('n')) {
        new_chat();
        ui->consumed = true;
    }
    if (key == KEY_ESC && busy()) {
        stop_answer();
        ui->consumed = true;
    }
    struct rect area = {0, TOOLBAR_HEIGHT, W, H - TOOLBAR_HEIGHT - INPUT_H - 2 * GAP};
    if (!loaded && model_card(ui, area))
        return;
    bool approve = gui_agent.state == AG_APPROVE;
    if (approve)
        area.h -= 110;
    if (chat->context_title[0])
        area.h -= 30;
    if (!gui_agent.count && !busy()) {
        empty_state(ui, area);
    } else {
        struct canvas clip, *saved = ui->c;
        if (ui->c) {
            clip = *ui->c;
            clip.clip = rect_intersect(ui->c->clip, area);
            ui->c = &clip;
        }
        int end = messages(ui, area.y + GAP - chat->scroll, W);
        ui->c = saved;
        chat->content_h = end + chat->scroll - area.y;
        int most = MAX(0, chat->content_h - area.h);
        if (chat->stick)
            chat->scroll = most;
        if (ui->e && ui->e->type == EV_WHEEL && rect_contains(area, ui->e->x, ui->e->y)) {
            chat->scroll = CLAMP(chat->scroll - ui->e->wheel * 48, 0, most);
            chat->stick = chat->scroll >= most;
            ui->consumed = true;
        }
        if (ui->c && chat->stick && chat->scroll != most)
            wm_invalidate(ui->win);
        if (most)
            ui_scrollbar(ui, (struct rect){W - 8, area.y, 6, area.h}, &chat->scroll, chat->content_h, area.h);
    }
    int y = area.y + area.h;
    if (approve) {
        approval_card(ui, (struct rect){GAP, y + 4, W - 2 * GAP, 100});
        y += 110;
    }
    if (chat->context_title[0]) {
        struct rect r = {GAP, y + 4, W - 2 * GAP, 24};
        if (ui->c) {
            gfx_round(ui->c, r, 12, style.group);
            gfx_symbol(ui->c, r.x + 8, r.y + 5, SYM_GLOBE, 14, style.secondary);
        }
        ui_label(ui, (struct rect){r.x + 28, r.y + 3, r.w - 64, 18}, chat->context_title, &font_small, style.secondary,
                 ALIGN_LEFT);
        if (ui_symbol_button(ui, (struct rect){r.x + r.w - 26, r.y + 2, 22, 20}, SYM_CLOSE, false)) {
            kfree(gui_agent.context);
            gui_agent.context = NULL;
            chat->context_title[0] = 0;
        }
        y += 30;
    }
    if (voice.note[0] || voice.state == V_RECORD || voice.state == V_HEAR) {
        char line[200];
        if (voice.state == V_RECORD)
            ksnprintf(line, sizeof(line), "Listening… %s",
                      voice.first_loud ? "click the microphone when done" : "say something");
        else if (voice.state == V_HEAR)
            str_copy(line, "Writing down what you said…", sizeof(line));
        else
            str_copy(line, voice.note, sizeof(line));
        if (ui->c && voice.state == V_RECORD) { /* how loud: a bar under the words */
            struct rect bar = {GAP + 24, H - GAP - INPUT_H - 6, (W - 2 * GAP - 24) * voice.level / 100, 3};
            gfx_round(ui->c, bar, 1, style.accent);
        }
        note_line(ui, H - GAP - INPUT_H - 26, W, SYM_MIC, line, voice.state == V_OFF ? style.secondary : style.accent);
    }
    struct rect mic = {W - 2 * GAP - 2 * INPUT_H + 14, H - GAP - INPUT_H + 4, INPUT_H - 8, INPUT_H - 8};
    bool recording = voice.state == V_RECORD || voice.state == V_HEAR;
    if (ui->c) {
        if (recording)
            gfx_round(ui->c, mic, mic.w / 2, style.error);
        gfx_symbol(ui->c, mic.x + (mic.w - 16) / 2, mic.y + (mic.h - 16) / 2, SYM_MIC, 16,
                   recording ? style.accent_text : style.secondary);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(mic, ui->e->x, ui->e->y)) {
        ui->consumed = true;
        if (!busy())
            voice_button();
    }
    struct rect box = {GAP, H - GAP - INPUT_H, W - 4 * GAP - 2 * INPUT_H + 14, INPUT_H};
    if (ui_textbox_hint(ui, box, &chat->input, busy() ? "Wait for the answer, or press Esc" : "Ask anything") &&
        !busy()) {
        char text[sizeof(chat->input.buf)];
        str_copy(text, chat->input.buf, sizeof(text));
        ui_text_set(&chat->input, "");
        gui_send(text);
    }
    struct rect send = {W - GAP - INPUT_H + 4, H - GAP - INPUT_H + 4, INPUT_H - 8, INPUT_H - 8};
    bool can = busy() || chat->input.len > 0;
    if (ui->c) {
        gfx_round(ui->c, send, send.w / 2, can ? style.accent : style.control);
        gfx_symbol(ui->c, send.x + (send.w - 14) / 2, send.y + (send.h - 14) / 2, busy() ? SYM_CLOSE : SYM_UP, 14,
                   can ? style.accent_text : style.tertiary);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(send, ui->e->x, ui->e->y)) {
        ui->consumed = true;
        if (busy()) {
            stop_answer();
        } else if (chat->input.len) {
            char text[sizeof(chat->input.buf)];
            str_copy(text, chat->input.buf, sizeof(text));
            ui_text_set(&chat->input, "");
            gui_send(text);
        }
    }
}

static void paint(struct window *w, struct canvas *c)
{
    ui_run(w, &chat->st, c, NULL, frame, NULL);
}

static void event(struct window *w, struct gui_event *e)
{
    ui_run(w, &chat->st, NULL, e, frame, NULL);
}

static void destroy(struct window *w)
{
    UNUSED(w);
    kfree(chat);
    chat = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (chat && wm_window_alive(chat->win)) {
        wm_show(chat->win);
        return;
    }
    chat = kzalloc(sizeof(*chat));
    if (!chat)
        return;
    chat->stick = true;
    chat->win = wm_open("Assistant", ICON_ASSISTANT, WIN_W, WIN_H, &ops, NULL,
                        WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!chat->win) {
        kfree(chat);
        chat = NULL;
        return;
    }
    chat->win->toolbar_h = TOOLBAR_HEIGHT;
    chat->st.focus = 0;
    read_start(); /* reading the model starts as soon as the window is there */
}

/* the window comes and goes: the icon in the menu bar and the shortcut */
static void toggle(void)
{
    if (chat && wm_window_alive(chat->win) && wm_focused() == chat->win) {
        wm_close(chat->win);
        return;
    }
    gui_app_launch(&assistant_app, NULL);
}

/* from another app: a conversation about what it shows (a page, a selection); question NULL only opens it */
static void ask_about(const char *title, const char *context, const char *question)
{
    gui_app_launch(&assistant_app, NULL);
    if (!chat)
        return;
    if (context && !busy()) {
        agent_clear(&gui_agent);
        size_t n = MIN(strlen(context), (size_t)12000);
        gui_agent.context = kmalloc(n + 1);
        if (gui_agent.context) {
            memcpy(gui_agent.context, context, n);
            gui_agent.context[n] = 0;
        }
        str_copy(chat->context_title, title ? title : "", sizeof(chat->context_title));
    }
    if (question && *question)
        gui_send(question);
    chat_invalidate();
}

/* the desktop calls this while the module is installed */
static void tick(void)
{
    gui_complete_ask = complete_ask; /* Tab completion in every text field */
    complete_tick();
    voice_tick();
    if (want_speech_download) {
        want_speech_download = false;
        voice.note[0] = 0;
        progress_open("Assistant", "Downloading the speech model");
        int err = download_model(&speech_model, progress_update, NULL, voice.note, sizeof(voice.note));
        progress_close();
        if (!err)
            gui_notify("Assistant", "Speech is ready: click the microphone and talk");
        chat_invalidate();
        return;
    }
    if (want_download) {
        want_download = false;
        download_error[0] = 0;
        progress_open("Assistant", "Downloading the model");
        int err = download_model(chat_choice(), progress_update, NULL, download_error, sizeof(download_error));
        progress_close();
        if (!err) {
            read_start();
            gui_notify("Assistant", "The model is ready");
        }
        chat_invalidate();
        return;
    }
    if (reading.active) {
        read_step();
        chat_invalidate();
        return;
    }
    if (gui_agent.state != AG_PREFILL && gui_agent.state != AG_DECODE)
        return;
    uint64_t start = hal_ticks_ms();
    while ((gui_agent.state == AG_PREFILL || gui_agent.state == AG_DECODE) && hal_ticks_ms() - start < 60)
        agent_step(&gui_agent);
    chat_invalidate();
}
