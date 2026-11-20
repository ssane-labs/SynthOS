/* Sound for 64-bit programs: Core Audio (mmdevapi, WASAPI shared mode) over the sound card (hal_audio_*).
   One mixer for every program: a ring of stereo frames at the card's rate, numbered by a clock. A stream writes
   what the program released straight into the ring ahead of the clock (converted from the program's format and
   rate, at its volume), so its padding is just how far ahead of the clock it has written. Whatever program calls
   next sends the frames that are about to play to the card; with no card the clock alone plays them.
   An event-driven stream's event turns signaled every period by itself (a deadline on the event object). */

#include <synth/types.h>

#ifdef SYNTH_X32

#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>

#include "win64.h"

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define E_NOTFOUND 0x80070490u
#define CLASS_NOT_REGISTERED 0x80040154u
#define AUDCLNT_E_NOT_INITIALIZED 0x88890001u
#define AUDCLNT_E_ALREADY_INITIALIZED 0x88890002u
#define AUDCLNT_E_WRONG_ENDPOINT_TYPE 0x88890003u
#define AUDCLNT_E_NOT_STOPPED 0x88890005u
#define AUDCLNT_E_BUFFER_TOO_LARGE 0x88890006u
#define AUDCLNT_E_OUT_OF_ORDER 0x88890007u
#define AUDCLNT_E_UNSUPPORTED_FORMAT 0x88890008u
#define AUDCLNT_E_INVALID_SIZE 0x88890009u
#define AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED 0x8889000eu
#define AUDCLNT_E_EVENTHANDLE_NOT_SET 0x88890014u
#define AUDCLNT_S_BUFFER_EMPTY 0x08890001u

#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                                      \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),               \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), a, b, c, d, e, f, g, h}

