/* Tab completion for the whole system. Every text field asks gui_complete_ask while it is typed into; a moment
   after the typing stops the answer is worked out here, a token at a time from the module's tick, and handed back in
   gui_hint (grey text after the caret, Tab takes it):
   * text: a small keyboard model (SmolLM2 135M, 105 MB) continues the word or the sentence, and offers to correct a
     word that was just finished when the model knows a close word that fits much better ("teh " -> "the ");
   * code: Qwen2.5-Coder 1.5B (1.1 GB) fills in the middle, with the text after the caret; StarCoder2 3B (1.8 GB)
     where it has been downloaded and there is memory for it;
   * sites: no model at all, only sites that exist (history, bookmarks, well-known sites: site_complete in Obscura);
   * addresses: no model either, only the address book (/home/addresses.txt, one address a line).
   The last word typed is taken back out of the prompt and the first token must continue it ("token healing"), so
   "hel" becomes "hello" and not "hel lo". */

#define COMPLETE_OFF_FILE MODELS_DIR "/complete-off"
#define ADDRESS_FILE "/home/addresses.txt"
#define COMPLETE_DELAY_MS 150
#define COMPLETE_BUDGET_MS 25
#define CODER_BIG_NEEDS (6000ull << 20)

struct helper {
    struct ai_model m;
    bool loaded, failed;
};
static struct helper keys_m, code_m;

enum { C_IDLE, C_WAIT, C_RUN };
static struct {
    const void *owner;
    struct window *win;
    int kind, state;
    uint64_t at;
    char before[1024], after[256];
    struct helper *h;
    char heal[48]; /* what was typed of the word the first tokens must continue */
    char out[160];
    int made, max;
    bool to_shell; /* ai complete: the answer goes to the shell, not to a field */
    int erase;     /* a correction: how much it takes back */
} cq;

static bool complete_off(void)
{
    struct vstat st;
    return !vfs_stat(COMPLETE_OFF_FILE, &st);
}

/* the model for a kind of text: loaded the first time it is needed; NULL when it has not been downloaded */
static struct helper *helper_for(int kind, const char *forced)
{
    struct helper *h = kind == COMPLETE_CODE ? &code_m : &keys_m;
    char path[200];
    if (forced) {
        str_copy(path, forced, sizeof(path));
        if (h->loaded && strcmp(h->m.path, path)) {
            model_free(&h->m);
            h->loaded = false;
        }
        h->failed = false;
    } else if (kind == COMPLETE_CODE) {
        struct vstat st;
        ksnprintf(path, sizeof(path), MODELS_DIR "/%s", coder_big_model.name);
        if (vfs_stat(path, &st) || (uint64_t)vmm_high_pages() * 4096 < CODER_BIG_NEEDS)
            ksnprintf(path, sizeof(path), MODELS_DIR "/%s", coder_model.name);
        if (vfs_stat(path, &st)) /* no code model: the keyboard model does what it can */
            return helper_for(COMPLETE_TEXT, NULL);
    } else {
        ksnprintf(path, sizeof(path), MODELS_DIR "/%s", keys_model.name);
    }
    if (h->loaded)
        return h;
    struct vstat st;
    if (h->failed || vfs_stat(path, &st))
        return NULL;
    char why[160];
    if (read_model(&h->m, NULL, path, 1024, why, sizeof(why))) {
        h->failed = true;
        return NULL;
    }
    h->loaded = true;
    return h;
}

static void hint_post(int erase, const char *text)
{
    if (cq.to_shell)
        return;
    gui_hint.owner = cq.owner;
    gui_hint.win = cq.win;
    gui_hint.erase = erase;
    str_copy(gui_hint.text, text, sizeof(gui_hint.text));
    wm_invalidate(cq.win);
}

static bool is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80;
}

static bool is_word_char(char c)
{
    return is_letter(c) || (c >= '0' && c <= '9') || c == '_';
}

/* ---- correcting the word just finished ---- */

static int edit_distance(const char *a, int n, const char *b, int m)
{
    if (n > 24 || m > 24)
        return 99;
    int d[26][26];
    for (int i = 0; i <= n; i++)
        d[i][0] = i;
    for (int j = 0; j <= m; j++)
        d[0][j] = j;
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            int cost = lower_ascii(a[i - 1]) != lower_ascii(b[j - 1]);
            int v = MIN(MIN(d[i - 1][j] + 1, d[i][j - 1] + 1), d[i - 1][j - 1] + cost);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                v = MIN(v, d[i - 2][j - 2] + 1); /* two letters swapped */
            d[i][j] = v;
        }
    return d[n][m];
}

