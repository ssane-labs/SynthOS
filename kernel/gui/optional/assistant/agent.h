/* The conversation and its tools. Messages are laid out in the model's chat format (ChatML for Qwen, the header
   format of Llama 3); the system message describes the tools in the Hermes style Qwen was trained on, and the model
   asks for one by writing <tool_call>{"name": ..., "arguments": {...}}</tool_call>. Tools that change something
   (commands that are not read-only, writing files, settings) wait for the user's approval. The agent advances in
   small steps (one batch of the prompt or one new token) so the desktop stays responsive. */

#define MAX_MSGS 64
#define TOOL_OUT 3000
#define MAX_REPLY 6000
#define MAX_ROUNDS 6

enum { R_USER, R_ASSISTANT, R_TOOL };
enum { AG_IDLE, AG_PREFILL, AG_DECODE, AG_APPROVE };

struct msg {
    int role;
    char *text;
};

struct agent {
    int state;
    struct msg msgs[MAX_MSGS];
    int count;
    char *context;        /* what the user is looking at (a page, a selection), for this conversation */
    int *toks, ntok, fed; /* the prompt being fed */
    char reply[MAX_REPLY];
    int reply_len, generated, rounds;
    struct sampler smp;
    bool auto_approve, no_tools;
    char call_name[32], call_args[1024], call_label[160]; /* the tool call waiting for approval */
    char status[160];     /* what it is doing, for the window */
    uint64_t started;
    int tokens_out;
    struct shell *sh;
};

/* ---- a little JSON: enough to read a tool call ---- */

static const char *js_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* the end of the value at p, or NULL */
static const char *js_end(const char *p, int depth)
{
    p = js_ws(p);
    if (depth > 16)
        return NULL;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1])
                p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1);
        if (*p == close)
            return p + 1;
        for (;;) {
            if (close == '}') {
                p = js_end(p, depth + 1); /* the key */
                if (!p || *(p = js_ws(p)) != ':')
                    return NULL;
                p++;
            }
            p = js_end(p, depth + 1);
            if (!p)
                return NULL;
            p = js_ws(p);
            if (*p == ',') {
                p = js_ws(p + 1);
                continue;
            }
            return *p == close ? p + 1 : NULL;
        }
    }
    const char *s = p;
    while (*p && !strchr(",}] \t\r\n", *p))
        p++;
    return p > s ? p : NULL;
}

/* the value of key in the object at obj (its start and end), or false */
static bool js_get(const char *obj, const char *key, const char **v, const char **v_end)
{
    const char *p = js_ws(obj);
    if (*p != '{')
        return false;
    p = js_ws(p + 1);
    size_t kn = strlen(key);
    while (*p == '"') {
        const char *k = p + 1, *k_end = js_end(p, 0);
        if (!k_end)
            return false;
        p = js_ws(k_end);
        if (*p != ':')
            return false;
        const char *val = js_ws(p + 1), *val_end = js_end(val, 0);
        if (!val_end)
            return false;
        if ((size_t)(k_end - 1 - k) == kn && !memcmp(k, key, kn)) {
            *v = val;
            *v_end = val_end;
            return true;
        }
        p = js_ws(val_end);
        if (*p != ',')
            return false;
        p = js_ws(p + 1);
    }
    return false;
}

/* a value as text: strings unescaped, anything else as written */
static void js_text(const char *v, const char *end, char *out, size_t cap)
{
    size_t n = 0;
    if (*v != '"') {
        while (v < end && n + 1 < cap)
            out[n++] = *v++;
        out[n] = 0;
        return;
    }
    for (v++; v < end - 1 && n + 4 < cap; v++) {
        if (*v != '\\') {
            out[n++] = *v;
            continue;
        }
        v++;
        switch (*v) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': break;
        case 'u': {
            uint32_t cp = 0;
            for (int i = 1; i <= 4 && v[i]; i++)
                cp = cp * 16 + (uint32_t)(v[i] <= '9' ? v[i] - '0' : (v[i] | 32) - 'a' + 10);
            v += 4;
            n += (size_t)utf8_put(cp, out + n);
            break;
        }
        default: out[n++] = *v;
        }
    }
    out[n] = 0;
}

