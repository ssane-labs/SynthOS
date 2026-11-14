/* Talking to the assistant. The microphone button records a question until a moment of quiet after it, then the
   speech model writes it down and it is sent. A voice detector (assistant/listen.h) tells speech from the room's
   noise, and while the question goes on what has been said so far is written down again every second or so and shown,
   so the words appear as they are said. With the wake phrase on, the microphone stays open: a short burst of
   speech is written down, and when it starts with "Hey Synth" the window opens and either takes the rest as the
   question or listens for one. All of it runs from the module's tick; the writing down takes a moment. */

#define VOICE_MAX_MS 12000
#define WAKE_MAX_MS 4000
#define VOICE_QUIET_MS 700 /* after the voice detector's own pause: the question has ended */
#define PARTIAL_EVERY_MS 800

enum voice_state { V_OFF, V_RECORD, V_HEAR, V_WAKE };

static struct {
    enum voice_state state;
    int16_t *frames;
    uint32_t count, cap, rate;
    uint64_t start, last_loud, first_loud;
    bool mic_open;
    int level; /* how loud now, 0 to 100 */
    char note[160];
    float *pcm; /* what was heard as 16 kHz mono, for the voice detector and the words so far */
    int pcm_n, pcm_cap, vad_done;
    struct vad vad;
    struct stream_asr *sa;
    uint64_t partial_at;
    int partial_cost;
    char partial[200]; /* the words so far */
} voice;

static void gui_send(const char *text);
static void orb_show(bool ask);
static void chat_invalidate(void);

static bool wake_enabled(void)
{
    return gui_ai.on && gui_ai.wake;
}

static void mic_close(void)
{
    if (voice.mic_open)
        hal_audio_record_close();
    voice.mic_open = false;
    kfree(voice.frames);
    kfree(voice.pcm);
    kfree(voice.sa);
    voice.frames = NULL;
    voice.pcm = NULL;
    voice.sa = NULL;
    voice.count = 0;
    voice.cap = 0;
}

/* opens the microphone with room for ms of sound; false (and a note) when there is none */
static bool mic_open(int ms)
{
    if (!voice.mic_open) {
        int r = hal_audio_record_open();
        if (r < 0) {
            str_copy(voice.note, "No microphone found", sizeof(voice.note));
            return false;
        }
        voice.rate = (uint32_t)r;
        voice.mic_open = true;
    }
    uint32_t cap = (uint32_t)((uint64_t)voice.rate * (uint64_t)ms / 1000) + 1024;
    if (cap != voice.cap || !voice.frames) {
        kfree(voice.frames);
        kfree(voice.pcm);
        voice.frames = kmalloc((size_t)cap * 4);
        voice.pcm_cap = ms * (WH_RATE / 1000) + 2 * VAD_FRAME;
        voice.pcm = kmalloc(sizeof(float) * (size_t)voice.pcm_cap);
        voice.cap = cap;
    }
    if (!voice.sa)
        voice.sa = kzalloc(sizeof(*voice.sa));
    if (!voice.frames || !voice.pcm || !voice.sa) {
        mic_close();
        str_copy(voice.note, err_str(E_NOMEM), sizeof(voice.note));
        return false;
    }
    voice.count = 0;
    voice.start = hal_ticks_ms();
    voice.last_loud = voice.first_loud = 0;
    voice.note[0] = 0;
    voice.partial[0] = 0;
    voice.pcm_n = voice.vad_done = 0;
    memset(voice.sa, 0, sizeof(*voice.sa));
    voice.partial_at = voice.start;
    vad_init(&voice.vad, VAD_ONSET_MS, VAD_HANG_MS);
    return true;
}

