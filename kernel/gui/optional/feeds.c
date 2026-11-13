#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/module.h>
#include <synth/string.h>
#include <synth/tls.h>

#include "../widgets.h"

/* The desktop widgets that come from the network:
     News    the headlines of an RSS or Atom feed (BBC World unless another feed's address is given);
     Stocks  quotes for a list of ticker symbols from stooq.com, with the change since the day opened;
     Sports  the scoreboard of a league from ESPN ("soccer/eng.1", "basketball/nba", "football/nfl", ...).
   The option of a widget (Edit Widget) is the feed, the symbols or the league; a full https:// address instead
   reads that source in the same format. A click opens the story, the quote or the match in the browser; a double
   click on the heading refreshes. 'feeds' prints the same in the terminal. */

#define MAX_ITEMS 12
#define MAX_CACHE 8
#define FETCH_LIMIT_MS 8000

#define NEWS_DEFAULT "https://feeds.bbci.co.uk/news/world/rss.xml"
#define STOCKS_DEFAULT "AAPL MSFT NVDA GOOGL AMZN"
#define SPORTS_DEFAULT "soccer/eng.1"

struct item {
    char title[128];
    char detail[48]; /* the price, the score and state of a match */
    char link[256];
    int trend; /* stocks: -1 down, 1 up */
};

struct feed {
    int kind;
    char opt[WIDGET_OPT];
    char title[64];
    struct item items[MAX_ITEMS];
    int n;
    bool loaded;
    char error[96];
    uint64_t next;
};

static struct feed *cache;

/* ---- text ---- */

