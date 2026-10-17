#include <synth/error.h>
#include <synth/heap.h>
#include <synth/lzma.h>
#include <synth/path.h>
#include <synth/sevenzip.h>
#include <synth/string.h>
#include <synth/vfs.h>

/* 7z reader: the (possibly LZMA-packed) header, folders built from Copy, LZMA and BCJ2 coders; stored files are
   streamed to disk, packed ones unpacked in memory. Other methods (LZMA2, PPMd, ...) are counted as skipped. */

#define SIG_SIZE 32
#define CHUNK (1u << 20)
#define MAX_HEADER (16u << 20)
#define MAX_LZMA_FOLDER (64u << 20)

enum {
    K_END,
    K_HEADER,
    K_ARCHIVE_PROPS,
    K_ADDITIONAL,
    K_MAIN_STREAMS,
    K_FILES,
    K_PACK_INFO,
    K_UNPACK_INFO,
    K_SUBSTREAMS,
    K_SIZE,
    K_CRC,
    K_FOLDER,
    K_UNPACK_SIZE,
    K_NUM_UNPACK,
    K_EMPTY_STREAM,
    K_EMPTY_FILE,
    K_ANTI,
    K_NAME,
    K_ENCODED_HEADER = 0x17,
    K_DUMMY = 0x19,
};

enum { CODER_COPY, CODER_LZMA, CODER_BCJ2, CODER_OTHER };

#define MAX_CODERS 4
#define MAX_STREAMS 8

struct coder {
    int kind, ins, outs, first_in, first_out;
    uint8_t props[5];
};

/* A folder is a small graph: coders whose inputs are either packed streams or other coders' outputs. */
struct folder {
    int coder;
    int coders;
    struct coder c[MAX_CODERS];
    int bind_in[MAX_STREAMS], bind_out[MAX_STREAMS], binds;
    int packed_in[MAX_STREAMS];
    uint64_t sizes[MAX_STREAMS];
    int outs;
    uint8_t props[5];
    uint64_t unpack;
    int first_pack, packs;
    int substreams;
    bool crc;
};

struct streams {
    uint64_t pack_pos;
    int pack_count;
    uint64_t *pack_sizes;
    int folder_count;
    struct folder *folders;
    uint64_t *sub_sizes;
    int sub_count;
};

struct cursor {
    const uint8_t *p, *end;
    bool bad;
};

static uint8_t byte(struct cursor *c)
{
    if (c->p < c->end)
        return *c->p++;
    c->bad = true;
    return 0;
}

static uint64_t number(struct cursor *c)
{
    uint8_t first = byte(c);
    uint8_t mask = 0x80;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        if (!(first & mask))
            return value | (uint64_t)(first & (mask - 1)) << (8 * i);
        value |= (uint64_t)byte(c) << (8 * i);
        mask >>= 1;
    }
    return value;
}

static void skip(struct cursor *c, uint64_t n)
{
    if (n > (uint64_t)(c->end - c->p)) {
        c->bad = true;
        c->p = c->end;
    } else {
        c->p += n;
    }
}

/* Skips a digest list; `mark`, when given, learns which of the entries had one. */
static void skip_digests(struct cursor *c, uint64_t count, bool *mark)
{
    uint64_t defined = count;
    bool all = byte(c);
    if (!all)
        defined = 0;
    for (uint64_t i = 0; i < count; i += 8) {
        uint8_t bits = all ? 0xff : byte(c);
        for (int k = 0; k < 8 && i + (uint64_t)k < count; k++) {
            bool on = bits >> (7 - k) & 1;
            defined += !all && on;
            if (mark)
                mark[i + (uint64_t)k] = on;
        }
    }
    skip(c, defined * 4);
}

static void streams_free(struct streams *s)
{
    kfree(s->pack_sizes);
    kfree(s->folders);
    kfree(s->sub_sizes);
    memset(s, 0, sizeof(*s));
}

