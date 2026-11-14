/* The assistant on the desktop: no chat window but a drop that comes out of the menu bar (the orb, below). The
   model is read in pieces and the answer is worked out a step at a time from the module's tick, so the desktop keeps
   moving while it thinks. */

#define GAP 14
#define BUBBLE_PAD 10

static struct chat {
    struct window *win;
    struct ui_state st;
    struct ui_text input;
    int scroll;
    bool stick; /* the small chat follows the end of the conversation */
    char context_title[96];
    int w, h;                       /* the drop's size now (it moves towards orb_target) */
    uint64_t opened, leaving;       /* coming out of the menu bar, going back */
    uint64_t done_at;               /* when the last answer was finished: it stays a while */
    bool typing, want_focus;        /* the field is there; give it the keyboard */
    bool asked, leave, hover;
    bool probing;                   /* smart input: listening for any sound at all */
    uint64_t probe_at;
    int probe_peak;
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
static void orb_hide(void);

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

static bool talk_wanted; /* the answer is said aloud */
static int talk_round;

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
    talk_stop(); /* a new question: whatever was being said is over */
    talk_wanted = gui_ai.speak && (voice_sending || gui_ai.answer == AI_OUT_VOICE);
    talk_round = -1;
    if (!gui_shell)
        gui_shell = shell_create();
    gui_agent.sh = gui_shell;
    gui_agent.smp = (struct sampler){0.7f, 0.9f, 1.1f, hal_ticks_ms() | 1};
    agent_ask(&gui_agent, text);
    if (chat) {
        chat->stick = chat->asked = true;
        chat->done_at = 0;
    }
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
    talk_stop();
    talk_wanted = false;
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

/* ---- the orb: the assistant without a chat window ----
   A glass drop slides out from under the menu bar (the window's own glass, round). In it the "core": a bead of the
   SynthOS colours that swirls slowly when idle, faster while it thinks, and breathes with the voice when listening or
   speaking — SynthOS's own drawing. When there is something to show, the drop stretches into a pill: the field to
   type in, the words as they are heard, the answer as subtitles or a small chat that goes away by itself. */

#define ORB 52
#define PILL_W 460
#define SUB_LINES 3
#define CHAT_MAX_H 300
#define SUB_LINGER_MS 9000
#define CHAT_LINGER_MS 30000
#define ORB_DROP_MS 360

enum { ORB_IDLE, ORB_LISTEN, ORB_THINK, ORB_SPEAK };

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

/* the small chat: this conversation in bubbles */
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
    }
    kfree(vis);
    return y;
}

static void approval_card(struct ui *ui, struct rect r)
{
    if (ui->c) {
        gfx_symbol(ui->c, r.x + 4, r.y + 8, SYM_HAND, 18, style.warning);
        ui_label(ui, (struct rect){r.x + 32, r.y + 6, r.w - 40, 20}, "Allow the assistant to do this?", &font_bold,
                 style.text, ALIGN_LEFT);
        ui_wrap_text(ui->c, (struct rect){r.x + 32, r.y + 28, r.w - 40, 34}, &font_small, gui_agent.call_label,
                     style.secondary);
    }
    if (ui_button_primary(ui, (struct rect){r.x + r.w - 96, r.y + r.h - 34, 90, 28}, "Allow"))
        agent_finish_tool(&gui_agent, true);
    if (ui_button(ui, (struct rect){r.x + r.w - 228, r.y + r.h - 34, 124, 28}, "Don't Allow"))
        agent_finish_tool(&gui_agent, false);
}

static bool want_download;
static char download_error[200];