/* when before ends with a finished word the model does not know but knows a close one that fits, offers that */
static bool correct_word(struct helper *h, const char *before)
{
    int n = (int)strlen(before), end = n;
    while (end > 0 && (before[end - 1] == ' ' || before[end - 1] == ',' || before[end - 1] == '.' ||
                       before[end - 1] == '!' || before[end - 1] == '?'))
        end--;
    if (end == n || n - end > 2)
        return false;
    int start = end;
    while (start > 0 && ((before[start - 1] >= 'a' && before[start - 1] <= 'z') ||
                         (before[start - 1] >= 'A' && before[start - 1] <= 'Z')))
        start--;
    int len = end - start;
    if (len < 4 || len > 20 || (start > 0 && is_word_char(before[start - 1])))
        return false;
    const struct ai_tok *t = &h->m.tok;
    char word[32];
    ksnprintf(word, sizeof(word), " %.*s", len, before + start);
    int ids[8];
    if (tok_encode(t, word, false, ids, 8) == 1)
        return false; /* a word the model knows */
    word[1] = lower_ascii(word[1]);
    if (tok_encode(t, word, false, ids, 8) == 1)
        return false;
    /* the context before the word decides between the close words */
    int ctx[256], m = 0;
    int from = MAX(0, start - 400);
    char head[420];
    ksnprintf(head, sizeof(head), "%.*s", start - from - (start > 0 && before[start - 1] == ' '), before + from);
    m = head[0] ? tok_encode(t, head, false, ctx, 256) : 0;
    if (m <= 0 || model_feed(&h->m, ctx, m, NULL))
        return false;
    int best = -1, cand[5] = {-1, -1, -1, -1, -1}, dists[5];
    float scores[5];
    char piece[48];
    for (int id = 0; id < h->m.vocab; id++) {
        int k = tok_piece(t, id, piece, sizeof(piece) - 1);
        if (k < 4 || piece[0] != ' ' || k - 1 < len - 2 || k - 1 > len + 2)
            continue;
        bool letters = true;
        for (int i = 1; i < k && letters; i++)
            letters = (piece[i] >= 'a' && piece[i] <= 'z') || (piece[i] >= 'A' && piece[i] <= 'Z');
        int dist = letters ? edit_distance(piece + 1, k - 1, before + start, len) : 99;
        if (dist > (len >= 6 ? 2 : 1))
            continue;
        /* the closest spellings first; among equally close ones, what fits the words before */
        float sc = h->m.logits[id];
        int at = (int)ARRAY_SIZE(cand);
        while (at > 0 && (cand[at - 1] < 0 || dist < dists[at - 1] || (dist == dists[at - 1] && sc > scores[at - 1])))
            at--;
        if (at >= (int)ARRAY_SIZE(cand))
            continue;
        for (int j = ARRAY_SIZE(cand) - 1; j > at; j--) {
            cand[j] = cand[j - 1];
            dists[j] = dists[j - 1];
            scores[j] = scores[j - 1];
        }
        cand[at] = id;
        dists[at] = dist;
        scores[at] = sc;
    }
    /* a whole word, not the start of a longer one ("proble"): the model must expect a space or a mark after it */
    int start_pos = h->m.n_past;
    for (int c = 0; c < (int)ARRAY_SIZE(cand) && best < 0 && cand[c] >= 0; c++) {
        h->m.n_past = start_pos;
        forward(&h->m, &cand[c], 1, true);
        int next = 0;
        for (int id = 1; id < h->m.vocab; id++)
            if (h->m.logits[id] > h->m.logits[next])
                next = id;
        int k = tok_piece(t, next, piece, sizeof(piece) - 1);
        if (k && (piece[0] == ' ' || piece[0] == ',' || piece[0] == '.' || piece[0] == '!' || piece[0] == '?' ||
                  piece[0] == '\n' || piece[0] == ';' || piece[0] == ':' || piece[0] == '\''))
            best = cand[c];
    }
    h->m.n_past = start_pos;
    if (best < 0)
        return false;
    int k = tok_piece(t, best, piece, sizeof(piece) - 1);
    piece[k] = 0;
    char fixed[64];
    ksnprintf(fixed, sizeof(fixed), "%s%s", piece + 1, before + end);
    if (before[start] >= 'A' && before[start] <= 'Z' && fixed[0] >= 'a' && fixed[0] <= 'z')
        fixed[0] = (char)(fixed[0] - 32);
    str_copy(cq.out, fixed, sizeof(cq.out));
    cq.erase = n - start;
    hint_post(n - start, fixed);
    return true;
}

/* ---- addresses ---- */