/* the recorded frames not yet in voice.pcm, as 16 kHz mono */
static void mic_resample(void)
{
    for (;;) {
        uint64_t pos = (uint64_t)voice.pcm_n * voice.rate * 256 / WH_RATE; /* in 1/256 of a frame */
        uint32_t a = (uint32_t)(pos >> 8);
        if (a + 1 >= voice.count || voice.pcm_n >= voice.pcm_cap)
            return;
        float f = (float)(pos & 255) / 256.0f;
        float va = ((float)voice.frames[a * 2] + (float)voice.frames[a * 2 + 1]) * 0.5f,
              vb = ((float)voice.frames[a * 2 + 2] + (float)voice.frames[a * 2 + 3]) * 0.5f;
        voice.pcm[voice.pcm_n++] = (va + (vb - va) * f) / 32768.0f;
    }
}

/* takes what the microphone has heard since the last call; true when an utterance has ended */
static bool mic_pump(void)
{
    uint64_t now = hal_ticks_ms();
    int peak = 0;
    for (;;) {
        if (voice.count >= voice.cap) {
            if (voice.first_loud)
                return true; /* full: what there is will do */
            voice.count = 0; /* only quiet so far: start again */
            voice.pcm_n = voice.vad_done = 0;
        }
        int got = hal_audio_record_read(voice.frames + (size_t)voice.count * 2, (int)(voice.cap - voice.count));
        if (got <= 0)
            break;
        for (int i = 0; i < got; i++) {
            int v = voice.frames[((size_t)voice.count + (size_t)i) * 2];
            peak = MAX(peak, v < 0 ? -v : v);
        }
        voice.count += (uint32_t)got;
    }
    voice.level = MIN(100, peak * 100 / 12000);
    mic_resample();
    for (; voice.vad_done + VAD_FRAME <= voice.pcm_n; voice.vad_done += VAD_FRAME) {
        bool speech = vad_frame(&voice.vad, voice.pcm + voice.vad_done, 0);
        if (voice.vad.voiced)
            voice.last_loud = now;
        if (speech && !voice.first_loud)
            voice.first_loud = now;
    }
    if (!voice.first_loud && voice.count > voice.rate / 2 && voice.state == V_WAKE) {
        /* nothing said yet: keep only the last third of a second, the start of what may come */
        uint32_t keep = voice.rate / 3;
        memmove(voice.frames, voice.frames + (size_t)(voice.count - keep) * 2, (size_t)keep * 4);
        voice.count = keep;
        voice.pcm_n = voice.vad_done = 0;
        mic_resample();
        voice.vad_done = voice.pcm_n / VAD_FRAME * VAD_FRAME; /* the detector has seen these already */
    }
    return voice.first_loud && !voice.vad.speech && now - voice.last_loud > VOICE_QUIET_MS;
}

/* the words said so far, written down again now and then while the question goes on */
static void mic_partial(void)
{
    uint64_t now = hal_ticks_ms();
    if (!voice.first_loud || !voice.sa || now - voice.partial_at < (uint64_t)MAX(PARTIAL_EVERY_MS, 2 * voice.partial_cost))
        return;
    char why[200];
    if (!ear_loaded && ear_load(NULL, why, sizeof(why)))
        return;
    int toks[WH_MAX_TOKENS], lang;
    int n = MIN(voice.pcm_n, 2 * ear.a_ctx * WH_HOP);
    int got = wh_transcribe(&ear, voice.pcm, n, toks, WH_MAX_TOKENS, &lang);
    voice.partial_at = hal_ticks_ms();
    voice.partial_cost = (int)(voice.partial_at - now);
    if (got <= 0)
        return;
    int fin = stream_update(voice.sa, toks, got);
    char done[200], maybe[200];
    wh_text(&ear, voice.sa->final, fin, done, sizeof(done));
    wh_text(&ear, toks + MIN(fin, got), got - MIN(fin, got), maybe, sizeof(maybe));
    ksnprintf(voice.partial, sizeof(voice.partial), "%s%s%s", done, *done && *maybe ? " " : "", maybe);
}

/* writes down what was recorded; false (and a note) when that fails */
static bool mic_hear(char *text, size_t cap)
{
    char why[200];
    if (ear_load(NULL, why, sizeof(why))) {
        str_copy(voice.note, ear_loaded ? why : "Speech needs its model: click the microphone again to download it",
                 sizeof(voice.note));
        return false;
    }
    int err = hear(voice.frames, voice.count, voice.rate, text, cap, NULL, NULL, NULL);
    if (err) {
        str_copy(voice.note, err_str(err), sizeof(voice.note));
        return false;
    }
    return true;
}