static bool js_arg(const char *args, const char *key, char *out, size_t cap)
{
    const char *v, *e;
    out[0] = 0;
    if (!js_get(args, key, &v, &e))
        return false;
    js_text(v, e, out, cap);
    return true;
}

/* ---- the tools ---- */

static const char tools_json[] =
    "{\"type\": \"function\", \"function\": {\"name\": \"run_command\", \"description\": \"Run a SynthOS shell command "
    "(a Unix-like shell: ls, cat, cp, mv, mkdir, rm, find, mdfind, date, ps, df, config, help) and return its "
    "output.\", \"parameters\": {\"type\": \"object\", \"properties\": {\"command\": {\"type\": \"string\"}}, "
    "\"required\": [\"command\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"open\", \"description\": \"Open an app by name, a file or "
    "folder by path, or a web address, on the screen.\", \"parameters\": {\"type\": \"object\", \"properties\": "
    "{\"target\": {\"type\": \"string\"}}, \"required\": [\"target\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"read_file\", \"description\": \"Read a text file or a "
    "document (PDF, Word, Excel, PowerPoint).\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": "
    "{\"type\": \"string\"}}, \"required\": [\"path\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"write_file\", \"description\": \"Create or replace a text "
    "file.\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}, \"content\": "
    "{\"type\": \"string\"}}, \"required\": [\"path\", \"content\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"find_files\", \"description\": \"Find the user's files "
    "whose name or text contains the words.\", \"parameters\": {\"type\": \"object\", \"properties\": {\"words\": "
    "{\"type\": \"string\"}}, \"required\": [\"words\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"setting\", \"description\": \"Read a system setting, or "
    "change it when value is given (config names: theme, wallpaper, volume, brightness, dnd, keyboard, "
    "uiscale...).\", \"parameters\": {\"type\": \"object\", \"properties\": {\"name\": {\"type\": \"string\"}, "
    "\"value\": {\"type\": \"string\"}}, \"required\": [\"name\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"screen\", \"description\": \"See and use the windows on "
    "the screen as text: do=look lists the front window's controls with numbers; then do=click (target: number), "
    "type or set (target: number, text), key (target: enter, tab, esc, ctrl+s...), menu (target: Menu > Item).\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {\"do\": {\"type\": \"string\"}, \"target\": "
    "{\"type\": \"string\"}, \"text\": {\"type\": \"string\"}}, \"required\": [\"do\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"web\", \"description\": \"Read the web as text: do=open "
    "(target: address) shows the page with links [n] and fields {n}; do=link (target: n) follows a link, type "
    "(target: n, text) fills a field, press (target: n) presses a button or checkbox, show shows the page again.\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {\"do\": {\"type\": \"string\"}, \"target\": "
    "{\"type\": \"string\"}, \"text\": {\"type\": \"string\"}}, \"required\": [\"do\"]}}}";

/* commands that only look */
static bool read_only_command(const char *line)
{
    static const char *const safe[] = {
        "ls", "dir", "cat", "type", "head", "tail", "wc", "grep", "find", "mdfind", "pwd", "date", "cal", "df",
        "du", "free", "ps", "uname", "whoami", "help", "echo", "uptime", "lsdev", "ifconfig", "ip", "ping",
        "file", "stat", "tree", "which", "doctext", "sysinfo", "weather", "nslookup", "history", "sort", "uniq",
        "battery", "users", "env", "hostname", "sha256sum", "md5sum", "lsusb", "lspci", "mount", "index"};
    if (strchr(line, '>') || strchr(line, '`') || strstr(line, "$("))
        return false;
    for (const char *seg = line; seg && *seg;) {
        seg = js_ws(seg);
        char word[24];
        size_t n = 0;
        while (seg[n] && !strchr(" \t|;&", seg[n]) && n + 1 < sizeof(word)) {
            word[n] = seg[n];
            n++;
        }
        word[n] = 0;
        bool ok = false;
        for (size_t i = 0; i < ARRAY_SIZE(safe) && !ok; i++)
            ok = !strcmp(word, safe[i]);
        if (!ok)
            return false;
        const char *next = seg + n;
        while (*next && !strchr("|;&", *next))
            next++;
        seg = *next ? next + 1 : NULL;
    }
    return true;
}

