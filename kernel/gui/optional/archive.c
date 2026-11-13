#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/input.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>
#include <synth/zip.h>

#include "../internal.h"

/* Archives besides ZIP and 7z (which the kernel unpacks): tar, alone or packed with gzip, xz or lzma, and single
   .gz/.xz/.lzma files; and making ZIP archives. Files uses the commands for Compress and Extract Here. */

#define UNPACK_MAX (512u << 20)

/* ---- LZMA2 inside xz: chunks share one dictionary (the whole output) and, unless reset, the coder state ---- */

struct rc {
    const uint8_t *in, *end;
    uint32_t range, code;
    bool bad;
};

static uint8_t rc_byte(struct rc *r)
{
    if (r->in < r->end)
        return *r->in++;
    r->bad = true;
    return 0;
}

static unsigned rc_bit(struct rc *r, uint16_t *p)
{
    uint32_t bound = (r->range >> 11) * *p;
    unsigned b;
    if (r->code < bound) {
        r->range = bound;
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        b = 0;
    } else {
        r->range -= bound;
        r->code -= bound;
        *p = (uint16_t)(*p - (*p >> 5));
        b = 1;
    }
    if (r->range < (1u << 24)) {
        r->range <<= 8;
        r->code = r->code << 8 | rc_byte(r);
    }
    return b;
}

static uint32_t rc_direct(struct rc *r, int count)
{
    uint32_t res = 0;
    while (count--) {
        r->range >>= 1;
        r->code -= r->range;
        uint32_t t = 0u - (r->code >> 31);
        r->code += r->range & t;
        if (r->range < (1u << 24)) {
            r->range <<= 8;
            r->code = r->code << 8 | rc_byte(r);
        }
        res = (res << 1) + (t + 1);
    }
    return res;
}

static unsigned rc_tree(struct rc *r, uint16_t *probs, int bits)
{
    unsigned m = 1;
    for (int i = 0; i < bits; i++)
        m = (m << 1) + rc_bit(r, &probs[m]);
    return m - (1u << bits);
}