static const uint8_t CLSID_Enumerator[16] =
    GUID(0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
static const uint8_t IID_Unknown[16] = GUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t IID_Enumerator[16] =
    GUID(0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
static const uint8_t IID_Collection[16] =
    GUID(0x0bd7a1be, 0x7a1a, 0x44db, 0x83, 0x97, 0xcc, 0x53, 0x92, 0x38, 0x7b, 0x5e);
static const uint8_t IID_Device[16] = GUID(0xd666063f, 0x1587, 0x4e43, 0x81, 0xf1, 0xb9, 0x48, 0xe8, 0x07, 0x36, 0x3f);
static const uint8_t IID_Endpoint[16] =
    GUID(0x1be09788, 0x6894, 0x4089, 0x85, 0x86, 0x9a, 0x2a, 0x6c, 0x26, 0x5a, 0xc5);
static const uint8_t IID_PropertyStore[16] =
    GUID(0x886d8eeb, 0x8cf2, 0x4446, 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99);
static const uint8_t IID_Client[16] = GUID(0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
static const uint8_t IID_Client2[16] = GUID(0x726778cd, 0xf60a, 0x4eda, 0x82, 0xde, 0xe4, 0x76, 0x10, 0xcd, 0x78, 0xaa);
static const uint8_t IID_Client3[16] = GUID(0x7ed4ee07, 0x8e67, 0x4cd4, 0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad, 0x42);
static const uint8_t IID_Render[16] = GUID(0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);
static const uint8_t IID_Clock[16] = GUID(0xcd63314f, 0x3fba, 0x4a1b, 0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7);
static const uint8_t IID_SimpleVolume[16] =
    GUID(0x87ce5498, 0x68d6, 0x44e5, 0x92, 0x15, 0x6d, 0xa4, 0x7e, 0xf8, 0x83, 0xd8);
static const uint8_t IID_StreamVolume[16] =
    GUID(0x93014887, 0x242d, 0x4068, 0x8a, 0x15, 0xcf, 0x5e, 0x93, 0xb9, 0x0f, 0xe3);
static const uint8_t IID_ChannelVolume[16] =
    GUID(0x1c158861, 0xb533, 0x4b30, 0xb1, 0xcf, 0xe8, 0x53, 0xe5, 0x1c, 0x59, 0xb8);
static const uint8_t IID_Session[16] = GUID(0xf4b1a599, 0x7266, 0x4319, 0xa8, 0xca, 0xe7, 0x0a, 0xcb, 0x11, 0xe8, 0xcd);
static const uint8_t IID_Session2[16] =
    GUID(0xbfb7ff88, 0x7239, 0x4fc9, 0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d);
static const uint8_t IID_EndpointVolume[16] =
    GUID(0x5cdf2c82, 0x841e, 0x4546, 0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a);
static const uint8_t SUB_PCM[16] = GUID(0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
static const uint8_t SUB_FLOAT[16] = GUID(0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
/* PKEY_Device_FriendlyName, PKEY_DeviceInterface_FriendlyName, PKEY_Device_DeviceDesc */
static const uint8_t FMTID_Device[16] =
    GUID(0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0);
static const uint8_t FMTID_Interface[16] =
    GUID(0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22);
/* PKEY_AudioEngine_DeviceFormat */
static const uint8_t FMTID_Engine[16] =
    GUID(0xf19f064d, 0x082c, 0x4e27, 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c);

static bool same(const void *a, const uint8_t *b)
{
    return a && !memcmp(a, b, 16);
}

static void put64(uint64_t where, uint64_t v)
{
    if (where)
        memcpy(w64_ptr(where), &v, 8);
}

static void put32(uint64_t where, uint32_t v)
{
    if (where)
        memcpy(w64_ptr(where), &v, 4);
}

/* ---- the mixer: shared by every program */

#define MIX_FRAMES 65536u /* a power of two: about 1.4 s */
#define MIX_LEAD_MS 40    /* what the card is given ahead of the clock */
#define MAX_BUFFER_MS 1000

static struct {
    int32_t *ring; /* stereo, in 16-bit steps (sums can go past them) */
    int rate;
    bool card;
    int users;
    uint64_t t0, flushed;
} mix;

static uint64_t mix_now(void)
{
    return (hal_ticks_ms() - mix.t0) * (uint64_t)mix.rate / 1000;
}

/* where a stream's next frames go at the earliest: the card has what comes before */
static uint64_t mix_base(void)
{
    return mix_now() + (uint64_t)mix.rate * MIX_LEAD_MS / 1000;
}

static int32_t *mix_at(uint64_t frame)
{
    return mix.ring + (frame & (MIX_FRAMES - 1)) * 2;
}

static void mix_clear(uint64_t from, uint64_t to)
{
    for (uint64_t f = from; f < to; f++) {
        int32_t *p = mix_at(f);
        p[0] = p[1] = 0;
    }
}

/* sends what is due soon to the card */
static void mix_pump(void)
{
    if (!mix.users)
        return;
    uint64_t now = mix_now(), want = mix_base();
    if (mix.flushed + MIX_FRAMES / 2 < now) { /* nobody called for a long time, or the card is stuck */
        memset(mix.ring, 0, MIX_FRAMES * 8);
        mix.flushed = now;
    }
    if (!mix.card) {
        if (mix.flushed < want)
            mix_clear(mix.flushed, want);
        mix.flushed = MAX(mix.flushed, want);
        return;
    }
    int16_t out[256 * 2];
    while (mix.flushed < want) {
        int n = (int)MIN(want - mix.flushed, (uint64_t)256);
        n = MIN(n, (int)(MIX_FRAMES - (mix.flushed & (MIX_FRAMES - 1))));
        n = MIN(n, hal_audio_space());
        if (n <= 0)
            break;
        const int32_t *p = mix_at(mix.flushed);
        for (int i = 0; i < n * 2; i++)
            out[i] = (int16_t)CLAMP(p[i], -32768, 32767);
        int done = hal_audio_write(out, n);
        if (done <= 0)
            break;
        mix_clear(mix.flushed, mix.flushed + (uint64_t)done);
        mix.flushed += (uint64_t)done;
        if (done < n)
            break;
    }
}

static bool mix_join(void)
{
    if (!mix.ring && !(mix.ring = kzalloc(MIX_FRAMES * 8)))
        return false;
    if (mix.users++)
        return true;
    int r = hal_audio_open(48000);
    mix.card = r > 0;
    mix.rate = r > 0 ? r : 48000;
    mix.t0 = hal_ticks_ms();
    mix.flushed = 0;
    memset(mix.ring, 0, MIX_FRAMES * 8);
    return true;
}

static void mix_leave(void)
{
    if (mix.users > 0 && !--mix.users && mix.card)
        hal_audio_close();
}

/* ---- streams */

enum sample { S_U8, S_S16, S_S24, S_S32, S_F32 };

struct astream {
    int refs;
    bool init, running, event_mode;
    uint32_t rate, channels, block, buffer; /* buffer: frames */
    enum sample type;
    uint8_t *data;        /* the buffer GetBuffer hands out, in the program's memory */
    uint32_t got;         /* frames handed out by GetBuffer, 0 when none */
    uint64_t wpos;        /* the mixer frame the next one goes to */
    uint64_t written;     /* frames the program released in all */
    uint32_t pos;         /* resampling: where the next output frame is, 16.16, from the previous input frame */
    int32_t prev[2];
    int32_t volume;       /* 0..65536 */
    int32_t chan[8];      /* IAudioStreamVolume */
    bool mute;
    uint32_t event;       /* the program's event handle */
    uint32_t frozen;      /* padding while stopped */
    uint32_t calls, silent, underruns; /* for the log line at the end */
    struct astream *next;
};

static W32_STATE struct astream *streams;

static void stream_padding_raw(struct astream *s, uint32_t *pad)
{
    uint64_t base = mix_base();
    uint64_t ahead = s->wpos > base ? s->wpos - base : 0;
    uint64_t frames = ahead * s->rate / (uint64_t)mix.rate;
    *pad = (uint32_t)MIN(frames, (uint64_t)s->buffer);
}

static uint32_t stream_padding(struct astream *s)
{
    if (!s->running)
        return s->frozen;
    uint32_t pad;
    stream_padding_raw(s, &pad);
    return pad;
}

static float to_float(const uint8_t *p, enum sample t)
{
    switch (t) {
    case S_U8: return ((float)p[0] - 128.0f) / 128.0f;
    case S_S16: return (float)(int16_t)(p[0] | p[1] << 8) / 32768.0f;
    case S_S24: return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) /
                       8388608.0f;
    case S_S32: return (float)(int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                                        (uint32_t)p[3] << 24) / 2147483648.0f;
    default: {
        float v;
        memcpy(&v, p, 4);
        return v == v ? v : 0.0f;
    }
    }
}

/* one frame of the program's format as stereo in 16-bit steps */
static void frame_stereo(struct astream *s, const uint8_t *p, int32_t out[2])
{
    int size = (int)(s->block / s->channels);
    float c[8] = {0};
    int n = (int)MIN(s->channels, 8u);
    for (int i = 0; i < n; i++)
        c[i] = to_float(p + i * size, s->type) * (float)s->chan[i] / 65536.0f;
    float l, r;
    if (n == 1)
        l = r = c[0];
    else if (n < 6)
        l = c[0] + (n >= 3 ? 0.7f * c[2] : 0.0f), r = c[1] + (n >= 3 ? 0.7f * c[2] : 0.0f);
    else /* 5.1 and 7.1: FL FR FC LFE BL BR (SL SR) */
        l = c[0] + 0.7f * c[2] + 0.5f * (c[4] + c[6]), r = c[1] + 0.7f * c[2] + 0.5f * (c[5] + c[7]);
    float g = s->mute ? 0.0f : (float)s->volume / 65536.0f * 32767.0f;
    l *= g, r *= g;
    out[0] = (int32_t)(l > 1e6f ? 1e6f : l < -1e6f ? -1e6f : l);
    out[1] = (int32_t)(r > 1e6f ? 1e6f : r < -1e6f ? -1e6f : r);
}

/* the program released n frames at data: into the mixer at the stream's place, at the card's rate */
static void stream_mix(struct astream *s, const uint8_t *data, uint32_t n, bool silent)
{
    mix_pump();
    if (s->wpos < mix.flushed) { /* it fell behind: what is gone is gone */
        s->underruns += s->running;
        s->wpos = mix.flushed;
    }
    s->calls++;
    s->silent += silent ? n : 0;
    uint64_t limit = mix.flushed + MIX_FRAMES - 1;
    uint32_t step = (uint32_t)(((uint64_t)s->rate << 16) / (uint32_t)mix.rate);
    for (uint32_t i = 0; i < n;) {
        int32_t cur[2] = {0, 0};
        if (!silent)
            frame_stereo(s, data + (size_t)i * s->block, cur);
        /* output frames between the previous input frame and this one */
        while (s->pos < 0x10000u) {
            if (s->wpos < limit) {
                int32_t *p = mix_at(s->wpos);
                int32_t f = (int32_t)s->pos;
                p[0] += s->prev[0] + (int32_t)(((int64_t)(cur[0] - s->prev[0]) * f) >> 16);
                p[1] += s->prev[1] + (int32_t)(((int64_t)(cur[1] - s->prev[1]) * f) >> 16);
            }
            s->wpos++;
            s->pos += step;
        }
        s->pos -= 0x10000u;
        s->prev[0] = cur[0], s->prev[1] = cur[1];
        i++;
    }
    s->written += n;
}

static void stream_arm(struct astream *s, bool on)
{
    struct w32_object *o = s->event ? w32_handle_get((HANDLE)(uintptr_t)s->event) : NULL;
    if (!o || o->type != WH_EVENT)
        return;
    uint32_t period = 10;
    o->period = on ? period : 0;
    o->due = on ? hal_ticks_ms() + period : 0;
}

static void stream_drop(struct astream *s)
{
    if (!s || --s->refs > 0)
        return;
    if (s->running)
        stream_arm(s, false);
    if (s->init) {
        mix_leave();
        klog("wasapi: a stream of %u Hz, %u channels ends: %u frames in %u writes, %u silent, %u late",
             s->rate, s->channels, (uint32_t)s->written, s->calls, s->silent, s->underruns);
    }
    for (struct astream **p = &streams; *p; p = &(*p)->next)
        if (*p == s) {
            *p = s->next;
            break;
        }
    w32_free(s->data);
    w32_free(s);
}

/* ---- COM objects: one interface each */

enum spec_id {
    SP_ENUM, SP_COLL, SP_DEVICE, SP_ENDPOINT, SP_PROPS, SP_CLIENT, SP_RENDER, SP_CLOCK, SP_SIMPLEVOL, SP_STREAMVOL,
    SP_SESSION, SP_EPVOL, SP_COUNT
};

struct aobj {
    uint64_t vt;
    int32_t refs;
    uint32_t spec;
    struct astream *st; /* clients and their services */
    struct aobj *device; /* an endpoint object's device */
    uint32_t flow;       /* a device: 0 render, 1 capture */
};

typedef uint64_t (*amethod)(struct aobj *o, uint64_t *a, const uint64_t *f);

struct aspec {
    const char *name;
    const uint8_t *iid[3];
    int count;
    const amethod *fns;
};

static const struct aspec *aspec_of(int s);
static W32_STATE uint64_t *avt[SP_COUNT];
static W32_STATE uint8_t missing_seen[SP_COUNT][4];

static uint64_t avtable(int s);

static struct aobj *anew(int spec, struct astream *st)
{
    struct aobj *o = w32_zalloc(sizeof(*o));
    if (!o)
        return NULL;
    if (!(o->vt = avtable(spec))) {
        w32_free(o);
        return NULL;
    }
    o->refs = 1;
    o->spec = (uint32_t)spec;
    o->st = st;
    if (st)
        st->refs++;
    return o;
}

static uint64_t give(uint64_t out, struct aobj *o)
{
    put64(out, (uintptr_t)o);
    return o ? S_OK : E_OUTOFMEMORY;
}

static uint64_t m_qi(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    const void *iid = a[1] ? w64_ptr(a[1]) : NULL;
    if (!a[2])
        return E_POINTER;
    const struct aspec *v = aspec_of((int)o->spec);
    bool ok = same(iid, IID_Unknown);
    for (int i = 0; i < 3 && !ok; i++)
        ok = v->iid[i] && same(iid, v->iid[i]);
    if (!ok && o->spec == SP_DEVICE && same(iid, IID_Endpoint)) /* the device is also IMMEndpoint */
        return give(a[2], anew(SP_ENDPOINT, NULL));
    if (!ok) {
        put64(a[2], 0);
        return E_NOINTERFACE;
    }
    o->refs++;
    put64(a[2], (uintptr_t)o);
    return S_OK;
}

static uint64_t m_addref(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    return (uint64_t)++o->refs;
}

static uint64_t m_release(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    int32_t n = --o->refs;
    if (n == 0) {
        stream_drop(o->st);
        w32_free(o);
    }
    return (uint64_t)(uint32_t)MAX(n, 0);
}

static uint64_t m_ok(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    return S_OK;
}

static uint64_t h_aslot(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t code = (uint32_t)(uintptr_t)t, s = code >> 8, slot = code & 255;
    const struct aspec *v = aspec_of((int)s);
    struct aobj *o = (struct aobj *)(uintptr_t)a[0];
    if (slot == 0)
        return m_qi(o, a, f);
    if (slot == 1)
        return m_addref(o, a, f);
    if (slot == 2)
        return m_release(o, a, f);
    amethod fn = (int)slot < v->count ? v->fns[slot] : NULL;
    if (!fn) {
        if (slot < 32 && !(missing_seen[s][slot / 8] & (1u << (slot % 8)))) {
            missing_seen[s][slot / 8] |= (uint8_t)(1u << (slot % 8));
            klog("wasapi: %s method %u is not done yet", v->name, slot);
        }
        return E_NOTIMPL;
    }
    return fn(o, a, f);
}

static uint64_t avtable(int s)
{
    if (avt[s])
        return (uintptr_t)avt[s];
    const struct aspec *v = aspec_of(s);
    int n = MAX(v->count, 3);
    uint64_t *t = w32_zalloc(sizeof(uint64_t) * (size_t)n);
    if (!t)
        return 0;
    for (int i = 0; i < n; i++) {
        void *stub = w64_target_stub(h_aslot, (void *)(uintptr_t)((uint32_t)s << 8 | (uint32_t)i));
        if (!stub)
            return 0;
        t[i] = (uintptr_t)stub;
    }
    avt[s] = t;
    return (uintptr_t)t;
}

/* ---- IMMDeviceEnumerator, IMMDeviceCollection, IMMDevice, IMMEndpoint, IPropertyStore */

static W32_STATE struct aobj *render_device;

static struct aobj *device_obj(void)
{
    if (!render_device)
        render_device = anew(SP_DEVICE, NULL);
    if (render_device)
        render_device->refs++;
    return render_device;
}

/* EnumAudioEndpoints(flow, mask, collection): the one speaker for render (or all), none for capture */
static uint64_t m_enum_endpoints(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct aobj *c = anew(SP_COLL, NULL);
    if (c)
        c->flow = (uint32_t)a[1] == 1 || !((uint32_t)a[2] & 1) ? 0 : 1; /* flow: the count */
    return give(a[3], c);
}

/* GetDefaultAudioEndpoint(flow, role, device) */
static uint64_t m_default_endpoint(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[3])
        return E_POINTER;
    if ((uint32_t)a[1] == 1) {
        put64(a[3], 0);
        return E_NOTFOUND;
    }
    return give(a[3], device_obj());
}

static const uint16_t device_id[] = u"{0.0.0.00000000}.{5ab1d1f0-77c6-4d5a-9f3c-5f1e0a5d1a01}";

/* GetDevice(id, device) */
static uint64_t m_get_device(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    const uint16_t *id = a[1] ? w64_ptr(a[1]) : NULL;
    size_t i = 0;
    while (id && device_id[i] && id[i] == device_id[i])
        i++;
    if (!id || device_id[i] || id[i]) {
        put64(a[2], 0);
        return E_NOTFOUND;
    }
    return give(a[2], device_obj());
}

static uint64_t m_coll_count(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->flow);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_coll_item(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if ((uint32_t)a[1] >= o->flow) {
        put64(a[2], 0);
        return E_INVALIDARG;
    }
    return give(a[2], device_obj());
}

/* Activate(iid, context, params, out) */
static uint64_t m_activate(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    const void *iid = a[1] ? w64_ptr(a[1]) : NULL;
    if (!a[4])
        return E_POINTER;
    if (same(iid, IID_Client) || same(iid, IID_Client2) || same(iid, IID_Client3)) {
        struct astream *s = w32_zalloc(sizeof(*s));
        if (!s)
            return E_OUTOFMEMORY;
        s->volume = 65536;
        for (int i = 0; i < 8; i++)
            s->chan[i] = 65536;
        s->next = streams;
        streams = s;
        struct aobj *c = anew(SP_CLIENT, s);
        if (!c) {
            s->refs = 1;
            stream_drop(s);
        }
        return give(a[4], c);
    }
    if (same(iid, IID_EndpointVolume))
        return give(a[4], anew(SP_EPVOL, NULL));
    put64(a[4], 0);
    return E_NOINTERFACE;
}

static uint64_t m_open_props(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    return give(a[2], anew(SP_PROPS, NULL));
}

static uint64_t m_get_id(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_POINTER;
    uint16_t *s = w32_alloc(sizeof(device_id));
    if (s)
        memcpy(s, device_id, sizeof(device_id));
    put64(a[1], (uintptr_t)s);
    return s ? S_OK : E_OUTOFMEMORY;
}

static uint64_t m_get_state(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 1); /* DEVICE_STATE_ACTIVE */
    return a[1] ? S_OK : E_POINTER;
}

/* the device is also IMMEndpoint: GetDataFlow */
static uint64_t m_data_flow(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_props_count(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t mix_format(uint8_t *w);

/* GetValue(key, PROPVARIANT *): the names, and the engine's format */
static uint64_t m_props_value(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    const uint8_t *key = a[1] ? w64_ptr(a[1]) : NULL;
    uint8_t *pv = a[2] ? w64_ptr(a[2]) : NULL;
    if (!key || !pv)
        return E_POINTER;
    memset(pv, 0, 24);
    uint32_t pid;
    memcpy(&pid, key + 16, 4);
    const char *name = NULL;
    if (same(key, FMTID_Device) && (pid == 14 || pid == 2))
        name = pid == 14 ? "Speakers (SynthOS Audio)" : "Speakers";
    else if (same(key, FMTID_Interface) && pid == 2)
        name = "SynthOS Audio";
    if (name) {
        size_t n = strlen(name);
        uint16_t *s = w32_alloc((n + 1) * 2);
        if (!s)
            return E_OUTOFMEMORY;
        for (size_t i = 0; i <= n; i++)
            s[i] = (uint8_t)name[i];
        uint16_t vt = 31; /* VT_LPWSTR */
        memcpy(pv, &vt, 2);
        uint64_t p = (uintptr_t)s;
        memcpy(pv + 8, &p, 8);
        return S_OK;
    }
    if (same(key, FMTID_Engine) && pid == 0) {
        uint8_t *w = w32_alloc(40);
        if (!w)
            return E_OUTOFMEMORY;
        mix_format(w);
        uint16_t vt = 65; /* VT_BLOB */
        memcpy(pv, &vt, 2);
        uint32_t size = 40;
        uint64_t p = (uintptr_t)w;
        memcpy(pv + 8, &size, 4);
        memcpy(pv + 16, &p, 8);
    }
    return S_OK;
}

/* ---- IAudioClient (1 to 3) */

static uint64_t mix_format(uint8_t *w)
{
    int rate = mix.users ? mix.rate : 48000;
    uint16_t h[9] = {0xfffe, 2, (uint16_t)rate, (uint16_t)(rate >> 16), (uint16_t)(rate * 8),
                     (uint16_t)((rate * 8) >> 16), 8, 32, 22};
    memcpy(w, h, 18);
    uint16_t valid = 32;
    uint32_t mask = 3;
    memcpy(w + 18, &valid, 2);
    memcpy(w + 20, &mask, 4);
    memcpy(w + 24, SUB_FLOAT, 16);
    return S_OK;
}

/* what a WAVEFORMATEX says, or false for what is not done */
static bool parse_format(const uint8_t *w, uint32_t *rate, uint32_t *channels, uint32_t *block, enum sample *type)
{
    uint16_t tag, ch, align, bits, extra;
    memcpy(&tag, w, 2), memcpy(&ch, w + 2, 2), memcpy(rate, w + 4, 4);
    memcpy(&align, w + 12, 2), memcpy(&bits, w + 14, 2), memcpy(&extra, w + 16, 2);
    bool is_float = tag == 3;
    if (tag == 0xfffe) {
        if (extra < 22)
            return false;
        if (same(w + 24, SUB_FLOAT))
            is_float = true;
        else if (!same(w + 24, SUB_PCM))
            return false;
    } else if (tag != 1 && tag != 3)
        return false;
    if (!ch || ch > 8 || *rate < 4000 || *rate > 384000 || align != ch * (bits / 8) || bits % 8)
        return false;
    if (is_float)
        *type = bits == 32 ? S_F32 : (enum sample)-1;
    else
        *type = bits == 8 ? S_U8 : bits == 16 ? S_S16 : bits == 24 ? S_S24 : bits == 32 ? S_S32 : (enum sample)-1;
    *channels = ch;
    *block = align;
    return (int)*type >= 0;
}

/* Initialize(mode, flags, duration, period, format, session) */
static uint64_t m_initialize(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    if (s->init)
        return AUDCLNT_E_ALREADY_INITIALIZED;
    if ((uint32_t)a[1] != 0)
        return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    uint32_t flags = (uint32_t)a[2];
    if (flags & 0x20000) /* loopback: this is not a recording device */
        return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    if (!a[5])
        return E_POINTER;
    if (!parse_format(w64_ptr(a[5]), &s->rate, &s->channels, &s->block, &s->type))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    int64_t duration = (int64_t)a[3];
    uint64_t ms = duration > 0 ? (uint64_t)duration / 10000 : 0;
    ms = CLAMP(ms, 20u, (uint64_t)MAX_BUFFER_MS);
    s->buffer = (uint32_t)(ms * s->rate / 1000);
    if (!(s->data = w32_zalloc((size_t)s->buffer * s->block)))
        return E_OUTOFMEMORY;
    if (!mix_join()) {
        w32_free(s->data);
        s->data = NULL;
        return E_OUTOFMEMORY;
    }
    s->event_mode = flags & 0x40000;
    s->wpos = mix_base();
    s->init = true;
    return S_OK;
}

static uint64_t m_buffer_size(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (!o->st->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    put32(a[1], o->st->buffer);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_latency(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (!o->st->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    put64(a[1], MIX_LEAD_MS * 10000ull);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_padding(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (!o->st->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    if (!a[1])
        return E_POINTER;
    mix_pump();
    put32(a[1], stream_padding(o->st));
    return S_OK;
}

/* IsFormatSupported(mode, format, closest): any PCM or float the mixer converts from */
static uint64_t m_format_supported(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[3], 0);
    if (!a[2])
        return E_POINTER;
    if ((uint32_t)a[1] != 0)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    uint32_t rate, ch, block;
    enum sample t;
    if (parse_format(w64_ptr(a[2]), &rate, &ch, &block, &t))
        return S_OK;
    if (a[3]) {
        uint8_t *w = w32_alloc(40);
        if (w) {
            mix_format(w);
            put64(a[3], (uintptr_t)w);
            return S_FALSE;
        }
    }
    return AUDCLNT_E_UNSUPPORTED_FORMAT;
}

static uint64_t m_mix_format(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_POINTER;
    uint8_t *w = w32_alloc(40);
    if (!w)
        return E_OUTOFMEMORY;
    mix_format(w);
    put64(a[1], (uintptr_t)w);
    return S_OK;
}

/* GetDevicePeriod(default, minimum): 10 ms and 3 ms */
static uint64_t m_period(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[1], 100000);
    put64(a[2], 30000);
    return S_OK;
}

static uint64_t m_start(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    if (!s->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    if (s->running)
        return AUDCLNT_E_NOT_STOPPED;
    if (s->event_mode && !s->event)
        return AUDCLNT_E_EVENTHANDLE_NOT_SET;
    mix_pump();
    /* what was written while stopped plays from now */
    uint64_t base = MAX(mix_base(), mix.flushed);
    uint64_t ahead = (uint64_t)s->frozen * (uint32_t)mix.rate / s->rate;
    if (s->wpos < base + ahead)
        s->wpos = base + ahead;
    s->running = true;
    stream_arm(s, true);
    return S_OK;
}

static uint64_t m_stop(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    if (!s->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    if (!s->running)
        return S_FALSE;
    s->frozen = stream_padding(s);
    s->running = false;
    stream_arm(s, false);
    return S_OK;
}

static uint64_t m_reset(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    if (!s->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    if (s->running)
        return AUDCLNT_E_NOT_STOPPED;
    s->frozen = 0;
    s->written = 0;
    s->wpos = mix_base();
    return S_OK;
}

static uint64_t m_set_event(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    if (!s->init)
        return AUDCLNT_E_NOT_INITIALIZED;
    struct w32_object *e = w32_handle_get((HANDLE)(uintptr_t)(uint32_t)a[1]);
    if (!e || e->type != WH_EVENT)
        return E_INVALIDARG;
    s->event = (uint32_t)a[1];
    return S_OK;
}

static uint64_t m_service(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    const void *iid = a[1] ? w64_ptr(a[1]) : NULL;
    if (!a[2])
        return E_POINTER;
    if (!o->st->init) {
        put64(a[2], 0);
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    int spec = same(iid, IID_Render) ? SP_RENDER : same(iid, IID_Clock) ? SP_CLOCK
             : same(iid, IID_SimpleVolume) ? SP_SIMPLEVOL : same(iid, IID_StreamVolume) || same(iid, IID_ChannelVolume)
                                                               ? SP_STREAMVOL
             : same(iid, IID_Session) || same(iid, IID_Session2) ? SP_SESSION : -1;
    if (spec < 0) {
        put64(a[2], 0);
        return E_NOINTERFACE;
    }
    return give(a[2], anew(spec, o->st));
}

/* IAudioClient2: IsOffloadCapable(category, out), SetClientProperties, GetBufferSizeLimits */
static uint64_t m_offload(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[2], 0);
    return S_OK;
}

static uint64_t m_size_limits(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[3], 30000);
    put64(a[4], MAX_BUFFER_MS * 10000ull);
    return S_OK;
}

/* IAudioClient3: GetSharedModeEnginePeriod(format, default, fundamental, min, max) */
static uint64_t m_engine_period(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t rate = mix.users ? (uint32_t)mix.rate : 48000;
    put32(a[2], rate / 100);
    put32(a[3], rate / 100);
    put32(a[4], rate / 100);
    put32(a[5], rate / 100);
    return S_OK;
}

/* GetCurrentSharedModeEnginePeriod(format, period) */
static uint64_t m_current_period(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1]) {
        uint8_t *w = w32_alloc(40);
        if (!w)
            return E_OUTOFMEMORY;
        mix_format(w);
        put64(a[1], (uintptr_t)w);
    }
    put32(a[2], (mix.users ? (uint32_t)mix.rate : 48000) / 100);
    return S_OK;
}

/* InitializeSharedAudioStream(flags, period, format, session) */
static uint64_t m_init_shared(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint64_t b[7] = {a[0], 0, a[1], 0, 0, a[3], a[4]};
    return m_initialize(o, b, f);
}

/* ---- IAudioRenderClient */

static uint64_t m_get_buffer(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    uint32_t n = (uint32_t)a[1];
    if (!a[2])
        return E_POINTER;
    put64(a[2], 0);
    if (s->got)
        return AUDCLNT_E_OUT_OF_ORDER;
    mix_pump();
    if (n > s->buffer - stream_padding(s))
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    if (!n)
        return S_OK;
    s->got = n;
    put64(a[2], (uintptr_t)s->data);
    return S_OK;
}

static uint64_t m_release_buffer(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    uint32_t n = (uint32_t)a[1], flags = (uint32_t)a[2];
    if (n > s->got)
        return AUDCLNT_E_INVALID_SIZE;
    s->got = 0;
    if (!n)
        return S_OK;
    if (s->running) {
        stream_mix(s, s->data, n, flags & 2 /* AUDCLNT_BUFFERFLAGS_SILENT */);
        return S_OK;
    }
    /* before Start: into the mixer at once, where it will be by the time Start comes */
    stream_mix(s, s->data, n, flags & 2);
    s->frozen = MIN(s->frozen + n, s->buffer);
    return S_OK;
}

/* ---- IAudioClock: in bytes of the program's format */

static uint64_t m_clock_freq(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[1], (uint64_t)o->st->rate * o->st->block);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_clock_pos(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    struct astream *s = o->st;
    mix_pump();
    uint64_t played = s->written - stream_padding(s);
    put64(a[1], played * s->block);
    put64(a[2], hal_ticks_ms() * 10000);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_clock_chars(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0);
    return S_OK;
}

/* ---- ISimpleAudioVolume, IAudioStreamVolume (and IChannelAudioVolume, the same layout) */

static int32_t level(const uint64_t *f, int i)
{
    float v;
    memcpy(&v, &f[i], 4);
    v = v != v ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (int32_t)(v * 65536.0f);
}

static void put_level(uint64_t where, int32_t v)
{
    float l = (float)v / 65536.0f;
    if (where)
        memcpy(w64_ptr(where), &l, 4);
}

static uint64_t m_set_master(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    o->st->volume = level(f, 1);
    return S_OK;
}

static uint64_t m_get_master(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put_level(a[1], o->st->volume);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_set_mute(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    o->st->mute = (uint32_t)a[1] != 0;
    return S_OK;
}

static uint64_t m_get_mute(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->st->mute);
    return a[1] ? S_OK : E_POINTER;
}

static uint64_t m_channel_count(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->st->channels);
    return a[1] ? S_OK : E_POINTER;
}

/* SetChannelVolume(index, level): IChannelAudioVolume has a context GUID after the level, harmless here */
static uint64_t m_set_channel(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t i = (uint32_t)a[1];
    if (i >= o->st->channels)
        return E_INVALIDARG;
    if (i < 8)
        o->st->chan[i] = level(f, 2);
    return S_OK;
}

static uint64_t m_get_channel(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t i = (uint32_t)a[1];
    if (i >= o->st->channels)
        return E_INVALIDARG;
    put_level(a[2], i < 8 ? o->st->chan[i] : 65536);
    return S_OK;
}

static uint64_t m_set_all(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t n = (uint32_t)a[1];
    if (n != o->st->channels || !a[2])
        return E_INVALIDARG;
    const float *v = w64_ptr(a[2]);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint64_t one = 0;
        memcpy(&one, &v[i], 4);
        o->st->chan[i] = level(&one, 0);
    }
    return S_OK;
}

static uint64_t m_get_all(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t n = (uint32_t)a[1];
    if (n != o->st->channels || !a[2])
        return E_INVALIDARG;
    for (uint32_t i = 0; i < n; i++)
        put_level(a[2] + i * 4, i < 8 ? o->st->chan[i] : 65536);
    return S_OK;
}

/* ---- IAudioSessionControl(2): a session that is always active */

static uint64_t m_session_state(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 1); /* AudioSessionStateActive */
    return S_OK;
}

static uint64_t m_session_string(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    uint16_t *s = w32_alloc(2);
    if (s)
        *s = 0;
    put64(a[1], (uintptr_t)s);
    return s ? S_OK : E_OUTOFMEMORY;
}

static uint64_t m_session_guid(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1])
        memset(w64_ptr(a[1]), 0, 16);
    return S_OK;
}

static uint64_t m_session_pid(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], w32_current_pid());
    return S_OK;
}