static void read_folder(struct cursor *c, struct folder *f, int *out_streams)
{
    uint64_t coders = number(c);
    int in_total = 0, out_total = 0;
    f->coder = CODER_OTHER;
    f->coders = (int)MIN(coders, (uint64_t)MAX_CODERS);
    bool known = coders <= MAX_CODERS;
    for (uint64_t i = 0; i < coders && !c->bad; i++) {
        uint8_t flags = byte(c);
        uint8_t id[16] = {0};
        int id_size = flags & 15;
        for (int k = 0; k < id_size; k++) {
            uint8_t b = byte(c);
            if (k < 16)
                id[k] = b;
        }
        int ins = 1, outs = 1;
        if (flags & 0x10) {
            ins = (int)number(c);
            outs = (int)number(c);
        }
        uint8_t props[5] = {0};
        if (flags & 0x20) {
            uint64_t n = number(c);
            for (uint64_t k = 0; k < n; k++) {
                uint8_t b = byte(c);
                if (k < 5)
                    props[k] = b;
            }
        }
        int kind = CODER_OTHER;
        if (id_size == 1 && id[0] == 0)
            kind = CODER_COPY;
        else if (id_size == 3 && id[0] == 3 && id[1] == 1 && id[2] == 1)
            kind = CODER_LZMA;
        else if (id_size == 4 && id[0] == 3 && id[1] == 3 && id[2] == 1 && id[3] == 0x1b && ins == 4 && outs == 1)
            kind = CODER_BCJ2;
        known &= kind != CODER_OTHER;
        if (i < MAX_CODERS)
            f->c[i] = (struct coder){kind, ins, outs, in_total, out_total, {0}};
        if (i < MAX_CODERS)
            memcpy(f->c[i].props, props, 5);
        in_total += ins;
        out_total += outs;
    }
    int bind_pairs = out_total - 1;
    f->binds = MIN(bind_pairs, MAX_STREAMS);
    for (int i = 0; i < bind_pairs; i++) {
        int in = (int)number(c), out = (int)number(c);
        if (i < MAX_STREAMS) {
            f->bind_in[i] = in;
            f->bind_out[i] = out;
        }
    }
    f->packs = in_total - bind_pairs;
    known &= in_total <= MAX_STREAMS && out_total <= MAX_STREAMS;
    if (f->packs > 1) {
        for (int i = 0; i < f->packs; i++) {
            int in = (int)number(c);
            if (i < MAX_STREAMS)
                f->packed_in[i] = in;
        }
    } else {
        for (int in = 0; in < in_total; in++) {
            bool bound = false;
            for (int k = 0; k < f->binds; k++)
                bound |= f->bind_in[k] == in;
            if (!bound)
                f->packed_in[0] = in;
        }
    }
    f->outs = out_total;
    if (known && f->coders == 1 && f->c[0].kind != CODER_BCJ2)
        f->coder = f->c[0].kind;
    else if (known)
        f->coder = CODER_BCJ2;
    if (f->coder == CODER_LZMA)
        memcpy(f->props, f->c[0].props, 5);
    *out_streams = out_total;
}

