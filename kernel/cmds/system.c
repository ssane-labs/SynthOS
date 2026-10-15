#include <synth/codepage.h>
#include <synth/command.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>
#include <synth/vmm.h>

static const char *const categories[] = {"files", "text", "shell", "system", "custom", "dev"};

struct help_ctx {
    const struct command *items[256];
    size_t count;
    size_t width;
};

static void help_collect(const struct command *cmd, void *ctx)
{
    struct help_ctx *h = ctx;
    if (h->count < ARRAY_SIZE(h->items))
        h->items[h->count++] = cmd;
    h->width = MAX(h->width, strlen(cmd->name));
}

static void help_sort(struct help_ctx *h)
{
    for (size_t i = 1; i < h->count; i++) {
        const struct command *c = h->items[i];
        size_t j = i;
        while (j > 0 && strcmp(h->items[j - 1]->name, c->name) > 0) {
            h->items[j] = h->items[j - 1];
            j--;
        }
        h->items[j] = c;
    }
}

static void help_detail(struct shell *sh, const struct command *cmd)
{
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%s", cmd->name);
    sh_reset(sh);
    sh_printf(sh, " - %s\n", cmd->summary);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "  usage:    ");
    sh_reset(sh);
    sh_printf(sh, "%s\n", cmd->usage);
    if (cmd->aliases) {
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "  aliases:  ");
        sh_reset(sh);
        sh_printf(sh, "%s\n", cmd->aliases);
    }
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "  category: ");
    sh_reset(sh);
    sh_printf(sh, "%s\n", cmd->category);
}

COMMAND(help, "man,?", "system", "help [command]", "list commands or describe one")
{
    if (argc > 1) {
        const struct command *cmd = command_find(argv[1]);
        if (!cmd)
            return sh_error(sh, "help", "no such command: %s", argv[1]);
        help_detail(sh, cmd);
        return 0;
    }
    static struct help_ctx h;
    h.count = h.width = 0;
    command_foreach(help_collect, &h);
    help_sort(&h);
    bool console = stream_is_console(sh->out);
    size_t width = h.width + 1;
    size_t per_row = MAX((size_t)1, (size_t)(con_cols() - 2) / (width + 2));
    for (size_t c = 0; c < ARRAY_SIZE(categories); c++) {
        size_t col = 0;
        if (console) {
            sh_role(sh, ROLE_ACCENT);
            sh_printf(sh, "%c%s\n", to_upper(categories[c][0]), categories[c] + 1);
            sh_reset(sh);
        }
        for (size_t i = 0; i < h.count; i++) {
            const struct command *cmd = h.items[i];
            if (strcmp(cmd->category, categories[c]) != 0)
                continue;
            if (!console) {
                sh_printf(sh, "%-12s %s\n", cmd->name, cmd->summary);
                continue;
            }
            sh_role(sh, ROLE_SYNTAX_COMMAND);
            sh_printf(sh, "  %-*s", (int)width, cmd->name);
            sh_reset(sh);
            if (++col % per_row == 0)
                sh_puts(sh, "\n");
        }
        if (console && col % per_row)
            sh_puts(sh, "\n");
    }
    if (console) {
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "\n%u commands. 'help <command>' shows details. Tab completes, Up/Down browse history.\n",
                  (unsigned)h.count);
        sh_reset(sh);
    }
    return 0;
}

COMMAND(clear, "cls", "system", "clear", "clear the screen")
{
    con_clear();
    con_update_status();
    con_flush();
    return 0;
}

COMMAND(ver, "uname,version", "system", "ver", "show the system version")
{
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%s %s", SYNTH_NAME, SYNTH_VERSION);
    sh_reset(sh);
    sh_printf(sh, " \"%s\" on %s\n", SYNTH_CODENAME, hal_arch());
    return 0;
}

static void format_uptime(char *buf, size_t cap)
{
    uint64_t s = hal_ticks_ms() / 1000;
    unsigned d = (unsigned)(s / 86400), h = (unsigned)(s / 3600 % 24), m = (unsigned)(s / 60 % 60);
    if (d)
        ksnprintf(buf, cap, "%ud %uh %um", d, h, m);
    else
        ksnprintf(buf, cap, "%uh %um %us", h, m, (unsigned)(s % 60));
}

