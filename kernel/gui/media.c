#include <synth/command.h>
#include <synth/shell.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

/* The media engine: a small synthesizer that plays the built-in songs, a WAV player, and the band levels the
   visualizer shows. It runs from the desktop pump and writes to the sound card when there is one. */

#define BASE_RATE 22050
#define ECHO_FRAMES 8192
#define MAX_VOICES 16
#define BLOCK 512

enum inst {
    I_KICK,
    I_SNARE,
    I_HAT,
    I_BASS,
    I_PAD,
    I_LEAD,
    I_KEYS,
};

struct voice {
    bool on;
    enum inst inst;
    uint32_t phase, phase2, inc, inc2;
    int env, peak, attack, decay, hold, age, lp, pan, note;
};

struct song {
    const char *title, *artist;
    int bpm, swing, bars;
    const uint8_t (*chords)[3];
    int chord_count;
    uint16_t kick, snare, hat;
    const int8_t *arp;
    int arp_len;
    uint16_t bass;
    int pad, lead, keys, crackle, drums_from;
};

static const uint32_t note_inc[128] = {
1592507u, 1687202u, 1787529u, 1893821u, 2006433u, 2125742u, 2252145u, 2386065u,
2527947u, 2678267u, 2837525u, 3006253u, 3185014u, 3374405u, 3575058u, 3787642u,
4012867u, 4251484u, 4504291u, 4772130u, 5055895u, 5356535u, 5675051u, 6012507u,
6370029u, 6748811u, 7150116u, 7575284u, 8025734u, 8502969u, 9008582u, 9544260u,
10111791u, 10713070u, 11350102u, 12025014u, 12740059u, 13497622u, 14300233u, 15150569u,
16051469u, 17005939u, 18017164u, 19088521u, 20223583u, 21426140u, 22700205u, 24050029u,
25480118u, 26995245u, 28600466u, 30301138u, 32102938u, 34011878u, 36034329u, 38177042u,
40447167u, 42852281u, 45400410u, 48100059u, 50960237u, 53990491u, 57200933u, 60602277u,
64205876u, 68023756u, 72068659u, 76354085u, 80894335u, 85704562u, 90800821u, 96200119u,
101920475u, 107980982u, 114401866u, 121204555u, 128411752u, 136047513u, 144137319u, 152708170u,
161788670u, 171409125u, 181601642u, 192400238u, 203840951u, 215961965u, 228803732u, 242409110u,
256823505u, 272095026u, 288274638u, 305416340u, 323577341u, 342818251u, 363203285u, 384800476u,
407681903u, 431923931u, 457607464u, 484818220u, 513647011u, 544190052u, 576549277u, 610832681u,
647154682u, 685636502u, 726406570u, 769600953u, 815363807u, 863847862u, 915214929u, 969636440u,
1027294023u, 1088380105u, 1153098554u, 1221665362u, 1294309365u, 1371273005u, 1452813141u, 1539201906u,
1630727614u, 1727695724u, 1830429858u, 1939272881u, 2054588047u, 2176760210u, 2306197108u, 2443330725u
};

static const int16_t sine[256] = {0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731, 23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793, -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757, -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279, -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804
};