static uint64_t m_session_false(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    return S_FALSE;
}

/* ---- IAudioEndpointVolume: the system's own volume stays the system's; this one only answers */

static uint64_t m_ep_channels(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 2);
    return S_OK;
}

static uint64_t m_ep_level(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put_level(a[1], 65536);
    return S_OK;
}

static uint64_t m_ep_db(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0);
    return S_OK;
}

static uint64_t m_ep_range(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    float r[3] = {-65.25f, 0.0f, 0.03125f};
    for (int i = 0; i < 3; i++)
        if (a[1 + i])
            memcpy(w64_ptr(a[1 + i]), &r[i], 4);
    return S_OK;
}

static uint64_t m_ep_get_mute(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0);
    return S_OK;
}

static uint64_t m_ep_channel_level(struct aobj *o, uint64_t *a, const uint64_t *f)
{
    put_level(a[2], 65536);
    return S_OK;
}

#define NOP m_ok
static const amethod enum_fns[] = {NULL, NULL, NULL, m_enum_endpoints, m_default_endpoint, m_get_device, NOP, NOP};
static const amethod coll_fns[] = {NULL, NULL, NULL, m_coll_count, m_coll_item};
static const amethod device_fns[] = {NULL, NULL, NULL, m_activate, m_open_props, m_get_id, m_get_state};
static const amethod endpoint_fns[] = {NULL, NULL, NULL, m_data_flow};
static const amethod props_fns[] = {NULL, NULL, NULL, m_props_count, NULL, m_props_value, NOP, NOP};
static const amethod client_fns[] = {
    NULL, NULL, NULL, m_initialize, m_buffer_size, m_latency, m_padding, m_format_supported, m_mix_format, m_period,
    m_start, m_stop, m_reset, m_set_event, m_service,
    /* IAudioClient2 */
    m_offload, NOP, m_size_limits,
    /* IAudioClient3 */
    m_engine_period, m_current_period, m_init_shared};
