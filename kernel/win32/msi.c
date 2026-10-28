#include <synth/command.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/lnk.h>
#include <synth/log.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "win32.h"

#if CONFIG_WIN32

/* Windows Installer packages (.msi) without the Windows Installer service: the OLE2 container, the tables, the
   cabinets of files, then the files, shortcuts and registry values the package describes are written. What is
   written is listed under C:\Windows\Installer so msiexec /x can take it back out. Custom actions and services are
   not run; the output says how many were skipped. */

#define MSI_MAXSEC 0xFFFFFFF0u
#define MSI_TEXT 512
#define MSI_PROPS 96
#define MSI_DIRS 128
#define MSI_COLS 32
#define MSI_CABS 8
#define MSI_INSTALLER "C:\\Windows\\Installer\\"

static uint32_t rd16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- the OLE2 compound file ---- */

struct cfb_entry {
    char name[64];
    uint32_t start, size;
};

struct cfb {
    const uint8_t *d;
    size_t size;
    uint32_t ssz, msz, cutoff;
    uint32_t *fat;
    size_t nfat;
    uint32_t *minifat;
    size_t nminifat;
    uint8_t *mini;
    size_t mini_len;
    struct cfb_entry *ents;
    uint32_t nents;
};

static const uint8_t *cfb_sector(const struct cfb *c, uint32_t n)
{
    size_t off = ((size_t)n + 1) * c->ssz;
    return n < MSI_MAXSEC && off + c->ssz <= c->size ? c->d + off : NULL;
}

/* the big sectors of a chain, copied into one buffer */
static int cfb_chain(const struct cfb *c, uint32_t at, uint8_t **out, size_t *len)
{
    size_t n = 0;
    for (uint32_t i = at; i < MSI_MAXSEC; i = c->fat[i]) {
        if (i >= c->nfat || ++n > c->nfat)
            return -1;
    }
    uint8_t *buf = kmalloc(n * c->ssz + 1);
    if (!buf)
        return -1;
    size_t k = 0;
    for (uint32_t i = at; k < n; i = c->fat[i], k++) {
        const uint8_t *s = cfb_sector(c, i);
        if (!s) {
            kfree(buf);
            return -1;
        }
        memcpy(buf + k * c->ssz, s, c->ssz);
    }
    *out = buf;
    *len = n * c->ssz;
    return 0;
}

