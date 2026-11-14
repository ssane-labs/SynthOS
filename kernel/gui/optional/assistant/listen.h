/* The parts of a spoken conversation that are not models: knowing when someone speaks (VAD), taking the
   assistant's own voice back out of the microphone (echo cancellation) and deciding what to do when the user talks
   over the assistant (barge-in). All of it works on 16 kHz mono floats in frames of 20 ms. */

#define VAD_FRAME 320 /* 20 ms at 16 kHz */
#define VAD_ONSET_MS 100 /* shorter is a click or a cough */
#define VAD_HANG_MS 300

static float frame_db(const float *x, int n)
{
    double e = 1e-10;
    for (int i = 0; i < n; i++)
        e += (double)x[i] * (double)x[i];
    return (float)(10.0 * ai_log(e / n) / 2.302585092994046);
}

/* ---- voice activity: louder than the room's own noise, with the shape of a voice ----
   The noise floor follows quieter frames quickly and steady louder ones slowly, so a fan or a hum becomes
   "silence" within a few seconds; a frame is voiced when it is well above that floor and its zero crossings are those
   of speech rather than hiss. While the assistant talks, a frame from which the echo canceller took most away is the
   assistant's own voice, not the user's. Speech starts after onset_ms of voiced frames, ends after hang_ms without. */
struct vad {
    float floor_db, level_db;
    int voiced_ms, quiet_ms, onset_ms, hang_ms;
    bool speech, voiced, primed;
};

static void vad_init(struct vad *v, int onset_ms, int hang_ms)
{
    memset(v, 0, sizeof(*v));
    v->onset_ms = onset_ms;
    v->hang_ms = hang_ms;
}

/* one frame of VAD_FRAME samples; removed_db: how much the echo canceller took from it (0 when nothing plays);
   returns whether speech is going on */
static bool vad_frame(struct vad *v, const float *x, float removed_db)
{
    float db = frame_db(x, VAD_FRAME);
    int zc = 0;
    for (int i = 1; i < VAD_FRAME; i++)
        zc += (x[i - 1] < 0) != (x[i] < 0);
    v->level_db = db;
    if (!v->primed) {
        v->floor_db = db;
        v->primed = true;
    }
    v->voiced = db > v->floor_db + 10.0f && db > -55.0f && zc < VAD_FRAME / 2 && removed_db < 8.0f;
    if (db < v->floor_db)
        v->floor_db += (db - v->floor_db) * 0.3f;
    else if (!v->voiced)
        v->floor_db += (db - v->floor_db) * 0.02f;
    if (v->voiced) {
        v->voiced_ms += 20;
        v->quiet_ms = 0;
        if (v->voiced_ms >= v->onset_ms)
            v->speech = true;
    } else {
        v->quiet_ms += 20;
        if (v->quiet_ms >= v->hang_ms) {
            v->speech = false;
            v->voiced_ms = 0;
        } else if (!v->speech) {
            v->voiced_ms = MAX(0, v->voiced_ms - 20);
        }
    }
    return v->speech;
}

/* ---- echo cancellation: an adaptive filter learns the way from the speaker to the microphone ----
   Two NLMS filters over AEC_TAPS samples (64 ms) of what was played. The background one always learns; the one
   whose estimate is taken from the microphone gets its weights only when they clearly do better and nobody else
   seems to talk (the microphone louder than the echo could be: the Geigel test), and the background one goes back to
   them when it has lost its way. So the user's voice over the assistant's is neither learnt nor cancelled. */
#define AEC_TAPS 1024
#define AEC_HIST (AEC_TAPS + VAD_FRAME)

struct aec {
    float wb[AEC_TAPS], wf[AEC_TAPS];
    float ref[AEC_HIST]; /* played samples, the newest at the end, reversed for the dot products */
    float power;         /* of the last AEC_TAPS played samples */
    float erle_db;       /* how much echo it takes away, smoothed over the frames where only the echo was there */
    float removed_db;    /* what it took from the last frame */
};

static void aec_init(struct aec *a)
{
    memset(a, 0, sizeof(*a));
}