static const amethod render_fns[] = {NULL, NULL, NULL, m_get_buffer, m_release_buffer};
static const amethod clock_fns[] = {NULL, NULL, NULL, m_clock_freq, m_clock_pos, m_clock_chars};
static const amethod simplevol_fns[] = {NULL, NULL, NULL, m_set_master, m_get_master, m_set_mute, m_get_mute};
static const amethod streamvol_fns[] = {NULL,          NULL,          NULL,     m_channel_count,
                                        m_set_channel, m_get_channel, m_set_all, m_get_all};
static const amethod session_fns[] = {
    NULL, NULL, NULL, m_session_state, m_session_string, NOP, m_session_string, NOP, m_session_guid, NOP, NOP, NOP,
    /* IAudioSessionControl2 */
    m_session_string, m_session_string, m_session_pid, m_session_false, NOP};
static const amethod epvol_fns[] = {
    NULL,         NULL,           NULL,          NOP,           NOP,         m_ep_channels, NOP, NOP, m_ep_db,
    m_ep_level,   NOP,            NOP,           m_ep_db,       m_ep_channel_level, NOP, NOP, m_ep_get_mute,
    m_ep_get_mute, NOP,           NOP,           m_ep_range};

static const struct aspec specs[SP_COUNT] = {
    [SP_ENUM] = {"IMMDeviceEnumerator", {IID_Enumerator}, ARRAY_SIZE(enum_fns), enum_fns},
    [SP_COLL] = {"IMMDeviceCollection", {IID_Collection}, ARRAY_SIZE(coll_fns), coll_fns},
    [SP_DEVICE] = {"IMMDevice", {IID_Device}, ARRAY_SIZE(device_fns), device_fns},
    [SP_ENDPOINT] = {"IMMEndpoint", {IID_Endpoint}, ARRAY_SIZE(endpoint_fns), endpoint_fns},
    [SP_PROPS] = {"IPropertyStore", {IID_PropertyStore}, ARRAY_SIZE(props_fns), props_fns},
    [SP_CLIENT] = {"IAudioClient", {IID_Client, IID_Client2, IID_Client3}, ARRAY_SIZE(client_fns), client_fns},
    [SP_RENDER] = {"IAudioRenderClient", {IID_Render}, ARRAY_SIZE(render_fns), render_fns},
    [SP_CLOCK] = {"IAudioClock", {IID_Clock}, ARRAY_SIZE(clock_fns), clock_fns},
    [SP_SIMPLEVOL] = {"ISimpleAudioVolume", {IID_SimpleVolume}, ARRAY_SIZE(simplevol_fns), simplevol_fns},
    [SP_STREAMVOL] = {"IAudioStreamVolume", {IID_StreamVolume, IID_ChannelVolume}, ARRAY_SIZE(streamvol_fns),
                      streamvol_fns},
    [SP_SESSION] = {"IAudioSessionControl", {IID_Session, IID_Session2}, ARRAY_SIZE(session_fns), session_fns},
    [SP_EPVOL] = {"IAudioEndpointVolume", {IID_EndpointVolume}, ARRAY_SIZE(epvol_fns), epvol_fns},
};