/* without a model: loading it, or getting it */
static void model_card(struct ui *ui, struct rect r)
{
    if (!reading.active && !reading.error[0] && !want_download)
        read_start(); /* when there is a model, it is read now */
    if (reading.active) {
        char line[96];
        ksnprintf(line, sizeof(line), "Loading the model… %u%%",
                  (unsigned)((uint64_t)reading.got * 100 / MAX(reading.size, (size_t)1)));
        ui_label(ui, (struct rect){r.x, r.y, r.w, 20}, line, &font_sans, style.secondary, ALIGN_LEFT);
        if (ui->c) {
            struct rect bar = {r.x, r.y + 28, r.w, 5};
            gfx_round(ui->c, bar, 2, style.control);
            bar.w = (int)((uint64_t)bar.w * reading.got / MAX(reading.size, (size_t)1));
            gfx_round(ui->c, bar, 2, style.accent);
        }
        return;
    }
    char about[240];
    const struct download *d = chat_choice();
    ksnprintf(about, sizeof(about), "The assistant needs its language model: %s, %u MB, downloaded once. Then it works "
                                    "offline.",
              d == &big_model ? "MiniCPM5 2B" : "Qwen2.5 0.5B Instruct", d->bytes >> 20);
    bool bad = reading.error[0] || download_error[0];
    if (ui->c)
        ui_wrap_text(ui->c, (struct rect){r.x, r.y, r.w, 40}, &font_small,
                     reading.error[0] ? reading.error : download_error[0] ? download_error : about,
                     bad ? style.error : style.secondary);
    if (ui_button_primary(ui, (struct rect){r.x + r.w - 150, r.y + 44, 150, 28},
                          reading.error[0] ? "Try Again" : "Download")) {
        if (reading.error[0]) {
            reading.error[0] = 0;
            read_start();
        } else {
            want_download = true;
        }
    }
}

/* ---- the core of the drop ---- */

static float orb_atan2(float y, float x) /* turns, 0..1 */
{
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a = MIN(ax, ay) / (MAX(ax, ay) + 1e-6f), s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a; /* atan on [0, 1] */
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0)
        r = 3.14159274f - r;
    if (y < 0)
        r = -r;
    return r * 0.159154943f + 0.5f;
}

/* a pixel over what is there; on clear pixels it becomes the colour with that coverage (the glass shows around) */
static void orb_put(struct canvas *c, int x, int y, uint32_t col, int a)
{
    if (a <= 0 || x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h)
        return;
    uint32_t *p = c->px + y * c->stride + x;
    a = MIN(a, 255);
    if ((*p >> 24) == 0)
        *p = (col & 0xffffffu) | (uint32_t)a << 24;
    else
        *p = (color_mix(*p, col, a) & 0xffffffu) | (uint32_t)MAX((int)(*p >> 24), a) << 24;
}