static bool tool_needs_approval(const char *name, const char *args)
{
    char buf[512];
    if (!strcmp(name, "run_command")) {
        js_arg(args, "command", buf, sizeof(buf));
        return !read_only_command(buf);
    }
    if (!strcmp(name, "write_file"))
        return true;
    if (!strcmp(name, "screen") || !strcmp(name, "web")) { /* looking is free; doing is asked */
        js_arg(args, "do", buf, sizeof(buf));
        return strcmp(buf, "look") && strcmp(buf, "show") && strcmp(buf, "open") && strcmp(buf, "link") && *buf;
    }
    if (!strcmp(name, "setting"))
        return js_arg(args, "value", buf, sizeof(buf)) && *buf;
    return false;
}

/* Settings → AI → what the assistant may do without asking */
static bool tool_asks(const char *name, const char *args)
{
    return gui_ai.allow == AI_ALLOW_ASK || (gui_ai.allow == AI_ALLOW_LOOK && tool_needs_approval(name, args));
}

/* a line for the window: what the tool is about to do */
static void tool_label(const char *name, const char *args, char *out, size_t cap)
{
    char a[120], b[60];
    if (!strcmp(name, "run_command")) {
        js_arg(args, "command", a, sizeof(a));
        ksnprintf(out, cap, "Run: %s", a);
    } else if (!strcmp(name, "open")) {
        js_arg(args, "target", a, sizeof(a));
        ksnprintf(out, cap, "Open %s", a);
    } else if (!strcmp(name, "read_file")) {
        js_arg(args, "path", a, sizeof(a));
        ksnprintf(out, cap, "Read %s", a);
    } else if (!strcmp(name, "write_file")) {
        js_arg(args, "path", a, sizeof(a));
        ksnprintf(out, cap, "Write %s", a);
    } else if (!strcmp(name, "find_files")) {
        js_arg(args, "words", a, sizeof(a));
        ksnprintf(out, cap, "Find files: %s", a);
    } else if (!strcmp(name, "setting")) {
        js_arg(args, "name", a, sizeof(a));
        if (js_arg(args, "value", b, sizeof(b)) && *b)
            ksnprintf(out, cap, "Change setting %s to %s", a, b);
        else
            ksnprintf(out, cap, "Look at setting %s", a);
    } else if (!strcmp(name, "screen") || !strcmp(name, "web")) {
        char t[80];
        js_arg(args, "do", b, sizeof(b));
        js_arg(args, "target", t, sizeof(t));
        js_arg(args, "text", a, sizeof(a));
        ksnprintf(out, cap, "%s %s %s%s%s", !strcmp(name, "web") ? "Web:" : "Screen:", b, t, *a ? ": " : "", a);
    } else {
        ksnprintf(out, cap, "%s", name);
    }
}

static void clip_output(char *out, size_t cap, const char *text)
{
    size_t n = strlen(text);
    if (n < cap) {
        memcpy(out, text, n + 1);
        return;
    }
    memcpy(out, text, cap - 40);
    ksnprintf(out + cap - 40, 40, "\n[... %u more bytes]", (unsigned)(n - (cap - 40)));
}

static int capture(struct agent *a, const char *line, char *out, size_t cap)
{
    char *text = NULL;
    int status = shell_capture(a->sh, line, &text);
    clip_output(out, cap, text && *text ? text : status ? "(failed, no output)" : "(done, no output)");
    kfree(text);
    return status;
}