static int read_streams(struct cursor *c, struct streams *s)
{
    int *outs = NULL;
    for (uint8_t id = byte(c); id != K_END && !c->bad; id = byte(c)) {
        if (id == K_PACK_INFO) {
            s->pack_pos = number(c);
            s->pack_count = (int)number(c);
            if (s->pack_count < 0 || s->pack_count > 100000)
                return E_INVAL;
            s->pack_sizes = kzalloc(sizeof(uint64_t) * (size_t)(s->pack_count + 1));
            if (!s->pack_sizes)
                return E_NOMEM;
            for (uint8_t k = byte(c); k != K_END && !c->bad; k = byte(c)) {
                if (k == K_SIZE)
                    for (int i = 0; i < s->pack_count; i++)
                        s->pack_sizes[i] = number(c);
                else if (k == K_CRC)
                    skip_digests(c, (uint64_t)s->pack_count, NULL);
                else
                    return E_INVAL;
            }
        } else if (id == K_UNPACK_INFO) {
            if (byte(c) != K_FOLDER)
                return E_INVAL;
            s->folder_count = (int)number(c);
            if (s->folder_count < 0 || s->folder_count > 100000 || byte(c) != 0)
                return E_INVAL;
            s->folders = kzalloc(sizeof(struct folder) * (size_t)(s->folder_count + 1));
            outs = kzalloc(sizeof(int) * (size_t)(s->folder_count + 1));
            if (!s->folders || !outs) {
                kfree(outs);
                return E_NOMEM;
            }
            int pack = 0;
            for (int i = 0; i < s->folder_count && !c->bad; i++) {
                read_folder(c, &s->folders[i], &outs[i]);
                s->folders[i].first_pack = pack;
                s->folders[i].substreams = 1;
                pack += s->folders[i].packs;
            }
            if (byte(c) != K_UNPACK_SIZE) {
                kfree(outs);
                return E_INVAL;
            }
            for (int i = 0; i < s->folder_count; i++) {
                struct folder *fo = &s->folders[i];
                for (int k = 0; k < outs[i]; k++) {
                    uint64_t v = number(c);
                    if (k < MAX_STREAMS)
                        fo->sizes[k] = v;
                }
                fo->unpack = fo->sizes[0];
                for (int k = 0; k < MIN(outs[i], MAX_STREAMS); k++) {
                    bool bound = false;
                    for (int b = 0; b < fo->binds; b++)
                        bound |= fo->bind_out[b] == k;
                    if (!bound)
                        fo->unpack = fo->sizes[k];
                }
            }
            kfree(outs);
            outs = NULL;
            for (uint8_t k = byte(c); k != K_END && !c->bad; k = byte(c)) {
                if (k == K_CRC) {
                    bool *mark = kzalloc((size_t)s->folder_count + 1);
                    if (!mark)
                        return E_NOMEM;
                    skip_digests(c, (uint64_t)s->folder_count, mark);
                    for (int i = 0; i < s->folder_count; i++)
                        s->folders[i].crc = mark[i];
                    kfree(mark);
                } else {
                    return E_INVAL;
                }
            }
        } else if (id == K_SUBSTREAMS) {
            uint8_t k = byte(c);
            if (k == K_NUM_UNPACK) {
                for (int i = 0; i < s->folder_count; i++)
                    s->folders[i].substreams = (int)number(c);
                k = byte(c);
            }
            int total = 0;
            for (int i = 0; i < s->folder_count; i++)
                total += s->folders[i].substreams;
            s->sub_count = total;
            s->sub_sizes = kzalloc(sizeof(uint64_t) * (size_t)(total + 1));
            if (!s->sub_sizes)
                return E_NOMEM;
            int at = 0;
            for (int i = 0; i < s->folder_count; i++) {
                int n = s->folders[i].substreams;
                if (!n)
                    continue;
                uint64_t sum = 0;
                for (int j = 0; j + 1 < n; j++) {
                    uint64_t v = k == K_SIZE ? number(c) : 0;
                    s->sub_sizes[at++] = v;
                    sum += v;
                }
                s->sub_sizes[at++] = s->folders[i].unpack - sum;
            }
            if (k == K_SIZE)
                k = byte(c);
            while (k != K_END && !c->bad) {
                if (k == K_CRC) {
                    uint64_t unknown = 0;
                    for (int i = 0; i < s->folder_count; i++)
                        unknown += s->folders[i].substreams == 1 && s->folders[i].crc ? 0
                                                                                       : (uint64_t)s->folders[i].substreams;
                    skip_digests(c, unknown, NULL);
                } else {
                    return E_INVAL;
                }
                k = byte(c);
            }
        } else {
            return E_INVAL;
        }
    }
    if (!s->sub_sizes && s->folder_count) {
        s->sub_count = s->folder_count;
        s->sub_sizes = kzalloc(sizeof(uint64_t) * (size_t)s->folder_count);
        if (!s->sub_sizes)
            return E_NOMEM;
        for (int i = 0; i < s->folder_count; i++)
            s->sub_sizes[i] = s->folders[i].unpack;
    }
    return c->bad ? E_INVAL : 0;
}

