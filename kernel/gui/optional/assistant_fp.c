/* Assistant: a language model that runs on this computer, nothing goes to the network. The model is a GGUF file
   (the format of llama.cpp) of the Llama, Qwen2 or Qwen3 kind, kept in /home/models; the engine is in assistant/.
   It does floating point, so this module is a *_fp.c file. */
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/http.h>
#include <synth/keys.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

#include "../internal.h"

#include "assistant/gguf.h"
#include "assistant/quant.h"
#include "assistant/tok.h"
#include "assistant/llm.h"
#include "assistant/whisper.h"

#define MODELS_DIR "/home/models"
#define CTX_DEFAULT 4096
/* the model the assistant downloads: Qwen2.5 0.5B Instruct (Apache-2.0), 4-bit, 470 MB */
struct download {
    const char *name, *url;
    uint32_t bytes;
};
static const struct download chat_model = {
    "qwen2.5-0.5b-instruct-q4_k_m.gguf",
    "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf",
    491400032u};
/* speech recognition: Whisper base (MIT), 8-bit, 78 MB, from the whisper.cpp project */
static const struct download speech_model = {
    "ggml-base-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base-q8_0.bin", 81768585u};

static struct ai_model model;
static bool loaded;

#include "assistant/agent.h"

/* the first .gguf file in the models folder */
static bool default_model(char *out, size_t cap)
{
    struct vnode *dir;
    if (vfs_lookup(MODELS_DIR, &dir))
        return false;
    bool found = false;
    struct vfs_dirent e;
    for (uint32_t i = 0; !found && dir->ops->readdir && !dir->ops->readdir(dir, i, &e); i++)
        if (str_iends(e.name, ".gguf")) {
            ksnprintf(out, cap, MODELS_DIR "/%s", e.name);
            found = true;
        }
    vnode_put(dir);
    return found;
}

static int load(struct shell *sh, const char *path, char *why, size_t cap)
{
    struct vstat st;
    int err = vfs_stat(path, &st);
    if (err) {
        ksnprintf(why, cap, "%s: %s", path, err_str(err));
        return err;
    }
    if (loaded && !strcmp(model.path, path))
        return 0;
    if (loaded) {
        model_free(&model);
        loaded = false;
    }
    uint8_t *buf = kmalloc(st.size ? st.size : 1);
    if (!buf) {
        char size[16];
        fmt_size(size, sizeof(size), st.size);
        ksnprintf(why, cap, "not enough memory to load %s (%s)", path, size);
        return E_NOMEM;
    }
    struct file *f;
    err = vfs_open(path, VFS_READ, &f);
    size_t got = 0;
    uint64_t last = hal_ticks_ms();
    while (!err && got < st.size) {
        long n = vfs_read(f, buf + got, MIN((size_t)(1u << 20), st.size - got));
        if (n <= 0) {
            err = n < 0 ? (int)n : E_IO;
            break;
        }
        got += (size_t)n;
        if (sh && hal_ticks_ms() - last > 2000) {
            sh_printf(sh, "\rloading %s: %u%%", path, (unsigned)((uint64_t)got * 100 / st.size));
            last = hal_ticks_ms();
        }
    }
    if (!err)
        vfs_close(f);
    if (err) {
        kfree(buf);
        ksnprintf(why, cap, "%s: %s", path, err_str(err));
        return err;
    }
    err = model_load_fit(&model, buf, st.size, CTX_DEFAULT, why, cap);
    if (err) {
        kfree(buf);
        return err;
    }
    str_copy(model.path, path, sizeof(model.path));
    loaded = true;
    return 0;
}

/* the loaded model, or the default one loaded now */
static bool ensure_model(struct shell *sh)
{
    if (loaded)
        return true;
    char path[160], why[160];
    if (!default_model(path, sizeof(path))) {
        sh_printf(sh, "ai: no model; put a .gguf file in " MODELS_DIR " or run: ai get\n");
        return false;
    }
    if (load(sh, path, why, sizeof(why))) {
        sh_printf(sh, "ai: %s\n", why);
        return false;
    }
    return true;
}