static void quote_arg(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    out[n++] = '"';
    for (; *in && n + 3 < cap; in++) {
        if (*in == '"' || *in == '\\' || *in == '$')
            out[n++] = '\\';
        out[n++] = *in;
    }
    out[n++] = '"';
    out[n] = 0;
}

/* runs a tool; the result goes to out */
static void tool_run(struct agent *a, const char *name, const char *args, char *out, size_t cap)
{
    char p[512], line[700];
    if (!strcmp(name, "run_command")) {
        js_arg(args, "command", p, sizeof(p));
        int st = capture(a, p, out, cap);
        if (st && strlen(out) + 24 < cap)
            ksnprintf(out + strlen(out), cap - strlen(out), "\n(exit status %d)", st);
    } else if (!strcmp(name, "open")) {
        js_arg(args, "target", p, sizeof(p));
        char path[400];
        struct vstat st;
        const struct gui_app *app = gui_app_find(p);
        if (app) {
            gui_app_launch(app, NULL);
            ksnprintf(out, cap, "opened the app %s", app->name);
        } else if (str_starts(p, "http://") || str_starts(p, "https://") || str_starts(p, "www.")) {
            ksnprintf(out, cap, gui_open_url(p) ? "opened %s in the browser" : "could not open %s", p);
        } else if (!sh_resolve(a->sh, p, path, sizeof(path)) && !vfs_stat(path, &st) && gui_open_file(path)) {
            ksnprintf(out, cap, "opened %s", path);
        } else {
            ksnprintf(out, cap, "nothing called %s to open (not an app, a file or a web address)", p);
        }
    } else if (!strcmp(name, "read_file")) {
        js_arg(args, "path", p, sizeof(p));
        char q[600];
        quote_arg(p, q, sizeof(q));
        bool doc = str_iends(p, ".pdf") || str_iends(p, ".docx") || str_iends(p, ".xlsx") ||
                   str_iends(p, ".pptx") || str_iends(p, ".odt") || str_iends(p, ".ods") || str_iends(p, ".odp");
        ksnprintf(line, sizeof(line), doc ? "doctext %s" : "cat %s", q);
        capture(a, line, out, cap);
    } else if (!strcmp(name, "write_file")) {
        char *content = kmalloc(16384), path[400];
        js_arg(args, "path", p, sizeof(p));
        if (!content) {
            str_copy(out, "out of memory", cap);
            return;
        }
        js_arg(args, "content", content, 16384);
        int err = sh_resolve(a->sh, p, path, sizeof(path));
        struct file *f;
        if (!err)
            err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &f);
        if (!err) {
            long n = vfs_write(f, content, strlen(content));
            vfs_close(f);
            err = n < 0 ? (int)n : 0;
        }
        if (err)
            ksnprintf(out, cap, "could not write %s: %s", p, err_str(err));
        else
            ksnprintf(out, cap, "wrote %s (%u bytes)", path, (unsigned)strlen(content));
        kfree(content);
    } else if (!strcmp(name, "find_files")) {
        js_arg(args, "words", p, sizeof(p));
        ksnprintf(line, sizeof(line), "mdfind %s | head -n 30", p);
        capture(a, line, out, cap);
    } else if (!strcmp(name, "setting")) {
        char v[200];
        js_arg(args, "name", p, sizeof(p));
        js_arg(args, "value", v, sizeof(v));
        char q[260];
        quote_arg(v, q, sizeof(q));
        if (*v)
            ksnprintf(line, sizeof(line), "config %s %s", p, q);
        else
            ksnprintf(line, sizeof(line), "config %s", p);
        capture(a, line, out, cap);
    } else if (!strcmp(name, "screen") || !strcmp(name, "web")) {
        char d[24], t[200], x[300], qt[420];
        js_arg(args, "do", d, sizeof(d));
        js_arg(args, "target", t, sizeof(t));
        js_arg(args, "text", x, sizeof(x));
        quote_arg(x, qt, sizeof(qt));
        bool web = !strcmp(name, "web");
        if (!web && !strcmp(d, "menu")) { /* "File > Open" */
            char *gt = strchr(t, '>');
            char item[200] = "";
            if (gt) {
                *gt = 0;
                str_copy(item, js_ws(gt + 1), sizeof(item));
                for (size_t n = strlen(t); n && t[n - 1] == ' '; n--)
                    t[n - 1] = 0;
            }
            char qm[220], qi[220];
            quote_arg(t, qm, sizeof(qm));
            quote_arg(*item ? item : x, qi, sizeof(qi));
            ksnprintf(line, sizeof(line), "screen menu %s %s", qm, qi);
        } else if (!strcmp(d, "look") || !strcmp(d, "show") || !*d) {
            ksnprintf(line, sizeof(line), web ? "web show" : "screen %s", t);
        } else if (web && !strcmp(d, "open")) {
            quote_arg(t, qt, sizeof(qt));
            ksnprintf(line, sizeof(line), "web %s", qt);
        } else {
            ksnprintf(line, sizeof(line), "%s %s %s %s", name, d, t, *x ? qt : "");
        }
        capture(a, line, out, cap);
    } else {
        ksnprintf(out, cap, "there is no tool called %s", name);
    }
}