static void cfb_name(const uint8_t *e, uint32_t nbytes, char *out, size_t cap)
{
    static const char set[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
    size_t o = 0;
    for (uint32_t k = 0; k + 1 < nbytes && k < 62 && o + 2 < cap; k += 2) {
        uint32_t ch = rd16(e + k);
        if (!ch)
            break;
        if (ch >= 0x3800 && ch < 0x4800) {
            uint32_t v = ch - 0x3800;
            out[o++] = set[v & 63];
            out[o++] = set[(v >> 6) & 63];
        } else if (ch >= 0x4800 && ch < 0x4840) {
            out[o++] = set[ch - 0x4800];
        } else if (ch == 0x4840) {
            out[o++] = '!';
        } else {
            out[o++] = ch < 128 ? (char)ch : '?';
        }
    }
    out[o] = 0;
}

static void cfb_close(struct cfb *c)
{
    kfree(c->fat);
    kfree(c->minifat);
    kfree(c->mini);
    kfree(c->ents);
    memset(c, 0, sizeof(*c));
}

static bool cfb_open(struct cfb *c, const uint8_t *d, size_t size)
{
    memset(c, 0, sizeof(*c));
    if (size < 512 || memcmp(d, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8))
        return false;
    c->d = d;
    c->size = size;
    c->ssz = 1u << rd16(d + 30);
    c->msz = 1u << rd16(d + 32);
    c->cutoff = rd32(d + 56);
    if (c->ssz < 512 || c->ssz > 4096 || c->msz > c->ssz)
        return false;
    uint32_t nfat_sec = rd32(d + 44), dir_start = rd32(d + 48), mf_start = rd32(d + 60);
    uint32_t difat_start = rd32(d + 68), ndifat = rd32(d + 72);
    size_t per = c->ssz / 4 - 1;
    if (nfat_sec > size / c->ssz || ndifat > 1024)
        return false;
    uint32_t *ids = kmalloc(sizeof(uint32_t) * (109 + per * ndifat + 1));
    if (!ids)
        return false;
    size_t nids = 0;
    for (int i = 0; i < 109; i++)
        ids[nids++] = rd32(d + 76 + 4 * i);
    uint32_t s = difat_start;
    for (uint32_t k = 0; k < ndifat && s < MSI_MAXSEC; k++) {
        const uint8_t *p = cfb_sector(c, s);
        if (!p)
            break;
        for (size_t i = 0; i < per; i++)
            ids[nids++] = rd32(p + 4 * i);
        s = rd32(p + 4 * per);
    }
    bool ok = nids >= nfat_sec;
    c->fat = ok ? kmalloc(sizeof(uint32_t) * (size_t)nfat_sec * (c->ssz / 4) + 4) : NULL;
    ok = ok && c->fat;
    for (uint32_t i = 0; ok && i < nfat_sec; i++) {
        const uint8_t *p = cfb_sector(c, ids[i]);
        ok = p != NULL;
        for (size_t j = 0; ok && j < c->ssz / 4; j++)
            c->fat[(size_t)i * (c->ssz / 4) + j] = rd32(p + 4 * j);
    }
    kfree(ids);
    c->nfat = (size_t)nfat_sec * (c->ssz / 4);
    uint8_t *dir = NULL;
    size_t dir_len = 0;
    ok = ok && cfb_chain(c, dir_start, &dir, &dir_len) == 0;
    if (ok && mf_start < MSI_MAXSEC) {
        uint8_t *mf = NULL;
        size_t mf_len = 0;
        ok = cfb_chain(c, mf_start, &mf, &mf_len) == 0;
        if (ok) {
            c->nminifat = mf_len / 4;
            c->minifat = kmalloc(sizeof(uint32_t) * (c->nminifat + 1));
            ok = c->minifat != NULL;
            for (size_t i = 0; ok && i < c->nminifat; i++)
                c->minifat[i] = rd32(mf + 4 * i);
            kfree(mf);
        }
    }
    if (!ok) {
        kfree(dir);
        cfb_close(c);
        return false;
    }
    c->nents = (uint32_t)(dir_len / 128);
    c->ents = kzalloc(sizeof(struct cfb_entry) * (c->nents + 1));
    uint32_t root_start = MSI_MAXSEC, root_size = 0, n = 0;
    if (!c->ents) {
        kfree(dir);
        cfb_close(c);
        return false;
    }
    for (uint32_t i = 0; i < c->nents; i++) {
        const uint8_t *e = dir + 128 * i;
        uint8_t type = e[66];
        if (type == 5) {
            root_start = rd32(e + 116);
            root_size = rd32(e + 120);
        }
        if (type != 2)
            continue;
        cfb_name(e, rd16(e + 64), c->ents[n].name, sizeof(c->ents[n].name));
        c->ents[n].start = rd32(e + 116);
        c->ents[n].size = rd32(e + 120);
        n++;
    }
    c->nents = n;
    kfree(dir);
    if (root_start < MSI_MAXSEC && root_size && cfb_chain(c, root_start, &c->mini, &c->mini_len) == 0)
        c->mini_len = MIN(c->mini_len, (size_t)root_size);
    return true;
}

/* a stream by its name, the table streams with the '!' the package gives them; the caller frees it */
static uint8_t *cfb_stream(const struct cfb *c, const char *name, size_t *len)
{
    for (uint32_t i = 0; i < c->nents; i++) {
        const struct cfb_entry *e = &c->ents[i];
        if (strcmp(e->name, name))
            continue;
        uint8_t *out = kzalloc(e->size + 1);
        if (!out)
            return NULL;
        size_t done = 0;
        bool ok = true;
        if (e->size < c->cutoff) {
            uint32_t at = e->start;
            while (done < e->size) {
                size_t off = (size_t)at * c->msz;
                if (at >= c->nminifat || off + c->msz > c->mini_len) {
                    ok = false;
                    break;
                }
                size_t take = MIN((size_t)c->msz, (size_t)e->size - done);
                memcpy(out + done, c->mini + off, take);
                done += take;
                at = c->minifat[at];
            }
        } else {
            uint8_t *buf = NULL;
            size_t got = 0;
            ok = cfb_chain(c, e->start, &buf, &got) == 0 && got >= e->size;
            if (ok)
                memcpy(out, buf, e->size);
            kfree(buf);
        }
        if (!ok) {
            kfree(out);
            return NULL;
        }
        *len = e->size;
        return out;
    }
    return NULL;
}

/* ---- the tables: strings, the column schema, and the column-major tables ---- */

struct msi_column {
    char table[48], name[48];
    uint32_t number;
    uint32_t width;
};

struct msi_db {
    struct cfb cfb;
    uint8_t *text;
    size_t text_len;
    uint32_t *off, *len, nstr, idx;
    struct msi_column *cols;
    uint32_t ncols;
};

struct msi_table {
    uint8_t *data;
    uint32_t rows, ncols;
    uint32_t base[MSI_COLS], width[MSI_COLS];
};

static uint32_t cell_raw(const struct msi_table *t, uint32_t row, uint32_t col)
{
    if (col >= t->ncols)
        return 0;
    const uint8_t *p = t->data + t->base[col] + (size_t)row * t->width[col];
    switch (t->width[col]) {
    case 4:
        return rd32(p);
    case 3:
        return p[0] | p[1] << 8 | p[2] << 16;
    default:
        return rd16(p);
    }
}

/* integers are stored with the sign bit flipped; 0 is null */
static uint32_t cell_int(const struct msi_table *t, uint32_t row, uint32_t col)
{
    uint32_t raw = cell_raw(t, row, col);
    if (!raw)
        return 0;
    return t->width[col] == 4 ? raw ^ 0x80000000u : (raw ^ 0x8000u) & 0xffffu;
}

static const char *cell_str(const struct msi_db *db, const struct msi_table *t, uint32_t row, uint32_t col, char *buf,
                            size_t cap)
{
    uint32_t idx = cell_raw(t, row, col);
    buf[0] = 0;
    if (!idx || idx > db->nstr || db->off[idx - 1] + (size_t)db->len[idx - 1] > db->text_len)
        return buf;
    size_t n = MIN((size_t)db->len[idx - 1], cap - 1);
    memcpy(buf, db->text + db->off[idx - 1], n);
    buf[n] = 0;
    return buf;
}

static bool msi_load_pool(struct msi_db *db)
{
    size_t sp_len = 0;
    uint8_t *sp = cfb_stream(&db->cfb, "!_StringPool", &sp_len);
    db->text = cfb_stream(&db->cfb, "!_StringData", &db->text_len);
    if (!sp || !db->text || sp_len < 4) {
        kfree(sp);
        return false;
    }
    db->nstr = (uint32_t)((sp_len - 4) / 4);
    db->idx = db->nstr > 0xffff ? 3 : 2;
    db->off = kmalloc(sizeof(uint32_t) * (db->nstr + 1));
    db->len = kmalloc(sizeof(uint32_t) * (db->nstr + 1));
    bool ok = db->off && db->len;
    uint32_t off = 0;
    for (uint32_t i = 0; ok && i < db->nstr; i++) {
        uint32_t n = rd16(sp + 4 + 4 * i);
        db->off[i] = off;
        db->len[i] = n;
        off += n;
    }
    kfree(sp);
    return ok;
}

static bool msi_load_schema(struct msi_db *db)
{
    size_t len = 0;
    uint8_t *data = cfb_stream(&db->cfb, "!_Columns", &len);
    uint32_t rowsz = 2 * db->idx + 4;
    if (!data)
        return false;
    uint32_t rows = (uint32_t)(len / rowsz);
    db->cols = kzalloc(sizeof(struct msi_column) * (rows + 1));
    if (!db->cols) {
        kfree(data);
        return false;
    }
    struct msi_table t = {.data = data, .rows = rows, .ncols = 4};
    t.width[0] = db->idx;
    t.width[1] = 2;
    t.width[2] = db->idx;
    t.width[3] = 2;
    t.base[1] = db->idx * rows;
    t.base[2] = (db->idx + 2) * rows;
    t.base[3] = (2 * db->idx + 2) * rows;
    char buf[64];
    for (uint32_t r = 0; r < rows; r++) {
        struct msi_column *c = &db->cols[db->ncols++];
        str_copy(c->table, cell_str(db, &t, r, 0, buf, sizeof(buf)), sizeof(c->table));
        c->number = cell_int(&t, r, 1);
        str_copy(c->name, cell_str(db, &t, r, 2, buf, sizeof(buf)), sizeof(c->name));
        uint32_t type = cell_int(&t, r, 3);
        c->width = type & 0x0800 ? db->idx : (type & 0xff) == 4 ? 4 : 2;
    }
    kfree(data);
    return true;
}

static void msi_table_close(struct msi_table *t)
{
    kfree(t->data);
    memset(t, 0, sizeof(*t));
}

static bool msi_table_open(struct msi_db *db, const char *name, struct msi_table *t)
{
    const struct msi_column *cols[MSI_COLS];
    uint32_t n = 0, rowsz = 0;
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < db->ncols && n < MSI_COLS; i++)
        if (!strcmp(db->cols[i].table, name))
            cols[n++] = &db->cols[i];
    if (!n)
        return false;
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && cols[j - 1]->number > cols[j]->number; j--) {
            const struct msi_column *tmp = cols[j - 1];
            cols[j - 1] = cols[j];
            cols[j] = tmp;
        }
    for (uint32_t i = 0; i < n; i++)
        rowsz += cols[i]->width;
    char stream[80];
    ksnprintf(stream, sizeof(stream), "!%s", name);
    size_t len = 0;
    t->data = cfb_stream(&db->cfb, stream, &len);
    if (!t->data)
        return false;
    if (!rowsz || len % rowsz) {
        msi_table_close(t);
        return false;
    }
    t->rows = (uint32_t)(len / rowsz);
    t->ncols = n;
    uint32_t base = 0;
    for (uint32_t i = 0; i < n; i++) {
        t->base[i] = base;
        t->width[i] = cols[i]->width;
        base += cols[i]->width * t->rows;
    }
    return true;
}