static unsigned rc_tree_rev(struct rc *r, uint16_t *probs, int bits)
{
    unsigned m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        unsigned b = rc_bit(r, &probs[m]);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

struct lencoder {
    uint16_t choice, choice2, low[16][8], mid[16][8], high[256];
};

static unsigned rc_len(struct rc *r, struct lencoder *l, unsigned ps)
{
    if (!rc_bit(r, &l->choice))
        return rc_tree(r, l->low[ps], 3);
    if (!rc_bit(r, &l->choice2))
        return 8 + rc_tree(r, l->mid[ps], 3);
    return 16 + rc_tree(r, l->high, 8);
}

struct lz2 {
    struct {
        uint16_t is_match[12][16], is_rep[12], g0[12], g1[12], g2[12], rep0_long[12][16];
        uint16_t slot[4][64], special[115], align[16];
        struct lencoder len, rep_len;
        uint16_t lit[0x300 << 4]; /* lc + lp <= 4 in LZMA2 */
    } p;
    unsigned lc, lp, pb, state;
    uint32_t rep[4];
    uint8_t *out;
    size_t pos, cap, dict_start;
    bool ended; /* an LZMA end marker was read */
};

static bool lz2_room(struct lz2 *z, size_t more);

static void lz2_reset_state(struct lz2 *z)
{
    uint16_t *p = (uint16_t *)&z->p;
    for (size_t i = 0; i < sizeof(z->p) / 2; i++)
        p[i] = 1024;
    z->state = 0;
    memset(z->rep, 0, sizeof(z->rep));
}

static bool lz2_chunk(struct lz2 *z, const uint8_t *in, size_t n, size_t usize)
{
    struct rc r = {in, in + n, 0xffffffffu, 0, false};
    if (rc_byte(&r))
        return false;
    for (int i = 0; i < 4; i++)
        r.code = r.code << 8 | rc_byte(&r);
    size_t end = z->pos + usize;
    unsigned pbm = (1u << z->pb) - 1, lpm = (1u << z->lp) - 1;
    uint8_t *o = z->out;
    while (z->pos < end && !r.bad) {
        if (z->pos + 274 > z->cap) {
            if (!lz2_room(z, 65536))
                return false;
            o = z->out;
        }
        size_t pos = z->pos;
        unsigned ps = (unsigned)pos & pbm, st = z->state;
        if (!rc_bit(&r, &z->p.is_match[st][ps])) {
            unsigned prev = pos > z->dict_start ? o[pos - 1] : 0;
            uint16_t *probs = &z->p.lit[0x300 * ((((unsigned)pos & lpm) << z->lc) + (prev >> (8 - z->lc)))];
            unsigned sym = 1;
            if (st >= 7 && pos - z->dict_start > z->rep[0]) {
                unsigned match = o[pos - z->rep[0] - 1];
                do {
                    unsigned mb = (match >> 7) & 1;
                    match <<= 1;
                    unsigned b = rc_bit(&r, &probs[((1 + mb) << 8) + sym]);
                    sym = sym << 1 | b;
                    if (mb != b)
                        break;
                } while (sym < 0x100);
            }
            while (sym < 0x100)
                sym = sym << 1 | rc_bit(&r, &probs[sym]);
            o[z->pos++] = (uint8_t)sym;
            z->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
            continue;
        }
        unsigned len;
        if (rc_bit(&r, &z->p.is_rep[st])) {
            if (pos == z->dict_start)
                return false;
            if (!rc_bit(&r, &z->p.g0[st])) {
                if (!rc_bit(&r, &z->p.rep0_long[st][ps])) {
                    z->state = st < 7 ? 9 : 11;
                    o[pos] = o[pos - z->rep[0] - 1];
                    z->pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(&r, &z->p.g1[st])) {
                    dist = z->rep[1];
                } else {
                    if (!rc_bit(&r, &z->p.g2[st])) {
                        dist = z->rep[2];
                    } else {
                        dist = z->rep[3];
                        z->rep[3] = z->rep[2];
                    }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = dist;
            }
            len = rc_len(&r, &z->p.rep_len, ps);
            z->state = st < 7 ? 8 : 11;
        } else {
            z->rep[3] = z->rep[2];
            z->rep[2] = z->rep[1];
            z->rep[1] = z->rep[0];
            len = rc_len(&r, &z->p.len, ps);
            z->state = st < 7 ? 7 : 10;
            unsigned slot = rc_tree(&r, z->p.slot[MIN(len, 3u)], 6);
            uint32_t dist = slot;
            if (slot >= 4) {
                int bits = (int)(slot >> 1) - 1;
                dist = (2 | (slot & 1)) << bits;
                if (slot < 14)
                    dist += rc_tree_rev(&r, &z->p.special[dist - slot], bits);
                else
                    dist += (rc_direct(&r, bits - 4) << 4) + rc_tree_rev(&r, z->p.align, 4);
            }
            if (dist == 0xffffffffu) {
                z->ended = true;
                return !r.bad;
            }
            z->rep[0] = dist;
        }
        len += 2;
        if (z->rep[0] >= z->pos - z->dict_start)
            return false;
        for (; len && z->pos < end; len--, z->pos++)
            o[z->pos] = o[z->pos - z->rep[0] - 1];
    }
    return !r.bad && z->pos == end;
}

static bool lz2_room(struct lz2 *z, size_t more)
{
    if (z->pos + more <= z->cap)
        return true;
    if (z->pos + more > UNPACK_MAX)
        return false;
    size_t cap = MAX(z->cap * 2, z->pos + more + 65536);
    uint8_t *grown = krealloc(z->out, cap);
    if (!grown)
        return false;
    z->out = grown;
    z->cap = cap;
    return true;
}

/* LZMA2 data from in; returns the bytes read, 0 when damaged */
static size_t lz2_decode(struct lz2 *z, const uint8_t *in, size_t n)
{
    size_t i = 0;
    bool need_props = true;
    while (i < n) {
        uint8_t c = in[i++];
        if (!c)
            return i;
        if (c == 1 || c == 2) {
            if (i + 2 > n)
                return 0;
            size_t len = ((size_t)in[i] << 8 | in[i + 1]) + 1;
            i += 2;
            if (c == 1)
                z->dict_start = z->pos;
            if (i + len > n || !lz2_room(z, len))
                return 0;
            memcpy(z->out + z->pos, in + i, len);
            z->pos += len;
            i += len;
            continue;
        }
        if (c < 0x80 || i + 4 > n)
            return 0;
        size_t usize = ((size_t)(c & 0x1f) << 16 | (size_t)in[i] << 8 | in[i + 1]) + 1;
        size_t psize = ((size_t)in[i + 2] << 8 | in[i + 3]) + 1;
        i += 4;
        int reset = (c >> 5) & 3;
        if (reset == 3)
            z->dict_start = z->pos;
        if (reset >= 2) {
            if (i >= n || in[i] >= 9 * 5 * 5)
                return 0;
            unsigned d = in[i++];
            z->lc = d % 9;
            z->lp = d / 9 % 5;
            z->pb = d / 45;
            if (z->lc + z->lp > 4)
                return 0;
            need_props = false;
        } else if (need_props) {
            return 0;
        }
        if (reset >= 1)
            lz2_reset_state(z);
        if (i + psize > n || !lz2_room(z, usize) || !lz2_chunk(z, in + i, psize, usize) || z->ended)
            return 0;
        i += psize;
        input_yield();
    }
    return 0;
}

static uint64_t varint(const uint8_t *d, size_t n, size_t *i)
{
    uint64_t v = 0;
    for (int s = 0; *i < n && s < 63; s += 7) {
        uint8_t b = d[(*i)++];
        v |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80))
            break;
    }
    return v;
}