static bool want_speech_download;
static bool voice_sending; /* gui_send is called with spoken words: the answer is said too */

/* ---- barge-in: while the assistant talks the microphone listens through the echo canceller ---- */

static struct aec *b_aec;
static struct vad b_vad;
static struct barge b_state;
static bool b_mic; /* the microphone was opened for this */

static void barge_end(void)
{
    kfree(b_aec);
    b_aec = NULL;
    if (b_mic && voice.state == V_OFF)
        mic_close();
    b_mic = false;
}

/* keeps the last ms of the recording, the rest dropped; the 16 kHz copy follows */
static void mic_keep(int ms)
{
    uint32_t keep = MIN(voice.count, (uint32_t)((uint64_t)voice.rate * (uint64_t)ms / 1000));
    memmove(voice.frames, voice.frames + (size_t)(voice.count - keep) * 2, (size_t)keep * 4);
    voice.count = keep;
    voice.pcm_n = 0;
    mic_resample();
}

static void talk_listen(void)
{
    if (!b_aec) {
        if (!voice.mic_open) {
            if (!mic_open(VOICE_MAX_MS))
                return; /* no microphone: no barge-in, the voice just plays */
            b_mic = true;
        }
        b_aec = kzalloc(sizeof(*b_aec));
        if (!b_aec)
            return;
        aec_init(b_aec);
        vad_init(&b_vad, VAD_ONSET_MS, VAD_HANG_MS);
        memset(&b_state, 0, sizeof(b_state));
        voice.count = 0;
        voice.pcm_n = voice.vad_done = 0;
    }
    for (;;) {
        if (voice.count + voice.rate / 4 >= voice.cap) { /* room for more: the last second is enough */
            mic_keep(1000);
            voice.vad_done = voice.pcm_n / VAD_FRAME * VAD_FRAME;
        }
        int got = hal_audio_record_read(voice.frames + (size_t)voice.count * 2, (int)(voice.cap - voice.count));
        if (got <= 0)
            break;
        voice.count += (uint32_t)got;
    }
    mic_resample();
    int64_t now16 = talk_playing16();
    float ref[VAD_FRAME], out[VAD_FRAME];
    for (; voice.vad_done + VAD_FRAME <= voice.pcm_n; voice.vad_done += VAD_FRAME) {
        /* this frame was heard (pcm_n - its end) samples ago; what played then, a little earlier still so the echo
           comes after it */
        int64_t at = now16 - (voice.pcm_n - voice.vad_done - VAD_FRAME) - VAD_FRAME - 160;
        talk_get_ref(at, ref, VAD_FRAME);
        aec_frame(b_aec, voice.pcm + voice.vad_done, ref, out);
        vad_frame(&b_vad, out, b_aec->removed_db);
        enum barge_event e = barge_frame(&b_state, b_vad.voiced, b_aec->erle_db > BARGE_READY_DB);
        if (e == BARGE_PAUSE) {
            talk.paused = true;
        } else if (e == BARGE_RESUME) {
            talk.paused = false;
        } else if (e == BARGE_CANCEL) { /* the user has the word: stop and listen to all of it */
            talk_stop();
            kfree(b_aec);
            b_aec = NULL;
            b_mic = false;
            mic_keep(800);
            uint64_t t = hal_ticks_ms();
            voice.start = voice.first_loud = voice.last_loud = voice.partial_at = t;
            voice.vad_done = 0;
            voice.partial[0] = 0;
            memset(voice.sa, 0, sizeof(*voice.sa));
            vad_init(&voice.vad, VAD_ONSET_MS, VAD_HANG_MS);
            voice.state = V_RECORD;
            chat_invalidate();
            return;
        }
    }
}