/* ---- the conversation ---- */

static void agent_clear(struct agent *a)
{
    for (int i = 0; i < a->count; i++)
        kfree(a->msgs[i].text);
    a->count = 0;
    kfree(a->context);
    a->context = NULL;
    a->state = AG_IDLE;
    a->reply_len = 0;
    a->status[0] = 0;
}

static void agent_add(struct agent *a, int role, const char *text)
{
    if (a->count == MAX_MSGS) { /* the oldest exchange goes */
        kfree(a->msgs[0].text);
        memmove(a->msgs, a->msgs + 1, sizeof(struct msg) * (MAX_MSGS - 1));
        a->count--;
    }
    char *copy = kmalloc(strlen(text) + 1);
    if (!copy)
        return;
    memcpy(copy, text, strlen(text) + 1);
    a->msgs[a->count++] = (struct msg){role, copy};
}

struct tokbuf {
    int *t;
    int n, cap;
};

static void put(struct tokbuf *b, const char *text, bool specials)
{
    if (b->n < b->cap)
        b->n += tok_encode(&model.tok, text, specials, b->t + b->n, b->cap - b->n);
}

static bool llama3_format(void)
{
    return tok_find(&model.tok, "<|start_header_id|>", 19) >= 0;
}

static void put_turn(struct tokbuf *b, const char *role, const char *text)
{
    char head[64];
    if (llama3_format()) {
        ksnprintf(head, sizeof(head), "<|start_header_id|>%s<|end_header_id|>\n\n", role);
        put(b, head, true);
        put(b, text, false);
        put(b, "<|eot_id|>", true);
    } else {
        ksnprintf(head, sizeof(head), "<|im_start|>%s\n", role);
        put(b, head, true);
        put(b, text, false);
        put(b, "<|im_end|>\n", true);
    }
}