static int unxz(const uint8_t *d, size_t n, uint8_t **out, size_t *out_len)
{
    if (n < 24 || memcmp(d, "\xfd" "7zXZ\0", 6))
        return E_INVAL;
    int check = d[7] & 15, check_size = check == 0 ? 0 : check == 1 ? 4 : check == 4 ? 8 : check == 10 ? 32 : 16;
    struct lz2 *z = kzalloc(sizeof(*z));
    if (!z)
        return E_NOMEM;
    size_t i = 12;
    int err = 0;
    while (!err && i < n && d[i]) {
        size_t hsize = ((size_t)d[i] + 1) * 4, h = i + 1;
        if (i + hsize > n)
            break;
        uint8_t flags = d[h++];
        if (flags & 0x40)
            varint(d, n, &h);
        if (flags & 0x80)
            varint(d, n, &h);
        if ((flags & 3) != 0 || varint(d, n, &h) != 0x21) {
            err = E_NOSYS; /* only plain LZMA2, no BCJ or delta filters */
            break;
        }
        i += hsize;
        size_t used = lz2_decode(z, d + i, n - i);
        if (!used) {
            err = E_INVAL;
            break;
        }
        i += used;
        i = (i + 3) & ~(size_t)3;
        i += (size_t)check_size;
    }
    if (!err && !z->pos && (i >= n || d[i]))
        err = E_INVAL;
    if (err) {
        kfree(z->out);
    } else {
        *out = z->out ? z->out : kzalloc(1);
        *out_len = z->pos;
    }
    kfree(z);
    return err;
}

/* the old .lzma format: properties, the size (all ones: unknown, the data ends with a marker), one LZMA stream */
static int unlzma(const uint8_t *d, size_t n, uint8_t **out, size_t *out_len)
{
    if (n < 13 || d[0] >= 9 * 5 * 5)
        return E_INVAL;
    uint64_t size = 0;
    for (int i = 0; i < 8; i++)
        size |= (uint64_t)d[5 + i] << (8 * i);
    bool known = size != ~(uint64_t)0;
    if (known && size > UNPACK_MAX)
        return E_NOMEM;
    struct lz2 *z = kzalloc(sizeof(*z));
    if (!z)
        return E_NOMEM;
    z->lc = d[0] % 9;
    z->lp = d[0] / 9 % 5;
    z->pb = d[0] / 45;
    int err = z->lc + z->lp > 4 ? E_NOSYS : 0; /* allowed by the format, never made by the tools */
    if (!err) {
        lz2_reset_state(z);
        bool ok = lz2_room(z, known ? (size_t)size : 65536) &&
                  lz2_chunk(z, d + 13, n - 13, known ? (size_t)size : UNPACK_MAX);
        err = ok || (!known && z->ended) ? 0 : E_INVAL;
    }
    if (err) {
        kfree(z->out);
    } else {
        *out = z->out ? z->out : kzalloc(1);
        *out_len = z->pos;
    }
    kfree(z);
    return err;
}

/* gzip, xz or lzma data unpacked; E_INVAL when it is none of them */
static int unpack(const uint8_t *d, size_t n, uint8_t **out, size_t *out_len, const char *name)
{
    if (n >= 3 && d[0] == 0x1f && d[1] == 0x8b)
        return gunzip_alloc(d, n, UNPACK_MAX, out, out_len);
    if (n >= 6 && !memcmp(d, "\xfd" "7zXZ", 5))
        return unxz(d, n, out, out_len);
    const char *ext = path_ext(name);
    if (ext && !str_casecmp(ext, ".lzma"))
        return unlzma(d, n, out, out_len);
    return E_INVAL;
}