static const char *main_type(void)
{
    uint64_t count[32] = {0};
    for (int i = 0; i < model.layers; i++) {
        const struct ai_tensor *t[] = {&model.L[i].q, &model.L[i].k,    &model.L[i].v,   &model.L[i].o,
                                       &model.L[i].gate, &model.L[i].up, &model.L[i].down};
        for (size_t k = 0; k < ARRAY_SIZE(t); k++)
            count[t[k]->type & 31] += (uint64_t)t[k]->cols * (uint64_t)t[k]->rows;
    }
    int best = 0;
    for (int i = 1; i < 32; i++)
        if (count[i] > count[best])
            best = i;
    return gguf_type_name((uint32_t)best);
}

static void print_info(struct shell *sh)
{
    char size[16];
    fmt_size(size, sizeof(size), model.file_size);
    sh_printf(sh, "loaded %s (%s)\n  %s, %d layers, %u.%02u B parameters, %s, %s\n  context %d tokens, %d in use, "
                  "%d processors\n",
              *model.name ? model.name : model.path, model.path, model.arch_name, model.layers,
              (unsigned)(model.params / 1000000000u), (unsigned)(model.params / 10000000u % 100u), main_type(), size,
              model.ctx, model.n_past, hal_parallel_cpus());
}

static int ids_from(int argc, char **argv, int from, int *ids, int cap, int *n_out)
{
    int n = 0, gen = 8;
    for (int i = from; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            gen = (int)str_to_long(argv[++i], NULL, 10);
            continue;
        }
        if (n < cap)
            ids[n++] = (int)str_to_long(argv[i], NULL, 10);
    }
    *n_out = gen;
    return n;
}

struct fetch {
    struct file *f;
    int err;
    void (*progress)(void *ctx, const char *stage, size_t done, size_t total);
    void *ctx;
};

static int fetch_sink(void *ctx, const void *data, size_t n)
{
    struct fetch *d = ctx;
    long w = vfs_write(d->f, data, n);
    if (w < 0 || (size_t)w != n)
        d->err = w < 0 ? (int)w : E_NOSPC;
    return d->err;
}

static void fetch_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    struct fetch *d = ctx;
    if (d->progress)
        d->progress(d->ctx, stage, done, total);
}

/* the model, from the web into the models folder (a .part file until it is all there) */
static int download_model(const struct download *d0, void (*progress)(void *ctx, const char *stage, size_t done,
                                                                        size_t total),
                          void *ctx, char *why, size_t cap)
{
    char final[160], part[168];
    ksnprintf(final, sizeof(final), MODELS_DIR "/%s", d0->name);
    ksnprintf(part, sizeof(part), "%s.part", final);
    vfs_mkdirs(MODELS_DIR);
    struct fetch d = {NULL, 0, progress, ctx};
    int err = vfs_open(part, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &d.f);
    if (err) {
        ksnprintf(why, cap, "%s: %s", part, err_str(err));
        return err;
    }
    struct http_request req = {.method = "GET", .url = d0->url, .max_size = (size_t)d0->bytes + (1u << 20),
                               .progress = fetch_progress, .sink = fetch_sink, .ctx = &d};
    struct http_response resp = {0};
    err = http_fetch(&req, &resp);
    vfs_close(d.f);
    if (!err && d.err)
        err = d.err;
    if (err)
        ksnprintf(why, cap, "download: %s", err_str(err));
    else if (resp.status != 200) {
        ksnprintf(why, cap, "the server answered %d", resp.status);
        err = E_IO;
    }
    http_free(&resp);
    struct vstat st;
    if (!err && (vfs_stat(part, &st) || st.size != d0->bytes)) {
        ksnprintf(why, cap, "the download is incomplete");
        err = E_IO;
    }
    if (err) {
        vfs_unlink(part);
        return err;
    }
    vfs_unlink(final);
    err = vfs_rename(part, final);
    if (err)
        ksnprintf(why, cap, "%s: %s", final, err_str(err));
    return err;
}

static void shell_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    static uint64_t last;
    if (hal_ticks_ms() - last < 1000 && done < total)
        return;
    last = hal_ticks_ms();
    char a[16], b[16];
    fmt_size(a, sizeof(a), done);
    fmt_size(b, sizeof(b), total);
    sh_printf(ctx, "\r%s: %s of %s   ", stage, a, b);
}


