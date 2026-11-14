/* The assistant's voice: the Qwen3-TTS engine (tts.h) with its two files in the models folder, sound to the
   speakers as it is made, ai say in the terminal. */

static struct tts mouth;
static bool mouth_loaded;

/* the first models-folder file starting with prefix */
static bool models_file(const char *prefix, char *out, size_t cap)
{
    struct vdir it;
    if (vfs_opendir(MODELS_DIR, &it))
        return false;
    bool found = false;
    struct vfs_dirent e;
    while (!found && !vfs_readdir(&it, &e))
        if (str_starts(e.name, prefix) && str_iends(e.name, ".gguf")) {
            ksnprintf(out, cap, MODELS_DIR "/%s", e.name);
            found = true;
        }
    vfs_closedir(&it);
    return found;
}

/* voice and codec: given paths, else the ones in the models folder */
static int mouth_load(struct shell *sh, const char *voice, const char *codec, char *why, size_t cap)
{
    char vp[160], cp[160];
    if (!voice && !models_file("qwen-talker-", vp, sizeof(vp))) {
        str_copy(why, "no voice; run: ai get voice", cap);
        return E_NOENT;
    }
    if (!codec && !models_file("qwen-tokenizer-", cp, sizeof(cp))) {
        str_copy(why, "no voice codec; run: ai get voice", cap);
        return E_NOENT;
    }
    voice = voice ? voice : vp;
    codec = codec ? codec : cp;
    if (mouth_loaded && !strcmp(mouth.path, voice) && !strcmp(mouth.codec_path, codec))
        return 0;
    if (mouth_loaded)
        tts_free(&mouth);
    mouth_loaded = false;
    uint8_t *a, *b;
    size_t na, nb;
    int err = read_big(sh, voice, &a, &na, why, cap);
    if (err)
        return err;
    if ((err = read_big(sh, codec, &b, &nb, why, cap))) {
        big_free(a);
        return err;
    }
    int ctx = 1024;
    while ((err = tts_load(&mouth, a, na, b, nb, ctx, why, cap)) == E_NOMEM && ctx > 256) {
        ctx /= 2; /* tts_load gave the files back: read them again */
        if ((err = read_big(NULL, voice, &a, &na, why, cap)))
            return err;
        if ((err = read_big(NULL, codec, &b, &nb, why, cap))) {
            big_free(a);
            return err;
        }
    }
    if (err)
        return err;
    str_copy(mouth.path, voice, sizeof(mouth.path));
    str_copy(mouth.codec_path, codec, sizeof(mouth.codec_path));
    mouth_loaded = true;
    return 0;
}

/* ---- playing 24 kHz floats on whatever rate the sound card has ---- */

static struct {
    bool open;
    int rate, cap;     /* cap: the card's free space when nothing is queued */
    uint64_t pos;      /* output frames made, in the card's rate */
    uint64_t in;       /* input samples given */
    uint64_t pushed;   /* output frames given to the card */
    float last;        /* the input sample before the current block */
} play;

static bool play_open(void)
{
    if (play.open)
        return true;
    int r = hal_audio_open(TTS_RATE);
    if (r <= 0)
        return false;
    play.open = true;
    play.rate = r;
    play.cap = MAX(hal_audio_space(), 1);
    play.pos = play.in = play.pushed = 0;
    play.last = 0;
    return true;
}

/* n samples at 24 kHz to stereo frames at the card's rate in out (room for n * rate / 24000 + 2); returns how many */
static int play_convert(const float *pcm, int n, int16_t *out, int cap)
{
    int count = 0;
    uint64_t end_in = play.in + (uint64_t)n;
    while (count < cap) {
        uint64_t num = play.pos * TTS_RATE, at = num / (uint64_t)play.rate; /* output k sits at k * 24000 / rate */
        if (at + 1 >= end_in)
            break;
        float f = (float)(num % (uint64_t)play.rate) / (float)play.rate;
        float a = at >= play.in ? pcm[at - play.in] : play.last, b = pcm[at + 1 - play.in];
        int v = (int)((a + (b - a) * f) * 32000.0f);
        out[count * 2] = out[count * 2 + 1] = (int16_t)CLAMP(v, -32768, 32767);
        count++;
        play.pos++;
    }
    play.last = n ? pcm[n - 1] : play.last;
    play.in = end_in;
    return count;
}