static const uint8_t drive_chords[][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
static const int8_t drive_arp[] = {0, 1, 2, 1, 0, 2, 1, 2, 0, 1, 2, 3, 2, 1, 0, 1};
static const uint8_t days_chords[][3] = {{53, 57, 64}, {52, 55, 62}, {50, 53, 60}, {48, 52, 59}};
static const int8_t days_arp[] = {-1, -1, 2, -1, -1, 1, -1, -1, -1, -1, 2, -1, 3, -1, -1, -1};
static const uint8_t wind_chords[][3] = {{50, 53, 57}, {46, 50, 53}, {53, 57, 60}, {48, 52, 55}};
static const int8_t wind_arp[] = {2, -1, -1, 3, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, 1, -1};

static const struct song songs[] = {
    {"Sunset Drive", "SynthOS Ambient", 100, 0, 52, drive_chords, 4, 0x1111, 0x1010, 0x5555, drive_arp, 16, 0x5555,
     150, 120, 0, 0, 4},
    {"Better Days", "SynthOS Lofi", 78, 3, 40, days_chords, 4, 0x0121, 0x1010, 0x5555, days_arp, 16, 0x0101, 90, 70,
     150, 40, 2},
    {"Desert Wind", "SynthOS Ambient", 64, 0, 32, wind_chords, 4, 0, 0, 0x0000, wind_arp, 16, 0x0001, 190, 90, 0, 0,
     99},
};

static struct {
    int song;
    bool playing, device;
    int rate;
    uint64_t frame, step_frame;
    int step;
    uint64_t wall;
    struct voice voices[MAX_VOICES];
    int16_t *echo;
    int echo_pos;
    uint32_t noise;
    int hat_lp;
    int levels[MEDIA_BANDS];
    struct media_track track;
    char title[64], artist[64];
    int16_t *wav;
    uint32_t wav_frames, wav_rate, wav_pos_frac;
    uint64_t wav_pos;
    int split[3];
    bool user_file;
} m;

static uint32_t rnd(void)
{
    m.noise = m.noise * 1664525u + 1013904223u;
    return m.noise;
}

static int song_length_ms(const struct song *s)
{
    return s->bars * 4 * 60000 / s->bpm;
}

static uint32_t inc_for(int note)
{
    return (uint32_t)((uint64_t)note_inc[CLAMP(note, 0, 127)] * BASE_RATE / (uint64_t)m.rate);
}

static struct voice *voice_new(enum inst inst)
{
    struct voice *best = &m.voices[0];
    for (int i = 0; i < MAX_VOICES; i++) {
        struct voice *v = &m.voices[i];
        if (!v->on)
            return memset(v, 0, sizeof(*v)), v->inst = inst, v->on = true, v;
        if (v->env < best->env)
            best = v;
    }
    memset(best, 0, sizeof(*best));
    best->inst = inst;
    best->on = true;
    return best;
}

static int ms_frames(int ms)
{
    return MAX(1, m.rate * ms / 1000);
}

static void trigger(enum inst inst, int note, int level, int decay_ms, int attack_ms, int hold_ms, int pan)
{
    struct voice *v = voice_new(inst);
    v->note = note;
    v->inc = note ? inc_for(note) : 0;
    v->inc2 = note ? inc_for(note) + inc_for(note) / 260 : 0;
    v->peak = level << 7;
    v->attack = ms_frames(attack_ms);
    v->hold = ms_frames(hold_ms);
    v->decay = ms_frames(decay_ms);
    v->pan = pan;
    v->env = attack_ms ? 0 : v->peak;
}

static void step(const struct song *s)
{
    int st = m.step % 16, bar = m.step / 16;
    const uint8_t *ch = s->chords[bar % s->chord_count];
    int step_ms = 60000 / s->bpm / 4;
    bool drums = bar >= s->drums_from && bar < s->bars - 2;
    if (drums && (s->kick >> st & 1))
        trigger(I_KICK, 0, 230, 260, 0, 0, 128);
    if (drums && (s->snare >> st & 1))
        trigger(I_SNARE, 0, 120, 180, 0, 0, 120);
    if (drums && (s->hat >> st & 1))
        trigger(I_HAT, 0, st & 2 ? 40 : 60, 50, 0, 0, st & 4 ? 80 : 176);
    if (s->bass >> st & 1)
        trigger(I_BASS, ch[0] - 24, 140, step_ms * 3, 4, step_ms, 128);
    if (st == 0 && s->pad)
        for (int i = 0; i < 3; i++)
            trigger(I_PAD, ch[i], s->pad / 2, step_ms * 10, step_ms * 4, step_ms * 10, 60 + i * 68);
    if (st % 4 == 0 && s->keys)
        for (int i = 0; i < 3; i++)
            trigger(I_KEYS, ch[i] + 12, s->keys / 2 - i * 8, step_ms * 6, 2, 0, 90 + i * 38);
    int a = s->arp[st % s->arp_len];
    if (a >= 0 && s->lead && bar >= 1) {
        int note = a == 3 ? ch[0] + 12 : ch[a];
        trigger(I_LEAD, note + 12, s->lead, step_ms * 2, 2, 0, st & 1 ? 90 : 166);
    }
}

static int band_of(int note)
{
    return CLAMP((note - 24) * MEDIA_BANDS / 84, 0, MEDIA_BANDS - 1);
}

static void voice_levels(const struct voice *v)
{
    int e = v->env >> 7;
    int b;
    switch (v->inst) {
    case I_KICK: b = 0; break;
    case I_SNARE: b = MEDIA_BANDS / 2; break;
    case I_HAT: b = MEDIA_BANDS - 2; break;
    default: b = band_of(v->note); break;
    }
    int lv = e * 4;
    m.levels[b] = MAX(m.levels[b], lv);
    if (b > 0)
        m.levels[b - 1] = MAX(m.levels[b - 1], lv / 2);
    if (b + 1 < MEDIA_BANDS)
        m.levels[b + 1] = MAX(m.levels[b + 1], lv * 2 / 3);
}

static void synth(int16_t *out, int frames)
{
    const struct song *s = &songs[m.song];
    int step_len = m.rate * 60 / s->bpm / 4;
    for (int f = 0; f < frames; f++) {
        if (m.frame >= m.step_frame) {
            step(s);
            m.step++;
            int swing = s->swing && (m.step & 1) ? step_len * s->swing / 10 : s->swing ? -step_len * s->swing / 10 : 0;
            m.step_frame += (uint64_t)(step_len + swing);
        }
        int32_t l = 0, r = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            struct voice *v = &m.voices[i];
            if (!v->on)
                continue;
            if (v->age < v->attack)
                v->env = v->peak * (v->age + 1) / v->attack;
            else if (v->age >= v->attack + v->hold)
                v->env -= MAX(1, v->peak / v->decay);
            if (v->env <= 0) {
                v->on = false;
                continue;
            }
            v->age++;
            int x;
            switch (v->inst) {
            case I_KICK: {
                int sweep = 160 - MIN(v->age * 120 / ms_frames(60), 115);
                v->phase += (uint32_t)((uint64_t)sweep * 4294967296ull / (uint64_t)m.rate);
                x = sine[v->phase >> 24];
                break;
            }
            case I_SNARE: {
                v->phase += inc_for(52);
                x = ((int)(rnd() >> 17) - 16384) + sine[v->phase >> 24] / 3;
                break;
            }
            case I_HAT: {
                int n = (int)(rnd() >> 16) - 32768;
                m.hat_lp += (n - m.hat_lp) / 3;
                x = n - m.hat_lp;
                break;
            }
            case I_BASS: {
                v->phase += v->inc;
                int saw = (int)(v->phase >> 16) - 32768;
                v->lp += (saw - v->lp) / 6;
                x = v->lp + sine[v->phase >> 24] / 2;
                break;
            }
            case I_PAD: {
                v->phase += v->inc;
                v->phase2 += v->inc2;
                int saw = ((int)(v->phase >> 16) - 32768 + (int)(v->phase2 >> 16) - 32768) / 2;
                v->lp += (saw - v->lp) / 14;
                x = v->lp;
                break;
            }
            case I_KEYS:
                v->phase += v->inc;
                v->phase2 += v->inc * 2;
                x = sine[v->phase >> 24] + sine[v->phase2 >> 24] / 4;
                break;
            default:
                v->phase += v->inc;
                x = (v->phase >> 31 ? 20000 : -20000) / 2 + ((int)(v->phase >> 16) - 32768) / 3;
                v->lp += (x - v->lp) / 3;
                x = v->lp;
                break;
            }
            int a = (int)((int64_t)x * v->env >> 15);
            l += a * (256 - v->pan) >> 8;
            r += a * v->pan >> 8;
        }
        if (s->crackle && (rnd() & 4095) < (uint32_t)s->crackle / 20) {
            int pop = (int)(rnd() >> 20) - 2048;
            l += pop;
            r += pop;
        }
        if (m.echo) {
            int16_t *e = &m.echo[m.echo_pos * 2];
            int el = e[0], er = e[1];
            l += er / 3;
            r += el / 3;
            e[0] = (int16_t)CLAMP(l / 2, -32767, 32767);
            e[1] = (int16_t)CLAMP(r / 2, -32767, 32767);
            m.echo_pos = (m.echo_pos + 1) % MIN(ECHO_FRAMES, step_len * 3);
        }
        l = l * 4 / 5;
        r = r * 4 / 5;
        if (out) {
            out[f * 2] = (int16_t)CLAMP(l, -32767, 32767);
            out[f * 2 + 1] = (int16_t)CLAMP(r, -32767, 32767);
        }
        m.frame++;
    }
    for (int i = 0; i < MAX_VOICES; i++)
        if (m.voices[i].on)
            voice_levels(&m.voices[i]);
}