static bool address_complete(const char *typed, char *out, size_t cap)
{
    char *data;
    size_t len;
    size_t tn = strlen(typed);
    if (tn < 2 || vfs_read_all(ADDRESS_FILE, &data, &len))
        return false;
    bool found = false;
    for (char *line = data; line && *line && !found;) {
        char *nl = memchr(line, '\n', len - (size_t)(line - data));
        size_t ln = nl ? (size_t)(nl - line) : len - (size_t)(line - data);
        if (ln > tn && !str_ncasecmp(line, typed, tn)) {
            ksnprintf(out, cap, "%.*s", (int)(ln - tn), line + tn);
            found = true;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    return found;
}

/* ---- the model's part ---- */

static int fim_token(const struct ai_tok *t, const char *a, const char *b)
{
    int id = tok_find(t, a, strlen(a));
    return id >= 0 ? id : tok_find(t, b, strlen(b));
}

/* the prompt for cq into the model's cache; false when there is nothing to work from */
static bool complete_start(void)
{
    struct helper *h = cq.h;
    const struct ai_tok *t = &h->m.tok;
    if (cq.kind == COMPLETE_TEXT && correct_word(h, cq.before))
        return false;
    /* the end of the last word, and the space before it, are left for the first tokens to continue */
    int n = (int)strlen(cq.before), cut = n;
    while (cut > 0 && n - cut < 40 && is_word_char(cq.before[cut - 1]))
        cut--;
    if (cut > 0 && cq.before[cut - 1] == ' ' && n - cut < 40)
        cut--;
    str_copy(cq.heal, cq.before + cut, sizeof(cq.heal));
    static int ids[1024];
    int m = 0, from = MAX(0, cut - (cq.kind == COMPLETE_CODE ? 900 : 400));
    while (from > 0 && from < cut && ((unsigned char)cq.before[from] & 0xc0) == 0x80)
        from++; /* not inside a character */
    char *text = kmalloc(1400);
    if (!text)
        return false;
    int pre = fim_token(t, "<|fim_prefix|>", "<fim_prefix>"), suf = fim_token(t, "<|fim_suffix|>", "<fim_suffix>"),
        mid = fim_token(t, "<|fim_middle|>", "<fim_middle>");
    if (cq.kind == COMPLETE_CODE && pre >= 0 && suf >= 0 && mid >= 0) {
        ids[m++] = pre;
        ksnprintf(text, 1400, "%.*s", cut - from, cq.before + from);
        m += tok_encode(t, text, false, ids + m, 700);
        ids[m++] = suf;
        m += tok_encode(t, cq.after, false, ids + m, 300);
        ids[m++] = mid;
    } else {
        /* plain text to continue; a line break stands in for nothing at all */
        ksnprintf(text, 1400, "%.*s", cut - from, cq.before + from);
        m += tok_encode(t, text[0] ? text : "\n", false, ids + m, 1000);
    }
    kfree(text);
    if (m <= 0 || m > h->m.ctx - 40 || model_feed(&h->m, ids, m, NULL))
        return false;
    cq.out[0] = 0;
    cq.made = 0;
    cq.max = cq.kind == COMPLETE_CODE ? 32 : 12;
    return true;
}

static bool special_token(const struct ai_tok *t, int id)
{
    return t->type[id] == 3 || id == t->eos || id == t->eot;
}

/* the next token; false when the completion is finished */
static bool complete_step(void)
{
    struct ai_model *m = &cq.h->m;
    const struct ai_tok *t = &m->tok;
    int best = -1, hn = (int)strlen(cq.heal);
    float score = -1e30f;
    char piece[64];
    for (int id = 0; id < m->vocab; id++) {
        if (m->logits[id] <= score)
            continue;
        if (hn) { /* the first tokens must agree with what was typed */
            if (special_token(t, id))
                continue;
            int k = tok_piece(t, id, piece, sizeof(piece));
            if (!k || memcmp(piece, cq.heal, (size_t)MIN(k, hn)))
                continue;
        }
        score = m->logits[id];
        best = id;
    }
    if (best < 0 || special_token(t, best))
        return false;
    int k = tok_piece(t, best, piece, sizeof(piece) - 1);
    piece[k] = 0;
    const char *add = piece;
    if (hn) {
        int used = MIN(k, hn);
        memmove(cq.heal, cq.heal + used, (size_t)(hn - used + 1));
        add = piece + used;
    }
    /* one line at most; code at the end of a line may go on to the next one */
    char *nl = strchr(cq.kind == COMPLETE_CODE && !cq.out[0] && add[0] == '\n' ? add + 1 : add, '\n');
    if (nl)
        *nl = 0;
    str_append(cq.out, add, sizeof(cq.out));
    cq.made++;
    size_t len = strlen(cq.out);
    bool sentence = cq.kind != COMPLETE_CODE && len && !cq.heal[0] &&
                    (cq.out[len - 1] == '.' || cq.out[len - 1] == '!' || cq.out[len - 1] == '?');
    if (nl || sentence || cq.made >= cq.max || len >= sizeof(cq.out) - 64 || m->n_past >= m->ctx - 1)
        return false;
    if (!cq.heal[0] && cq.out[0] && cq.kind != COMPLETE_CODE)
        hint_post(0, cq.out); /* the words so far, while the rest is worked out */
    forward(m, &best, 1, true);
    return true;
}

static void complete_finish(void)
{
    cq.state = C_IDLE;
    if (cq.out[0] && !cq.heal[0])
        hint_post(0, cq.out);
}

/* gui_complete_ask: every text field calls this as it is typed into */
static void complete_ask(const void *owner, struct window *win, int kind, const char *before, const char *after)
{
    if (complete_off())
        return;
    cq.owner = owner;
    cq.win = win;
    cq.kind = kind;
    cq.to_shell = false;
    str_copy(cq.before, before, sizeof(cq.before));
    str_copy(cq.after, after ? after : "", sizeof(cq.after));
    cq.at = hal_ticks_ms();
    cq.state = C_WAIT;
}

/* what needs no model is answered at once; true when cq.out holds the answer */
static bool complete_plain(void)
{
    cq.out[0] = 0;
    if (cq.kind == COMPLETE_URL) {
#ifdef __x86_64__
        char site[128];
        if (site_complete(cq.before, site, sizeof(site)))
            str_copy(cq.out, site + strlen(cq.before), sizeof(cq.out));
#endif
        return true;
    }
    if (cq.kind == COMPLETE_ADDRESS) {
        address_complete(cq.before, cq.out, sizeof(cq.out));
        return true;
    }
    return false;
}

static void complete_tick(void)
{
    if (cq.state == C_WAIT && hal_ticks_ms() - cq.at >= COMPLETE_DELAY_MS) {
        cq.state = C_IDLE;
        if (complete_plain()) {
            if (cq.out[0])
                hint_post(0, cq.out);
            return;
        }
        if (!(cq.h = helper_for(cq.kind, NULL)) || !complete_start())
            return;
        cq.state = C_RUN;
    }
    uint64_t until = hal_ticks_ms() + COMPLETE_BUDGET_MS;
    while (cq.state == C_RUN && hal_ticks_ms() < until)
        if (!complete_step())
            complete_finish();
}

/* ai complete [--model file] [--code|--url|--address] text: what Tab would offer after text */
static int complete_command(struct shell *sh, int argc, char **argv)
{
    const char *forced = NULL;
    int kind = COMPLETE_TEXT, i = 2;
    char path[160];
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            sh_resolve(sh, argv[++i], path, sizeof(path));
            forced = path;
        } else if (!strcmp(argv[i], "--code"))
            kind = COMPLETE_CODE;
        else if (!strcmp(argv[i], "--url"))
            kind = COMPLETE_URL;
        else if (!strcmp(argv[i], "--address"))
            kind = COMPLETE_ADDRESS;
        else
            return sh_error(sh, "ai", "unknown option %s", argv[i]);
    }
    if (i < argc && (!strcmp(argv[i], "on") || !strcmp(argv[i], "off")) && i == argc - 1) {
        if (!strcmp(argv[i], "off"))
            vfs_write_all(COMPLETE_OFF_FILE, "off\n", 4);
        else
            vfs_unlink(COMPLETE_OFF_FILE);
        sh_printf(sh, "Tab completion is %s\n", argv[i]);
        return 0;
    }
    cq.before[0] = 0;
    for (int j = i; j < argc; j++)
        ksnprintf(cq.before + strlen(cq.before), sizeof(cq.before) - strlen(cq.before), "%s%s", j > i ? " " : "",
                  argv[j]);
    /* a trailing "_" stands for a space, which the shell would drop */
    size_t bl = strlen(cq.before);
    if (bl && cq.before[bl - 1] == '_')
        cq.before[bl - 1] = ' ';
    cq.after[0] = 0;
    cq.kind = kind;
    cq.to_shell = true;
    cq.state = C_IDLE;
    cq.erase = 0;
    if (!complete_plain()) {
        if (!(cq.h = helper_for(kind, forced))) {
            cq.to_shell = false;
            return sh_error(sh, "ai", "no completion model: run ai get keys (or ai get coder)");
        }
        if (complete_start())
            while (complete_step())
                ;
    }
    cq.to_shell = false;
    if (forced && cq.h && cq.h->loaded) { /* a model named here is for this once: memory is short on small machines */
        model_free(&cq.h->m);
        cq.h->loaded = false;
    }
    sh_printf(sh, "%s: [", cq.erase ? "correction" : "completion");
    for (const unsigned char *c = (const unsigned char *)cq.out; *c; c++) /* bytes that are not ASCII as \xNN */
        sh_printf(sh, *c >= 32 && *c < 127 && *c != '\\' ? "%c" : "\\x%02x", *c);
    sh_printf(sh, "]\n");
    return 0;
}