/* as many of n frames as the card takes now */
static int play_push(const int16_t *out, int n)
{
    int space = hal_audio_space(), done = 0;
    if (space > 0 && n > 0)
        done = hal_audio_write(out, MIN(space, n));
    play.pushed += (uint64_t)MAX(done, 0);
    return MAX(done, 0);
}

/* frames given to the card that it has not played yet */
static int play_queued(void)
{
    return play.open ? MAX(0, play.cap - hal_audio_space()) : 0;
}

/* n samples, waiting while the card's buffer is full */
static void play_write(const float *pcm, int n)
{
    if (!play.open)
        return;
    int cap = (int)((int64_t)n * play.rate / TTS_RATE) + 4;
    int16_t *out = kmalloc((size_t)cap * 4);
    if (!out)
        return;
    int m = play_convert(pcm, n, out, cap);
    for (int done = 0; done < m;) {
        int k = play_push(out + done * 2, m - done);
        if (!k)
            hal_sleep_ms(5);
        done += k;
    }
    kfree(out);
}

/* lets what was written play out, then closes */
static void play_close(bool drain)
{
    if (!play.open)
        return;
    for (uint64_t until = hal_ticks_ms() + (drain ? 4000 : 0); play_queued() > 0 && hal_ticks_ms() < until;)
        hal_sleep_ms(10);
    hal_audio_close();
    play.open = false;
}

static void write_wav24(const char *path, const float *pcm, int n)
{
    size_t bytes = (size_t)n * 2;
    uint8_t *f = kmalloc(44 + bytes);
    if (!f)
        return;
    memcpy(f, "RIFF", 4);
    uint32_t v[] = {(uint32_t)(36 + bytes)};
    memcpy(f + 4, v, 4);
    memcpy(f + 8, "WAVEfmt ", 8);
    uint32_t fmt[4] = {16, 1u | 1u << 16, TTS_RATE, TTS_RATE * 2};
    memcpy(f + 16, fmt, 16);
    uint16_t align[2] = {2, 16};
    memcpy(f + 32, align, 4);
    memcpy(f + 36, "data", 4);
    uint32_t len = (uint32_t)bytes;
    memcpy(f + 40, &len, 4);
    for (int i = 0; i < n; i++) {
        int s = (int)(CLAMP(pcm[i], -1.0f, 1.0f) * 32767.0f + (pcm[i] >= 0 ? 0.5f : -0.5f));
        f[44 + i * 2] = (uint8_t)s;
        f[45 + i * 2] = (uint8_t)(s >> 8);
    }
    vfs_write_all(path, (const char *)f, 44 + bytes);
    kfree(f);
}