static void count_command(const struct command *cmd, void *ctx)
{
    UNUSED(cmd);
    (*(unsigned *)ctx)++;
}

static void collect_format(const struct binfmt *fmt, void *ctx)
{
    char *buf = ctx;
    if (*buf)
        str_append(buf, ", ", 64);
    str_append(buf, fmt->name, 64);
}

#define LB "\xdc"
#define UB "\xdf"
#define FB "\xdb"

COMMAND(fetch, "sysinfo,neofetch", "system", "fetch", "show system information with style")
{
    static const char *const art[] = {
        "     " LB LB LB LB LB LB LB "     ",
        "  " LB FB FB FB FB FB FB FB FB FB FB FB LB "  ",
        " " FB FB FB FB FB FB FB FB FB FB FB FB FB FB FB " ",
        " " UB UB UB UB UB UB UB UB UB UB UB UB UB UB UB " ",
        "  " UB UB UB UB UB UB UB UB UB UB UB UB UB "  ",
        "    " UB UB UB UB UB UB UB UB UB "    ",
        "\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd",
        "  /  /  \xb3  \\  \\  ",
        " /  /   \xb3   \\  \\ ",
    };
    static const enum theme_role art_roles[] = {ROLE_LOGO1, ROLE_LOGO1, ROLE_LOGO1, ROLE_LOGO2, ROLE_LOGO2,
                                                ROLE_LOGO2, ROLE_LOGO3, ROLE_LOGO3, ROLE_LOGO3};
    char lines[12][96];
    const char *labels[12];
    int n = 0;
    struct heap_stats hs;
    heap_get_stats(&hs);
    char used[16], total[16], uptime[32], formats[64] = "";
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(total, sizeof(total), hs.total);
    format_uptime(uptime, sizeof(uptime));
    unsigned cmds = 0;
    command_foreach(count_command, &cmds);
    exec_foreach_format(collect_format, formats);
    const char *user = sh_get(sh, "USER"), *host = sh_get(sh, "HOSTNAME");

    labels[n] = "OS";
    ksnprintf(lines[n++], 96, "%s %s \"%s\"", SYNTH_NAME, SYNTH_VERSION, SYNTH_CODENAME);
    labels[n] = "Arch";
    ksnprintf(lines[n++], 96, "%s", hal_arch());
    labels[n] = "CPU";
    ksnprintf(lines[n++], 96, "%.40s", hal_machine());
    labels[n] = "Uptime";
    ksnprintf(lines[n++], 96, "%s", uptime);
    labels[n] = "Memory";
    ksnprintf(lines[n++], 96, "%s / %s", used, total);
    labels[n] = "Display";
    ksnprintf(lines[n++], 96, "%s %dx%d", con_display()->name, con_cols(), con_rows());
    labels[n] = "Theme";
    ksnprintf(lines[n++], 96, "%s, cp%d", theme_current()->name, codepage_current());
    labels[n] = "Shell";
    ksnprintf(lines[n++], 96, "synsh, %u commands", cmds);
    labels[n] = "Runs";
    ksnprintf(lines[n++], 96, "%s", formats);

    bool art_on = stream_is_console(sh->out) && con_cols() >= 64;
    int rows = MAX(n + 2, art_on ? (int)ARRAY_SIZE(art) : 0);
    for (int r = 0; r < rows + 1; r++) {
        if (art_on) {
            int a = r - 0;
            sh_puts(sh, " ");
            if (a >= 0 && a < (int)ARRAY_SIZE(art)) {
                sh_role(sh, art_roles[a]);
                sh_puts(sh, art[a]);
                sh_reset(sh);
            } else {
                sh_puts(sh, "                 ");
            }
            sh_puts(sh, "   ");
        }
        if (r == 0) {
            sh_role(sh, ROLE_PROMPT_USER);
            sh_printf(sh, "%s", user ? user : "root");
            sh_role(sh, ROLE_MUTED);
            sh_puts(sh, "@");
            sh_role(sh, ROLE_PROMPT_HOST);
            sh_printf(sh, "%s", host ? host : "synth");
            sh_reset(sh);
        } else if (r == 1) {
            sh_role(sh, ROLE_MUTED);
            sh_puts(sh, "\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4");
            sh_reset(sh);
        } else if (r - 2 < n) {
            sh_role(sh, ROLE_ACCENT);
            sh_printf(sh, "%-8s", labels[r - 2]);
            sh_reset(sh);
            sh_puts(sh, lines[r - 2]);
        } else if (r - 2 == n && stream_is_console(sh->out)) {
            for (int c = 0; c < 16; c++) {
                con_set_attr(ATTR(c, c));
                sh_puts(sh, "  ");
            }
            sh_reset(sh);
        }
        sh_puts(sh, "\n");
    }
    return 0;
}