static void wav_render(int16_t *out, int frames)
{
    uint64_t step = (uint64_t)m.wav_rate * 65536 / (uint64_t)m.rate;
    int32_t lo = 0, mid = 0, hi = 0;
    for (int f = 0; f < frames; f++) {
        uint64_t idx = m.wav_pos >> 16;
        int l = 0, r = 0;
        if (idx < m.wav_frames) {
            l = m.wav[idx * 2];
            r = m.wav[idx * 2 + 1];
        }
        if (out) {
            out[f * 2] = (int16_t)l;
            out[f * 2 + 1] = (int16_t)r;
        }
        int x = (l + r) / 2;
        m.split[0] += (x - m.split[0]) / 24;
        m.split[1] += (x - m.split[1]) / 4;
        int a = m.split[0], b = m.split[1] - m.split[0], c = x - m.split[1];
        lo += a < 0 ? -a : a;
        mid += b < 0 ? -b : b;
        hi += c < 0 ? -c : c;
        m.wav_pos += step;
        m.frame++;
    }
    frames = MAX(frames, 1);
    int bands[3] = {lo / frames, mid / frames, hi / frames};
    for (int i = 0; i < MEDIA_BANDS; i++) {
        int k = i * 2 / MEDIA_BANDS, t = i * 2 % MEDIA_BANDS;
        int v = bands[k] + (bands[MIN(k + 1, 2)] - bands[k]) * t / MEDIA_BANDS;
        m.levels[i] = MAX(m.levels[i], v / 6);
    }
}