/* ---- speech ---- */

static struct whisper ear;
static bool ear_loaded;

/* the speech model: ggml-*.bin in the models folder */
static bool default_speech(char *out, size_t cap)
{
    struct vdir it;
    if (vfs_opendir(MODELS_DIR, &it))
        return false;
    bool found = false;
    struct vfs_dirent e;
    while (!found && !vfs_readdir(&it, &e))
        if (str_starts(e.name, "ggml-") && str_iends(e.name, ".bin")) {
            ksnprintf(out, cap, MODELS_DIR "/%s", e.name);
            found = true;
        }
    vfs_closedir(&it);
    return found;
}

static int ear_load(const char *path, char *why, size_t cap)
{
    if (ear_loaded && !strcmp(ear.path, path))
        return 0;
    char found[160];
    if (!path && !default_speech(found, sizeof(found))) {
        str_copy(why, "no speech model; run: ai get speech", cap);
        return E_NOENT;
    }
    if (!path)
        path = found;
    if (ear_loaded && !strcmp(ear.path, path))
        return 0;
    if (ear_loaded)
        wh_free(&ear);
    ear_loaded = false;
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err) {
        ksnprintf(why, cap, "%s: %s", path, err_str(err));
        return err;
    }
    if ((err = wh_load(&ear, (uint8_t *)data, size, why, cap))) {
        kfree(data);
        return err;
    }
    str_copy(ear.path, path, sizeof(ear.path));
    ear_loaded = true;
    return 0;
}