static void bar(struct shell *sh, size_t used, size_t total, int width)
{
    int filled = total ? (int)((uint64_t)used * (uint64_t)width / total) : 0;
    sh_role(sh, ROLE_ACCENT);
    for (int i = 0; i < width; i++) {
        if (i == filled)
            sh_role(sh, ROLE_MUTED);
        sh_puts(sh, i < filled ? "\xdb" : "\xb0");
    }
    sh_reset(sh);
}

COMMAND(mem, "free", "system", "mem", "show memory usage")
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    char used[16], free[16], total[16], peak[16], largest[16];
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(free, sizeof(free), hs.free);
    fmt_size(total, sizeof(total), hs.total);
    fmt_size(peak, sizeof(peak), hs.peak);
    fmt_size(largest, sizeof(largest), hs.largest_free);
    sh_printf(sh, "Memory  ");
    bar(sh, hs.used, hs.total, 30);
    sh_printf(sh, " %u%%\n", (unsigned)(hs.total ? hs.used * 100 / hs.total : 0));
    sh_printf(sh, "  used %s, free %s, total %s\n  peak %s, largest free block %s, %u blocks\n", used, free,
              total, peak, largest, (unsigned)hs.blocks);
    uint64_t high_total = (uint64_t)vmm_high_total_pages() * 4096, high_free = (uint64_t)vmm_high_pages() * 4096;
    if (high_total) {
        fmt_size(free, sizeof(free), high_free);
        fmt_size(total, sizeof(total), high_total);
        sh_printf(sh, "Above 4G");
        bar(sh, high_total - high_free, high_total, 30);
        sh_printf(sh, " %u%%\n  free %s of %s, used for Windows programs\n",
                  (unsigned)((high_total - high_free) * 100 / high_total), free, total);
    }
    sh_printf(sh, "Paging  %s\n", vmm_pae() ? "PAE (64-bit page tables, physical memory up to 64 GB)" : "32-bit");
    return 0;
}

COMMAND(uptime, NULL, "system", "uptime", "time since boot")
{
    char buf[32];
    format_uptime(buf, sizeof(buf));
    sh_printf(sh, "up %s\n", buf);
    return 0;
}

COMMAND(date, NULL, "system", "date", "show the current date and time")
{
    struct rtc_time t;
    hal_rtc(&t);
    sh_printf(sh, "%s %s %2u %02u:%02u:%02u %04u\n", time_weekday_name(time_weekday(&t)),
              time_month_name(t.month), t.day, t.hour, t.minute, t.second, t.year);
    return 0;
}

int run_argv(struct shell *sh, int argc, char **argv);