static void orb_core(struct canvas *c, int ox, int oy, int size, float t, float level, int mode)
{
    float R = size * 0.5f, cx = ox + R, cy = oy + R;
    float core = R * (0.56f + 0.12f * level + (mode == ORB_THINK ? 0.03f * tts_sinf(t * 5.0f) : 0.0f));
    float spin = mode == ORB_THINK ? 0.9f : mode == ORB_LISTEN || mode == ORB_SPEAK ? 0.45f : 0.12f;
    /* the ring that answers the voice */
    if (level > 0.02f) {
        float rr = core + 2.0f + level * 5.0f;
        for (int y = (int)(cy - rr - 2); y <= (int)(cy + rr + 2); y++)
            for (int x = (int)(cx - rr - 2); x <= (int)(cx + rr + 2); x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = ai_sqrtf(dx * dx + dy * dy);
                float k = 1.5f - (d > rr ? d - rr : rr - d);
                if (k > 0)
                    orb_put(c, x, y, gfx_halo_tone((int)((orb_atan2(dy, dx) + t * 0.3f) * 1024)),
                            (int)(k * 90.0f * MIN(level * 2.0f, 1.0f)));
            }
    }
    for (int y = (int)(cy - core - 1); y <= (int)(cy + core + 1); y++)
        for (int x = (int)(cx - core - 1); x <= (int)(cx + core + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = ai_sqrtf(dx * dx + dy * dy);
            float edge = core + 0.5f - d;
            if (edge <= 0)
                continue;
            float k = d / core;
            /* the colours turn around the centre, bent more towards the rim: a slow whirl */
            float u = orb_atan2(dy, dx) + t * spin + 0.22f * k * k + 0.05f * tts_sinf(k * 6.0f - t * 2.2f);
            uint32_t col = gfx_halo_tone((int)(u * 1024.0f));
            col = color_mix(col, 0xffffffffu, (int)((1.0f - k * k) * 120.0f)); /* lit from inside */
            if (dy < 0 && dx < 0) { /* the bright spot up and to the left, like on a bead */
                float hx = (dx + core * 0.34f) / (core * 0.42f), hy = (dy + core * 0.42f) / (core * 0.24f);
                float h = 1.0f - (hx * hx + hy * hy);
                if (h > 0)
                    col = color_mix(col, 0xffffffffu, (int)(h * 200.0f));
            }
            float a = MIN(edge, 1.0f) * (0.78f + 0.22f * (1.0f - k));
            orb_put(c, x, y, col, (int)(a * 255.0f));
        }
}

/* ---- the pill ---- */

static int orb_mode(void)
{
    if (voice.state == V_RECORD)
        return ORB_LISTEN;
    if (talk.active)
        return ORB_SPEAK;
    if (busy() || voice.state == V_HEAR || reading.active)
        return ORB_THINK;
    return ORB_IDLE;
}

/* what the answer area shows: the reply being written, else the last one */
static const char *orb_answer(char *vis, size_t cap)
{
    const char *src = gui_agent.state == AG_PREFILL || gui_agent.state == AG_DECODE ? gui_agent.reply
                      : gui_agent.count && gui_agent.msgs[gui_agent.count - 1].role == R_ASSISTANT
                          ? gui_agent.msgs[gui_agent.count - 1].text
                          : "";
    visible_reply(src, vis, cap);
    return vis;
}

static bool orb_lingering(void)
{
    uint64_t linger = gui_ai.answer == AI_OUT_CHAT ? CHAT_LINGER_MS : SUB_LINGER_MS;
    return chat->done_at && hal_ticks_ms() - chat->done_at < linger;
}

/* the height under the drop's row; 0 when there is nothing */
static int orb_below(int W)
{
    struct ui m = {.win = chat->win};
    if (!loaded)
        return 84; /* the model card: get it, or how far it has loaded */
    if (gui_agent.state == AG_APPROVE)
        return 104;
    if (gui_ai.answer == AI_OUT_VOICE || (!busy() && !orb_lingering()))
        return 0;
    if (gui_ai.answer == AI_OUT_CHAT) {
        int h = messages(&m, 0, W) + 8;
        return h > 8 ? MIN(h, CHAT_MAX_H) : 0;
    }
    char *vis = kmalloc(MAX_REPLY);
    int h = 0;
    if (vis && *orb_answer(vis, MAX_REPLY))
        h = MIN(ui_wrap_text(NULL, (struct rect){0, 0, W - 2 * GAP, 10000}, &font_sans, vis, 0),
                SUB_LINES * (font_sans.height + 3)) + 14;
    kfree(vis);
    return h;
}

static bool orb_wide(void)
{
    return chat->typing || voice.state == V_RECORD || voice.state == V_HEAR || busy() || voice.note[0] ||
           chat->context_title[0] || !loaded;
}

static void orb_target(int *w, int *h)
{
    bool wide = orb_wide();
    *w = wide ? PILL_W : ORB;
    *h = ORB + (wide ? orb_below(PILL_W) : 0) + (chat->context_title[0] && wide ? 28 : 0);
}

static void orb_send_typed(void)
{
    char text[sizeof(chat->input.buf)];
    str_copy(text, chat->input.buf, sizeof(text));
    ui_text_set(&chat->input, "");
    gui_send(text);
}

static void frame(struct ui *ui, void *data)
{
    UNUSED(data);
    int W = ui->win->surface.w, H = ui->win->surface.h;
    float t = (float)(hal_ticks_ms() % 3600000u) / 1000.0f;
    if (ui->c) { /* clear: the glass is the background; a little tint where there is text */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                ui->c->px[y * ui->c->stride + x] = W > ORB ? (style.dark ? 0x30000000u : 0x40ffffffu) : 0;
        int mode = orb_mode();
        float level = mode == ORB_LISTEN ? voice.level / 100.0f : mode == ORB_SPEAK ? 0.25f + 0.2f * tts_sinf(t * 7.0f)
                                                                                      : 0.0f;
        orb_core(ui->c, 0, 0, ORB, t, CLAMP(level, 0.0f, 1.0f), mode);
    }
    struct rect drop = {0, 0, ORB, ORB};
    int key = ui_key(ui);
    if (key == KEY_ESC) {
        ui->consumed = true;
        if (busy())
            stop_answer();
        else if (voice.state == V_RECORD)
            voice_button();
        else
            orb_hide();
        return;
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(drop, ui->e->x, ui->e->y)) {
        ui->consumed = true; /* the drop: talk, stop talking, or stop the answer */
        if (busy())
            stop_answer();
        else if (talk.active)
            talk_stop();
        else
            voice_button();
    }
    if (W <= ORB)
        return;
    int x0 = ORB + 4, rw = W - x0 - 46;
    struct rect row = {x0, (ORB - 34) / 2, rw, 34};
    if (chat->typing && !busy() && voice.state != V_RECORD && voice.state != V_HEAR && loaded) {
        int fid = ui->id + 1;
        if (chat->want_focus) {
            ui->st->focus = fid;
            chat->want_focus = false;
        }
        if (ui_search(ui, row, &chat->input, "Ask anything") && chat->input.len)
            orb_send_typed();
    } else {
        char line[240];
        if (voice.state == V_RECORD && voice.partial[0])
            ksnprintf(line, sizeof(line), "%s…", voice.partial);
        else if (voice.state == V_RECORD)
            str_copy(line, "Listening…", sizeof(line));
        else if (voice.state == V_HEAR)
            str_copy(line, "Writing down what you said…", sizeof(line));
        else if (busy() && gui_agent.status[0])
            str_copy(line, gui_agent.status, sizeof(line));
        else if (busy())
            str_copy(line, "Thinking…", sizeof(line));
        else if (talk.active)
            str_copy(line, gui_ai.barge ? "Speaking. Talk over me to interrupt" : "Speaking", sizeof(line));
        else if (voice.note[0])
            str_copy(line, voice.note, sizeof(line));
        else if (!loaded)
            str_copy(line, "Assistant", sizeof(line));
        else
            str_copy(line, "Ask me anything", sizeof(line));
        ui_label(ui, row, line, &font_sans, voice.state == V_RECORD ? style.text : style.secondary, ALIGN_LEFT);
    }
    /* the microphone, or the keyboard when listening */
    bool listening = voice.state == V_RECORD;
    if (ui_symbol_button(ui, (struct rect){W - 42, (ORB - 30) / 2, 30, 30}, listening ? SYM_KEYBOARD : SYM_MIC,
                         listening)) {
        if (listening) {
            voice_button();
            chat->typing = chat->want_focus = true;
        } else {
            voice_button();
        }
    }
    int y = ORB;
    if (chat->context_title[0]) {
        struct rect r = {GAP, y, W - 2 * GAP, 24};
        if (ui->c)
            gfx_symbol(ui->c, r.x, r.y + 4, SYM_GLOBE, 14, style.secondary);
        ui_label(ui, (struct rect){r.x + 20, r.y + 2, r.w - 50, 18}, chat->context_title, &font_small, style.secondary,
                 ALIGN_LEFT);
        if (ui_symbol_button(ui, (struct rect){r.x + r.w - 24, r.y, 22, 22}, SYM_CLOSE, false)) {
            kfree(gui_agent.context);
            gui_agent.context = NULL;
            chat->context_title[0] = 0;
        }
        y += 28;
    }
    struct rect below = {GAP, y, W - 2 * GAP, H - y - 6};
    if (below.h < 20)
        return;
    if (!loaded) {
        model_card(ui, below);
    } else if (gui_agent.state == AG_APPROVE) {
        approval_card(ui, below);
    } else if (gui_ai.answer == AI_OUT_CHAT) {
        struct canvas clip, *saved = ui->c;
        if (ui->c) {
            clip = *ui->c;
            clip.clip = rect_intersect(ui->c->clip, (struct rect){0, y, W, below.h});
            ui->c = &clip;
        }
        int end = messages(ui, y - chat->scroll, W);
        ui->c = saved;
        int most = MAX(0, end + chat->scroll - y - below.h);
        if (chat->stick)
            chat->scroll = most;
        if (ui->e && ui->e->type == EV_WHEEL && ui->e->y >= y) {
            chat->scroll = CLAMP(chat->scroll - ui->e->wheel * 40, 0, most);
            chat->stick = chat->scroll >= most;
            ui->consumed = true;
        }
    } else if (gui_ai.answer == AI_OUT_SUBTITLES && ui->c) {
        char *vis = kmalloc(MAX_REPLY);
        if (vis && *orb_answer(vis, MAX_REPLY)) { /* the last lines, as they are written */
            int full = ui_wrap_text(NULL, (struct rect){0, 0, below.w, 10000}, &font_sans, vis, 0);
            struct canvas clip = *ui->c;
            clip.clip = rect_intersect(ui->c->clip, below);
            ui_wrap_text(&clip, (struct rect){below.x, below.y + MIN(0, below.h - full - 4), below.w, full},
                         &font_sans, vis, style.text);
        }
        kfree(vis);
    }
}

static void paint(struct window *w, struct canvas *c)
{
    ui_run(w, &chat->st, c, NULL, frame, NULL);
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_BLUR && !busy() && !talk.active && voice.state != V_RECORD && voice.state != V_HEAR &&
        !chat->input.len && gui_agent.state != AG_APPROVE && !reading.active && !want_download)
        chat->leave = true; /* clicked elsewhere: the drop goes back up */
    if (e->type == EV_MOUSE_MOVE || e->type == EV_LEAVE)
        chat->hover = e->type == EV_MOUSE_MOVE; /* read while the pointer is on it: it stays */
    ui_run(w, &chat->st, NULL, e, frame, NULL);
}

