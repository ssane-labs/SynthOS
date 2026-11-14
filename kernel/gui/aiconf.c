#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/gui.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

/* Settings → AI: one switch for all of the system's AI, then each part on its own. They live here, not in the
   Assistant module, so that turning AI off holds even with the module removed, and `config` knows them all. */

struct gui_ai_conf gui_ai = {.on = true, .barge = true, .speak = true, .complete = AI_COMPLETE_ALL, .allow = AI_ALLOW_LOOK,
                             .voice = "ryan"};

static const char *const inputs[] = {"smart", "voice", "text", "both", NULL};
static const char *const answers[] = {"subtitles", "voice", "chat", NULL};
static const char *const styles[] = {"friendly", "brief", "detailed", NULL};
static const char *const allows[] = {"ask", "look", "all", NULL};
static const char *const speeds[] = {"fast", "exact", NULL};
static const char *const models[] = {"auto", "main", "small", NULL};
static const char *const kinds[] = {"text", "code", "url", "address", NULL};

static int pick(const char *const *names, const char *value, uint8_t *out)
{
    for (int i = 0; names[i]; i++)
        if (!str_casecmp(names[i], value)) {
            *out = (uint8_t)i;
            return 0;
        }
    return E_INVAL;
}

#define AI_FLAG(id, field, key, help)                                                                                  \
    static void get_##id(char *buf, size_t cap)                                                                        \
    {                                                                                                                  \
        setting_flag_get(gui_ai.field, buf, cap);                                                                      \
    }                                                                                                                  \
    static int set_##id(struct shell *sh, const char *value)                                                           \
    {                                                                                                                  \
        UNUSED(sh);                                                                                                    \
        return setting_flag(value, &gui_ai.field);                                                                     \
    }                                                                                                                  \
    static const struct setting s_##id = {key, "on|off", help, get_##id, set_##id};                                   \
    SETTING(s_##id)

#define AI_CHOICE(id, field, key, names, values, help)                                                                 \
    static void get_##id(char *buf, size_t cap)                                                                        \
    {                                                                                                                  \
        str_copy(buf, names[gui_ai.field], cap);                                                                       \
    }                                                                                                                  \
    static int set_##id(struct shell *sh, const char *value)                                                           \
    {                                                                                                                  \
        UNUSED(sh);                                                                                                    \
        return pick(names, value, &gui_ai.field);                                                                      \
    }                                                                                                                  \
    static const struct setting s_##id = {key, values, help, get_##id, set_##id};                                      \
    SETTING(s_##id)

AI_FLAG(on, on, "ai", "all AI in the system: the assistant, its icon and shortcut, Hey Synth and Tab completion");
AI_FLAG(wake, wake, "aiwake", "the assistant listens for \"Hey Synth\"");
AI_FLAG(barge, barge, "aibarge", "talking while the assistant speaks pauses it, and more talking stops it");
AI_FLAG(speak, speak, "aispeak", "answers to spoken questions are said aloud");
AI_CHOICE(input, input, "aiinput", inputs, "smart|voice|text|both",
          "how to ask: by voice, typing, both, or smart (voice when the microphone hears anything)");
AI_CHOICE(answer, answer, "aianswer", answers, "subtitles|voice|chat", "how answers are shown");
AI_CHOICE(style, style, "aistyle", styles, "friendly|brief|detailed", "how the assistant talks");
AI_CHOICE(allow, allow, "aiallow", allows, "ask|look|all",
          "what the assistant may do without asking: nothing, only look, or everything");
AI_CHOICE(speed, speed, "aispeed", speeds, "fast|exact", "8-bit integer products (fast) or floats everywhere (exact)");
AI_CHOICE(model, model, "aimodel", models, "auto|main|small", "which language model: by memory, MiniCPM5 or Qwen2.5 0.5B");

static void get_voice(char *buf, size_t cap)
{
    str_copy(buf, gui_ai.voice, cap);
}

static int set_voice(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!*value || strlen(value) >= sizeof(gui_ai.voice) || strchr(value, ' '))
        return E_INVAL;
    str_copy(gui_ai.voice, value, sizeof(gui_ai.voice));
    return 0;
}

static const struct setting s_voice = {"aivoice", "ryan|aiden|eric|dylan|serena|vivian|uncle_fu|ono_anna|sohee",
                                       "the assistant's voice", get_voice, set_voice};
SETTING(s_voice);

static void get_complete(char *buf, size_t cap)
{
    buf[0] = 0;
    for (int i = 0; kinds[i]; i++)
        if (gui_ai.complete & (1u << i)) {
            if (buf[0])
                str_append(buf, ",", cap);
            str_append(buf, kinds[i], cap);
        }
    if (!buf[0])
        str_copy(buf, "off", cap);
}

static int set_complete(struct shell *sh, const char *value)
{
    UNUSED(sh);
    uint8_t mask = 0;
    if (!strcmp(value, "off")) {
        gui_ai.complete = 0;
        return 0;
    }
    if (!strcmp(value, "on") || !strcmp(value, "all")) {
        gui_ai.complete = AI_COMPLETE_ALL;
        return 0;
    }
    for (const char *p = value; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char word[16];
        uint8_t k;
        str_copy(word, p, MIN(n + 1, sizeof(word)));
        if (pick(kinds, word, &k))
            return E_INVAL;
        mask |= (uint8_t)(1u << k);
        p += n + (e ? 1 : 0);
    }
    gui_ai.complete = mask;
    return 0;
}

static const struct setting s_complete = {"aicomplete", "off|all|text,code,url,address",
                                          "where Tab completion offers the rest", get_complete, set_complete};
SETTING(s_complete);