COMMAND(time, NULL, "system", "time command [args...]", "measure how long a command takes")
{
    if (argc < 2)
        return command_usage(sh, command_find("time"));
    uint64_t start = hal_ticks_ms();
    int rc = run_argv(sh, argc - 1, argv + 1);
    uint64_t ms = hal_ticks_ms() - start;
    sh_role(sh, ROLE_MUTED);
    stream_printf(sh->err, "real %u.%03us\n", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    sh_reset(sh);
    return rc;
}

static void sift(uint32_t *a, uint32_t root, uint32_t n)
{
    for (uint32_t child; (child = 2 * root + 1) < n; root = child) {
        if (child + 1 < n && a[child + 1] > a[child])
            child++;
        if (a[root] >= a[child])
            return;
        uint32_t t = a[root];
        a[root] = a[child];
        a[child] = t;
    }
}

static void sort_u32(uint32_t *a, uint32_t n)
{
    for (uint32_t i = n / 2; i-- > 0;)
        sift(a, i, n);
    for (uint32_t end = n; end-- > 1;) {
        uint32_t t = a[0];
        a[0] = a[end];
        a[end] = t;
        sift(a, 0, end);
    }
}

COMMAND(prof, NULL, "dev", "prof [-o file] command [args...]",
        "run a command and list where the CPU spent its time (addresses for addr2line)")
{
    const char *report = NULL;
    if (argc > 3 && !strcmp(argv[1], "-o")) {
        report = argv[2];
        argc -= 2;
        argv += 2;
    }
    if (argc < 2)
        return command_usage(sh, command_find("prof"));
    uint32_t cap = 1u << 18;
    uint32_t *buf = kmalloc(cap * sizeof(uint32_t));
    if (!buf)
        return sh_error(sh, "prof", "out of memory");
    hal_profile_start(buf, cap);
    int rc = run_argv(sh, argc - 1, argv + 1);
    uint32_t n = hal_profile_stop();
    sort_u32(buf, n);
    char *text = kmalloc(8192);
    size_t len = 0;
    if (!text) {
        kfree(buf);
        return rc;
    }
    uint32_t user = 0;
    for (uint32_t i = 0; i < n; i++)
        user += buf[i] >= VMM_USER_START && buf[i] < VMM_USER_END;
    len += (size_t)ksnprintf(text + len, 8192 - len, "prof: %u samples, %u in the kernel, %u in programs\n", n, n - user,
                             user);
    for (int k = 0; k < 60; k++) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0, j; i < n; i = j) {
            for (j = i; j < n && buf[j] == buf[i];)
                j++;
            if (j - i > best && buf[i] != 0xffffffffu) {
                best = j - i;
                at = i;
            }
        }
        if (!best)
            break;
        len += (size_t)ksnprintf(text + len, 8192 - len, "prof %08x %u\n", buf[at], best);
        for (uint32_t i = at; i < at + best; i++)
            buf[i] = 0xffffffffu;
    }
    kfree(buf);
    char path[PATH_MAX_LEN];
    if (report && sh_resolve(sh, report, path, sizeof(path)) == 0)
        vfs_write_all(path, text, len);
    else
        sh_puts(sh, text);
    kfree(text);
    return rc;
}

COMMAND(sleep, NULL, "system", "sleep seconds|<n>ms", "pause for a while")
{
    long n;
    if (argc != 2)
        return command_usage(sh, command_find("sleep"));
    char buf[32];
    str_copy(buf, argv[1], sizeof(buf));
    bool ms = str_ends(buf, "ms");
    if (ms)
        buf[strlen(buf) - 2] = 0;
    if (!str_parse_long(buf, &n) || n < 0)
        return command_usage(sh, command_find("sleep"));
    uint64_t end = hal_ticks_ms() + (uint64_t)(ms ? n : n * 1000);
    while (hal_ticks_ms() < end) {
        if (sh_interrupted())
            return 130;
        hal_sleep_ms((uint32_t)MIN(end - hal_ticks_ms(), (uint64_t)20));
    }
    return 0;
}

COMMAND(reboot, "restart", "system", "reboot", "restart the computer")
{
    UNUSED(sh);
    con_clear();
    hal_reboot();
}

COMMAND(shutdown, "poweroff,halt", "system", "shutdown", "power off the computer")
{
    history_save(&sh->history, "/home/.history");
    hal_poweroff();
}

COMMAND(exit, "quit", "shell", "exit [code]", "leave the current script (or shut down)")
{
    long code = sh->status;
    if (argc > 1 && !str_parse_long(argv[1], &code))
        return command_usage(sh, command_find("exit"));
    sh->status = (int)code;
    sh->ctl = CTL_EXIT;
    return (int)code;
}