/* one frame each of the microphone and of what reached the speaker at the same time; out gets the microphone
   without the echo (it may be mic) */
static void aec_frame(struct aec *a, const float *mic, const float *played, float *out)
{
    /* ref holds the history backwards: ref[0] is the newest sample, so the filter is a plain dot product */
    float eb2 = 0, ef2 = 0, em2 = 0, mic_peak = 0, ref_peak = 0;
    for (int i = 0; i < VAD_FRAME; i++) {
        float in = played[i], gone = a->ref[AEC_TAPS - 1];
        memmove(a->ref + 1, a->ref, sizeof(float) * (AEC_TAPS - 1));
        a->ref[0] = in;
        a->power += in * in - gone * gone;
        if (a->power < 0)
            a->power = 0;
        float eb = mic[i] - dot(a->wb, a->ref, AEC_TAPS), ef = mic[i] - dot(a->wf, a->ref, AEC_TAPS);
        if (a->power > 1e-4f) {
            float g = 0.5f * eb / (a->power + 1e-6f);
            for (int k = 0; k < AEC_TAPS; k++)
                a->wb[k] += g * a->ref[k];
        }
        eb2 += eb * eb;
        ef2 += ef * ef;
        em2 += mic[i] * mic[i];
        mic_peak = MAX(mic_peak, mic[i] < 0 ? -mic[i] : mic[i]);
        out[i] = ef;
    }
    for (int k = 0; k < AEC_TAPS; k++)
        ref_peak = MAX(ref_peak, a->ref[k] < 0 ? -a->ref[k] : a->ref[k]);
    bool double_talk = mic_peak > 0.6f * ref_peak + 1e-3f;
    if (!double_talk && eb2 < 0.5f * ef2 && eb2 < 0.1f * em2)
        memcpy(a->wf, a->wb, sizeof(a->wf));
    else if (double_talk || (eb2 > 4.0f * ef2 && ef2 < em2))
        memcpy(a->wb, a->wf, sizeof(a->wb));
    a->removed_db = (float)(10.0 * ai_log((em2 + 1e-9) / (ef2 + 1e-9)) / 2.302585092994046);
    if (!double_talk && em2 > 1e-5f)
        a->erle_db += (a->removed_db - a->erle_db) * 0.1f;
}

/* ---- barge-in: the user talks while the assistant is answering ----
   Two steps, so a cough or a door does not throw the answer away: a short burst pauses the voice (BARGE_PAUSE); if
   it goes on into real speech the answer is cancelled and the user is listened to (BARGE_CANCEL); if it stops first,
   the voice goes on where it was (BARGE_RESUME). Until the echo canceller has learnt the room nothing counts: the
   assistant's own voice would sound like the user's. */
enum barge_event { BARGE_NONE, BARGE_PAUSE, BARGE_RESUME, BARGE_CANCEL };
#define BARGE_PAUSE_MS 120
#define BARGE_CONFIRM_MS 400
#define BARGE_GIVE_UP_MS 500
#define BARGE_READY_DB 12.0f

struct barge {
    int voiced_ms, quiet_ms;
    bool paused;
};

/* one 20 ms frame while the assistant answers; voiced: the VAD's view of the frame after echo cancellation */
static enum barge_event barge_frame(struct barge *b, bool voiced, bool ready)
{
    if (!ready && !b->paused)
        return BARGE_NONE;
    if (voiced) {
        b->voiced_ms += 20;
        b->quiet_ms = 0;
    } else {
        b->quiet_ms += 20;
    }
    if (!b->paused && b->voiced_ms >= BARGE_PAUSE_MS) {
        b->paused = true;
        return BARGE_PAUSE;
    }
    if (b->paused && b->voiced_ms >= BARGE_CONFIRM_MS) {
        b->paused = false;
        b->voiced_ms = b->quiet_ms = 0;
        return BARGE_CANCEL;
    }
    if (b->quiet_ms >= (b->paused ? BARGE_GIVE_UP_MS : 200)) {
        b->voiced_ms = 0;
        if (b->paused) {
            b->paused = false;
            return BARGE_RESUME;
        }
    }
    return BARGE_NONE;
}