/* the text between from and to: CDATA unwrapped, tags dropped, entities decoded, spaces folded */
static void clean(const char *from, const char *to, char *out, size_t cap)
{
    size_t n = 0;
    bool space = false;
    for (const char *p = from; p < to && n + 4 < cap;) {
        if (!strncmp(p, "<![CDATA[", 9)) {
            p += 9;
            continue;
        }
        if (!strncmp(p, "]]>", 3)) {
            p += 3;
            continue;
        }
        if (*p == '<') {
            while (p < to && *p != '>')
                p++;
            p++;
            continue;
        }
        char c = *p;
        if (c == '&') {
            static const struct {
                const char *name;
                const char *text;
            } ents[] = {{"&amp;", "&"}, {"&lt;", "<"},   {"&gt;", ">"},   {"&quot;", "\""}, {"&apos;", "'"},
                        {"&#39;", "'"}, {"&#8217;", "'"}, {"&#8216;", "'"}, {"&#8220;", "\""}, {"&#8221;", "\""}};
            bool hit = false;
            for (size_t i = 0; i < ARRAY_SIZE(ents) && !hit; i++) {
                size_t k = strlen(ents[i].name);
                if (!strncmp(p, ents[i].name, k)) {
                    out[n++] = ents[i].text[0];
                    p += k;
                    hit = true;
                }
            }
            if (hit) {
                space = false;
                continue;
            }
            if (p[1] == '#') { /* any other number: as UTF-8 */
                const char *q = p + 2;
                uint32_t cp = (uint32_t)(*q == 'x' ? str_to_long(q + 1, NULL, 16) : str_to_long(q, NULL, 10));
                const char *semi = strchr(q, ';');
                if (semi && semi < to && cp) {
                    if (cp < 0x80) {
                        out[n++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[n++] = (char)(0xc0 | cp >> 6);
                        out[n++] = (char)(0x80 | (cp & 63));
                    } else if (cp < 0x10000) {
                        out[n++] = (char)(0xe0 | cp >> 12);
                        out[n++] = (char)(0x80 | (cp >> 6 & 63));
                        out[n++] = (char)(0x80 | (cp & 63));
                    }
                    p = semi + 1;
                    space = false;
                    continue;
                }
            }
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (n && !space)
                out[n++] = ' ';
            space = true;
            p++;
            continue;
        }
        space = false;
        out[n++] = c;
        p++;
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

/* the text of the first <tag>...</tag> between from and to */
static bool element(const char *from, const char *to, const char *tag, char *out, size_t cap)
{
    char open[32], close[32];
    ksnprintf(open, sizeof(open), "<%s", tag);
    ksnprintf(close, sizeof(close), "</%s>", tag);
    size_t k = strlen(open);
    for (const char *p = from; p && p < to; p++) {
        p = strstr(p, open);
        if (!p || p >= to)
            return false;
        if (p[k] != '>' && p[k] != ' ')
            continue;
        const char *gt = strchr(p, '>');
        if (!gt || gt >= to)
            return false;
        if (gt[-1] == '/') { /* <link href="..."/> */
            out[0] = 0;
            const char *h = strstr(p, "href=\"");
            if (h && h < gt) {
                h += 6;
                const char *e = strchr(h, '"');
                clean(h, e ? e : h, out, cap);
            }
            return true;
        }
        const char *end = strstr(gt, close);
        if (!end || end > to)
            return false;
        clean(gt + 1, end, out, cap);
        if (!out[0]) { /* Atom: <link href="..."></link> */
            const char *h = strstr(p, "href=\"");
            if (h && h < gt) {
                h += 6;
                const char *e = strchr(h, '"');
                clean(h, e ? e : h, out, cap);
            }
        }
        return true;
    }
    return false;
}

/* ---- the three formats ---- */

static void parse_news(struct feed *f, const char *xml)
{
    bool atom = !strstr(xml, "<item") && strstr(xml, "<entry");
    const char *tag = atom ? "<entry" : "<item";
    const char *first = strstr(xml, tag);
    element(xml, first ? first : xml + strlen(xml), "title", f->title, sizeof(f->title));
    for (const char *p = first; p && f->n < MAX_ITEMS;) {
        const char *next = strstr(p + 1, tag);
        const char *end = next ? next : p + strlen(p);
        struct item *it = &f->items[f->n];
        memset(it, 0, sizeof(*it));
        if (element(p, end, "title", it->title, sizeof(it->title)) && it->title[0]) {
            element(p, end, "link", it->link, sizeof(it->link));
            char date[64] = "";
            if (element(p, end, "pubDate", date, sizeof(date)) || element(p, end, "updated", date, sizeof(date)) ||
                element(p, end, "published", date, sizeof(date))) {
                /* "Fri, 13 Nov 2026 06:12:00 GMT" -> "13 Nov 06:12"; "2026-11-13T06:12:00Z" -> "11-13 06:12" */
                const char *comma = strchr(date, ',');
                if (comma && strlen(comma) >= 19)
                    ksnprintf(it->detail, sizeof(it->detail), "%.6s %.5s", comma + 2, comma + 14);
                else if (strlen(date) >= 16 && date[10] == 'T')
                    ksnprintf(it->detail, sizeof(it->detail), "%.5s %.5s", date + 5, date + 11);
            }
            f->n++;
        }
        p = next;
    }
    if (!f->title[0])
        str_copy(f->title, "News", sizeof(f->title));
}

/* a decimal like 187.42 as hundredths */
static long long cents(const char *s)
{
    long long whole = 0, frac = 0;
    int digits = 0;
    bool neg = *s == '-';
    if (neg)
        s++;
    for (; *s >= '0' && *s <= '9'; s++)
        whole = whole * 10 + (*s - '0');
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9' && digits < 2; s++, digits++)
            frac = frac * 10 + (*s - '0');
    while (digits++ < 2)
        frac *= 10;
    return (neg ? -1 : 1) * (whole * 100 + frac);
}

/* CSV: Symbol,Date,Time,Open,High,Low,Close,Volume */
static void parse_stocks(struct feed *f, const char *csv)
{
    str_copy(f->title, "Stocks", sizeof(f->title));
    for (const char *line = csv; line && *line && f->n < MAX_ITEMS;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char row[256], *col[10];
        str_copy(row, line, MIN(sizeof(row), len + 1));
        line = end ? end + 1 : NULL;
        int n = 0;
        for (char *p = row; n < 10;) {
            col[n++] = p;
            char *comma = strchr(p, ',');
            if (!comma)
                break;
            *comma = 0;
            p = comma + 1;
        }
        size_t rl = strlen(row);
        if (rl && row[rl - 1] == '\r')
            row[rl - 1] = 0;
        if (n < 7 || !str_casecmp(col[0], "Symbol") || !(col[6][0] >= '0' && col[6][0] <= '9'))
            continue;
        struct item *it = &f->items[f->n++];
        memset(it, 0, sizeof(*it));
        str_copy(it->title, col[0], sizeof(it->title));
        char *dot = strchr(it->title, '.');
        if (dot && !str_casecmp(dot, ".US"))
            *dot = 0;
        long long open = cents(col[3]), close = cents(col[6]);
        long long bp = open ? (close - open) * 10000 / open : 0; /* basis points */
        long long abp = bp < 0 ? -bp : bp;
        ksnprintf(it->detail, sizeof(it->detail), "%lld.%02lld %c%lld.%02lld%%", close / 100, close % 100,
                  bp < 0 ? '-' : '+', abp / 100, abp % 100);
        it->trend = bp < 0 ? -1 : bp > 0 ? 1 : 0;
        ksnprintf(it->link, sizeof(it->link), "https://stooq.com/q/?s=%s", col[0]);
    }
}

/* the string after "key":" at p, before limit */
static bool json_str(const char *p, const char *limit, const char *key, char *out, size_t cap, const char **at)
{
    char pat[48];
    ksnprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *q = strstr(p, pat);
    if (!q || (limit && q >= limit))
        return false;
    q += strlen(pat);
    size_t n = 0;
    while (*q && *q != '"' && n + 1 < cap) {
        if (*q == '\\' && q[1])
            q++;
        out[n++] = *q++;
    }
    out[n] = 0;
    if (at)
        *at = q;
    return true;
}

/* ESPN's scoreboard: each event has "shortName", its competitors their "homeAway", "score" and team
   "abbreviation", the state a "shortDetail", and a link with "gameId" in it */
static void parse_sports(struct feed *f, const char *json)
{
    json_str(json, NULL, "name", f->title, sizeof(f->title), NULL);
    const char *events = strstr(json, "\"events\":[");
    if (!events) {
        str_copy(f->error, "No matches in this league's scoreboard", sizeof(f->error));
        return;
    }
    /* each event begins with its id and a uid ending in ~e:<id> (a competition's goes on with ~c:) */
    const char *starts[MAX_ITEMS + 1];
    int count = 0;
    for (const char *p = events; count < MAX_ITEMS && (p = strstr(p, "~e:"));) {
        const char *q = p + 3;
        while (*q >= '0' && *q <= '9')
            q++;
        if (*q == '"') {
            const char *s = p;
            while (s > events && strncmp(s, "{\"id\"", 5))
                s--;
            starts[count++] = s;
        }
        p = q;
    }
    starts[count] = json + strlen(json);
    for (int e = 0; e < count && f->n < MAX_ITEMS; e++) {
        const char *s = starts[e], *end = starts[e + 1];
        struct item *it = &f->items[f->n];
        memset(it, 0, sizeof(*it));
        char state[32] = "", home[16] = "", away[16] = "", hs[8] = "", as[8] = "";
        json_str(s, end, "shortDetail", state, sizeof(state), NULL);
        const char *p = s;
        for (int side = 0; side < 2; side++) {
            char which[8], score[8] = "", abbr[16] = "";
            if (!json_str(p, end, "homeAway", which, sizeof(which), &p))
                break;
            json_str(p, end, "score", score, sizeof(score), NULL);
            const char *team = strstr(p, "\"team\":{");
            if (team && team < end)
                json_str(team, end, "abbreviation", abbr, sizeof(abbr), NULL);
            if (!strcmp(which, "home"))
                str_copy(home, abbr, sizeof(home)), str_copy(hs, score, sizeof(hs));
            else
                str_copy(away, abbr, sizeof(away)), str_copy(as, score, sizeof(as));
        }
        if (!home[0] && !away[0] && !json_str(s, end, "shortName", it->title, sizeof(it->title), NULL))
            continue;
        if (home[0] || away[0]) {
            bool started = hs[0] && as[0] && strncmp(state, "Sch", 3) && !strchr(state, ':');
            if (started)
                ksnprintf(it->title, sizeof(it->title), "%s %s - %s %s", away, as, hs, home);
            else
                ksnprintf(it->title, sizeof(it->title), "%s @ %s", away, home);
        }
        str_copy(it->detail, state, sizeof(it->detail));
        for (const char *h = s; h && h < end && (h = strstr(h, "\"href\":\"")) && h < end; h += 8) {
            char link[256];
            json_str(h, end, "href", link, sizeof(link), NULL);
            if (strstr(link, "gameId")) {
                str_copy(it->link, link, sizeof(it->link));
                break;
            }
        }
        f->n++;
    }
    if (!f->title[0])
        str_copy(f->title, "Sports", sizeof(f->title));
}

/* ---- fetching ---- */

static uint64_t fetch_started;

static bool too_long(void)
{
    return hal_ticks_ms() - fetch_started > FETCH_LIMIT_MS;
}

/* the address to read for a widget's option */
static void source(int kind, const char *opt, char *url, size_t cap)
{
    if (!strncmp(opt, "https://", 8) || !strncmp(opt, "http://", 7)) {
        str_copy(url, opt, cap);
        return;
    }
    if (kind == WK_NEWS) {
        str_copy(url, opt[0] ? opt : NEWS_DEFAULT, cap);
    } else if (kind == WK_SPORTS) {
        ksnprintf(url, cap, "https://site.api.espn.com/apis/site/v2/sports/%s/scoreboard", opt[0] ? opt
                                                                                               : SPORTS_DEFAULT);
    } else { /* symbols: AAPL MSFT -> aapl.us,msft.us */
        size_t n = (size_t)ksnprintf(url, cap, "https://stooq.com/q/l/?s=");
        const char *p = opt[0] ? opt : STOCKS_DEFAULT;
        bool first = true;
        while (*p && n + 16 < cap) {
            while (*p == ' ' || *p == ',')
                p++;
            if (!*p)
                break;
            if (!first)
                url[n++] = ',';
            first = false;
            bool dot = false;
            while (*p && *p != ' ' && *p != ',' && n + 8 < cap) {
                dot |= *p == '.';
                url[n++] = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
                p++;
            }
            if (!dot)
                n += (size_t)ksnprintf(url + n, cap - n, ".us");
        }
        ksnprintf(url + n, cap - n, "&f=sd2t2ohlcv&h&e=csv");
    }
}

static void load(struct feed *f)
{
    char url[512];
    source(f->kind, f->opt, url, sizeof(url));
    f->n = 0;
    f->error[0] = 0;
    fetch_started = hal_ticks_ms();
    struct http_request req = {.url = url, .max_size = 1u << 20, .flags = HTTP_NO_COOKIES | HTTP_GZIP,
                               .cancelled = too_long};
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    struct http_url u;
    const char *host = http_parse_url(url, &u) ? u.host : url;
    if (err) {
        ksnprintf(f->error, sizeof(f->error), "Can't reach %s (%s)", host, tls_strerror(err));
    } else {
        if (resp.status != 200 || !resp.body)
            ksnprintf(f->error, sizeof(f->error), "%s answered %d", host, resp.status);
        else if (f->kind == WK_NEWS)
            parse_news(f, resp.body);
        else if (f->kind == WK_STOCKS)
            parse_stocks(f, resp.body);
        else
            parse_sports(f, resp.body);
        http_free(&resp);
        if (!f->error[0] && !f->n)
            ksnprintf(f->error, sizeof(f->error), f->kind == WK_SPORTS ? "No matches right now"
                                                  : "Nothing could be read from %s", host);
    }
    f->loaded = true;
    int minutes = f->error[0] ? 2 : f->kind == WK_NEWS ? 20 : f->kind == WK_STOCKS ? 5 : 3;
    f->next = hal_ticks_ms() + (uint64_t)minutes * 60000;
}

static struct feed *find(int kind, const char *opt, bool make)
{
    if (!cache && !(cache = kzalloc(sizeof(struct feed) * MAX_CACHE)))
        return NULL;
    struct feed *free_slot = NULL;
    for (int i = 0; i < MAX_CACHE; i++) {
        if (cache[i].kind == kind && cache[i].kind && !strcmp(cache[i].opt, opt))
            return &cache[i];
        if (!cache[i].kind && !free_slot)
            free_slot = &cache[i];
    }
    if (!make)
        return NULL;
    if (!free_slot)
        free_slot = &cache[0];
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->kind = kind;
    str_copy(free_slot->opt, opt, sizeof(free_slot->opt));
    return free_slot;
}

/* ---- the widgets ---- */

static int row_height(const struct widget *w)
{
    return w->kind == WK_NEWS && w->cw > 2 ? 36 : 22;
}

static int rows(const struct widget *w, struct rect r)
{
    return MAX(0, MIN(MAX_ITEMS, (r.h - 30) / row_height(w)));
}

static struct rect row(const struct widget *w, struct rect r, int i)
{
    return (struct rect){r.x, r.y + 28 + i * row_height(w), r.w, row_height(w)};
}

/* text cut with an ellipsis to fit */
static void fit(const struct ui_font *f, const char *s, int width, char *out, size_t cap)
{
    str_copy(out, s, cap);
    if (gfx_text_width(f, out) <= width)
        return;
    size_t n = strlen(out);
    while (n > 0) {
        n--;
        while (n > 0 && (out[n] & 0xc0) == 0x80)
            n--;
        out[n] = 0;
        char tmp[160];
        ksnprintf(tmp, sizeof(tmp), "%s…", out);
        if (gfx_text_width(f, tmp) <= width) {
            str_copy(out, tmp, cap);
            return;
        }
    }
}

static void paint(struct canvas *c, struct widget *w, struct rect r)
{
    struct feed *f = find(w->kind, w->opt, true);
    uint32_t ink = widget_ink(w), soft = widget_soft(w);
    const char *head = w->kind == WK_NEWS ? "News" : w->kind == WK_STOCKS ? "Stocks" : "Sports";
    if (f && f->loaded && f->title[0])
        head = f->title;
    char line[160];
    fit(&font_bold, head, r.w, line, sizeof(line));
    gfx_text(c, r.x, r.y + 2, &font_bold, line, ink);
    if (!f || !f->loaded) {
        widget_text(c, w, (struct rect){r.x, r.y + 28, r.w, r.h - 28}, &font_small, "Loading…", soft, ALIGN_CENTER);
        return;
    }
    if (!f->n) {
        ui_wrap_text(c, (struct rect){r.x, r.y + 30, r.w, r.h - 30}, &font_small, f->error, soft);
        return;
    }
    int n = MIN(rows(w, r), f->n);
    for (int i = 0; i < n; i++) {
        struct rect b = row(w, r, i);
        const struct item *it = &f->items[i];
        bool hot = widget_hover_w == w && widget_hover_part == i;
        if (hot)
            gfx_round(c, (struct rect){b.x - 4, b.y, b.w + 8, b.h - 2}, 6, ALPHA(ink, 28));
        if (w->kind == WK_STOCKS) {
            const char *pct = strchr(it->detail, ' ');
            char price[24];
            str_copy(price, it->detail, MIN(sizeof(price), pct ? (size_t)(pct - it->detail) + 1 : sizeof(price)));
            gfx_text(c, b.x, b.y + 3, &font_bold, it->title, ink);
            uint32_t col = it->trend > 0 ? RGB(52, 199, 89) : it->trend < 0 ? RGB(255, 69, 58) : soft;
            int pw = pct ? gfx_text_width(&font_small, pct + 1) : 0;
            if (pct)
                gfx_text(c, b.x + b.w - pw, b.y + 4, &font_small, pct + 1, col);
            if (b.w > 150)
                gfx_text(c, b.x + b.w - pw - 12 - gfx_text_width(&font_small, price), b.y + 4, &font_small, price,
                         soft);
        } else if (w->kind == WK_SPORTS) {
            int dw = MIN(gfx_text_width(&font_small, it->detail), b.w / 2);
            fit(&font_sans, it->title, b.w - dw - 10, line, sizeof(line));
            gfx_text(c, b.x, b.y + 3, &font_sans, line, ink);
            fit(&font_small, it->detail, b.w / 2, line, sizeof(line));
            gfx_text(c, b.x + b.w - gfx_text_width(&font_small, line), b.y + 4, &font_small, line, soft);
        } else {
            fit(&font_sans, it->title, b.w, line, sizeof(line));
            gfx_text(c, b.x, b.y + 2, &font_sans, line, ink);
            if (row_height(w) > 30 && it->detail[0])
                gfx_text(c, b.x, b.y + 18, &font_small, it->detail, soft);
        }
    }
}

static int parts(const struct widget *w, struct rect r, struct rect *out, int max)
{
    struct feed *f = find(w->kind, w->opt, false);
    int n = f ? MIN(MIN(rows(w, r), f->n), max) : 0;
    for (int i = 0; i < n; i++)
        out[i] = row(w, r, i);
    return n;
}

static void click(struct widget *w, int part, int clicks)
{
    struct feed *f = find(w->kind, w->opt, false);
    if (!f)
        return;
    if (part >= 0 && part < f->n && f->items[part].link[0]) {
        const struct gui_app *browser = gui_default_browser();
        if (browser)
            gui_app_launch(browser, f->items[part].link);
    } else if (part < 0 && clicks >= 2) {
        f->next = 0;
    }
}

static void tick(void)
{
    static uint64_t last;
    uint64_t now = hal_ticks_ms();
    if (now - last < 1000) /* look once a second, fetch at most one feed */
        return;
    last = now;
    int n;
    struct widget *list = widget_list(&n);
    for (int i = 0; i < n; i++) {
        struct widget *w = &list[i];
        if (!w->used || w->leaving || (w->kind != WK_NEWS && w->kind != WK_STOCKS && w->kind != WK_SPORTS))
            continue;
        struct feed *f = find(w->kind, w->opt, true);
        if (f && (!f->loaded || now >= f->next)) {
            load(f);
            for (int k = 0; k < n; k++)
                if (list[k].used && list[k].kind == w->kind && !strcmp(list[k].opt, w->opt))
                    widget_damage(&list[k]);
            return;
        }
    }
}

static const char *option_hint(int kind)
{
    return kind == WK_NEWS ? "RSS or Atom address" : kind == WK_STOCKS ? STOCKS_DEFAULT : SPORTS_DEFAULT;
}

static const struct widget_feeds service = {paint, parts, click, tick, option_hint};

/* ---- the command ---- */

COMMAND(feeds, "headlines", "net", "feeds news|stocks|sports [feed address | symbols... | league]",
        "what the News, Stocks and Sports widgets show: headlines, quotes or scores")
{
    if (argc < 2)
        return command_usage(sh, command_find("feeds"));
    int kind = !strcmp(argv[1], "news") ? WK_NEWS : !strcmp(argv[1], "stocks") ? WK_STOCKS
             : !strcmp(argv[1], "sports") ? WK_SPORTS : -1;
    if (kind < 0)
        return command_usage(sh, command_find("feeds"));
    struct feed *f = kzalloc(sizeof(*f));
    if (!f)
        return sh_error(sh, "feeds", "out of memory");
    f->kind = kind;
    for (int i = 2; i < argc; i++) {
        if (i > 2)
            str_append(f->opt, " ", sizeof(f->opt));
        str_append(f->opt, argv[i], sizeof(f->opt));
    }
    load(f);
    int rc = 0;
    if (!f->n) {
        rc = sh_error(sh, "feeds", "%s", f->error);
    } else {
        sh_printf(sh, "%s\n", f->title);
        for (int i = 0; i < f->n; i++)
            sh_printf(sh, "  %-40s %s\n", f->items[i].title, f->items[i].detail);
    }
    kfree(f);
    return rc;
}

static const struct command *const commands[] = {&cmddef_feeds};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, "widget-feeds",
                                      &service};
GUI_APP_FULL(NULL, NULL, &ext);