static void system_prompt(struct agent *a, char *out, size_t cap, bool tools)
{
    struct rtc_time t;
    time_to_rtc(time_now() + (uint32_t)(time_zone() * 60), &t);
    const char *home = sh_get(a->sh, "HOME");
    struct window *w = wm_focused();
    ksnprintf(out, cap,
              "You are the assistant of SynthOS, an operating system. You run on the user's own computer, offline. "
              "%s, in the user's language. You can act on the computer with tools: "
              "use one only when the request needs it, then tell the user in plain words what you did or found.\n"
              "Now: %d %s %d, %02d:%02d. The user's home folder: %s (Documents, Downloads, Pictures, Music, "
              "Desktop inside).%s%s%s%s"
              "# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are provided with "
              "function signatures within <tools></tools> XML tags:\n<tools>\n%s\n</tools>\n\nFor each function "
              "call, return a json object with function name and arguments within <tool_call></tool_call> XML "
              "tags:\n<tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>",
              gui_ai.style == AI_STYLE_BRIEF      ? "Answer in as few words as possible"
              : gui_ai.style == AI_STYLE_DETAILED ? "Answer fully and explain, step by step when it helps"
                                                  : "Answer briefly, warmly and helpfully",
              t.day, time_month_name(t.month), t.year, t.hour, t.minute, home ? home : "/home",
              w && w->app ? " The app in front: " : "", w && w->app ? w->app->name : "", w && w->app ? "." : "",
              tools ? "\n\n" : "", tools_json);
    if (!tools) /* too little context for the tools: the conversation only */
        *strstr(out, "# Tools") = 0;
}

/* the whole conversation as tokens, ending where the assistant speaks */
static int agent_prompt(struct agent *a, int *out, int cap, bool tools)
{
    struct tokbuf b = {out, 0, cap};
    char *sys = kmalloc(8192);
    if (!sys)
        return 0;
    system_prompt(a, sys, 8192, tools);
    if (a->context) {
        size_t n = strlen(sys);
        ksnprintf(sys + n, 8192 - n, "\n\n# What the user is looking at\n\n%s", a->context);
    }
    if (llama3_format())
        put(&b, "<|begin_of_text|>", true);
    else if (model.tok.bos_first && b.n < b.cap)
        b.t[b.n++] = model.tok.bos;
    put_turn(&b, "system", sys);
    kfree(sys);
    /* the newest messages that fit, keeping room for the answer */
    int first = 0, budget = model.ctx - 700 - b.n;
    for (int i = a->count - 1; i >= 0; i--) {
        budget -= (int)strlen(a->msgs[i].text) / 3 + 8;
        if (budget < 0) {
            first = i + 1;
            break;
        }
    }
    char *wrap = NULL;
    for (int i = first; i < a->count; i++) {
        const struct msg *m = &a->msgs[i];
        if (m->role == R_TOOL) {
            wrap = kmalloc(strlen(m->text) + 64);
            if (wrap) {
                ksnprintf(wrap, strlen(m->text) + 64, "<tool_response>\n%s\n</tool_response>", m->text);
                put_turn(&b, llama3_format() ? "ipython" : "user", wrap);
                kfree(wrap);
            }
        } else {
            put_turn(&b, m->role == R_USER ? "user" : "assistant", m->text);
        }
    }
    put(&b, llama3_format() ? "<|start_header_id|>assistant<|end_header_id|>\n\n" : "<|im_start|>assistant\n", true);
    if (model.arch == ARCH_QWEN3)
        put(&b, "<think>\n\n</think>\n\n", true); /* Qwen3 answers without thinking aloud */
    return b.n;
}

static void agent_begin_turn(struct agent *a)
{
    kfree(a->toks);
    a->toks = kmalloc(sizeof(int) * (size_t)model.ctx);
    a->ntok = a->toks ? agent_prompt(a, a->toks, model.ctx, !a->no_tools) : 0;
    if (a->toks && !a->no_tools && a->ntok > model.ctx * 3 / 4) /* the tools would leave no room to talk */
        a->ntok = agent_prompt(a, a->toks, model.ctx, false);
    /* what the cache already holds of this prompt need not be computed again */
    int keep = 0;
    while (keep < model.n_past && keep < a->ntok - 1 && model.past[keep] == a->toks[keep])
        keep++;
    model.n_past = keep;
    a->fed = keep;
    a->reply_len = 0;
    a->reply[0] = 0;
    a->generated = 0;
    a->state = a->ntok ? AG_PREFILL : AG_IDLE;
    str_copy(a->status, "Reading…", sizeof(a->status));
}