/* ---- streaming recognition: words while they are said ----
   Every second or so what has been said so far is written down again; the words two of these agree on from the
   start no longer change (LocalAgreement-2), the rest is shown as it may still be. */
struct stream_asr {
    int prev[WH_MAX_TOKENS], n_prev; /* the last attempt */
    int final[WH_MAX_TOKENS], n_final; /* what no longer changes */
};

/* a new attempt at the whole utterance; returns how many tokens are final now (they are in s->final) */
static int stream_update(struct stream_asr *s, const int *toks, int n)
{
    int same = 0;
    while (same < n && same < s->n_prev && toks[same] == s->prev[same])
        same++;
    for (; s->n_final < same; s->n_final++)
        s->final[s->n_final] = toks[s->n_final];
    memcpy(s->prev, toks, sizeof(int) * (size_t)n);
    s->n_prev = n;
    return s->n_final;
}

/* the end of the utterance: the last attempt, which saw all of it, is the answer */
static void stream_finish(struct stream_asr *s)
{
    memcpy(s->final, s->prev, sizeof(int) * (size_t)s->n_prev);
    s->n_final = s->n_prev;
}

/* ---- ai voice-test: the three on a made-up room ----
   The assistant speaks (a voice of harmonics at 180 Hz in syllables) through a room with two echoes and some hiss;
   the user coughs at 1.8 s, says something short at 2.4 s and talks over the answer from 3.4 s. */
struct synth_voice {
    float pitch, syllables, amp;
    double phase, syl;
};

static float synth_voice_next(struct synth_voice *v)
{
    v->phase += 2 * 3.14159265358979 * v->pitch / 16000;
    v->syl += 2 * 3.14159265358979 * v->syllables / 16000;
    if (v->phase > 2 * 3.14159265358979)
        v->phase -= 2 * 3.14159265358979;
    double s, c, s1, c1, es, ec;
    ai_sincos(v->phase, &s1, &c1);
    ai_sincos(v->syl, &es, &ec);
    double sum = 0, sh = s1, ch = c1;
    for (int h = 1; h <= 8; h++) { /* sin(h x) by turning the angle h times */
        sum += sh / h;
        s = sh * c1 + ch * s1;
        c = ch * c1 - sh * s1;
        sh = s;
        ch = c;
    }
    return (float)(v->amp * sum * (0.5 - 0.5 * ec) * 0.5);
}

static float test_noise(uint32_t *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (float)((*seed >> 8) & 0xffff) / 32768.0f - 1.0f;
}

/* ai voice-test: what it prints, kept (the simulation takes a while on a slow machine) */
static char vt_out[512];

static void vt_printf(const char *fmt, ...)
{
    size_t n = strlen(vt_out);
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(vt_out + n, sizeof(vt_out) - n, fmt, ap);
    va_end(ap);
}