static void msi_db_close(struct msi_db *db)
{
    cfb_close(&db->cfb);
    kfree(db->text);
    kfree(db->off);
    kfree(db->len);
    kfree(db->cols);
    memset(db, 0, sizeof(*db));
}

/* a Windows Installer name is "short|long", and a directory's default is "target:source" */
static void long_name(const char *in, char *out, size_t cap)
{
    char tmp[MSI_TEXT];
    str_copy(tmp, in, sizeof(tmp));
    char *colon = strchr(tmp, ':');
    if (colon)
        *colon = 0;
    const char *bar = strchr(tmp, '|');
    str_copy(out, bar ? bar + 1 : tmp, cap);
}

/* ---- the cabinet ---- */

struct cab_file {
    char name[256];
    uint32_t size, offset, folder;
};

struct cab_folder {
    uint32_t start;
    uint32_t blocks, type;
};

struct cab {
    uint8_t *d;
    size_t size;
    uint32_t nfiles, nfolders, data_res;
    struct cab_file *files;
    struct cab_folder *folders;
    uint8_t *cache;
    size_t cache_len;
    int cache_folder;
};

static void cab_close(struct cab *c)
{
    kfree(c->d);
    kfree(c->files);
    kfree(c->folders);
    kfree(c->cache);
    memset(c, 0, sizeof(*c));
    c->cache_folder = -1;
}

/* takes the cabinet's bytes; they are freed with it, also when it does not parse */
static bool cab_open(struct cab *c, uint8_t *d, size_t size)
{
    memset(c, 0, sizeof(*c));
    c->cache_folder = -1;
    c->d = d;
    c->size = size;
    if (size < 36 || memcmp(d, "MSCF", 4))
        goto fail;
    uint32_t flags = rd16(d + 30);
    c->nfolders = rd16(d + 26);
    c->nfiles = rd16(d + 28);
    size_t p = 36;
    uint32_t folder_res = 0;
    if (flags & 4) {
        p += rd16(d + 32);
        folder_res = d[34];
        c->data_res = d[35];
    }
    c->folders = kzalloc(sizeof(struct cab_folder) * (c->nfolders + 1));
    c->files = kzalloc(sizeof(struct cab_file) * (c->nfiles + 1));
    if (!c->folders || !c->files)
        goto fail;
    for (uint32_t i = 0; i < c->nfolders; i++) {
        if (p + 8 > size)
            goto fail;
        c->folders[i].start = rd32(d + p);
        c->folders[i].blocks = rd16(d + p + 4);
        c->folders[i].type = rd16(d + p + 6);
        p += 8 + folder_res;
    }
    p = rd32(d + 16);
    for (uint32_t i = 0; i < c->nfiles; i++) {
        if (p + 16 > size)
            goto fail;
        struct cab_file *f = &c->files[i];
        f->size = rd32(d + p);
        f->offset = rd32(d + p + 4);
        f->folder = rd16(d + p + 8);
        size_t n = 0;
        while (p + 16 + n < size && d[p + 16 + n] && n < sizeof(f->name) - 1) {
            f->name[n] = (char)d[p + 16 + n];
            n++;
        }
        f->name[n] = 0;
        while (p + 16 + n < size && d[p + 16 + n])
            n++;
        p += 17 + n;
    }
    return true;
fail:
    cab_close(c);
    return false;
}