COMMAND(history, NULL, "shell", "history [-c]", "show or clear command history")
{
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        for (size_t i = 0; i < sh->history.count; i++)
            kfree(sh->history.items[i]);
        sh->history.count = 0;
        return 0;
    }
    for (size_t i = 0; i < sh->history.count; i++) {
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%5u  ", (unsigned)i + 1);
        sh_reset(sh);
        sh_printf(sh, "%s\n", sh->history.items[i]);
    }
    return 0;
}

COMMAND(dmesg, "log", "system", "dmesg", "show the kernel log")
{
    char *buf = kmalloc(CONFIG_LOG_SIZE + 1);
    if (!buf)
        return 1;
    log_snapshot(buf, CONFIG_LOG_SIZE + 1);
    sh_puts(sh, buf);
    kfree(buf);
    return 0;
}

COMMAND(beep, NULL, "system", "beep [hz] [ms]", "play a tone on the speaker")
{
    long hz = 880, ms = 120;
    if (argc > 1 && !str_parse_long(argv[1], &hz))
        return command_usage(sh, command_find("beep"));
    if (argc > 2 && !str_parse_long(argv[2], &ms))
        return command_usage(sh, command_find("beep"));
    hal_beep((uint32_t)hz, (uint32_t)ms);
    return 0;
}

COMMAND(keytest, NULL, "dev", "keytest", "show key codes (Esc to quit)")
{
    sh_printf(sh, "Press keys, Esc to quit.\n");
    input_set_break(false);
    for (;;) {
        int key = input_getkey(-1);
        if (key == KEY_ESC || key == KEY_EOF)
            break;
        char name[32];
        sh_role(sh, ROLE_ACCENT2);
        sh_printf(sh, "%-16s", key_name(key, name, sizeof(name)));
        sh_reset(sh);
        sh_printf(sh, " code 0x%04x\n", key);
    }
    input_set_break(true);
    return 0;
}

static uint32_t bench_cpu(uint32_t ms)
{
    uint64_t end = hal_ticks_ms() + ms;
    uint32_t x = 2463534242u, rounds = 0;
    while (hal_ticks_ms() < end) {
        for (int i = 0; i < 10000; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        rounds++;
    }
    return rounds + (x & 0);
}

COMMAND(bench, NULL, "dev", "bench", "quick CPU and memory benchmark")
{
    sh_printf(sh, "CPU     ");
    uint32_t rounds = bench_cpu(500);
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%u", rounds * 2 * 10000 / 1000000);
    sh_reset(sh);
    sh_printf(sh, " M xorshift ops/s\n");

    size_t size = 1 << 20;
    uint8_t *a = kmalloc(size), *b = kmalloc(size);
    if (a && b) {
        memset(a, 0x5a, size);
        uint64_t start = hal_ticks_ms(), copied = 0;
        while (hal_ticks_ms() - start < 500) {
            memcpy(b, a, size);
            copied += size;
        }
        uint64_t ms = MAX(hal_ticks_ms() - start, (uint64_t)1);
        sh_printf(sh, "Memory  ");
        sh_role(sh, ROLE_ACCENT);
        sh_printf(sh, "%u", (unsigned)(copied * 1000 / ms / (1024 * 1024)));
        sh_reset(sh);
        sh_printf(sh, " MB/s memcpy\n");
    }
    kfree(a);
    kfree(b);
    return 0;
}

static void print_format(const struct binfmt *fmt, void *ctx)
{
    struct shell *sh = ctx;
    sh_role(sh, ROLE_ACCENT2);
    sh_printf(sh, "  %-8s", fmt->name);
    sh_reset(sh);
    sh_printf(sh, " %-28s ", fmt->description);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "%s\n", fmt->extensions);
    sh_reset(sh);
}

COMMAND(formats, NULL, "dev", "formats", "list supported program formats")
{
    exec_foreach_format(print_format, sh);
    return 0;
}