/* ---- tar ---- */

static uint64_t octal(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && p[i] == ' '; i++)
        p++, n--;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static bool is_tar(const uint8_t *d, size_t n)
{
    if (n < 512)
        return false;
    if (!memcmp(d + 257, "ustar", 5))
        return true;
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += i >= 148 && i < 156 ? ' ' : d[i];
    return d[0] && sum == octal(d + 148, 8);
}

/* a name from the archive, made safe: no leading "/", no ".." */
static bool safe_name(const char *name, char *out, size_t cap)
{
    while (*name == '/' || (name[0] == '.' && name[1] == '/'))
        name += *name == '/' ? 1 : 2;
    str_copy(out, name, cap);
    size_t n = strlen(out);
    while (n && out[n - 1] == '/')
        out[--n] = 0;
    if (!n || !strcmp(out, ".."))
        return false;
    for (const char *p = out; (p = strstr(p, "..")); p += 2)
        if ((p == out || p[-1] == '/') && (!p[2] || p[2] == '/'))
            return false;
    return true;
}

struct tar_result {
    int files, dirs;
    uint64_t bytes;
};

static int untar(struct shell *sh, const uint8_t *d, size_t n, const char *dir, bool list, struct tar_result *res)
{
    char longname[PATH_MAX_LEN] = "";
    for (size_t i = 0; i + 512 <= n;) {
        const uint8_t *h = d + i;
        if (!h[0])
            break;
        uint64_t size = octal(h + 124, 12);
        char type = (char)h[156];
        size_t data = i + 512;
        i = data + (size_t)((size + 511) & ~(uint64_t)511);
        if (data + size > n)
            return E_INVAL;
        char name[PATH_MAX_LEN];
        if (type == 'L') { /* GNU long name for the next entry */
            str_copy(longname, (const char *)d + data, MIN(sizeof(longname), (size_t)size + 1));
            continue;
        }
        if (type == 'x') { /* pax: take path= */
            const char *p = (const char *)d + data, *e = p + size;
            while (p < e) {
                long len = str_to_long(p, NULL, 10);
                const char *kv = memchr(p, ' ', (size_t)(e - p));
                if (len <= 0 || !kv || p + len > e)
                    break;
                if (!strncmp(kv + 1, "path=", 5))
                    str_copy(longname, kv + 6, MIN(sizeof(longname), (size_t)(p + len - kv - 6)));
                p += len;
            }
            continue;
        }
        if (type == 'g')
            continue;
        if (longname[0]) {
            str_copy(name, longname, sizeof(name));
            longname[0] = 0;
        } else {
            char raw[260] = "";
            if (!memcmp(h + 257, "ustar", 5) && h[345]) {
                str_copy(raw, (const char *)h + 345, MIN(sizeof(raw), (size_t)156));
                str_append(raw, "/", sizeof(raw));
            }
            size_t at = strlen(raw);
            str_copy(raw + at, (const char *)h, MIN(sizeof(raw) - at, (size_t)101));
            str_copy(name, raw, sizeof(name));
        }
        char rel[PATH_MAX_LEN], path[PATH_MAX_LEN];
        if (!safe_name(name, rel, sizeof(rel)))
            continue;
        if (list) {
            sh_printf(sh, "%10llu  %s%s\n", (unsigned long long)size, rel, type == '5' ? "/" : "");
            res->files += type != '5';
            res->bytes += size;
            continue;
        }
        path_join(dir, rel, path, sizeof(path));
        if (type == '5') {
            vfs_mkdirs(path);
            res->dirs++;
        } else if (type == '0' || type == 0 || type == '7') {
            char parent[PATH_MAX_LEN];
            path_dirname(path, parent, sizeof(parent));
            vfs_mkdirs(parent);
            int err = vfs_write_all(path, d + data, (size_t)size);
            if (err)
                return sh_fs_error(sh, "tar", rel, err), err;
            res->files++;
            res->bytes += size;
        }
        if (input_break_pending())
            return E_INTR;
    }
    return 0;
}

/* the name an unpacked .gz/.xz/.tgz gets: "a.tar.gz" → "a.tar", "a.tgz" → "a.tar" */
static void unpacked_name(const char *name, char *out, size_t cap)
{
    str_copy(out, path_basename(name), cap);
    char *dot = strrchr(out, '.');
    if (!dot)
        str_append(out, ".out", cap);
    else if (!str_casecmp(dot, ".tgz") || !str_casecmp(dot, ".txz") || !str_casecmp(dot, ".tlz"))
        str_copy(dot, ".tar", cap - (size_t)(dot - out));
    else
        *dot = 0;
}