static int voice_test_command(struct shell *sh)
{
    if (*vt_out) { /* the same every time: worked out once */
        sh_puts(sh, vt_out);
        return 0;
    }
    enum { N = 5 * 16000, D1 = 400, D2 = 900 };
    float *played = kzalloc(sizeof(float) * N), *mic = kzalloc(sizeof(float) * N), *user = kzalloc(sizeof(float) * N),
          *out = kzalloc(sizeof(float) * N);
    struct aec *a = kzalloc(sizeof(*a));
    if (!played || !mic || !user || !out || !a) {
        kfree(played), kfree(mic), kfree(user), kfree(out), kfree(a);
        return sh_error(sh, "ai", "%s", err_str(E_NOMEM));
    }
    struct synth_voice me = {180, 3.0f, 0.3f, 0, 0};
    for (int i = 0; i < N; i++)
        played[i] = synth_voice_next(&me);
    static const struct {
        float from, to;
    } says[] = {{1.8f, 1.88f}, {2.4f, 2.6f}, {3.4f, 4.6f}};
    for (size_t k = 0; k < ARRAY_SIZE(says); k++) {
        struct synth_voice u = {120, 4.0f, k == 2 ? 0.25f : 0.3f, 0, 0};
        for (int i = (int)(says[k].from * 16000); i < (int)(says[k].to * 16000); i++)
            user[i] = synth_voice_next(&u);
    }
    uint32_t seed = 7;
    for (int i = 0; i < N; i++)
        mic[i] = (i >= D1 ? 0.5f * played[i - D1] : 0) + (i >= D2 ? 0.2f * played[i - D2] : 0) + user[i] +
                 0.002f * test_noise(&seed);

    /* the voice detector alone, on the user without the assistant */
    struct vad v;
    vad_init(&v, VAD_ONSET_MS, VAD_HANG_MS);
    int starts = 0;
    float first = -1, last = -1;
    bool was = false;
    for (int f = 0; f + VAD_FRAME <= N; f += VAD_FRAME) {
        float clean[VAD_FRAME];
        for (int i = 0; i < VAD_FRAME; i++)
            clean[i] = user[f + i] + 0.002f * test_noise(&seed);
        bool now = vad_frame(&v, clean, 0);
        if (now && !was) {
            starts++;
            if (first < 0 && f >= 3 * 16000)
                first = (float)f / 16000;
        }
        if (!now && was)
            last = (float)f / 16000;
        was = now;
    }
    vt_printf("vad: %d stretches of speech, the long one from %d ms to %d ms\n", starts, (int)(first * 1000),
              (int)(last * 1000));
    /* the cough is too short for speech; the word and the talk are two stretches */
    if (starts == 2 && first > 3.39f && first < 3.55f && last > 4.6f && last < 5.0f)
        vt_printf("vad: ok\n");

    /* echo cancellation and barge-in on what the microphone hears */
    aec_init(a);
    vad_init(&v, VAD_ONSET_MS, VAD_HANG_MS);
    struct barge b = {0};
    enum barge_event seen[8];
    int n_seen = 0, at_ms[8];
    for (int f = 0; f + VAD_FRAME <= N; f += VAD_FRAME) {
        aec_frame(a, mic + f, played + f, out + f);
        vad_frame(&v, out + f, a->removed_db);
        enum barge_event e = BARGE_NONE;
        if (!n_seen || seen[n_seen - 1] != BARGE_CANCEL) /* after a cancel the answer is gone: nothing to barge into */
            e = barge_frame(&b, v.voiced, a->erle_db > BARGE_READY_DB);
        if (e != BARGE_NONE && n_seen < 8) {
            at_ms[n_seen] = f / 16;
            seen[n_seen++] = e;
        }
    }
    double echo_in = 0, echo_out = 0, user_in = 0, left = 0;
    for (int i = 16000; i < (int)(1.7f * 16000); i++) {
        echo_in += (double)mic[i] * mic[i];
        echo_out += (double)out[i] * out[i];
    }
    for (int i = (int)(3.5f * 16000); i < (int)(4.5f * 16000); i++) {
        user_in += (double)user[i] * user[i];
        left += (double)(out[i] - user[i]) * (out[i] - user[i]);
    }
    int erle = (int)(10 * ai_log(echo_in / (echo_out + 1e-12)) / 2.302585092994046),
        under = (int)(10 * ai_log(user_in / (left + 1e-12)) / 2.302585092994046);
    vt_printf("aec: echo reduced by %d dB; over the user's voice the echo left is %d dB below it\n", erle, under);
    if (erle >= 15 && under >= 6)
        vt_printf("aec: ok\n");
    static const char *const names[] = {"none", "pause", "resume", "cancel"};
    vt_printf("barge-in:");
    for (int i = 0; i < n_seen; i++)
        vt_printf(" %s at %d ms", names[seen[i]], at_ms[i]);
    vt_printf("\n");
    if (n_seen == 4 && seen[0] == BARGE_PAUSE && at_ms[0] >= 2400 && at_ms[0] < 2600 &&
             seen[1] == BARGE_RESUME && seen[2] == BARGE_PAUSE && at_ms[2] >= 3400 && at_ms[2] < 3600 &&
             seen[3] == BARGE_CANCEL && at_ms[3] < 4100)
        vt_printf("barge-in: ok\n");
    kfree(played), kfree(mic), kfree(user), kfree(out), kfree(a);
    sh_puts(sh, vt_out);
    return 0;
}