static uint64_t pack_offset(const struct streams *s, int pack_index)
{
    uint64_t at = SIG_SIZE + s->pack_pos;
    for (int i = 0; i < pack_index && i < s->pack_count; i++)
        at += s->pack_sizes[i];
    return at;
}

static bool read_at(struct file *f, uint64_t at, void *buf, size_t n)
{
    return at <= 0x7fffffffu && vfs_seek(f, (long)at, VFS_SEEK_SET) >= 0 && vfs_read(f, buf, n) == (long)n;
}

/* BCJ2: x86 CALL/JMP targets were moved into two side streams; a range-coded stream says which bytes were. */
static int bcj2_decode(const uint8_t *buf0, size_t size0, const uint8_t *buf1, size_t size1, const uint8_t *buf2,
                       size_t size2, const uint8_t *buf3, size_t size3, uint8_t *out, size_t out_size)
{
    uint16_t p[258];
    for (int i = 0; i < 258; i++)
        p[i] = 1024;
    const uint8_t *rc = buf3, *rc_end = buf3 + size3;
    uint32_t range = 0xffffffffu, code = 0;
    for (int i = 0; i < 5; i++)
        code = code << 8 | (rc < rc_end ? *rc++ : 0);
    size_t in = 0, pos = 0;
    uint8_t prev = 0;
    while (pos < out_size) {
        size_t limit = MIN(size0 - in, out_size - pos);
        bool jump = false;
        while (limit) {
            uint8_t b = buf0[in];
            out[pos++] = b;
            if ((b & 0xfe) == 0xe8 || (prev == 0x0f && (b & 0xf0) == 0x80)) {
                jump = true;
                break;
            }
            in++;
            prev = b;
            limit--;
        }
        if (!jump || pos == out_size)
            break;
        uint8_t b = buf0[in++];
        uint16_t *prob = b == 0xe8 ? &p[prev] : b == 0xe9 ? &p[256] : &p[257];
        uint32_t bound = (range >> 11) * *prob;
        if (code < bound) {
            range = bound;
            *prob = (uint16_t)(*prob + ((2048 - *prob) >> 5));
            prev = b;
        } else {
            range -= bound;
            code -= bound;
            *prob = (uint16_t)(*prob - (*prob >> 5));
            const uint8_t *v;
            if (b == 0xe8) {
                if (size1 < 4)
                    return E_INVAL;
                v = buf1;
                buf1 += 4;
                size1 -= 4;
            } else {
                if (size2 < 4)
                    return E_INVAL;
                v = buf2;
                buf2 += 4;
                size2 -= 4;
            }
            uint32_t dest = ((uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3]) - (uint32_t)(pos + 4);
            for (int k = 0; k < 4 && pos < out_size; k++) {
                out[pos++] = (uint8_t)(dest >> (8 * k));
                prev = (uint8_t)(dest >> (8 * k));
            }
        }
        if (range < (1u << 24)) {
            range <<= 8;
            code = code << 8 | (rc < rc_end ? *rc++ : 0);
        }
    }
    return pos == out_size ? 0 : E_INVAL;
}

struct graph {
    struct file *f;
    uint64_t base;
    const struct streams *s;
    const struct folder *fo;
};

static int decode_out(struct graph *g, int out, uint8_t **data, size_t *size, int depth);

/* The bytes going into coder input `in`: another coder's output or a packed stream read from the archive. */
static int decode_in(struct graph *g, int in, uint8_t **data, size_t *size, int depth)
{
    const struct folder *fo = g->fo;
    for (int b = 0; b < fo->binds; b++)
        if (fo->bind_in[b] == in)
            return decode_out(g, fo->bind_out[b], data, size, depth + 1);
    for (int k = 0; k < fo->packs && k < MAX_STREAMS; k++)
        if (fo->packed_in[k] == in) {
            uint64_t n = g->s->pack_sizes[fo->first_pack + k];
            if (n > MAX_LZMA_FOLDER)
                return E_NOSPC;
            uint8_t *buf = kmalloc((size_t)n + 1);
            if (!buf)
                return E_NOMEM;
            if (!read_at(g->f, g->base + pack_offset(g->s, fo->first_pack + k), buf, (size_t)n)) {
                kfree(buf);
                return E_IO;
            }
            *data = buf;
            *size = (size_t)n;
            return 0;
        }
    return E_INVAL;
}