static void destroy(struct window *w)
{
    UNUSED(w);
    kfree(chat);
    chat = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};
static uint64_t orb_gone_at;

static int orb_x(int w)
{
    int sw, sh;
    wm_screen_size(&sw, &sh);
    return (sw - w) / 2;
}

/* the drop comes out of the menu bar; ask: start asking the way Settings → AI says */
static void orb_show(bool ask)
{
    if (!gui_ai.on)
        return;
    if (!chat) {
        chat = kzalloc(sizeof(*chat));
        if (!chat)
            return;
        chat->stick = true;
        chat->win = wm_open("Assistant", ICON_ASSISTANT, ORB, ORB, &ops, NULL, WIN_OVERLAY | WIN_GLASS | WIN_NOTITLE);
        if (!chat->win) {
            kfree(chat);
            chat = NULL;
            return;
        }
        chat->win->radius = ORB / 2;
        chat->w = chat->h = ORB;
        chat->opened = hal_ticks_ms();
        wm_move(chat->win, orb_x(ORB), MENUBAR_HEIGHT - ORB);
        if (orb_gone_at && chat->opened - orb_gone_at > 5 * 60000)
            new_chat(); /* a while later: a new conversation */
        read_start();
    }
    chat->leave = false;
    chat->done_at = 0;
    wm_show(chat->win);
    wm_focus(chat->win);
    if (!ask)
        return;
    int in = gui_ai.input;
    if (in == AI_IN_SMART) { /* a microphone that hears anything at all: voice; silence or none: typing */
        chat->probe_at = hal_ticks_ms();
        chat->probe_peak = 0;
        if (voice.state == V_OFF && mic_open(VOICE_MAX_MS)) {
            chat->probing = true;
            return;
        }
        in = voice.state == V_WAKE ? AI_IN_VOICE : AI_IN_TEXT;
        if (in == AI_IN_TEXT)
            voice.note[0] = 0; /* no microphone is no news when typing */
    }
    if (in == AI_IN_TEXT || in == AI_IN_BOTH)
        chat->typing = chat->want_focus = true;
    if ((in == AI_IN_VOICE || in == AI_IN_BOTH) && voice.state != V_RECORD)
        voice_button();
}