/* 16-bit stereo at any rate to what whisper hears: 16 kHz mono floats; at most max samples */
static float *to_16k(const int16_t *frames, uint32_t count, uint32_t rate, int max, int *n)
{
    uint64_t want = (uint64_t)count * WH_RATE / MAX(rate, 1u);
    *n = (int)MIN(want, (uint64_t)max);
    float *out = kmalloc(sizeof(float) * (size_t)MAX(*n, 1));
    if (!out)
        return NULL;
    for (int i = 0; i < *n; i++) {
        uint64_t pos = (uint64_t)i * rate * 256 / WH_RATE; /* in 1/256 of a frame */
        uint32_t a = (uint32_t)(pos >> 8), b = MIN(a + 1, count - 1);
        float f = (float)(pos & 255) / 256.0f;
        float va = ((float)frames[a * 2] + (float)frames[a * 2 + 1]) * 0.5f,
              vb = ((float)frames[b * 2] + (float)frames[b * 2 + 1]) * 0.5f;
        out[i] = (va + (vb - va) * f) / 32768.0f;
    }
    return out;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* a sound file as 16-bit stereo frames: WAV here, other kinds through the decoders of modules */
static int read_sound(const char *path, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    const uint8_t *p = (const uint8_t *)data;
    if (size < 44 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        err = module_audio_decode ? module_audio_decode(p, size, frames, count, rate) : E_INVAL;
        kfree(data);
        return err;
    }
    int channels = 0, bits = 0;
    const uint8_t *pcm = NULL;
    uint32_t len = 0;
    *rate = 0;
    for (size_t off = 12; off + 8 <= size;) {
        uint32_t n = le32(p + off + 4);
        if (!memcmp(p + off, "fmt ", 4) && n >= 16) {
            channels = p[off + 10] | p[off + 11] << 8;
            *rate = le32(p + off + 12);
            bits = p[off + 22] | p[off + 23] << 8;
        } else if (!memcmp(p + off, "data", 4)) {
            pcm = p + off + 8;
            len = (uint32_t)MIN((size_t)n, size - off - 8);
        }
        off += 8 + (size_t)n + (n & 1);
    }
    if (!pcm || channels < 1 || channels > 2 || (bits != 8 && bits != 16) || *rate < 4000 || *rate > 192000) {
        kfree(data);
        return E_INVAL;
    }
    *count = len / (uint32_t)(channels * bits / 8);
    *frames = kmalloc((size_t)*count * 4 + 4);
    if (!*frames) {
        kfree(data);
        return E_NOMEM;
    }
    for (uint32_t i = 0; i < *count; i++)
        for (int c = 0; c < 2; c++) {
            int ch = MIN(c, channels - 1);
            (*frames)[i * 2 + c] = (int16_t)(bits == 16 ? (int16_t)(pcm[(i * channels + ch) * 2] |
                                                                    pcm[(i * channels + ch) * 2 + 1] << 8)
                                                        : (pcm[i * channels + ch] - 128) << 8);
        }
    kfree(data);
    return 0;
}

/* the words in some speech; tokens and language for the curious (both may be NULL) */
static int hear(const int16_t *frames, uint32_t count, uint32_t rate, char *text, size_t cap, int *tokens, int *n_tokens,
                int *lang)
{
    int n, toks[WH_MAX_TOKENS], l;
    float *pcm = to_16k(frames, count, rate, 2 * ear.a_ctx * WH_HOP, &n);
    if (!pcm)
        return E_NOMEM;
    int got = wh_transcribe(&ear, pcm, n, toks, WH_MAX_TOKENS, &l);
    kfree(pcm);
    if (got < 0)
        return got;
    wh_text(&ear, toks, got, text, cap);
    if (tokens)
        memcpy(tokens, toks, sizeof(int) * (size_t)got);
    if (n_tokens)
        *n_tokens = got;
    if (lang)
        *lang = l;
    return 0;
}

/* records from the microphone for up to ms, or until a second of quiet after speech */
static int record(int ms, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    int r = hal_audio_record_open();
    if (r < 0)
        return r;
    *rate = (uint32_t)r;
    uint32_t cap = (uint32_t)((uint64_t)r * (uint64_t)ms / 1000) + 1024;
    *frames = kmalloc((size_t)cap * 4);
    if (!*frames) {
        hal_audio_record_close();
        return E_NOMEM;
    }
    *count = 0;
    uint64_t start = hal_ticks_ms(), last_loud = 0;
    while (hal_ticks_ms() - start < (uint64_t)ms && *count < cap) {
        int got = hal_audio_record_read(*frames + (size_t)*count * 2, (int)(cap - *count));
        for (int i = 0; i < got; i++) {
            int v = (*frames)[((size_t)*count + (size_t)i) * 2];
            if (v > 2000 || v < -2000)
                last_loud = hal_ticks_ms();
        }
        *count += (uint32_t)MAX(got, 0);
        if (last_loud && hal_ticks_ms() - last_loud > 1000)
            break;
        hal_sleep_ms(20);
    }
    hal_audio_record_close();
    return 0;
}

static int hear_command(struct shell *sh, int argc, char **argv)
{
    bool listen = !strcmp(argv[1], "listen");
    const char *model_path = NULL, *file = NULL;
    char path[160], why[200];
    int seconds = 5;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            sh_resolve(sh, argv[++i], path, sizeof(path));
            model_path = path;
        } else if (listen) {
            seconds = (int)CLAMP(str_to_long(argv[i], NULL, 10), 1, 30);
        } else {
            file = argv[i];
        }
    }
    if (!listen && !file)
        return sh_error(sh, "ai", "hear needs a sound file");
    int16_t *frames = NULL;
    uint32_t count = 0, rate = 0;
    int err;
    if (listen) {
        sh_printf(sh, "listening for %d s...\n", seconds);
        err = record(seconds * 1000, &frames, &count, &rate);
        if (err)
            return sh_error(sh, "ai", "no microphone (%s)", err_str(err));
    } else {
        char f[160];
        sh_resolve(sh, file, f, sizeof(f));
        if ((err = read_sound(f, &frames, &count, &rate)))
            return sh_error(sh, "ai", "%s: %s", f, err_str(err));
    }
    if ((err = ear_load(model_path, why, sizeof(why)))) {
        kfree(frames);
        return sh_error(sh, "ai", "%s", why);
    }
    char text[1024];
    int toks[WH_MAX_TOKENS], n = 0, lang = -1;
    uint64_t start = hal_ticks_ms();
    err = hear(frames, count, rate, text, sizeof(text), toks, &n, &lang);
    kfree(frames);
    if (err)
        return sh_error(sh, "ai", "%s", err_str(err));
    sh_printf(sh, "heard tokens:");
    for (int i = 0; i < n; i++)
        sh_printf(sh, " %d", toks[i]);
    sh_printf(sh, " (language %d)\nheard (%s, %u ms): %s\n", lang,
              lang >= 0 && lang < (int)ARRAY_SIZE(wh_lang_codes) ? wh_lang_codes[lang] : "en",
              (unsigned)(hal_ticks_ms() - start), text);
    return 0;
}