/* a folder decompressed whole: stored, or MSZIP blocks, each given the 32 KB before it as its dictionary */
static bool cab_load_folder(struct cab *c, uint32_t index)
{
    if ((int)index == c->cache_folder)
        return true;
    if (index >= c->nfolders)
        return false;
    const struct cab_folder *f = &c->folders[index];
    uint32_t method = f->type & 0x0f;
    if (method != 0 && method != 1) {
        klog("win32: msi cabinet folder %u uses compression %u, which is not supported", index, method);
        return false;
    }
    size_t total = 0, p = f->start;
    for (uint32_t b = 0; b < f->blocks; b++) {
        if (p + 8 + c->data_res > c->size)
            return false;
        total += rd16(c->d + p + 6);
        p += 8 + c->data_res + rd16(c->d + p + 4);
    }
    uint8_t *out = kmalloc(total + 1);
    if (!out)
        return false;
    size_t pos = 0;
    p = f->start;
    for (uint32_t b = 0; b < f->blocks; b++) {
        size_t csize = rd16(c->d + p + 4), usize = rd16(c->d + p + 6);
        const uint8_t *data = c->d + p + 8 + c->data_res;
        if (p + 8 + c->data_res + csize > c->size || pos + usize > total)
            goto fail;
        p += 8 + c->data_res + csize;
        if (method == 0) {
            memcpy(out + pos, data, usize);
            pos += usize;
            continue;
        }
        if (csize < 2 || data[0] != 'C' || data[1] != 'K')
            goto fail;
        size_t dict = MIN(pos, (size_t)32768);
        uint8_t *in = kmalloc(5 + dict + csize);
        uint8_t *dec = kmalloc(dict + usize + 1);
        if (!in || !dec) {
            kfree(in);
            kfree(dec);
            goto fail;
        }
        size_t n = 0;
        if (dict) {
            in[0] = 0;
            in[1] = dict & 0xff;
            in[2] = dict >> 8;
            in[3] = ~dict & 0xff;
            in[4] = (~dict >> 8) & 0xff;
            memcpy(in + 5, out + pos - dict, dict);
            n = 5 + dict;
        }
        memcpy(in + n, data + 2, csize - 2);
        n += csize - 2;
        size_t olen = 0, used = 0;
        bool ok = inflate_into(in, n, dec, dict + usize + 1, &olen, &used) == 0 && olen >= dict + usize;
        kfree(in);
        if (!ok) {
            kfree(dec);
            goto fail;
        }
        memcpy(out + pos, dec + dict, usize);
        kfree(dec);
        pos += usize;
    }
    kfree(c->cache);
    c->cache = out;
    c->cache_len = pos;
    c->cache_folder = (int)index;
    return true;
fail:
    kfree(out);
    return false;
}

static uint8_t *cab_extract(struct cab *c, const char *name, size_t *len)
{
    for (uint32_t i = 0; i < c->nfiles; i++) {
        const struct cab_file *f = &c->files[i];
        if (str_casecmp(f->name, name))
            continue;
        if (!cab_load_folder(c, f->folder) || (size_t)f->offset + f->size > c->cache_len)
            return NULL;
        uint8_t *out = kmalloc(f->size + 1);
        if (out) {
            memcpy(out, c->cache + f->offset, f->size);
            *len = f->size;
        }
        return out;
    }
    return NULL;
}

/* ---- the installation ---- */

struct msi_prop {
    char name[80];
    char value[MSI_TEXT];
};

struct msi_dir {
    char id[80];
    char path[PATH_MAX_LEN];
};

struct msi_file {
    char key[80];
    char path[PATH_MAX_LEN];
};

struct msi_run {
    struct shell *sh;
    struct msi_db db;
    struct msi_table prop, dir, comp, file, feature, fcomp, reg, shortcut, folder, custom;
    struct cab cabs[MSI_CABS];
    uint32_t ncabs;
    struct msi_file *files;
    uint32_t nfiles;
    struct msi_prop props[MSI_PROPS];
    uint32_t nprops;
    struct msi_dir dirs[MSI_DIRS];
    uint32_t ndirs;
    char guid[48];
    char installdir[PATH_MAX_LEN];
    char *manifest;
    size_t manifest_len, manifest_cap;
    uint32_t n_files, n_links, n_regs, n_skipped;
    struct lnk link;
    uint8_t blob[4096];
    char text[MSI_TEXT];
};

static void say(struct msi_run *r, const char *fmt, ...)
{
    if (!r->sh)
        return;
    va_list ap;
    va_start(ap, fmt);
    char line[MSI_TEXT];
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sh_printf(r->sh, "%s\n", line);
}

static void manifest_add(struct msi_run *r, char kind, const char *text)
{
    size_t n = strlen(text);
    if (r->manifest_len + n + 3 > r->manifest_cap) {
        size_t cap = MAX(r->manifest_cap * 2, r->manifest_len + n + 256);
        char *grown = kmalloc(cap);
        if (!grown)
            return;
        if (r->manifest_len)
            memcpy(grown, r->manifest, r->manifest_len);
        kfree(r->manifest);
        r->manifest = grown;
        r->manifest_cap = cap;
    }
    r->manifest[r->manifest_len++] = kind;
    r->manifest[r->manifest_len++] = ' ';
    memcpy(r->manifest + r->manifest_len, text, n);
    r->manifest_len += n;
    r->manifest[r->manifest_len++] = '\n';
}

static const char *prop_get(const struct msi_run *r, const char *name)
{
    for (uint32_t i = 0; i < r->nprops; i++)
        if (!strcmp(r->props[i].name, name))
            return r->props[i].value;
    return NULL;
}

static void prop_set(struct msi_run *r, const char *name, const char *value)
{
    struct msi_prop *p = NULL;
    for (uint32_t i = 0; i < r->nprops && !p; i++)
        if (!strcmp(r->props[i].name, name))
            p = &r->props[i];
    if (!p) {
        if (r->nprops >= MSI_PROPS)
            return;
        p = &r->props[r->nprops++];
        str_copy(p->name, name, sizeof(p->name));
    }
    str_copy(p->value, value, sizeof(p->value));
}

static void prop_default(struct msi_run *r, const char *name, const char *value)
{
    if (!prop_get(r, name))
        prop_set(r, name, value);
}

static void join_win(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '\\')
        ksnprintf(out, cap, "%s%s", dir, name);
    else
        ksnprintf(out, cap, "%s\\%s", dir, name);
}

static const char *dir_path(struct msi_run *r, const char *id, int depth)
{
    for (uint32_t i = 0; i < r->ndirs; i++)
        if (!strcmp(r->dirs[i].id, id))
            return r->dirs[i].path;
    if (depth > 24 || r->ndirs >= MSI_DIRS)
        return NULL;
    const char *prop = prop_get(r, id);
    int row = -1;
    char buf[MSI_TEXT];
    for (uint32_t i = 0; i < r->dir.rows && row < 0; i++)
        if (!strcmp(cell_str(&r->db, &r->dir, i, 0, buf, sizeof(buf)), id))
            row = (int)i;
    char full[PATH_MAX_LEN] = "";
    if (prop && prop[0]) {
        str_copy(full, prop, sizeof(full));
    } else if (row < 0) {
        return NULL;
    } else {
        char parent[80], name[PATH_MAX_LEN];
        cell_str(&r->db, &r->dir, (uint32_t)row, 1, parent, sizeof(parent));
        if (!parent[0]) {
            str_copy(full, prop_get(r, "TARGETDIR") ? prop_get(r, "TARGETDIR") : "C:\\", sizeof(full));
        } else {
            const char *base = dir_path(r, parent, depth + 1);
            if (!base)
                return NULL;
            long_name(cell_str(&r->db, &r->dir, (uint32_t)row, 2, buf, sizeof(buf)), name, sizeof(name));
            if (!name[0] || !strcmp(name, "."))
                str_copy(full, base, sizeof(full));
            else
                join_win(full, sizeof(full), base, name);
        }
    }
    struct msi_dir *d = &r->dirs[r->ndirs++];
    str_copy(d->id, id, sizeof(d->id));
    str_copy(d->path, full, sizeof(d->path));
    return d->path;
}

