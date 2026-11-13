/* More sound and picture formats for the whole system (Music, Photos, Files, Quick Look, wallpapers): MP3 (MPEG-1/2
 * layers I-III), Ogg Vorbis and WebP (lossy, lossless and with alpha). The decoders are small public ones, kept as
 * they are in media/: minimp3 (CC0), stb_vorbis (public domain) and simplewebp (BSD-3-Clause, from libwebp); their licences are in
 * /system/LICENSES.txt. They do floating point, so this module is a *_fp.c file. */
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/picture.h>
#include <synth/string.h>

#include "../internal.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wold-style-declaration"
#pragma GCC diagnostic ignored "-Wpedantic"

#define get_bits mp3_get_bits
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "media/minimp3.h"
#undef get_bits

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "media/stb_vorbis.h"

#define SIMPLEWEBP_IMPLEMENTATION
#define SIMPLEWEBP_DISABLE_STDIO
#include "media/simplewebp.h"

#pragma GCC diagnostic pop

#define MAX_FRAMES (48000u * 60 * 30) /* half an hour of stereo at 48 kHz */

/* a growing buffer of interleaved stereo frames */
struct pcm {
    int16_t *p;
    uint32_t n, cap;
};

static bool pcm_room(struct pcm *o, uint32_t more)
{
    if (o->n + more <= o->cap)
        return true;
    if (o->n + more > MAX_FRAMES)
        return false;
    uint32_t cap = o->cap ? o->cap * 2 : 65536;
    while (cap < o->n + more)
        cap *= 2;
    int16_t *p = krealloc(o->p, (size_t)cap * 4);
    if (!p)
        return false;
    o->p = p;
    o->cap = cap;
    return true;
}

static void pcm_add(struct pcm *o, const int16_t *in, uint32_t frames, int channels)
{
    for (uint32_t i = 0; i < frames; i++) {
        o->p[(o->n + i) * 2] = in[i * (uint32_t)channels];
        o->p[(o->n + i) * 2 + 1] = in[i * (uint32_t)channels + (channels > 1 ? 1 : 0)];
    }
    o->n += frames;
}

static int pcm_done(struct pcm *o, int16_t **frames, uint32_t *count)
{
    if (!o->n) {
        kfree(o->p);
        return E_INVAL;
    }
    *frames = o->p;
    *count = o->n;
    return 0;
}

static int mp3_decode(const uint8_t *data, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    mp3dec_t *dec = kmalloc(sizeof(*dec));
    int16_t *buf = kmalloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    struct pcm o = {0};
    *rate = 0;
    size_t at = 0;
    if (size >= 10 && !memcmp(data, "ID3", 3)) /* an ID3v2 tag: its size is in four 7-bit bytes */
        at = 10 + ((size_t)(data[6] & 0x7f) << 21 | (size_t)(data[7] & 0x7f) << 14 | (size_t)(data[8] & 0x7f) << 7 |
                   (data[9] & 0x7f));
    /* other formats are tried here too: only data that starts with an MPEG audio frame is MP3 */
    if (at + 4 > size || data[at] != 0xff || (data[at + 1] & 0xe0) != 0xe0) {
        kfree(dec);
        kfree(buf);
        return E_INVAL;
    }
    if (dec && buf) {
        mp3dec_init(dec);
        while (at < size) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(dec, data + at, (int)MIN(size - at, (size_t)INT_MAX), buf, &info);
            if (!info.frame_bytes)
                break; /* nothing more that looks like audio */
            at += (size_t)info.frame_bytes;
            if (samples <= 0)
                continue; /* ID3 tags and the like are skipped */
            if (!*rate)
                *rate = (uint32_t)info.hz;
            if (!pcm_room(&o, (uint32_t)samples))
                break;
            pcm_add(&o, buf, (uint32_t)samples, info.channels);
        }
    }
    kfree(dec);
    kfree(buf);
    return pcm_done(&o, frames, count);
}

static int vorbis_decode(const uint8_t *data, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    /* every allocation of the decoder comes from this arena: no alloca on the kernel stack */
    stb_vorbis_alloc arena = {kmalloc(512u * 1024), 512 * 1024};
    if (!arena.alloc_buffer)
        return E_NOMEM;
    int error = 0;
    stb_vorbis *v = stb_vorbis_open_memory(data, (int)MIN(size, (size_t)INT_MAX), &error, &arena);
    struct pcm o = {0};
    if (v) {
        stb_vorbis_info info = stb_vorbis_get_info(v);
        *rate = info.sample_rate;
        int16_t *buf = kmalloc(4096 * 2 * sizeof(int16_t));
        for (; buf;) {
            int got = stb_vorbis_get_samples_short_interleaved(v, 2, buf, 4096 * 2);
            if (got <= 0 || !pcm_room(&o, (uint32_t)got))
                break;
            pcm_add(&o, buf, (uint32_t)got, 2);
        }
        kfree(buf);
        stb_vorbis_close(v);
    }
    kfree(arena.alloc_buffer);
    return pcm_done(&o, frames, count);
}

static int audio_any(const uint8_t *d, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    if (size >= 4 && !memcmp(d, "OggS", 4))
        return vorbis_decode(d, size, frames, count, rate);
    return mp3_decode(d, size, frames, count, rate);
}

static void *webp_alloc(void *unused, size_t size)
{
    (void)unused;
    return kmalloc(size);
}

static void webp_free(void *unused, void *p)
{
    (void)unused;
    kfree(p);
}

static int webp_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WEBP", 4))
        return E_INVAL;
    simplewebp_allocator allocator = {webp_alloc, webp_free, NULL};
    simplewebp *webp;
    if (simplewebp_load_from_memory((void *)d, size, &allocator, &webp) != SIMPLEWEBP_NO_ERROR)
        return E_INVAL;
    size_t w, h;
    simplewebp_get_dimensions(webp, &w, &h);
    int result = E_NOMEM;
    if (w && h && (uint64_t)w * h <= PICTURE_MAX_PIXELS) {
        memset(out, 0, sizeof(*out));
        out->px = kmalloc(w * h * 4);
        if (out->px && simplewebp_decode(webp, out->px, NULL) != SIMPLEWEBP_NO_ERROR) {
            kfree(out->px);
            out->px = NULL;
            result = E_INVAL;
        } else if (out->px) {
            const uint8_t *rgba = (const uint8_t *)out->px; /* RGBA bytes, turned into ARGB words in place */
            for (size_t i = 0; i < w * h; i++, rgba += 4)
                out->px[i] = (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
            out->w = (int)w;
            out->h = (int)h;
            out->frames = 1;
            result = 0;
        }
    }
    simplewebp_unload(webp);
    return result;
}

static const struct module_ext ext = {NULL, 0, ".webp", webp_decode, ".mp3 .mp2 .ogg .oga", audio_any,
                                      NULL, NULL, NULL};
GUI_APP_FULL(NULL, NULL, &ext);