static void orb_hide(void)
{
    if (!chat || chat->leaving)
        return;
    if (voice.state == V_RECORD)
        voice_button();
    chat->leaving = orb_gone_at = hal_ticks_ms();
}

/* every tick while the drop is out: the probe, the size, coming and going */
static void orb_tick(void)
{
    if (!chat || !wm_window_alive(chat->win))
        return;
    uint64_t now = hal_ticks_ms();
    if (chat->probing) { /* smart: listen a quarter of a second for anything */
        int got = hal_audio_record_read(voice.frames, (int)MIN(voice.cap, 4096u));
        for (int i = 0; i < got * 2; i++)
            chat->probe_peak = MAX(chat->probe_peak, voice.frames[i] < 0 ? -voice.frames[i] : voice.frames[i]);
        if (now - chat->probe_at >= 250) {
            chat->probing = false;
            mic_close();
            if (chat->probe_peak > 0)
                voice_button();
            else
                chat->typing = chat->want_focus = true;
        }
    }
    bool working = busy() || talk.active || voice.state == V_RECORD || voice.state == V_HEAR || reading.active;
    if (working)
        chat->done_at = 0;
    else if (!chat->done_at && chat->asked)
        chat->done_at = now;
    if (!working && !chat->leaving && !chat->typing && !want_download && gui_agent.state != AG_APPROVE && loaded &&
        chat->asked && !orb_lingering() && !chat->hover)
        orb_hide(); /* said and shown: back into the menu bar */
    if (chat->leave && !chat->leaving)
        orb_hide();
    int tw, th;
    orb_target(&tw, &th);
    int nw = chat->w + (tw - chat->w) * 2 / 5, nh = chat->h + (th - chat->h) * 2 / 5;
    if (ABS(tw - nw) < 3)
        nw = tw;
    if (ABS(th - nh) < 3)
        nh = th;
    int top = MENUBAR_HEIGHT + 8, y = top;
    int k = anim_progress(chat->opened, ORB_DROP_MS);
    if (k < 1000)
        y = lerp_i(MENUBAR_HEIGHT - nh, top, ease_spring(k));
    if (chat->leaving) {
        k = anim_progress(chat->leaving, ORB_DROP_MS * 2 / 3);
        y = lerp_i(top, MENUBAR_HEIGHT - nh - 4, ease_in_out(k));
        if (k >= 1000) {
            wm_close(chat->win);
            return;
        }
    }
    if (nw != chat->w || nh != chat->h) {
        chat->w = nw;
        chat->h = nh;
        wm_resize_client(chat->win, nw, nh);
        chat->win->radius = ORB / 2;
    }
    wm_move(chat->win, orb_x(chat->w), y);
    wm_invalidate(chat->win);
}