COMMAND(whoami, NULL, "system", "whoami", "print the current user name")
{
    const char *user = sh_get(sh, "USER");
    sh_printf(sh, "%s\n", user ? user : "root");
    return 0;
}

COMMAND(random, "rand", "system", "random [max]", "print a random number (0..max-1)")
{
    long max = 0;
    if (argc > 1 && (!str_parse_long(argv[1], &max) || max <= 0))
        return command_usage(sh, command_find("random"));
    uint32_t v = hal_entropy();
    sh_printf(sh, "%u\n", max ? v % (uint32_t)max : v);
    return 0;
}

COMMAND(cal, "calendar", "system", "cal [month [year]]", "show a calendar")
{
    struct rtc_time now;
    hal_rtc(&now);
    long month = now.month, year = now.year;
    if ((argc > 1 && (!str_parse_long(argv[1], &month) || month < 1 || month > 12)) ||
        (argc > 2 && (!str_parse_long(argv[2], &year) || year < 1970 || year > 2100)))
        return command_usage(sh, command_find("cal"));
    struct rtc_time first = {(uint16_t)year, (uint8_t)month, 1, 0, 0, 0};
    int start = time_weekday(&first);
    struct rtc_time next = first;
    next.month = (uint8_t)(month % 12 + 1);
    next.year = (uint16_t)(month == 12 ? year + 1 : year);
    int days = (int)((time_from_rtc(&next) - time_from_rtc(&first)) / 86400);
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "      %s %ld\n", time_month_name((int)month), year);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, " Su Mo Tu We Th Fr Sa\n");
    sh_reset(sh);
    for (int i = 0; i < start; i++)
        sh_puts(sh, "   ");
    for (int d = 1; d <= days; d++) {
        bool today = d == now.day && month == now.month && year == now.year;
        if (today)
            sh_role(sh, ROLE_SELECTION);
        sh_printf(sh, "%3d", d);
        if (today)
            sh_reset(sh);
        if ((start + d) % 7 == 0 || d == days)
            sh_puts(sh, "\n");
    }
    return 0;
}

COMMAND(displays, "screens", "system", "displays", "list displays and the resolutions they support")
{
    UNUSED(argc);
    UNUSED(argv);
    int count = hal_screen_count();
    if (!count) {
        sh_puts(sh, "no displays (text console only)\n");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        struct display_mode modes[64];
        int n = hal_screen_modes(i, modes, (int)ARRAY_SIZE(modes));
        sh_role(sh, ROLE_ACCENT);
        sh_printf(sh, "%d: %s\n", i + 1, hal_screen_name(i));
        sh_role(sh, ROLE_MUTED);
        for (int m = 0; m < n; m++)
            sh_printf(sh, "%s%ux%u", m ? (m % 8 ? "  " : "\n   ") : "   ", modes[m].width, modes[m].height);
        sh_puts(sh, "\n");
        sh_role(sh, ROLE_TEXT);
    }
    return 0;
}

COMMAND(lsusb, "usb", "system", "lsusb", "list USB devices")
{
    UNUSED(argc);
    UNUSED(argv);
    static const char *const speeds[] = {"?", "12 Mb/s", "1.5 Mb/s", "480 Mb/s", "5 Gb/s", "10 Gb/s"};
    struct usb_info list[32];
    int n = hal_usb_devices(list, (int)ARRAY_SIZE(list));
    if (!n) {
        sh_puts(sh, "no USB devices\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        const struct usb_info *u = &list[i];
        sh_printf(sh, "%s port %d%*s ", u->host, u->port, u->depth * 2, "");
        sh_role(sh, ROLE_ACCENT);
        sh_printf(sh, "%04x:%04x ", u->vendor, u->product);
        sh_role(sh, ROLE_TEXT);
        sh_printf(sh, "%-32s", u->name);
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, " %-8s %s\n", u->speed >= 0 && u->speed < (int)ARRAY_SIZE(speeds) ? speeds[u->speed] : "?",
                  u->driver[0] ? u->driver : "-");
        sh_role(sh, ROLE_TEXT);
    }
    return 0;
}