static const struct aspec *aspec_of(int s)
{
    return &specs[s];
}

/* CoCreateInstance for the classes here: true when clsid is one, with the result in *hr */
bool w64_audio_create(const void *clsid, const void *iid, uint64_t out, uint64_t *hr)
{
    if (!same(clsid, CLSID_Enumerator))
        return false;
    if (!out) {
        *hr = E_POINTER;
        return true;
    }
    if (!same(iid, IID_Enumerator) && !same(iid, IID_Unknown)) {
        put64(out, 0);
        *hr = E_NOINTERFACE;
        return true;
    }
    *hr = give(out, anew(SP_ENUM, NULL));
    return true;
}

/* a program that ends: its streams leave the mixer */
void w64_audio_cleanup(void)
{
    while (streams) {
        struct astream *s = streams;
        streams = s->next;
        if (s->init)
            mix_leave();
        s->init = false;
    }
    render_device = NULL;
}

#else

#include "win64.h"

/* the 32-bit kernel runs 64-bit programs without the sound card */
bool w64_audio_create(const void *clsid, const void *iid, uint64_t out, uint64_t *hr)
{
    UNUSED(clsid);
    UNUSED(iid);
    UNUSED(out);
    UNUSED(hr);
    return false;
}

void w64_audio_cleanup(void)
{
}

#endif