static int decode_out(struct graph *g, int out, uint8_t **data, size_t *size, int depth)
{
    const struct folder *fo = g->fo;
    const struct coder *c = NULL;
    for (int i = 0; i < fo->coders && !c; i++)
        if (out >= fo->c[i].first_out && out < fo->c[i].first_out + fo->c[i].outs)
            c = &fo->c[i];
    if (!c || depth > MAX_CODERS || out >= MAX_STREAMS || fo->sizes[out] > MAX_LZMA_FOLDER)
        return E_INVAL;
    size_t want = (size_t)fo->sizes[out];
    uint8_t *in[4] = {0};
    size_t in_size[4] = {0};
    int err = 0;
    for (int k = 0; k < c->ins && k < 4 && !err; k++)
        err = decode_in(g, c->first_in + k, &in[k], &in_size[k], depth);
    uint8_t *buf = err ? NULL : kmalloc(want + 1);
    if (!err && !buf)
        err = E_NOMEM;
    if (!err && c->kind == CODER_COPY)
        memcpy(buf, in[0], MIN(want, in_size[0]));
    else if (!err && c->kind == CODER_LZMA)
        err = lzma_decode(c->props, in[0], in_size[0], buf, want);
    else if (!err && c->kind == CODER_BCJ2)
        err = bcj2_decode(in[0], in_size[0], in[1], in_size[1], in[2], in_size[2], in[3], in_size[3], buf, want);
    else if (!err)
        err = E_NOSYS;
    for (int k = 0; k < 4; k++)
        kfree(in[k]);
    if (err) {
        kfree(buf);
        return err;
    }
    *data = buf;
    *size = want;
    return 0;
}

/* Unpacks folder `i` into a fresh buffer. */
static int unpack_folder(struct file *f, uint64_t base, const struct streams *s, int i, uint8_t **out)
{
    const struct folder *fo = &s->folders[i];
    struct graph g = {f, base, s, fo};
    int main_out = 0;
    for (int k = 0; k < fo->outs && k < MAX_STREAMS; k++) {
        bool bound = false;
        for (int b = 0; b < fo->binds; b++)
            bound |= fo->bind_out[b] == k;
        if (!bound)
            main_out = k;
    }
    size_t size;
    return decode_out(&g, main_out, out, &size, 0);
}

static int64_t find_signature(struct file *f, uint32_t from)
{
    uint8_t *buf = kmalloc(CHUNK + 8);
    if (!buf)
        return E_NOMEM;
    int64_t found = E_INVAL;
    for (uint32_t at = from; at < from + (8u << 20) && found < 0; at += CHUNK) {
        if (vfs_seek(f, (long)at, VFS_SEEK_SET) < 0)
            break;
        long n = vfs_read(f, buf, CHUNK + 6);
        if (n < 6)
            break;
        for (long i = 0; i + 6 <= n && found < 0; i++)
            if (buf[i] == '7' && buf[i + 1] == 'z' && buf[i + 2] == 0xbc && buf[i + 3] == 0xaf && buf[i + 4] == 0x27 &&
                buf[i + 5] == 0x1c)
                found = (int64_t)at + i;
    }
    kfree(buf);
    return found;
}