/* ai say [--voice f] [--codec f] [--speaker name] [--lang name] [--greedy] [--seed n] [--frames n] [-o f.wav] text */
static int say_command(struct shell *sh, int argc, char **argv)
{
    char vpath[160], cpath[160], out[160], text[1024] = "", why[200];
    const char *voice = NULL, *codec = NULL, *speaker = NULL, *lang = NULL;
    bool greedy = false, to_file = false;
    long seed = (long)hal_ticks_ms(), max_frames = 750;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--voice") && i + 1 < argc) {
            sh_resolve(sh, argv[++i], vpath, sizeof(vpath));
            voice = vpath;
        } else if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
            sh_resolve(sh, argv[++i], cpath, sizeof(cpath));
            codec = cpath;
        } else if (!strcmp(argv[i], "--speaker") && i + 1 < argc) {
            speaker = argv[++i];
        } else if (!strcmp(argv[i], "--lang") && i + 1 < argc) {
            lang = argv[++i];
        } else if (!strcmp(argv[i], "--greedy")) {
            greedy = true;
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = str_to_long(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            max_frames = str_to_long(argv[++i], NULL, 10);
            max_frames = CLAMP(max_frames, 1, 3000);
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            sh_resolve(sh, argv[++i], out, sizeof(out));
            to_file = true;
        } else {
            size_t n = strlen(text);
            ksnprintf(text + n, sizeof(text) - n, "%s%s", n ? " " : "", argv[i]);
        }
    }
    if (!*text)
        return sh_error(sh, "ai", "say needs some text");
    int err = mouth_load(sh, voice, codec, why, sizeof(why));
    if (err) {
        sh_printf(sh, "\r");
        return sh_error(sh, "ai", "%s", why);
    }
    uint64_t start = hal_ticks_ms();
    if ((err = tts_begin(&mouth, text, lang, speaker, greedy ? 0.0f : 0.9f, (uint64_t)seed, why, sizeof(why))))
        return sh_error(sh, "ai", "%s", why);
    int cap = to_file ? (int)max_frames * TTS_HOP : TTS_CHUNK * TTS_HOP;
    float *pcm = big_alloc(sizeof(float) * (size_t)cap);
    int *codes = kmalloc(sizeof(int) * TTS_CHUNK * TTS_MAX_GROUPS), first[TTS_MAX_GROUPS], c0[64], n_c0 = 0;
    if (!pcm || !codes) {
        big_free(pcm);
        kfree(codes);
        return sh_error(sh, "ai", "%s", err_str(E_NOMEM));
    }
    bool sound = !to_file && play_open();
    int frames = 0, pending = 0, total = 0, G = mouth.groups;
    uint64_t first_sound = 0;
    float peak = 0;
    for (;;) {
        bool more = frames < max_frames && tts_step(&mouth);
        if (more) {
            memcpy(codes + pending * G, mouth.frame, sizeof(int) * (size_t)G);
            if (!frames)
                memcpy(first, mouth.frame, sizeof(int) * (size_t)G);
            if (n_c0 < 64)
                c0[n_c0++] = mouth.frame[0];
            frames++;
            pending++;
        }
        if (pending && (pending == TTS_CHUNK || !more)) {
            float *at = to_file ? pcm + total : pcm;
            if ((err = codec_decode(&mouth.codec, codes, pending, at)))
                break;
            for (int i = 0; i < pending * TTS_HOP; i++)
                peak = MAX(peak, at[i] < 0 ? -at[i] : at[i]);
            if (!first_sound)
                first_sound = hal_ticks_ms();
            if (sound)
                play_write(at, pending * TTS_HOP);
            total += pending * TTS_HOP;
            pending = 0;
        }
        if (!more)
            break;
    }
    if (sound)
        play_close(true);
    if (!err && to_file)
        write_wav24(out, pcm, total);
    big_free(pcm);
    kfree(codes);
    if (err)
        return sh_error(sh, "ai", "%s", err_str(err));
    unsigned ms = (unsigned)(hal_ticks_ms() - start), p = (unsigned)(peak * 100.0f + 0.5f);
    sh_printf(sh, "said %d frames: %d.%02d s of sound in %u ms, the first after %u ms%s\n", frames,
              total / TTS_RATE, total % TTS_RATE * 100 / TTS_RATE, ms, frames ? (unsigned)(first_sound - start) : 0,
              sound ? "" : to_file ? "" : " (no sound card)");
    sh_printf(sh, "first codes:");
    for (int g = 0; frames && g < G; g++)
        sh_printf(sh, " %d", first[g]);
    sh_printf(sh, "\nleading codes:");
    for (int i = 0; i < n_c0; i++)
        sh_printf(sh, " %d", c0[i]);
    sh_printf(sh, "\npeak %u.%02u\n", p / 100, p % 100);
    if (to_file)
        sh_printf(sh, "saved %s\n", out);
    return 0;
}

/* ---- talking in the window: the reply sentence by sentence while it is written, a frame or so per tick ----
   What goes to the speakers is also kept at 16 kHz, so the microphone's echo of it can be taken out
   (listen.h) and the user can talk over the assistant: barge-in. */