static void agent_ask(struct agent *a, const char *text)
{
    agent_add(a, R_USER, text);
    a->rounds = 0;
    a->started = hal_ticks_ms();
    a->tokens_out = 0;
    agent_begin_turn(a);
}

/* the text of the reply for people: tool calls and thinking left out */
static void visible_reply(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in && n + 1 < cap) {
        if (str_starts(in, "<tool_call>")) {
            const char *e = strstr(in, "</tool_call>");
            if (!e)
                break;
            in = e + 12;
            continue;
        }
        if (str_starts(in, "<function name=")) {
            const char *e = strstr(in, "</function>");
            if (!e)
                break;
            in = e + 11;
            continue;
        }
        if (str_starts(in, "<think>")) {
            const char *e = strstr(in, "</think>");
            if (!e)
                break;
            in = e + 8;
            continue;
        }
        out[n++] = *in++;
    }
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
        n--;
    out[n] = 0;
    size_t lead = 0;
    while (out[lead] == '\n' || out[lead] == ' ')
        lead++;
    if (lead)
        memmove(out, out + lead, n - lead + 1);
}

/* the first tool call in the reply: name and arguments; false if none */
/* appends text to a JSON string being built, escaped */
static size_t js_put(char *out, size_t n, size_t cap, const char *s, size_t len)
{
    for (size_t i = 0; i < len && n + 7 < cap; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = c;
        } else if (c == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if ((unsigned char)c < 0x20) {
            n += (size_t)ksnprintf(out + n, cap - n, "\\u%04x", (unsigned)(unsigned char)c);
        } else {
            out[n++] = c;
        }
    }
    out[n] = 0;
    return n;
}

/* the XML form MiniCPM5 writes: <function name="x"><param name="p">value</param>...</function>, a value maybe in
   <![CDATA[...]]>; the arguments become a JSON object */
static bool xml_call(const char *reply, char *name, size_t ncap, char *args, size_t acap)
{
    const char *s = strstr(reply, "<function name=\"");
    if (!s)
        return false;
    s += 16;
    const char *q = strchr(s, '"');
    if (!q)
        return false;
    size_t nl = MIN((size_t)(q - s), ncap - 1);
    memcpy(name, s, nl);
    name[nl] = 0;
    const char *end = strstr(q, "</function>");
    if (!end)
        end = q + strlen(q);
    size_t n = 0;
    args[n++] = '{';
    for (const char *p = strstr(q, "<param name=\""); p && p < end; p = strstr(p, "<param name=\"")) {
        p += 13;
        const char *pq = strchr(p, '"'), *gt = pq ? strchr(pq, '>') : NULL;
        if (!gt || gt > end)
            break;
        const char *v = gt + 1, *ve = strstr(v, "</param>");
        if (!ve || ve > end)
            ve = end;
        const char *cd = strstr(v, "<![CDATA[");
        if (cd && cd < ve) {
            v = cd + 9;
            const char *ce = strstr(v, "]]>");
            if (ce && ce < ve)
                ve = ce;
        }
        if (n > 1 && n + 2 < acap)
            args[n++] = ',', args[n++] = ' ';
        if (n + 2 < acap)
            args[n++] = '"';
        n = js_put(args, n, acap, p, (size_t)(pq - p));
        if (n + 4 < acap)
            args[n++] = '"', args[n++] = ':', args[n++] = ' ', args[n++] = '"';
        n = js_put(args, n, acap, v, (size_t)(ve - v));
        if (n + 2 < acap)
            args[n++] = '"';
        p = ve;
    }
    if (n + 2 < acap)
        args[n++] = '}';
    args[MIN(n, acap - 1)] = 0;
    return true;
}