static int load_header(struct file *f, uint64_t base, uint8_t **out, size_t *size)
{
    uint8_t sig[SIG_SIZE];
    if (!read_at(f, base, sig, sizeof(sig)) || memcmp(sig, "7z\xbc\xaf\x27\x1c", 6))
        return E_INVAL;
    uint64_t off = 0, len = 0;
    for (int i = 7; i >= 0; i--) {
        off = off << 8 | sig[12 + i];
        len = len << 8 | sig[20 + i];
    }
    if (len == 0 || len > MAX_HEADER)
        return E_INVAL;
    uint8_t *h = kmalloc((size_t)len);
    if (!h)
        return E_NOMEM;
    if (!read_at(f, base + SIG_SIZE + off, h, (size_t)len)) {
        kfree(h);
        return E_IO;
    }
    while (h[0] == K_ENCODED_HEADER) {
        struct cursor c = {h + 1, h + len, false};
        struct streams s = {0};
        int err = read_streams(&c, &s);
        uint8_t *plain = NULL;
        if (!err && (s.folder_count != 1 || s.folders[0].coder == CODER_OTHER || !s.pack_count))
            err = E_NOSYS;
        if (!err)
            err = unpack_folder(f, base, &s, 0, &plain);
        len = err ? 0 : s.folders[0].unpack;
        streams_free(&s);
        kfree(h);
        if (err)
            return err;
        h = plain;
    }
    *out = h;
    *size = (size_t)len;
    return 0;
}

static void utf16_name(const uint8_t *p, const uint8_t *end, char *out, size_t cap, const uint8_t **next)
{
    size_t n = 0;
    while (p + 1 < end && (p[0] | p[1])) {
        unsigned ch = p[0] | (unsigned)p[1] << 8;
        if (n + 3 < cap) {
            if (ch < 0x80) {
                out[n++] = ch == '\\' ? '/' : (char)ch;
            } else if (ch < 0x800) {
                out[n++] = (char)(0xc0 | ch >> 6);
                out[n++] = (char)(0x80 | (ch & 0x3f));
            } else {
                out[n++] = (char)(0xe0 | ch >> 12);
                out[n++] = (char)(0x80 | (ch >> 6 & 0x3f));
                out[n++] = (char)(0x80 | (ch & 0x3f));
            }
        }
        p += 2;
    }
    out[n] = 0;
    *next = p + 2;
}

static bool safe_name(const char *name)
{
    return name[0] && name[0] != '/' && !strstr(name, "..");
}

int64_t sevenzip_find(const char *archive, uint64_t from)
{
    struct file *f;
    int err = vfs_open(archive, VFS_READ, &f);
    if (err)
        return err;
    int64_t at = from <= 0x7fffffffu ? find_signature(f, (uint32_t)from) : E_INVAL;
    vfs_close(f);
    return at;
}

