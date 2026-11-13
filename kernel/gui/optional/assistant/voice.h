/* Talking to the assistant. The microphone button records a question until a moment of quiet after it, then the
   speech model writes it down and it is sent. With the wake phrase on, the microphone stays open: a short burst of
   speech is written down, and when it starts with "Hey Synth" the window opens and either takes the rest as the
   question or listens for one. All of it runs from the module's tick; the writing down takes a moment. */

#define WAKE_FILE MODELS_DIR "/wake-phrase"
#define VOICE_MAX_MS 12000
#define WAKE_MAX_MS 4000
#define VOICE_LOUD 1500
#define VOICE_QUIET_MS 900

enum voice_state { V_OFF, V_RECORD, V_HEAR, V_WAKE };

static struct {
    enum voice_state state;
    int16_t *frames;
    uint32_t count, cap, rate;
    uint64_t start, last_loud, first_loud;
    bool wake_on, wake_read, mic_open;
    int level; /* how loud now, 0 to 100 */
    char note[160];
} voice;

static void gui_send(const char *text);
static void launch(const char *arg);
static void chat_invalidate(void);

static bool wake_enabled(void)
{
    if (!voice.wake_read) {
        struct vstat st;
        voice.wake_on = !vfs_stat(WAKE_FILE, &st);
        voice.wake_read = true;
    }
    return voice.wake_on;
}

static void mic_close(void)
{
    if (voice.mic_open)
        hal_audio_record_close();
    voice.mic_open = false;
    kfree(voice.frames);
    voice.frames = NULL;
    voice.count = 0;
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
        voice.frames = kmalloc((size_t)cap * 4);
        voice.cap = cap;
    }
    if (!voice.frames) {
        mic_close();
        str_copy(voice.note, err_str(E_NOMEM), sizeof(voice.note));
        return false;
    }
    voice.count = 0;
    voice.start = hal_ticks_ms();
    voice.last_loud = voice.first_loud = 0;
    voice.note[0] = 0;
    return true;
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
            voice.count = 0;  /* only quiet so far: start again */
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
    if (peak > VOICE_LOUD) {
        voice.last_loud = now;
        if (!voice.first_loud)
            voice.first_loud = now;
    }
    if (!voice.first_loud && voice.count > voice.rate / 2 && voice.state == V_WAKE) {
        /* nothing said yet: keep only the last tenth of a second, the start of what may come */
        uint32_t keep = voice.rate / 10;
        memmove(voice.frames, voice.frames + (size_t)(voice.count - keep) * 2, (size_t)keep * 4);
        voice.count = keep;
    }
    return voice.first_loud && now - voice.last_loud > VOICE_QUIET_MS;
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
    voice.wake_on = on;
    voice.wake_read = true;
    if (on)
        vfs_write_all(WAKE_FILE, "Hey Synth\n", 10);
    else
        vfs_unlink(WAKE_FILE);
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
        if (ok && *text)
            gui_send(text);
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
            launch(NULL);
            if (strlen(rest) > 3) {
                gui_send(rest);
            } else if (mic_open(VOICE_MAX_MS)) {
                voice.state = V_RECORD;
                hal_beep(880, 60);
            }
            chat_invalidate();
        }
    }
}