COMMAND(tar, "untar,gunzip,unxz", "files", "tar [-t] [-C dir] <file.tar|.tar.gz|.tgz|.tar.xz|.gz|.xz|.lzma>",
        "list (-t) or unpack a tar archive or a gzip, xz or lzma file")
{
    bool list = false;
    const char *dir = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-t"))
            list = true;
        else if (!strcmp(argv[i], "-x") || !strcmp(argv[i], "-xf") || !strcmp(argv[i], "-f"))
            continue;
        else if (!strcmp(argv[i], "-C") && i + 1 < argc)
            dir = argv[++i];
        else
            return command_usage(sh, command_find("tar"));
    }
    if (i != argc - 1)
        return command_usage(sh, command_find("tar"));
    char path[PATH_MAX_LEN], target[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[i], path, sizeof(path));
    if (!err) {
        if (dir)
            err = sh_resolve(sh, dir, target, sizeof(target));
        else
            path_dirname(path, target, sizeof(target));
    }
    char *raw;
    size_t n;
    if (!err)
        err = vfs_read_all(path, &raw, &n);
    if (err)
        return sh_fs_error(sh, "tar", argv[i], err);
    uint8_t *d = (uint8_t *)raw, *plain = NULL;
    size_t plain_len;
    err = unpack(d, n, &plain, &plain_len, path);
    bool packed = !err;
    if (err == E_INVAL) {
        plain = d;
        plain_len = n;
        err = 0;
    } else {
        kfree(raw);
    }
    if (!err && !is_tar(plain, plain_len)) {
        if (!packed) {
            err = E_INVAL;
        } else if (list) {
            sh_printf(sh, "%10u  (one packed file)\n", (unsigned)plain_len);
        } else {
            char name[NAME_MAX_LEN], out[PATH_MAX_LEN];
            unpacked_name(path, name, sizeof(name));
            path_join(target, name, out, sizeof(out));
            err = vfs_write_all(out, plain, plain_len);
            if (!err) {
                char size[16];
                fmt_size(size, sizeof(size), plain_len);
                sh_printf(sh, "Unpacked %s (%s)\n", out, size);
            }
        }
        kfree(plain);
        return err ? sh_error(sh, "tar", "%s: %s", argv[i], err == E_INVAL ? "not an archive" : err_str(err)) : 0;
    }
    struct tar_result res = {0};
    if (!err && !list)
        err = vfs_is_dir(target) ? 0 : vfs_mkdirs(target);
    if (!err)
        err = untar(sh, plain, plain_len, target, list, &res);
    kfree(plain);
    if (err)
        return sh_error(sh, "tar", "%s: %s", argv[i],
                        err == E_INVAL ? "not an archive (or a damaged one)"
                        : err == E_NOSYS ? "uses a filter SynthOS cannot unpack" : err_str(err));
    char size[16];
    fmt_size(size, sizeof(size), (size_t)MIN(res.bytes, (uint64_t)0xffffffffu));
    sh_printf(sh, "%s %d files (%s)%s%s\n", list ? "Listed" : "Extracted", res.files, size, list ? "" : " to ",
              list ? "" : target);
    return 0;
}

/* ---- making ZIP archives ---- */

struct zipper {
    uint8_t *out, *dir;
    size_t len, cap, dir_len, dir_cap;
    int count;
    uint32_t dostime;
};

static bool put(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
    if (*len + n > *cap) {
        size_t c = MAX(*cap * 2, *len + n + 4096);
        uint8_t *g = krealloc(*buf, c);
        if (!g)
            return false;
        *buf = g;
        *cap = c;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v);
    le16(p + 2, v >> 16);
}