static void render(int16_t *out, int frames)
{
    if (m.wav)
        wav_render(out, frames);
    else
        synth(out, frames);
}

static void reset_song(void)
{
    memset(m.voices, 0, sizeof(m.voices));
    m.frame = m.step_frame = 0;
    m.step = 0;
    m.wav_pos = 0;
    if (m.echo)
        memset(m.echo, 0, ECHO_FRAMES * 4);
    if (!m.wav) {
        str_copy(m.title, songs[m.song].title, sizeof(m.title));
        str_copy(m.artist, songs[m.song].artist, sizeof(m.artist));
    }
}

static void ensure_device(void)
{
    if (m.rate)
        return;
    int rate = hal_audio_open(44100);
    m.device = rate > 0;
    m.rate = rate > 0 ? rate : BASE_RATE;
    if (!m.echo)
        m.echo = kzalloc(ECHO_FRAMES * 4);
}

int media_count(void)
{
    return (int)ARRAY_SIZE(songs);
}

const char *media_song_title(int i)
{
    return i >= 0 && i < media_count() ? songs[i].title : "";
}

const char *media_song_artist(int i)
{
    return i >= 0 && i < media_count() ? songs[i].artist : "";
}

static int length_ms(void)
{
    if (m.wav)
        return (int)((uint64_t)m.wav_frames * 1000 / MAX(m.wav_rate, 1u));
    return song_length_ms(&songs[m.song]);
}

const struct media_track *media_track(void)
{
    if (!m.title[0])
        reset_song();
    m.track.title = m.title;
    m.track.artist = m.artist;
    m.track.length_ms = length_ms();
    m.track.position_ms = m.rate ? (int)(m.frame * 1000 / (uint64_t)m.rate) : 0;
    m.track.playing = m.playing;
    m.track.index = m.wav ? -1 : m.song;
    return &m.track;
}