static void launch(const char *arg)
{
    UNUSED(arg);
    orb_show(true);
}

/* the sparkle in the menu bar and the shortcut: the drop comes, or goes */
static void toggle(void)
{
    if (chat && wm_window_alive(chat->win) && !chat->leaving) {
        orb_hide();
        return;
    }
    if (chat && chat->leaving)
        return;
    orb_show(true);
}

/* from another app: a conversation about what it shows (a page, a selection); question NULL only opens it */
static void ask_about(const char *title, const char *context, const char *question)
{
    orb_show(!(question && *question));
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
    if (question && *question) {
        gui_send(question);
    }
    chat_invalidate();
}

/* the answer so far to the voice, while it is being written */
static void talk_follow(void)
{
    if (!talk_wanted)
        return;
    bool writing = gui_agent.state == AG_PREFILL || gui_agent.state == AG_DECODE;
    const char *src = writing ? gui_agent.reply
                      : gui_agent.count && gui_agent.msgs[gui_agent.count - 1].role == R_ASSISTANT
                          ? gui_agent.msgs[gui_agent.count - 1].text
                          : "";
    char *vis = kmalloc(MAX_REPLY);
    if (!vis)
        return;
    visible_reply(src, vis, MAX_REPLY);
    bool fresh = gui_agent.rounds != talk_round;
    talk_round = gui_agent.rounds;
    if (*vis || !writing)
        talk_offer(vis, !writing, fresh);
    kfree(vis);
    if (!writing)
        talk_wanted = false;
}

/* the desktop calls this while the module is installed */
static void tick(void)
{
    gui_complete_ask = complete_ask; /* Tab completion in every text field */
    complete_tick();
    voice_tick();
    talk_follow();
    talk_tick();
    orb_tick();
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