#include "assistant/voice.h"
#include "assistant/window.h"

/* a conversation of one question in the terminal; tools that change something run only with --yes */
static int ask(struct shell *sh, int argc, char **argv)
{
    struct agent *a = kzalloc(sizeof(*a));
    if (!a)
        return sh_error(sh, "ai", "%s", err_str(E_NOMEM));
    char *text = kmalloc(4096);
    if (!text) {
        kfree(a);
        return sh_error(sh, "ai", "%s", err_str(E_NOMEM));
    }
    text[0] = 0;
    int limit = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) {
            a->auto_approve = true;
            continue;
        }
        if (!strcmp(argv[i], "--no-tools")) { /* only talk */
            a->no_tools = true;
            continue;
        }
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            limit = (int)str_to_long(argv[++i], NULL, 10);
            continue;
        }
        size_t n = strlen(text);
        ksnprintf(text + n, 4096 - n, "%s%s", n ? " " : "", argv[i]);
    }
    if (!*text) {
        kfree(text);
        kfree(a);
        return sh_error(sh, "ai", "ask needs a question");
    }
    a->sh = sh;
    a->smp = (struct sampler){0.7f, 0.9f, 1.1f, (uint32_t)hal_ticks_ms() | 1};
    if (limit)
        a->smp.temp = 0; /* a fixed length is for trying the engine: the same answer every time */
    agent_ask(a, text);
    int shown = 0, tools = 0;
    while (a->state != AG_IDLE) {
        if (a->state == AG_APPROVE) {
            sh_printf(sh, "\n[%s: not allowed; ask again with --yes to let it]\n", a->call_label);
            agent_finish_tool(a, false);
            continue;
        }
        int before = a->rounds;
        agent_step(a);
        if (a->rounds != before) {
            tools++;
            sh_printf(sh, "\n[%s]\n", a->call_label);
            shown = 0;
            continue;
        }
        if (a->state == AG_DECODE && a->reply_len > shown) {
            /* the reply as it comes, held back while it may be a tool call */
            if (!strstr(a->reply, "<tool_call>") && !strstr(a->reply, "<think>") &&
                !(a->reply_len < 12 && a->reply[0] == '<')) {
                sh_puts(sh, a->reply + shown);
                shown = a->reply_len;
            }
        }
        if (limit && a->generated >= limit && a->state == AG_DECODE) {
            agent_add(a, R_ASSISTANT, a->reply);
            a->state = AG_IDLE;
        }
    }
    if (shown < a->reply_len && !strstr(a->reply, "<tool_call>"))
        sh_puts(sh, a->reply + shown);
    unsigned ms = (unsigned)(hal_ticks_ms() - a->started);
    sh_printf(sh, "\n[%d tokens, %d tool calls, %u.%u s]\n", a->tokens_out, tools, ms / 1000, ms / 100 % 10);
    agent_clear(a);
    kfree(a->toks);
    kfree(a);
    kfree(text);
    return 0;
}