void media_play(int index)
{
    ensure_device();
    kfree(m.wav);
    m.wav = NULL;
    m.song = CLAMP(index, 0, media_count() - 1);
    reset_song();
    m.playing = true;
    m.wall = hal_ticks_ms();
}

void media_toggle(void)
{
    ensure_device();
    m.playing = !m.playing;
    m.wall = hal_ticks_ms();
}

void media_next(void)
{
    media_play(m.wav ? m.song : (m.song + 1) % media_count());
}

void media_prev(void)
{
    if (m.rate && m.frame > (uint64_t)m.rate * 3) {
        reset_song();
        return;
    }
    media_play(m.wav ? m.song : (m.song + media_count() - 1) % media_count());
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int media_open(const char *path)
{
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    const uint8_t *p = (const uint8_t *)data;
    if (size < 44 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        kfree(data);
        return E_INVAL;
    }
    int channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = NULL;
    uint32_t pcm_len = 0;
    for (size_t off = 12; off + 8 <= size;) {
        uint32_t len = rd32(p + off + 4);
        if (!memcmp(p + off, "fmt ", 4) && len >= 16) {
            channels = p[off + 10] | p[off + 11] << 8;
            rate = rd32(p + off + 12);
            bits = p[off + 22] | p[off + 23] << 8;
        } else if (!memcmp(p + off, "data", 4)) {
            pcm = p + off + 8;
            pcm_len = (uint32_t)MIN((size_t)len, size - off - 8);
        }
        off += 8 + len + (len & 1);
    }
    if (!pcm || (channels != 1 && channels != 2) || (bits != 8 && bits != 16) || rate < 4000 || rate > 192000) {
        kfree(data);
        return E_INVAL;
    }
    uint32_t frames = pcm_len / (uint32_t)(channels * bits / 8);
    int16_t *out = kmalloc((size_t)frames * 4 + 4);
    if (!out) {
        kfree(data);
        return E_NOMEM;
    }
    for (uint32_t i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            int ch = MIN(c, channels - 1);
            int v = bits == 16 ? (int16_t)(pcm[(i * channels + ch) * 2] | pcm[(i * channels + ch) * 2 + 1] << 8)
                               : (pcm[i * channels + ch] - 128) << 8;
            out[i * 2 + c] = (int16_t)v;
        }
    kfree(data);
    ensure_device();
    kfree(m.wav);
    m.wav = out;
    m.wav_frames = frames;
    m.wav_rate = rate;
    reset_song();
    char name[NAME_MAX_LEN];
    str_copy(name, path_basename(path), sizeof(name));
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = 0;
    str_copy(m.title, name, sizeof(m.title));
    str_copy(m.artist, "Music", sizeof(m.artist));
    m.playing = true;
    m.wall = hal_ticks_ms();
    return 0;
}

int media_play_pcm(int16_t *frames, uint32_t count, uint32_t rate, const char *title, const char *artist)
{
    ensure_device();
    kfree(m.wav);
    m.wav = frames;
    m.wav_frames = count;
    m.wav_rate = rate;
    reset_song();
    str_copy(m.title, title, sizeof(m.title));
    str_copy(m.artist, artist, sizeof(m.artist));
    m.playing = true;
    m.wall = hal_ticks_ms();
    return 0;
}

void media_seek(int ms)
{
    if (!m.rate)
        ensure_device();
    ms = CLAMP(ms, 0, length_ms());
    if (m.wav) {
        m.wav_pos = (uint64_t)ms * m.wav_rate / 1000 << 16;
        m.frame = (uint64_t)ms * (uint64_t)m.rate / 1000;
        return;
    }
    reset_song();
    uint64_t target = (uint64_t)ms * (uint64_t)m.rate / 1000;
    static int16_t scratch[BLOCK * 2];
    while (m.frame < target)
        synth(scratch, (int)MIN((uint64_t)BLOCK, target - m.frame));
}

void media_resume(void)
{
    ensure_device();
    m.playing = true;
    m.wall = hal_ticks_ms();
}

bool media_is_file(void)
{
    return m.wav != NULL;
}

void media_stop(void)
{
    m.playing = false;
}

int media_levels(int *out, int n)
{
    n = MIN(n, MEDIA_BANDS);
    for (int i = 0; i < n; i++)
        out[i] = CLAMP(m.levels[i], 0, 1024);
    return n;
}

static int volume = 100;
static bool muted;

int media_volume(void)
{
    return muted ? 0 : volume;
}

static void get_volume(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", volume);
}

static int set_volume(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 0 || v > 100)
        return E_INVAL;
    volume = (int)v;
    return 0;
}