#define TALK_TEXT 4096
#define TALK_REF 65536 /* 4 s at 16 kHz */

static struct {
    bool active, final, paused, in_utt, failed;
    char text[TALK_TEXT];
    int seg, at;          /* where the current message starts; what has been said */
    int codes[TTS_CHUNK * TTS_MAX_GROUPS], pending;
    float pcm[TTS_CHUNK * TTS_HOP];
    int16_t *out;
    int out_n, out_at, out_cap;
    float ref[TALK_REF];  /* 16 kHz of what was made, by index modulo TALK_REF */
    uint64_t ref_n, in24; /* 16 kHz samples made; 24 kHz samples made */
    float ref_last;
} talk;

/* ends talking now (what the card holds still plays out) */
static void talk_stop(void)
{
    if (play.open) {
        hal_audio_close();
        play.open = false;
    }
    kfree(talk.out);
    talk.out = NULL;
    talk.active = talk.in_utt = talk.paused = false;
    talk.text[0] = 0;
    talk.seg = talk.at = talk.pending = talk.out_n = talk.out_at = talk.out_cap = 0;
}

/* the reply as it is so far (visible text); new_message: a new reply after a tool's, said after the earlier ones */
static void talk_offer(const char *text, bool final, bool new_message)
{
    if (!talk.active) {
        talk_stop();
        talk.active = true;
        talk.failed = false;
        talk.ref_n = talk.in24 = 0;
        talk.ref_last = 0;
    }
    if (new_message && talk.seg < (int)strlen(talk.text)) {
        talk.seg = (int)strlen(talk.text);
        if (talk.seg && talk.seg < TALK_TEXT - 2)
            talk.text[talk.seg++] = ' ';
        talk.text[talk.seg] = 0;
    }
    str_copy(talk.text + talk.seg, text, (size_t)(TALK_TEXT - talk.seg));
    talk.final = final;
}

/* the next sentence to say into out (up to cap); false when none is complete yet */
static bool talk_sentence(char *out, size_t cap)
{
    int n = (int)strlen(talk.text), i = talk.at;
    while (i < n && (talk.text[i] == ' ' || talk.text[i] == '\n' || talk.text[i] == '*' || talk.text[i] == '#'))
        i++;
    int end = -1;
    for (int j = i; j < n; j++) {
        char c = talk.text[j];
        bool stop = c == '\n' || ((c == '.' || c == '!' || c == '?' || c == ';' || c == ':') &&
                                  (j + 1 == n ? talk.final : talk.text[j + 1] == ' ' || talk.text[j + 1] == '\n'));
        if (stop || j - i >= 220) { /* a long sentence goes in parts */
            end = j + 1;
            break;
        }
    }
    if (end < 0 && talk.final && i < n)
        end = n;
    if (end < 0)
        return false;
    talk.at = end;
    size_t k = 0;
    for (int j = i; j < end && k + 1 < cap; j++) /* markdown marks are not said */
        if (talk.text[j] != '*' && talk.text[j] != '`' && talk.text[j] != '#' && talk.text[j] != '\n')
            out[k++] = talk.text[j];
    out[k] = 0;
    return true;
}

/* 24 kHz samples to the 16 kHz copy for the echo canceller */
static void talk_keep_ref(const float *pcm, int n)
{
    uint64_t end = talk.in24 + (uint64_t)n;
    for (;;) {
        uint64_t at2 = talk.ref_n * 3; /* in half samples of 24 kHz: 16 kHz sample k is at 1.5 k */
        uint64_t at = at2 / 2;
        if (at + 1 >= end)
            break;
        float a = at >= talk.in24 ? pcm[at - talk.in24] : talk.ref_last, b = pcm[at + 1 - talk.in24];
        talk.ref[talk.ref_n % TALK_REF] = (at2 & 1) ? (a + b) * 0.5f : a;
        talk.ref_n++;
    }
    talk.ref_last = n ? pcm[n - 1] : talk.ref_last;
    talk.in24 = end;
}

