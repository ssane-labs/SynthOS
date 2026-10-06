#include <synth/config.h>
#include <synth/console.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/string.h>

#include "internal.h"

static const struct {
    const char *name;
    const char *format;
} presets[] = {
    {"neon", "%{prompt.user}%u%{muted}@%{prompt.host}%h %{prompt.path}%w %e%{prompt.symbol}%>%{/} "},
    {"minimal", "%e%{prompt.symbol}%>%{/} "},
    {"arrow", "%{prompt.path}%W %e%{prompt.symbol}\x1a%{/} "},
    {"classic", "%{text}%w%{prompt.symbol}$%{/} "},
    {"dos", "%{text}%D>%{/}"},
    {"twoline", "%{muted}\xda\xc4[%{prompt.user}%u%{muted}@%{prompt.host}%h%{muted}]\xc4[%{prompt.path}%w"
                "%{muted}]\xc4[%{accent2}%t%{muted}]%n\xc0\xc4%e%{prompt.symbol}%>%{/} "},
    {"blocks", "%{status.accent} %u %{status} %w %{/}%e%{prompt.symbol} %>%{/} "},
    {"clock", "%{muted}[%{accent2}%T%{muted}] %{prompt.path}%W %e%{prompt.symbol}%>%{/} "},
};

const char *prompt_preset(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(presets); i++)
        if (strcmp(presets[i].name, name) == 0)
            return presets[i].format;
    return NULL;
}

void prompt_presets(void (*fn)(const char *name, const char *format, void *ctx), void *ctx)
{
    for (size_t i = 0; i < ARRAY_SIZE(presets); i++)
        fn(presets[i].name, presets[i].format, ctx);
}

static void display_path(struct shell *sh, char *out, size_t cap)
{
    const char *home = sh_get(sh, "HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && strncmp(sh->cwd, home, hl) == 0 && (sh->cwd[hl] == 0 || sh->cwd[hl] == '/'))
        ksnprintf(out, cap, "~%s", sh->cwd + hl);
    else
        str_copy(out, sh->cwd, cap);
}

static void dos_path(struct shell *sh, char *out, size_t cap)
{
    ksnprintf(out, cap, "C:%s", sh->cwd);
    for (char *p = out; *p; p++)
        *p = *p == '/' ? '\\' : (char)to_upper((uint8_t)*p);
}

void prompt_render(struct shell *sh)
{
    const char *fmt = sh_get(sh, "PROMPT");
    if (!fmt || !*fmt)
        fmt = presets[0].format;
    char buf[PATH_MAX_LEN + 16];
    struct rtc_time t;
    con_reset();
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) {
            con_putc(*p);
            continue;
        }
        p++;
        buf[0] = 0;
        switch (*p) {
        case 'u':
            str_copy(buf, sh_get(sh, "USER") ? sh_get(sh, "USER") : CONFIG_USER, sizeof(buf));
            break;
        case 'h':
            str_copy(buf, sh_get(sh, "HOSTNAME") ? sh_get(sh, "HOSTNAME") : CONFIG_HOSTNAME, sizeof(buf));
            break;
        case 'w':
            display_path(sh, buf, sizeof(buf));
            break;
        case 'W':
            str_copy(buf, strcmp(sh->cwd, "/") == 0 ? "/" : path_basename(sh->cwd), sizeof(buf));
            break;
        case 'D':
            dos_path(sh, buf, sizeof(buf));
            break;
        case 't':
        case 'T':
            hal_rtc(&t);
            if (*p == 't')
                ksnprintf(buf, sizeof(buf), "%02u:%02u", t.hour, t.minute);
            else
                ksnprintf(buf, sizeof(buf), "%02u:%02u:%02u", t.hour, t.minute, t.second);
            break;
        case 'd':
            hal_rtc(&t);
            ksnprintf(buf, sizeof(buf), "%04u-%02u-%02u", t.year, t.month, t.day);
            break;
        case '?':
            ksnprintf(buf, sizeof(buf), "%d", sh->status);
            break;
        case 'e':
            if (sh->status != 0) {
                uint8_t saved = con_attr();
                con_role(ROLE_ERROR);
                kprintf("%d ", sh->status);
                con_set_attr(saved);
            }
            continue;
        case 'v':
            str_copy(buf, SYNTH_VERSION, sizeof(buf));
            break;
        case 'n':
            str_copy(buf, "\n", sizeof(buf));
            break;
        case '>':
            str_copy(buf, "\x10", sizeof(buf));
            break;
        case '%':
            str_copy(buf, "%", sizeof(buf));
            break;
        case '{': {
            const char *end = strchr(p, '}');
            if (!end) {
                con_putc('%');
                con_putc('{');
                continue;
            }
            char name[24];
            size_t n = MIN((size_t)(end - p - 1), sizeof(name) - 1);
            memcpy(name, p + 1, n);
            name[n] = 0;
            p = end;
            int role = theme_role_lookup(name);
            if (strcmp(name, "/") == 0 || strcmp(name, "reset") == 0)
                con_reset();
            else if (role >= 0)
                con_role((enum theme_role)role);
            continue;
        }
        default:
            buf[0] = '%';
            buf[1] = *p;
            buf[2] = 0;
            break;
        }
        con_puts(buf);
    }
    con_reset();
}