static void get_mute(char *buf, size_t cap)
{
    setting_flag_get(muted, buf, cap);
}

static int set_mute(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return setting_flag(value, &muted);
}

static const struct setting s_volume = {"volume", "0..100", "output volume", get_volume, set_volume};
static const struct setting s_mute = {"mute", "on|off", "silence all sound", get_mute, set_mute};
SETTING(s_volume);
SETTING(s_mute);

bool media_tick(void)
{
    uint64_t now = hal_ticks_ms();
    bool active = m.playing;
    for (int i = 0; i < MEDIA_BANDS; i++) {
        if (m.levels[i])
            active = true;
        m.levels[i] = m.levels[i] * 7 / 8;
    }
    if (!m.playing)
        return active;
    static int16_t buf[BLOCK * 2];
    if (m.device) {
        for (int guard = 0; guard < 8; guard++) {
            int space = hal_audio_space();
            if (space < BLOCK / 2)
                break;
            int n = MIN(space, BLOCK);
            render(buf, n);
            int gain = muted ? 0 : volume * volume * 256 / 10000;
            if (gain < 256)
                for (int k = 0; k < n * 2; k++)
                    buf[k] = (int16_t)(buf[k] * gain / 256);
            hal_audio_write(buf, n);
        }
    } else {
        int frames = (int)MIN((now - m.wall) * (uint64_t)m.rate / 1000, (uint64_t)m.rate / 4);
        m.wall += (uint64_t)frames * 1000 / (uint64_t)m.rate;
        while (frames > 0) {
            int n = MIN(frames, BLOCK);
            render(NULL, n);
            frames -= n;
        }
    }
    if (m.frame * 1000 / (uint64_t)m.rate >= (uint64_t)length_ms()) {
        if (m.wav) {
            m.playing = false;
            reset_song();
        } else {
            media_play((m.song + 1) % media_count());
        }
    }
    return true;
}

COMMAND(music, NULL, "system", "music [play [n]|pause|next|prev|status|<file.wav>]", "play the built-in songs or a WAV file")
{
    const char *op = argc > 1 ? argv[1] : "status";
    if (!strcmp(op, "play")) {
        long n = m.song;
        if (argc > 2 && (!str_parse_long(argv[2], &n) || n < 1 || n > media_count()))
            return sh_error(sh, "music", "songs are numbered 1 to %d", media_count());
        media_play(argc > 2 ? (int)n - 1 : (int)n);
    } else if (!strcmp(op, "pause")) {
        media_stop();
    } else if (!strcmp(op, "next")) {
        media_next();
    } else if (!strcmp(op, "prev")) {
        media_prev();
    } else if (!strcmp(op, "list")) {
        for (int i = 0; i < media_count(); i++)
            sh_printf(sh, "%d  %s - %s\n", i + 1, songs[i].title, songs[i].artist);
        return 0;
    } else if (strcmp(op, "status")) {
        char path[PATH_MAX_LEN];
        if (sh_resolve(sh, op, path, sizeof(path)) != 0)
            return sh_error(sh, "music", "bad path: %s", op);
        int err = media_open(path);
        if (err)
            return sh_error(sh, "music", "%s: %s", op, err == E_INVAL ? "not a PCM WAV file" : err_str(err));
    }
    const struct media_track *t = media_track();
    const char *dev = hal_audio_name();
    sh_printf(sh, "%s - %s, %s %d:%02d of %d:%02d (%s)\n", t->title, t->artist, t->playing ? "playing" : "paused",
              t->position_ms / 60000, t->position_ms / 1000 % 60, t->length_ms / 60000, t->length_ms / 1000 % 60,
              dev ? dev : "no sound card");
    return 0;
}