static int zip_add(struct zipper *z, const char *path, const char *name, bool is_dir)
{
    char *data = NULL;
    size_t n = 0;
    if (!is_dir) {
        int err = vfs_read_all(path, &data, &n);
        if (err)
            return err;
    }
    uint8_t *packed = NULL;
    size_t plen = n;
    uint32_t method = 0, crc = crc32_update(0, data, n);
    if (n > 64 && deflate_alloc(data, n, &packed, &plen, 0) == 0 && plen < n)
        method = 8;
    else
        kfree(packed), packed = NULL, plen = n;
    char nm[PATH_MAX_LEN];
    ksnprintf(nm, sizeof(nm), "%s%s", name, is_dir ? "/" : "");
    size_t nl = strlen(nm);
    uint8_t lh[30] = {'P', 'K', 3, 4, 20, 0, 0, 8}, ch[46] = {'P', 'K', 1, 2, 20, 3, 20, 0, 0, 8};
    lh[6] = 0;
    lh[7] = 8; /* UTF-8 names */
    le16(lh + 8, method);
    le32(lh + 10, z->dostime);
    le32(lh + 14, crc);
    le32(lh + 18, (uint32_t)plen);
    le32(lh + 22, (uint32_t)n);
    le16(lh + 26, (uint32_t)nl);
    memcpy(ch + 10, lh + 8, 18);
    le16(ch + 28, (uint32_t)nl);
    le32(ch + 38, is_dir ? 0x41ed0010u : 0x81a40000u);
    le32(ch + 42, (uint32_t)z->len);
    bool ok = put(&z->out, &z->len, &z->cap, lh, 30) && put(&z->out, &z->len, &z->cap, nm, nl) &&
              put(&z->out, &z->len, &z->cap, packed ? packed : (uint8_t *)data, plen) &&
              put(&z->dir, &z->dir_len, &z->dir_cap, ch, 46) && put(&z->dir, &z->dir_len, &z->dir_cap, nm, nl);
    kfree(packed);
    kfree(data);
    z->count++;
    return ok ? 0 : E_NOMEM;
}

static int zip_tree(struct zipper *z, const char *path, const char *name, int depth)
{
    if (!vfs_is_dir(path))
        return zip_add(z, path, name, false);
    int err = zip_add(z, path, name, true);
    struct vdir d;
    if (err || depth > 16 || vfs_opendir(path, &d) != 0)
        return err;
    struct vfs_dirent ent;
    while (!err && vfs_readdir(&d, &ent) == 0) {
        char sub[PATH_MAX_LEN], subname[PATH_MAX_LEN];
        path_join(path, ent.name, sub, sizeof(sub));
        ksnprintf(subname, sizeof(subname), "%s/%s", name, ent.name);
        err = zip_tree(z, sub, subname, depth + 1);
        if (!err && input_break_pending())
            err = E_INTR;
    }
    vfs_closedir(&d);
    return err;
}

COMMAND(zip, NULL, "files", "zip <archive.zip> <file|folder>...", "make a ZIP archive of files and folders")
{
    if (argc < 3)
        return command_usage(sh, command_find("zip"));
    char out[PATH_MAX_LEN];
    if (sh_resolve(sh, argv[1], out, sizeof(out)) != 0)
        return sh_error(sh, "zip", "bad path: %s", argv[1]);
    struct rtc_time t;
    time_local(&t);
    struct zipper z = {0};
    z.dostime = (uint32_t)(t.hour << 11 | t.minute << 5 | t.second / 2) |
                (uint32_t)((t.year - 1980) << 9 | t.month << 5 | t.day) << 16;
    int err = 0;
    for (int i = 2; i < argc && !err; i++) {
        char path[PATH_MAX_LEN];
        err = sh_resolve(sh, argv[i], path, sizeof(path));
        if (!err && !strcmp(path, out))
            continue;
        if (!err)
            err = vfs_exists(path) ? zip_tree(&z, path, path_basename(path), 0) : E_NOENT;
        if (err)
            sh_fs_error(sh, "zip", argv[i], err);
    }
    uint8_t end[22] = {'P', 'K', 5, 6};
    le16(end + 8, (uint32_t)z.count);
    le16(end + 10, (uint32_t)z.count);
    le32(end + 12, (uint32_t)z.dir_len);
    le32(end + 16, (uint32_t)z.len);
    if (!err && !(put(&z.out, &z.len, &z.cap, z.dir, z.dir_len) && put(&z.out, &z.len, &z.cap, end, 22)))
        err = E_NOMEM;
    if (!err)
        err = vfs_write_all(out, z.out, z.len);
    kfree(z.out);
    kfree(z.dir);
    if (err)
        return err == E_NOMEM || err == E_INTR ? sh_error(sh, "zip", "%s", err_str(err)) : 1;
    char size[16];
    fmt_size(size, sizeof(size), z.len - z.dir_len - 22);
    sh_printf(sh, "Made %s: %d entries, %s\n", out, z.count, size);
    return 0;
}

static const struct command *const commands[] = {&cmddef_tar, &cmddef_zip};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(NULL, NULL, &ext);