/* the microphone button: starts listening, or stops and sends what was said */
static void voice_button(void)
{
    if (voice.state == V_RECORD) {
        voice.state = voice.first_loud ? V_HEAR : (wake_enabled() ? V_WAKE : V_OFF);
        if (voice.state == V_OFF)
            mic_close();
        return;
    }
    char path[160];
    if (!ear_loaded && !default_speech(path, sizeof(path))) {
        if (str_starts(voice.note, "Speech needs"))
            want_speech_download = true;
        else
            str_copy(voice.note, "Speech needs its model (80 MB): click the microphone again to download it",
                     sizeof(voice.note));
        return;
    }
    if (mic_open(VOICE_MAX_MS))
        voice.state = V_RECORD;
}

static void voice_wake_set(bool on)
{
    gui_setting("aiwake", on ? "on" : "off");
    if (!on && voice.state == V_WAKE) {
        mic_close();
        voice.state = V_OFF;
    }
}

static char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* where the question starts after the wake phrase, or NULL when the words do not start with it */
static const char *after_wake(const char *text)
{
    static const char *const names[] = {"synth", "sinth", "cinth", "sint ", "since"};
    char low[96];
    size_t n = 0;
    for (; text[n] && n < sizeof(low) - 1; n++)
        low[n] = lower_ascii(text[n]);
    low[n] = 0;
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        const char *at = strstr(low, names[i]);
        if (at && at - low <= 12) { /* "hey", "hi", "okay" or nothing before it */
            const char *rest = text + (at - low) + strlen(names[i]);
            while (*rest && ((*rest >= 'a' && *rest <= 'z') || (*rest >= 'A' && *rest <= 'Z')))
                rest++; /* the end of the word: "Synthos" */
            while (*rest == ',' || *rest == '.' || *rest == '!' || *rest == ' ')
                rest++;
            return rest;
        }
    }
    return NULL;
}

static void voice_tick(void)
{
    static uint64_t tried;
    if (talk.active && gui_ai.barge && voice.state != V_RECORD && voice.state != V_HEAR) {
        talk_listen();
        return;
    }
    if (b_aec || b_mic)
        barge_end();
    if (voice.state == V_OFF && wake_enabled() && !want_speech_download && hal_ticks_ms() - tried > 2000) {
        tried = hal_ticks_ms();
        char path[160];
        if (ear_loaded || default_speech(path, sizeof(path))) {
            if (mic_open(WAKE_MAX_MS))
                voice.state = V_WAKE;
            else
                voice_wake_set(false); /* no microphone: nothing to listen with */
        }
    }
    if (voice.state == V_RECORD) {
        mic_partial();
        if (mic_pump() || hal_ticks_ms() - voice.start > VOICE_MAX_MS)
            voice.state = voice.first_loud ? V_HEAR : V_OFF;
        if (voice.state == V_OFF) {
            mic_close();
            str_copy(voice.note, "Nothing heard", sizeof(voice.note));
        }
        chat_invalidate();
        return;
    }
    if (voice.state == V_HEAR) { /* the window has shown "Writing down…" since the last tick */
        char text[1024];
        bool ok = mic_hear(text, sizeof(text));
        voice.state = V_OFF;
        if (!wake_enabled())
            mic_close();
        while (ok && *text == ' ')
            memmove(text, text + 1, strlen(text));
        if (ok && *text) {
            voice_sending = true;
            gui_send(text);
            voice_sending = false;
        }
        else if (ok)
            str_copy(voice.note, "Nothing heard", sizeof(voice.note));
        chat_invalidate();
        return;
    }
    if (voice.state == V_WAKE && mic_pump()) {
        char text[512];
        uint64_t spoke = voice.last_loud - voice.first_loud;
        const char *rest = NULL;
        if (spoke < WAKE_MAX_MS && mic_hear(text, sizeof(text)))
            rest = after_wake(text);
        voice.count = 0;
        voice.first_loud = voice.last_loud = 0;
        if (rest) {
            orb_show(false);
            if (strlen(rest) > 3) {
                voice_sending = true;
                gui_send(rest);
                voice_sending = false;
            } else if (mic_open(VOICE_MAX_MS)) {
                voice.state = V_RECORD;
                hal_beep(880, 60);
            }
            chat_invalidate();
        }
    }
}