/* a component is installed when a feature that holds it has a level above zero */
static bool component_selected(struct msi_run *r, const char *comp)
{
    char feature[MSI_TEXT], name[MSI_TEXT];
    for (uint32_t i = 0; i < r->fcomp.rows; i++) {
        if (strcmp(cell_str(&r->db, &r->fcomp, i, 1, feature, sizeof(feature)), comp))
            continue;
        cell_str(&r->db, &r->fcomp, i, 0, feature, sizeof(feature));
        for (uint32_t f = 0; f < r->feature.rows; f++)
            if (!strcmp(cell_str(&r->db, &r->feature, f, 0, name, sizeof(name)), feature) &&
                cell_int(&r->feature, f, 5) > 0)
                return true;
    }
    return false;
}

static const char *file_path(const struct msi_run *r, const char *key)
{
    for (uint32_t i = 0; i < r->nfiles; i++)
        if (!strcmp(r->files[i].key, key))
            return r->files[i].path;
    return NULL;
}

/* [PROPERTY], [directory], [#file] and [\] in the text of a table */
static void expand(struct msi_run *r, const char *in, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (const char *p = in; *p && o + 1 < cap;) {
        if (*p != '[') {
            out[o++] = *p++;
            continue;
        }
        const char *end = strchr(p + 1, ']');
        if (!end) {
            out[o++] = *p++;
            continue;
        }
        char key[PATH_MAX_LEN];
        size_t n = MIN((size_t)(end - p - 1), sizeof(key) - 1);
        memcpy(key, p + 1, n);
        key[n] = 0;
        const char *value = "";
        if (!strcmp(key, "\\")) {
            value = "\\";
        } else if (key[0] == '#') {
            value = file_path(r, key + 1) ? file_path(r, key + 1) : "";
        } else if (prop_get(r, key)) {
            value = prop_get(r, key);
        } else if (dir_path(r, key, 0)) {
            value = dir_path(r, key, 0);
        }
        size_t copy = MIN(strlen(value), cap - 1 - o);
        memcpy(out + o, value, copy);
        o += copy;
        p = end + 1;
    }
    out[o] = 0;
}