static bool find_call(const char *reply, char *name, size_t ncap, char *args, size_t acap)
{
    const char *s = strstr(reply, "<tool_call>");
    if (!s || (!strstr(s, "{") && strstr(s, "<function name=")))
        return xml_call(reply, name, ncap, args, acap);
    s += 11;
    const char *e = strstr(s, "</tool_call>");
    char body[1200];
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len >= sizeof(body))
        return false;
    memcpy(body, s, len);
    body[len] = 0;
    const char *v, *ve;
    if (!js_get(body, "name", &v, &ve))
        return false;
    js_text(v, ve, name, ncap);
    if (js_get(body, "arguments", &v, &ve) || js_get(body, "parameters", &v, &ve)) {
        size_t n = MIN((size_t)(ve - v), acap - 1);
        memcpy(args, v, n);
        args[n] = 0;
    } else {
        str_copy(args, "{}", acap);
    }
    return true;
}

static void agent_finish_tool(struct agent *a, bool allowed)
{
    char *out = kmalloc(TOOL_OUT);
    if (!out)
        return;
    if (allowed) {
        ksnprintf(a->status, sizeof(a->status), "%s", a->call_label);
        tool_run(a, a->call_name, a->call_args, out, TOOL_OUT);
    } else {
        str_copy(out, "The user did not allow this.", TOOL_OUT);
    }
    agent_add(a, R_TOOL, out);
    kfree(out);
    agent_begin_turn(a);
}

/* the reply is complete: keep it and run the tool it asks for, if any */
static void agent_reply_done(struct agent *a)
{
    agent_add(a, R_ASSISTANT, a->reply);
    if (a->rounds < MAX_ROUNDS &&
        find_call(a->reply, a->call_name, sizeof(a->call_name), a->call_args, sizeof(a->call_args))) {
        a->rounds++;
        tool_label(a->call_name, a->call_args, a->call_label, sizeof(a->call_label));
        if (!a->auto_approve && tool_asks(a->call_name, a->call_args)) {
            a->state = AG_APPROVE;
            str_copy(a->status, "Waiting for your answer", sizeof(a->status));
            return;
        }
        agent_finish_tool(a, true);
        return;
    }
    a->state = AG_IDLE;
    a->status[0] = 0;
}

static bool stop_token(int id)
{
    return id == model.tok.eos || id == model.tok.eot || id == tok_find(&model.tok, "<|endoftext|>", 13) ||
           id == tok_find(&model.tok, "<|eom_id|>", 10);
}

/* does one piece of work; returns true while there is more */
static bool agent_step(struct agent *a)
{
    if (a->state == AG_PREFILL) {
        if (a->ntok > model.ctx - 16) {
            str_copy(a->reply, "(The conversation is too long for the model; start a new one.)", sizeof(a->reply));
            a->reply_len = (int)strlen(a->reply);
            a->state = AG_IDLE;
            agent_add(a, R_ASSISTANT, a->reply);
            return false;
        }
        int b = MIN(AI_BATCH, a->ntok - a->fed);
        forward(&model, a->toks + a->fed, b, a->fed + b == a->ntok);
        a->fed += b;
        if (a->fed == a->ntok) {
            a->state = AG_DECODE;
            str_copy(a->status, "Writing…", sizeof(a->status));
        }
        return true;
    }
    if (a->state != AG_DECODE)
        return false;
    int id = sample(&model, &a->smp);
    bool done = stop_token(id) || model.n_past >= model.ctx - 1 || a->reply_len >= MAX_REPLY - 64;
    if (!done) {
        char piece[64];
        int n = tok_piece(&model.tok, id, piece, sizeof(piece) - 1);
        memcpy(a->reply + a->reply_len, piece, (size_t)n);
        a->reply_len += n;
        a->reply[a->reply_len] = 0;
        a->generated++;
        a->tokens_out++;
        /* a finished tool call needs no more words */
        done = strstr(a->reply, "</tool_call>") || strstr(a->reply, "</function>");
        if (!done)
            forward(&model, &id, 1, true);
    }
    if (done) {
        if (model.n_past < model.ctx)
            model.past[model.n_past] = id;
        agent_reply_done(a);
    }
    return a->state != AG_IDLE;
}