/* the 16 kHz sample being played now (an estimate from what the card still holds) */
static int64_t talk_playing16(void)
{
    if (!play.open)
        return (int64_t)talk.ref_n;
    int64_t played = (int64_t)play.pushed - play_queued();
    return played * 16000 / play.rate;
}

static bool talk_get_ref(int64_t from, float *out, int n)
{
    for (int i = 0; i < n; i++) {
        int64_t k = from + i;
        out[i] = k >= 0 && k < (int64_t)talk.ref_n && (int64_t)talk.ref_n - k < TALK_REF ? talk.ref[k % TALK_REF] : 0;
    }
    return true;
}

/* one step: keeps the card fed, makes sound when it needs more; about one frame of the talker per call */
/* the voice answers speak with: the settings may name one, else the first English one the model has */

static const char *talk_voice(void)
{
    for (int i = 0; i < mouth.n_spk; i++)
        if (!str_casecmp(mouth.spk[i], gui_ai.voice))
            return mouth.spk[i];
    return mouth.n_spk ? mouth.spk[0] : NULL;
}

/* Cyrillic letters make it Russian, Latin ones English; otherwise the model guesses */
static const char *talk_lang(const char *text)
{
    int cyr = 0, lat = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == 0xD0 || *p == 0xD1)
            cyr++;
        else if ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z')
            lat++;
    }
    const char *want = cyr > lat ? "russian" : lat ? "english" : NULL;
    for (int i = 0; want && i < mouth.n_lang; i++)
        if (!str_casecmp(mouth.lang[i], want))
            return mouth.lang[i];
    return NULL;
}

static void talk_tick(void)
{
    if (!talk.active)
        return;
    if (talk.out_at < talk.out_n) {
        if (!talk.paused)
            talk.out_at += play_push(talk.out + talk.out_at * 2, talk.out_n - talk.out_at);
        if (talk.out_at < talk.out_n || play_queued() > play.rate / 2)
            return; /* half a second queued is enough */
    }
    if (talk.paused)
        return;
    if (!talk.in_utt && talk.pending == 0) {
        char sentence[256], why[200];
        if (!talk_sentence(sentence, sizeof(sentence))) {
            if (talk.final && talk.at >= (int)strlen(talk.text) && play_queued() == 0)
                talk_stop(); /* all said and played */
            return;
        }
        if (!*sentence)
            return;
        if (!mouth_loaded && (talk.failed || mouth_load(NULL, NULL, NULL, why, sizeof(why)))) {
            talk.failed = true;
            talk_stop();
            return;
        }
        if (!play_open()) {
            talk_stop();
            return;
        }
        if (tts_begin(&mouth, sentence, talk_lang(sentence), talk_voice(), 0.9f, hal_ticks_ms(), why, sizeof(why)))
            return; /* nothing sayable: the next one */
        talk.in_utt = true;
    }
    bool more = talk.in_utt && mouth.frames < 750 && tts_step(&mouth);
    if (more) {
        memcpy(talk.codes + talk.pending * mouth.groups, mouth.frame, sizeof(int) * (size_t)mouth.groups);
        talk.pending++;
    } else {
        talk.in_utt = false;
    }
    if (talk.pending && (talk.pending == TTS_CHUNK || !more || !talk.out_n)) {
        int n = talk.pending * TTS_HOP;
        if (codec_decode(&mouth.codec, talk.codes, talk.pending, talk.pcm)) {
            talk_stop();
            return;
        }
        talk.pending = 0;
        talk_keep_ref(talk.pcm, n);
        int cap = (int)((int64_t)n * play.rate / TTS_RATE) + 4;
        if (cap > talk.out_cap) {
            kfree(talk.out);
            talk.out = kmalloc((size_t)cap * 4);
            talk.out_cap = talk.out ? cap : 0;
            if (!talk.out) {
                talk_stop();
                return;
            }
        }
        talk.out_n = play_convert(talk.pcm, n, talk.out, cap);
        talk.out_at = play_push(talk.out, talk.out_n);
    }
}