static bool write_win(const char *win, const void *data, size_t len)
{
    char vfs[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    if (winpath_to_vfs("/", win, vfs, sizeof(vfs)) != 0)
        return false;
    path_dirname(vfs, dir, sizeof(dir));
    vfs_mkdirs(dir);
    return vfs_write_all(vfs, data, len) == 0;
}

static uint8_t *cab_read(struct msi_run *r, const char *key, size_t *len)
{
    for (uint32_t i = 0; i < r->ncabs; i++) {
        uint8_t *d = cab_extract(&r->cabs[i], key, len);
        if (d)
            return d;
    }
    return NULL;
}

static void install_files(struct msi_run *r)
{
    char key[MSI_TEXT], comp[MSI_TEXT], dirid[MSI_TEXT], name[MSI_TEXT], win[PATH_MAX_LEN];
    for (uint32_t i = 0; i < r->file.rows; i++) {
        cell_str(&r->db, &r->file, i, 1, comp, sizeof(comp));
        if (!component_selected(r, comp))
            continue;
        cell_str(&r->db, &r->file, i, 0, key, sizeof(key));
        dirid[0] = 0;
        for (uint32_t c = 0; c < r->comp.rows; c++)
            if (!strcmp(cell_str(&r->db, &r->comp, c, 0, name, sizeof(name)), comp)) {
                cell_str(&r->db, &r->comp, c, 2, dirid, sizeof(dirid));
                break;
            }
        const char *dir = dir_path(r, dirid, 0);
        long_name(cell_str(&r->db, &r->file, i, 2, name, sizeof(name)), name, sizeof(name));
        join_win(win, sizeof(win), dir ? dir : "C:\\", name);
        size_t len = 0;
        uint8_t *data = cab_read(r, key, &len);
        if (!data) {
            r->n_skipped++;
            klog("win32: msi file %s is not in the package's cabinets", key);
            continue;
        }
        bool ok = write_win(win, data, len);
        kfree(data);
        if (!ok) {
            klog("win32: msi cannot write %s", win);
            continue;
        }
        if (r->nfiles < r->file.rows) {
            struct msi_file *f = &r->files[r->nfiles++];
            str_copy(f->key, key, sizeof(f->key));
            str_copy(f->path, win, sizeof(f->path));
        }
        manifest_add(r, 'F', win);
        r->n_files++;
    }
}

static void install_shortcuts(struct msi_run *r)
{
    char comp[MSI_TEXT], dirid[MSI_TEXT], name[MSI_TEXT], text[MSI_TEXT], win[PATH_MAX_LEN];
    for (uint32_t i = 0; i < r->shortcut.rows; i++) {
        cell_str(&r->db, &r->shortcut, i, 3, comp, sizeof(comp));
        if (!component_selected(r, comp))
            continue;
        cell_str(&r->db, &r->shortcut, i, 1, dirid, sizeof(dirid));
        const char *dir = dir_path(r, dirid, 0);
        if (!dir)
            continue;
        long_name(cell_str(&r->db, &r->shortcut, i, 2, name, sizeof(name)), name, sizeof(name));
        memset(&r->link, 0, sizeof(r->link));
        expand(r, cell_str(&r->db, &r->shortcut, i, 4, text, sizeof(text)), r->link.target, sizeof(r->link.target));
        expand(r, cell_str(&r->db, &r->shortcut, i, 5, text, sizeof(text)), r->link.args, sizeof(r->link.args));
        expand(r, cell_str(&r->db, &r->shortcut, i, 6, text, sizeof(text)), r->link.desc, sizeof(r->link.desc));
        const char *wk = dir_path(r, cell_str(&r->db, &r->shortcut, i, 11, text, sizeof(text)), 0);
        str_copy(r->link.dir, wk ? wk : "", sizeof(r->link.dir));
        str_copy(r->link.icon, r->link.target, sizeof(r->link.icon));
        r->link.show = (int)cell_int(&r->shortcut, i, 10);
        size_t n = lnk_encode(&r->link, r->blob, sizeof(r->blob));
        ksnprintf(text, sizeof(text), "%s.lnk", name);
        join_win(win, sizeof(win), dir, text);
        if (n && write_win(win, r->blob, n)) {
            manifest_add(r, 'L', win);
            r->n_links++;
        }
    }
}

static uint32_t hex_digit(char c)
{
    return c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
           : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 0;
}

static void install_registry(struct msi_run *r)
{
    char comp[MSI_TEXT], key[PATH_MAX_LEN], name[MSI_TEXT], value[MSI_TEXT], path[PATH_MAX_LEN + 8];
    for (uint32_t i = 0; i < r->reg.rows; i++) {
        cell_str(&r->db, &r->reg, i, 5, comp, sizeof(comp));
        if (!component_selected(r, comp))
            continue;
        uint32_t root = cell_int(&r->reg, i, 1);
        const char *root_name = root == 0 ? "HKCR" : root == 1 ? "HKCU" : root == 2 ? "HKLM"
                                : prop_get(r, "ALLUSERS") && prop_get(r, "ALLUSERS")[0] ? "HKLM" : "HKCU";
        char raw[MSI_TEXT];
        expand(r, cell_str(&r->db, &r->reg, i, 2, raw, sizeof(raw)), key, sizeof(key));
        expand(r, cell_str(&r->db, &r->reg, i, 3, raw, sizeof(raw)), name, sizeof(name));
        if (!strcmp(name, "*"))
            name[0] = 0;
        expand(r, cell_str(&r->db, &r->reg, i, 4, raw, sizeof(raw)), value, sizeof(value));
        ksnprintf(path, sizeof(path), "%s\\%s", root_name, key);
        bool ok;
        long number;
        if (value[0] == '#' && value[1] == 'x') {
            uint8_t bin[512];
            size_t n = 0;
            for (const char *p = value + 2; p[0] && p[1] && n < sizeof(bin); p += 2)
                bin[n++] = (uint8_t)(hex_digit(p[0]) << 4 | hex_digit(p[1]));
            ok = w32_reg_set(path, name, 3, bin, (uint32_t)n);
        } else if (value[0] == '#' && value[1] != '%' && str_parse_long(value + 1, &number)) {
            uint32_t v = (uint32_t)number;
            ok = w32_reg_set(path, name, 4, &v, 4);
        } else {
            uint32_t type = value[0] == '#' ? 2 : 1;
            const char *text = value[0] == '#' ? value + 2 : value;
            WCHAR wide[MSI_TEXT];
            size_t units = w32_cp_to_wide(text, -1, wide, sizeof(wide) / sizeof(WCHAR), 1);
            ok = w32_reg_set(path, name, type, wide, (uint32_t)(units * 2));
        }
        if (ok) {
            ksnprintf(raw, sizeof(raw), "%s\t%s", path, name);
            manifest_add(r, 'R', raw);
            r->n_regs++;
        }
    }
}

static void create_folders(struct msi_run *r)
{
    char comp[MSI_TEXT], dirid[MSI_TEXT], vfs[PATH_MAX_LEN];
    for (uint32_t i = 0; i < r->folder.rows; i++) {
        cell_str(&r->db, &r->folder, i, 1, comp, sizeof(comp));
        if (!component_selected(r, comp))
            continue;
        cell_str(&r->db, &r->folder, i, 0, dirid, sizeof(dirid));
        const char *dir = dir_path(r, dirid, 0);
        if (dir && winpath_to_vfs("/", dir, vfs, sizeof(vfs)) == 0)
            vfs_mkdirs(vfs);
    }
}

static void set_default_folders(struct msi_run *r)
{
    prop_default(r, "TARGETDIR", "C:\\");
    prop_default(r, "ProgramFilesFolder", "C:\\Program Files\\");
    prop_default(r, "ProgramFiles64Folder", "C:\\Program Files\\");
    prop_default(r, "ProgramFilesFolder64", "C:\\Program Files\\");
    prop_default(r, "CommonFilesFolder", "C:\\Program Files\\Common Files\\");
    prop_default(r, "CommonFiles64Folder", "C:\\Program Files\\Common Files\\");
    prop_default(r, "WindowsFolder", "C:\\Windows\\");
    prop_default(r, "SystemFolder", "C:\\Windows\\System32\\");
    prop_default(r, "System64Folder", "C:\\Windows\\System32\\");
    prop_default(r, "TempFolder", "C:\\Windows\\Temp\\");
    prop_default(r, "CommonAppDataFolder", "C:\\ProgramData\\");
    prop_default(r, "AppDataFolder", "C:\\Users\\%s\\AppData\\Roaming\\");
    prop_default(r, "LocalAppDataFolder", "C:\\Users\\%s\\AppData\\Local\\");
    prop_default(r, "PersonalFolder", "C:\\Users\\%s\\Documents\\");
    prop_default(r, "ProgramMenuFolder", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\");
    prop_default(r, "StartMenuFolder", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\");
    prop_default(r, "StartupFolder", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\");
    prop_default(r, "DesktopFolder", "C:\\Users\\Public\\Desktop\\");
    prop_default(r, "FontsFolder", "C:\\Windows\\Fonts\\");
    prop_default(r, "ALLUSERS", "1");
    const char *user = user_current()->name;
    static const char *per_user[] = {"AppDataFolder", "LocalAppDataFolder", "PersonalFolder"};
    for (size_t i = 0; i < ARRAY_SIZE(per_user); i++) {
        const char *v = prop_get(r, per_user[i]);
        if (!v || !strstr(v, "%s"))
            continue;
        char text[MSI_TEXT];
        ksnprintf(text, sizeof(text), v, user);
        prop_set(r, per_user[i], text);
    }
}

static bool parse_guid(const char *text, char *out, size_t cap)
{
    const char *end = text ? strchr(text, '}') : NULL;
    if (!end || text[0] != '{' || (size_t)(end - text) + 2 > cap)
        return false;
    memcpy(out, text, (size_t)(end - text) + 1);
    out[end - text + 1] = 0;
    return true;
}

static void register_uninstall(struct msi_run *r)
{
    char key[PATH_MAX_LEN], text[MSI_TEXT];
    ksnprintf(key, sizeof(key), "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", r->guid);
    if (prop_get(r, "ProductName"))
        w32_reg_write(key, "DisplayName", prop_get(r, "ProductName"));
    if (prop_get(r, "ProductVersion"))
        w32_reg_write(key, "DisplayVersion", prop_get(r, "ProductVersion"));
    if (prop_get(r, "Manufacturer"))
        w32_reg_write(key, "Publisher", prop_get(r, "Manufacturer"));
    w32_reg_write(key, "InstallLocation", r->installdir);
    ksnprintf(text, sizeof(text), "MsiExec.exe /X%s", r->guid);
    w32_reg_write(key, "UninstallString", text);
    uint32_t one = 1;
    w32_reg_set(key, "NoModify", 4, &one, 4);
    w32_reg_set(key, "NoRepair", 4, &one, 4);
    w32_reg_set(key, "WindowsInstaller", 4, &one, 4);
}

static void load_props(struct msi_run *r)
{
    char name[MSI_TEXT], value[MSI_TEXT];
    for (uint32_t i = 0; i < r->prop.rows; i++) {
        cell_str(&r->db, &r->prop, i, 0, name, sizeof(name));
        prop_set(r, name, cell_str(&r->db, &r->prop, i, 1, value, sizeof(value)));
    }
}

static void open_cabs(struct msi_run *r, const char *package_vfs)
{
    struct msi_table media;
    if (!msi_table_open(&r->db, "Media", &media))
        return;
    char cabname[MSI_TEXT], dir[PATH_MAX_LEN], full[PATH_MAX_LEN];
    for (uint32_t i = 0; i < media.rows && r->ncabs < MSI_CABS; i++) {
        cell_str(&r->db, &media, i, 3, cabname, sizeof(cabname));
        uint8_t *data = NULL;
        size_t len = 0;
        if (cabname[0] == '#') {
            data = cfb_stream(&r->db.cfb, cabname + 1, &len);
        } else if (cabname[0]) {
            char *text = NULL;
            path_dirname(package_vfs, dir, sizeof(dir));
            ksnprintf(full, sizeof(full), "%s/%s", dir, cabname);
            if (vfs_read_all(full, &text, &len) == 0)
                data = (uint8_t *)text;
        }
        if (data && cab_open(&r->cabs[r->ncabs], data, len))
            r->ncabs++;
    }
    msi_table_close(&media);
}

static void msi_run_free(struct msi_run *r)
{
    for (uint32_t i = 0; i < r->ncabs; i++)
        cab_close(&r->cabs[i]);
    msi_table_close(&r->prop);
    msi_table_close(&r->dir);
    msi_table_close(&r->comp);
    msi_table_close(&r->file);
    msi_table_close(&r->feature);
    msi_table_close(&r->fcomp);
    msi_table_close(&r->reg);
    msi_table_close(&r->shortcut);
    msi_table_close(&r->folder);
    msi_table_close(&r->custom);
    msi_db_close(&r->db);
    kfree(r->files);
    kfree(r->manifest);
    kfree(r);
}

static void open_tables(struct msi_run *r)
{
    msi_table_open(&r->db, "Property", &r->prop);
    msi_table_open(&r->db, "Directory", &r->dir);
    msi_table_open(&r->db, "Component", &r->comp);
    msi_table_open(&r->db, "File", &r->file);
    msi_table_open(&r->db, "Feature", &r->feature);
    msi_table_open(&r->db, "FeatureComponents", &r->fcomp);
    msi_table_open(&r->db, "Registry", &r->reg);
    msi_table_open(&r->db, "Shortcut", &r->shortcut);
    msi_table_open(&r->db, "CreateFolder", &r->folder);
    msi_table_open(&r->db, "CustomAction", &r->custom);
}

static void save_manifest(struct msi_run *r)
{
    char path[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), MSI_INSTALLER "%s.txt", r->guid);
    if (r->manifest)
        write_win(path, r->manifest, r->manifest_len);
}

static int msi_install_db(struct msi_run *r, const char *package, const char *package_vfs, uint8_t *file, size_t size,
                          int nprops, char **props)
{
    if (!cfb_open(&r->db.cfb, file, size) || !msi_load_pool(&r->db) || !msi_load_schema(&r->db)) {
        say(r, "%s is not a readable Windows Installer package.", package);
        return 1620;
    }
    open_tables(r);
    load_props(r);
    set_default_folders(r);
    for (int i = 0; i < nprops; i++) {
        const char *eq = strchr(props[i], '=');
        char name[80];
        if (!eq)
            continue;
        size_t n = MIN((size_t)(eq - props[i]), sizeof(name) - 1);
        memcpy(name, props[i], n);
        name[n] = 0;
        prop_set(r, name, eq + 1);
    }
    if (!parse_guid(prop_get(r, "ProductCode"), r->guid, sizeof(r->guid))) {
        say(r, "%s has no product code.", package);
        return 1620;
    }
    const char *name = prop_get(r, "ProductName"), *version = prop_get(r, "ProductVersion");
    say(r, "Installing %s %s", name ? name : "(unnamed)", version ? version : "");
    open_cabs(r, package_vfs);
    if (!r->ncabs) {
        say(r, "The package has no cabinet of files that can be read.");
        return 1334;
    }
    r->files = kzalloc(sizeof(struct msi_file) * (r->file.rows + 1));
    if (!r->files)
        return 1601;
    const char *idir = dir_path(r, "INSTALLDIR", 0);
    str_copy(r->installdir, idir ? idir : "", sizeof(r->installdir));
    install_files(r);
    install_shortcuts(r);
    install_registry(r);
    create_folders(r);
    register_uninstall(r);
    save_manifest(r);
    say(r, "Installed %lu files, %lu shortcuts and %lu registry values.", (unsigned long)r->n_files,
        (unsigned long)r->n_links, (unsigned long)r->n_regs);
    if (r->n_skipped)
        say(r, "%lu files were not in the package's cabinets.", (unsigned long)r->n_skipped);
    if (r->custom.rows)
        say(r, "%lu custom actions were not run (not supported).", (unsigned long)r->custom.rows);
    return 0;
}

static int msi_install(struct shell *sh, const char *package, int nprops, char **props)
{
    struct msi_run *r = kzalloc(sizeof(*r));
    if (!r)
        return 1601;
    r->sh = sh;
    char vfs[PATH_MAX_LEN];
    char *file = NULL;
    size_t size = 0;
    int rc;
    if (winpath_to_vfs("/", package, vfs, sizeof(vfs)) != 0 || vfs_read_all(vfs, &file, &size) != 0) {
        say(r, "The package could not be opened: %s", package);
        rc = 1619;
    } else {
        rc = msi_install_db(r, package, vfs, (uint8_t *)file, size, nprops, props);
    }
    kfree(file);
    msi_run_free(r);
    return rc;
}

/* the product code a package names, for msiexec /x with a file */
static bool package_code(const char *package, char *guid, size_t cap)
{
    char vfs[PATH_MAX_LEN];
    char *file = NULL;
    size_t size = 0;
    bool ok = false;
    if (winpath_to_vfs("/", package, vfs, sizeof(vfs)) != 0 || vfs_read_all(vfs, &file, &size) != 0)
        return false;
    struct msi_db db;
    memset(&db, 0, sizeof(db));
    if (cfb_open(&db.cfb, (uint8_t *)file, size) && msi_load_pool(&db) && msi_load_schema(&db)) {
        struct msi_table p;
        if (msi_table_open(&db, "Property", &p)) {
            char key[MSI_TEXT], value[MSI_TEXT];
            for (uint32_t i = 0; i < p.rows; i++)
                if (!strcmp(cell_str(&db, &p, i, 0, key, sizeof(key)), "ProductCode")) {
                    str_copy(guid, cell_str(&db, &p, i, 1, value, sizeof(value)), cap);
                    ok = true;
                }
            msi_table_close(&p);
        }
    }
    msi_db_close(&db);
    kfree(file);
    return ok;
}

/* msiexec /x: the files, shortcuts and registry values the package wrote come back out */
static int msi_remove(struct shell *sh, const char *guid)
{
    struct msi_run *r = kzalloc(sizeof(*r));
    if (!r)
        return 1601;
    r->sh = sh;
    str_copy(r->guid, guid, sizeof(r->guid));
    char path[PATH_MAX_LEN], vfs[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), MSI_INSTALLER "%s.txt", guid);
    char *text = NULL;
    size_t n = 0;
    int rc = 0;
    if (winpath_to_vfs("/", path, vfs, sizeof(vfs)) != 0 || vfs_read_all(vfs, &text, &n) != 0) {
        say(r, "This action is only valid for products that are currently installed (%s).", guid);
        rc = 1605;
    } else {
        uint32_t files = 0, links = 0, regs = 0;
        const char *p = text, *end = text + n;
        while (p < end) {
            const char *eol = memchr(p, '\n', (size_t)(end - p));
            size_t len = (size_t)((eol ? eol : end) - p);
            char line[PATH_MAX_LEN + 96];
            size_t copy = MIN(len, sizeof(line) - 1);
            memcpy(line, p, copy);
            line[copy] = 0;
            char target[PATH_MAX_LEN];
            if (line[0] == 'R' && line[1] == ' ') {
                char *tab = strchr(line + 2, '\t');
                if (tab) {
                    *tab = 0;
                    if (w32_reg_delete_value(line + 2, tab + 1))
                        regs++;
                }
            } else if (line[1] == ' ' && winpath_to_vfs("/", line + 2, target, sizeof(target)) == 0 &&
                       vfs_unlink(target) == 0) {
                if (line[0] == 'F')
                    files++;
                else if (line[0] == 'L')
                    links++;
            }
            p = eol ? eol + 1 : end;
        }
        say(r, "Removed %lu files, %lu shortcuts and %lu registry values.", (unsigned long)files,
            (unsigned long)links, (unsigned long)regs);
        ksnprintf(path, sizeof(path), "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", guid);
        w32_reg_delete_key(path);
        vfs_unlink(vfs);
        kfree(text);
    }
    msi_run_free(r);
    return rc;
}

static void msi_msg(struct shell *sh, const char *text)
{
    if (sh)
        sh_printf(sh, "%s", text);
}

/* msiexec /i package [/qn] [PROPERTY=value] | msiexec /x {product code} | msiexec /x package */
int msiexec_main(struct shell *sh, int argc, char **argv)
{
    char package[PATH_MAX_LEN] = "", guid[48] = "";
    char *props[64];
    int nprops = 0;
    char mode = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '/' || a[0] == '-') && a[1]) {
            char c = (char)(a[1] | 0x20);
            if (c == 'i' || c == 'x') {
                mode = c;
                const char *rest = a + 2;
                while (*rest == ' ')
                    rest++;
                if (*rest && c == 'x' && rest[0] == '{')
                    str_copy(guid, rest, sizeof(guid));
                else if (*rest)
                    str_copy(package, rest, sizeof(package));
            }
            continue;
        }
        if (strchr(a, '=') && nprops < 64) {
            props[nprops++] = argv[i];
            continue;
        }
        if (mode == 'x' && a[0] == '{' && !guid[0])
            str_copy(guid, a, sizeof(guid));
        else if (!package[0])
            str_copy(package, a, sizeof(package));
    }
    if (mode == 'x') {
        if (!guid[0] && package[0] && !package_code(package, guid, sizeof(guid))) {
            msi_msg(sh, "msiexec: the package has no product code\n");
            return 1620;
        }
        char check[48];
        if (!parse_guid(guid, check, sizeof(check))) {
            msi_msg(sh, "msiexec: a product code like {GUID} is needed\n");
            return 1619;
        }
        return msi_remove(sh, guid);
    }
    if (mode == 'i' && package[0])
        return msi_install(sh, package, nprops, props);
    msi_msg(sh, "usage: msiexec /i package.msi [/qn] [PROPERTY=value]\n       msiexec /x {product code}\n");
    return 1619;
}

COMMAND(msiexec, "msi", "system", "msiexec /i <package.msi> [/qn] [PROP=value] | /x {product code}",
        "install or remove a Windows Installer package")
{
    return msiexec_main(sh, argc, argv);
}

#endif