int sevenzip_extract(const char *archive, int64_t base, const char *dest, const char *strip,
                     void (*progress)(void *ctx, const char *name, uint64_t done, uint64_t total), void *ctx,
                     struct sevenzip_result *result)
{
    struct file *f;
    int err = vfs_open(archive, VFS_READ, &f);
    if (err)
        return err;
    if (base < 0)
        base = find_signature(f, 0);
    if (base < 0) {
        vfs_close(f);
        return (int)base;
    }
    uint8_t *h = NULL, *buf = NULL, *folder_data = NULL;
    size_t hsize = 0;
    struct streams s = {0};
    err = load_header(f, (uint64_t)base, &h, &hsize);
    struct cursor c = {h, h + hsize, false};
    if (!err && byte(&c) != K_HEADER)
        err = E_INVAL;
    uint8_t id = err ? K_END : byte(&c);
    if (id == K_ARCHIVE_PROPS) {
        for (uint8_t t = byte(&c); t != K_END && !c.bad; t = byte(&c))
            skip(&c, number(&c));
        id = byte(&c);
    }
    if (id == K_ADDITIONAL)
        err = E_NOSYS;
    if (!err && id == K_MAIN_STREAMS) {
        err = read_streams(&c, &s);
        id = byte(&c);
    }
    uint64_t files = 0, total = 0;
    bool *empty = NULL, *empty_file = NULL;
    const uint8_t *names = NULL, *names_end = NULL;
    if (!err && id == K_FILES) {
        files = number(&c);
        if (files > 100000)
            err = E_INVAL;
        empty = kzalloc(files + 1);
        empty_file = kzalloc(files + 1);
        if (!empty || !empty_file)
            err = E_NOMEM;
        for (uint8_t t = err ? K_END : byte(&c); t != K_END && !c.bad; t = byte(&c)) {
            uint64_t size = number(&c);
            const uint8_t *start = c.p;
            if (t == K_EMPTY_STREAM) {
                for (uint64_t i = 0; i < files; i++)
                    if (start[i / 8] >> (7 - i % 8) & 1)
                        empty[i] = true;
            } else if (t == K_EMPTY_FILE) {
                for (uint64_t i = 0, e = 0; i < files; i++)
                    if (empty[i]) {
                        empty_file[i] = start[e / 8] >> (7 - e % 8) & 1;
                        e++;
                    }
            } else if (t == K_NAME && size > 1 && start[0] == 0) {
                names = start + 1;
                names_end = start + size;
            }
            skip(&c, size);
        }
        if (c.bad)
            err = E_INVAL;
    }
    for (int i = 0; !err && i < s.sub_count; i++)
        total += s.sub_sizes[i];
    if (!err && !names)
        err = E_INVAL;
    if (!err && !(buf = kmalloc(CHUNK)))
        err = E_NOMEM;
    struct sevenzip_result r = {0};
    uint64_t done = 0;
    int folder = 0, sub_in_folder = 0, sub = 0, loaded = -1;
    uint64_t folder_pos = 0;
    while (!err && folder < s.folder_count && s.folders[folder].substreams == 0)
        folder++;
    for (uint64_t i = 0; !err && i < files; i++) {
        char name[PATH_MAX_LEN], path[PATH_MAX_LEN];
        utf16_name(names, names_end, name, sizeof(name), &names);
        const char *rel = name;
        if (strip && str_starts(rel, strip))
            rel += strlen(strip);
        bool keep = safe_name(rel) && (!strip || rel != name);
        path_join(dest, rel, path, sizeof(path));
        if (empty[i]) {
            if (!keep)
                continue;
            if (empty_file[i])
                vfs_write_all(path, "", 0);
            else
                vfs_mkdirs(path);
            continue;
        }
        if (folder >= s.folder_count || sub >= s.sub_count) {
            err = E_INVAL;
            break;
        }
        struct folder *fo = &s.folders[folder];
        uint64_t size = s.sub_sizes[sub];
        if (keep && fo->coder != CODER_OTHER) {
            char dir[PATH_MAX_LEN];
            path_dirname(path, dir, sizeof(dir));
            vfs_mkdirs(dir);
            struct file *o;
            err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &o);
            if (err)
                break;
            if (fo->coder != CODER_COPY && loaded != folder) {
                kfree(folder_data);
                folder_data = NULL;
                err = unpack_folder(f, (uint64_t)base, &s, folder, &folder_data);
                loaded = err ? -1 : folder;
            }
            for (uint64_t at = 0; !err && at < size;) {
                size_t n = (size_t)MIN((uint64_t)CHUNK, size - at);
                const void *src = buf;
                if (fo->coder != CODER_COPY)
                    src = folder_data + folder_pos + at;
                else if (!read_at(f, (uint64_t)base + pack_offset(&s, fo->first_pack) + folder_pos + at, buf, n))
                    err = E_IO;
                if (!err && vfs_write(o, src, n) != (long)n)
                    err = E_NOSPC;
                at += n;
                if (progress)
                    progress(ctx, rel, done + at, total);
            }
            vfs_close(o);
            if (!err)
                r.files++;
        } else if (keep) {
            r.skipped++;
        }
        done += size;
        folder_pos += size;
        sub++;
        if (++sub_in_folder >= fo->substreams) {
            folder++;
            sub_in_folder = 0;
            folder_pos = 0;
            while (folder < s.folder_count && s.folders[folder].substreams == 0)
                folder++;
        }
    }
    r.bytes = done;
    if (result)
        *result = r;
    kfree(folder_data);
    kfree(buf);
    kfree(empty);
    kfree(empty_file);
    kfree(h);
    streams_free(&s);
    vfs_close(f);
    return err;
}