COMMAND(ai, NULL, "system",
        "ai [get [speech]|load <file.gguf>|unload|info|hear <sound file>|listen [seconds]|ask [--yes] [--no-tools] [-n count] <question>|tool <name> <json> [--yes]|tokens [-s] <text>|"
        "next <ids…> [-n count]]",
        "the assistant's language model, on this computer")
{
    const char *sub = argc > 1 ? argv[1] : "info";
    char why[200];
    if (!strcmp(sub, "load")) {
        char path[160];
        if (argc > 2)
            sh_resolve(sh, argv[2], path, sizeof(path));
        else if (!default_model(path, sizeof(path)))
            return sh_error(sh, "ai", "no model in " MODELS_DIR);
        uint64_t start = hal_ticks_ms();
        if (load(sh, path, why, sizeof(why))) {
            sh_printf(sh, "\rai: %s\n", why);
            return 1;
        }
        sh_printf(sh, "\r");
        print_info(sh);
        sh_printf(sh, "  ready in %u ms\n", (unsigned)(hal_ticks_ms() - start));
        return 0;
    }
    if (!strcmp(sub, "unload")) {
        if (loaded)
            model_free(&model);
        loaded = false;
        sh_printf(sh, "model unloaded\n");
        return 0;
    }
    if (!strcmp(sub, "info")) {
        if (!loaded) {
            sh_printf(sh, "no model loaded\n");
            return 0;
        }
        print_info(sh);
        return 0;
    }
    if (!strcmp(sub, "get")) {
        const struct download *d = argc > 2 && !strcmp(argv[2], "speech") ? &speech_model : &chat_model;
        if (d == &chat_model && default_model(why, sizeof(why)))
            sh_printf(sh, "there is already a model: %s\n", why);
        sh_printf(sh, "downloading %s (%u MB) to " MODELS_DIR "\n", d->name, d->bytes >> 20);
        if (download_model(d, shell_progress, sh, why, sizeof(why))) {
            sh_printf(sh, "\n");
            return sh_error(sh, "ai", "%s", why);
        }
        sh_printf(sh, "\nsaved " MODELS_DIR "/%s\n", d->name);
        return 0;
    }
    if (!strcmp(sub, "hear") || !strcmp(sub, "listen"))
        return hear_command(sh, argc, argv);
    if (!strcmp(sub, "tool")) {
        if (argc < 4)
            return sh_error(sh, "ai", "tool needs a name and its arguments as JSON");
        bool yes = argc > 4 && !strcmp(argv[4], "--yes");
        char label[160];
        tool_label(argv[2], argv[3], label, sizeof(label));
        if (!yes && tool_needs_approval(argv[2], argv[3]))
            return sh_error(sh, "ai", "%s needs approval; add --yes", label);
        struct agent *a = kzalloc(sizeof(*a));
        char *out = kmalloc(TOOL_OUT);
        if (a && out) {
            a->sh = sh;
            tool_run(a, argv[2], argv[3], out, TOOL_OUT);
            sh_printf(sh, "%s\nresult: %s\n", label, out);
        }
        kfree(out);
        kfree(a);
        return 0;
    }
    if (!ensure_model(sh))
        return 1;
    if (!strcmp(sub, "ask"))
        return ask(sh, argc, argv);
    if (!strcmp(sub, "tokens")) {
        bool specials = argc > 2 && !strcmp(argv[2], "-s");
        char text[1024] = "";
        for (int i = specials ? 3 : 2; i < argc; i++)
            ksnprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s", i > (specials ? 3 : 2) ? " " : "",
                      argv[i]);
        int ids[512], n = tok_encode(&model.tok, text, specials, ids, 512);
        sh_printf(sh, "tokens:");
        for (int i = 0; i < n; i++)
            sh_printf(sh, " %d", ids[i]);
        sh_printf(sh, "\n");
        return 0;
    }
    if (!strcmp(sub, "next")) {
        int ids[512], gen, n = ids_from(argc, argv, 2, ids, 400, &gen);
        if (!n)
            return sh_error(sh, "ai", "next needs token numbers");
        uint64_t start = hal_ticks_ms();
        model.n_past = 0;
        int err = model_feed(&model, ids, n, NULL);
        if (err)
            return sh_error(sh, "ai", "%s", err_str(err));
        struct sampler s = {0, 0, 1.0f, 1};
        sh_printf(sh, "next:");
        for (int i = 0; i < gen && n < model.ctx; i++) {
            int id = sample(&model, &s);
            sh_printf(sh, " %d", id);
            ids[n++] = id;
            forward(&model, &id, 1, true);
        }
        sh_printf(sh, "\n(%u ms)\n", (unsigned)(hal_ticks_ms() - start));
        return 0;
    }
    return sh_error(sh, "ai", "unknown command %s", sub);
}

static const struct command *const commands[] = {&cmddef_ai};
static const struct assistant_service service = {toggle, ask_about};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, "assistant",
                                      &service};
static const struct gui_app assistant_app = {
    .name = "Assistant", .description = "Chat with an AI that runs on this computer", .icon = ICON_ASSISTANT,
    .launch = launch};
GUI_APP_FULL(&assistant_app, tick, &ext);
