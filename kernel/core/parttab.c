#include <synth/block.h>
#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/parttab.h>
#include <synth/string.h>
#include <synth/zip.h>

/* Reads and edits partition tables: MBR with four primary entries and GPT with both copies and their checksums.
   Free space between partitions is listed in 1 MiB steps so new partitions start aligned. */

#define GPT_MAX_BYTES (256 * 128)

static const uint8_t guid_esp[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t guid_basic[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                       0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
static const uint8_t guid_msr[16] = {0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d,
                                     0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae};
static const uint8_t guid_winre[16] = {0xa4, 0xbb, 0x94, 0xde, 0xd1, 0x06, 0x40, 0x4d,
                                       0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac};
static const uint8_t guid_linux[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                       0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
static const uint8_t guid_synth[16] = {0x54, 0x4e, 0x59, 0x53, 0x4f, 0x48, 0x53, 0x4f,
                                       0x80, 0x53, 0x79, 0x6e, 0x74, 0x68, 0x4f, 0x53};

struct gpt {
    uint8_t header[BLOCK_SIZE];
    uint8_t *entries;
    uint32_t count, size, sectors;
    uint64_t entries_lba, alternate, first, last;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

const char *part_kind_name(enum part_kind kind)
{
    static const char *const names[] = {"Free space", "EFI system", "SynthOS",  "Windows",  "Microsoft reserved",
                                        "Recovery",   "Linux",      "Data",     "Extended", "Other"};
    return kind <= PART_OTHER ? names[kind] : "Other";
}

const char *part_scheme_name(enum part_scheme scheme)
{
    return scheme == SCHEME_GPT ? "GPT" : scheme == SCHEME_MBR ? "MBR" : "empty";
}

static void copy_label(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap && src[i]; i++)
        dst[k++] = src[i] >= 32 && src[i] < 127 ? (char)src[i] : '?';
    while (k && dst[k - 1] == ' ')
        k--;
    dst[k] = 0;
    if (!strcmp(dst, "NO NAME"))
        dst[0] = 0;
}

static void detect_fs(struct blockdev *dev, struct part *p)
{
    static uint8_t s[BLOCK_SIZE];
    if (block_read(dev, p->start, 1, s))
        return;
    bool marker = false;
    for (int i = 0; i + 8 <= 446 && !marker; i++)
        marker = !memcmp(s + i, "SynthOS:", 8);
    if (!memcmp(s + 3, "NTFS    ", 8)) {
        str_copy(p->fs, "NTFS", sizeof(p->fs));
        if (p->kind == PART_DATA || p->kind == PART_OTHER)
            p->kind = PART_WINDOWS;
    } else if (!memcmp(s + 3, "EXFAT   ", 8)) {
        str_copy(p->fs, "exFAT", sizeof(p->fs));
    } else if (!memcmp(s + 82, "FAT32", 5)) {
        str_copy(p->fs, "FAT32", sizeof(p->fs));
        if (!p->label[0])
            copy_label(p->label, sizeof(p->label), s + 71, 11);
    } else if (!memcmp(s + 54, "FAT1", 4)) {
        str_copy(p->fs, "FAT", sizeof(p->fs));
        if (!p->label[0])
            copy_label(p->label, sizeof(p->label), s + 43, 11);
    } else if (marker) {
        str_copy(p->fs, "SynthOS", sizeof(p->fs));
        p->kind = PART_SYNTHOS;
    }
}

static enum part_kind mbr_kind(uint8_t type)
{
    switch (type) {
    case 0xef: return PART_EFI;
    case 0x7f: return PART_SYNTHOS;
    case 0x07: return PART_DATA;
    case 0x27: return PART_RECOVERY;
    case 0x83:
    case 0x82: return PART_LINUX;
    case 0x05:
    case 0x0f:
    case 0x85: return PART_EXTENDED;
    case 0x01:
    case 0x04:
    case 0x06:
    case 0x0b:
    case 0x0c:
    case 0x0e: return PART_DATA;
    default: return PART_OTHER;
    }
}

static enum part_kind gpt_kind(const uint8_t *type)
{
    if (!memcmp(type, guid_esp, 16))
        return PART_EFI;
    if (!memcmp(type, guid_synth, 16))
        return PART_SYNTHOS;
    if (!memcmp(type, guid_basic, 16))
        return PART_DATA;
    if (!memcmp(type, guid_msr, 16))
        return PART_MSR;
    if (!memcmp(type, guid_winre, 16))
        return PART_RECOVERY;
    if (!memcmp(type, guid_linux, 16))
        return PART_LINUX;
    return PART_OTHER;
}

static bool zero16(const uint8_t *p)
{
    for (int i = 0; i < 16; i++)
        if (p[i])
            return false;
    return true;
}

static void gpt_free(struct gpt *g)
{
    kfree(g->entries);
    g->entries = NULL;
}

static int gpt_load(struct blockdev *dev, struct gpt *g)
{
    memset(g, 0, sizeof(*g));
    if (block_read(dev, 1, 1, g->header) || memcmp(g->header, "EFI PART", 8))
        return E_INVAL;
    uint32_t hsize = rd32(g->header + 12);
    if (hsize < 92 || hsize > BLOCK_SIZE)
        return E_INVAL;
    uint8_t copy[BLOCK_SIZE];
    memcpy(copy, g->header, BLOCK_SIZE);
    wr32(copy + 16, 0);
    if (crc32_update(0, copy, hsize) != rd32(g->header + 16))
        return E_INVAL;
    g->entries_lba = rd64(g->header + 72);
    g->count = rd32(g->header + 80);
    g->size = rd32(g->header + 84);
    g->alternate = rd64(g->header + 32);
    g->first = rd64(g->header + 40);
    g->last = rd64(g->header + 48);
    if (g->size < 128 || g->size > 512 || g->size % 8 || !g->count || (uint64_t)g->count * g->size > GPT_MAX_BYTES)
        return E_INVAL;
    g->sectors = (g->count * g->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (g->entries_lba + g->sectors > dev->sectors)
        return E_INVAL;
    if (!(g->entries = kmalloc(g->sectors * BLOCK_SIZE)))
        return E_NOMEM;
    int err = block_read(dev, (uint32_t)g->entries_lba, g->sectors, g->entries);
    if (!err && crc32_update(0, g->entries, g->count * g->size) != rd32(g->header + 88))
        err = E_INVAL;
    if (err)
        gpt_free(g);
    return err;
}

static void seal(uint8_t *header)
{
    uint32_t hsize = rd32(header + 12);
    wr32(header + 16, 0);
    wr32(header + 16, crc32_update(0, header, hsize));
}

/* The backup copy goes first: if the write stops halfway, the primary table is still the old, valid one. */
static int gpt_store(struct blockdev *dev, struct gpt *g)
{
    if (g->alternate >= dev->sectors || g->alternate <= g->last)
        return E_RANGE;
    wr32(g->header + 88, crc32_update(0, g->entries, g->count * g->size));
    uint8_t backup[BLOCK_SIZE];
    memcpy(backup, g->header, BLOCK_SIZE);
    uint64_t backup_entries = g->alternate - g->sectors;
    uint8_t old[BLOCK_SIZE];
    if (!block_read(dev, (uint32_t)g->alternate, 1, old) && !memcmp(old, "EFI PART", 8)) {
        uint64_t lba = rd64(old + 72);
        if (lba > g->last && lba + g->sectors <= g->alternate)
            backup_entries = lba;
    }
    wr64(backup + 24, g->alternate);
    wr64(backup + 32, 1);
    wr64(backup + 72, backup_entries);
    seal(backup);
    int err = block_write(dev, (uint32_t)backup_entries, g->sectors, g->entries);
    if (!err)
        err = block_write(dev, (uint32_t)g->alternate, 1, backup);
    if (err)
        return err;
    seal(g->header);
    if ((err = block_write(dev, (uint32_t)g->entries_lba, g->sectors, g->entries)) != 0)
        return err;
    return block_write(dev, 1, 1, g->header);
}

static void add_part(struct disk_map *map, const struct part *p)
{
    if (map->count == PART_MAX)
        return;
    int i = map->count++;
    while (i > 0 && map->parts[i - 1].start > p->start) {
        map->parts[i] = map->parts[i - 1];
        i--;
    }
    map->parts[i] = *p;
}

static void add_free(struct disk_map *map, uint32_t first, uint32_t last)
{
    uint32_t used = map->count;
    struct part spans[PART_MAX];
    memcpy(spans, map->parts, used * sizeof(struct part));
    uint64_t cursor = first;
    for (uint32_t i = 0; i <= used; i++) {
        uint64_t end = i < used ? spans[i].start : (uint64_t)last + 1;
        uint64_t start = (cursor + PART_ALIGN - 1) / PART_ALIGN * PART_ALIGN;
        if (end > start && end - start >= PART_ALIGN)
            add_part(map, &(struct part){(uint32_t)start, (uint32_t)(end - start), -1, PART_FREE, "", ""});
        if (i < used && (uint64_t)spans[i].start + spans[i].count > cursor)
            cursor = (uint64_t)spans[i].start + spans[i].count;
    }
}

int disk_map_read(struct blockdev *dev, struct disk_map *map)
{
    memset(map, 0, sizeof(*map));
    map->dev = dev;
    static uint8_t mbr[BLOCK_SIZE];
    int err = block_read(dev, 0, 1, mbr);
    if (err)
        return err;
    bool valid = mbr[510] == 0x55 && mbr[511] == 0xaa, protective = false;
    for (int i = 0; valid && i < 4; i++)
        protective |= mbr[446 + i * 16 + 4] == 0xee;
    uint32_t last = dev->sectors - 1;
    struct gpt g;
    if (protective && gpt_load(dev, &g) == 0) {
        map->scheme = SCHEME_GPT;
        map->read_only = g.alternate >= dev->sectors;
        for (uint32_t i = 0; i < g.count; i++) {
            const uint8_t *e = g.entries + i * g.size;
            if (zero16(e)) {
                map->free_slots++;
                continue;
            }
            uint64_t first = rd64(e + 32), end = rd64(e + 40);
            if (end < first || first >= dev->sectors)
                continue;
            struct part p = {(uint32_t)first, (uint32_t)MIN(end - first + 1, (uint64_t)dev->sectors - first), (int)i,
                             gpt_kind(e), "", ""};
            for (int k = 0; k < 36 && k + 1 < (int)sizeof(p.label); k++) {
                uint16_t ch = e[56 + 2 * k] | e[57 + 2 * k] << 8;
                if (!ch)
                    break;
                p.label[k] = ch < 127 && ch >= 32 ? (char)ch : '?';
            }
            detect_fs(dev, &p);
            add_part(map, &p);
        }
        uint64_t first = g.first, end = MIN(g.last, (uint64_t)last);
        gpt_free(&g);
        add_free(map, (uint32_t)first, (uint32_t)end);
        return 0;
    }
    if (protective) {
        map->scheme = SCHEME_GPT;
        map->read_only = true;
        return 0;
    }
    if (valid && (mbr[0] == 0xeb || mbr[0] == 0xe9) &&
        (!memcmp(mbr + 3, "NTFS    ", 8) || !memcmp(mbr + 3, "EXFAT   ", 8) || !memcmp(mbr + 54, "FAT", 3) ||
         !memcmp(mbr + 82, "FAT32", 5))) {
        map->scheme = SCHEME_NONE;
        map->read_only = true;
        struct part p = {0, dev->sectors, -1, PART_DATA, "", ""};
        detect_fs(dev, &p);
        add_part(map, &p);
        return 0;
    }
    if (!valid) {
        map->scheme = SCHEME_NONE;
        add_free(map, 1, last);
        return 0;
    }
    map->scheme = SCHEME_MBR;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint32_t start = rd32(e + 8), count = rd32(e + 12);
        if (!e[4] || !count || start >= dev->sectors) {
            map->free_slots++;
            continue;
        }
        struct part p = {start, MIN(count, dev->sectors - start), i, mbr_kind(e[4]), "", ""};
        if (p.kind != PART_EXTENDED)
            detect_fs(dev, &p);
        add_part(map, &p);
    }
    add_free(map, 1, last);
    return 0;
}

static bool overlaps(const struct disk_map *map, uint32_t start, uint32_t count)
{
    for (int i = 0; i < map->count; i++) {
        const struct part *p = &map->parts[i];
        if (p->kind != PART_FREE && start < p->start + p->count && p->start < start + count)
            return true;
    }
    return false;
}

int disk_map_add(struct disk_map *map, uint32_t start, uint32_t count, enum part_kind kind, const char *name)
{
    struct blockdev *dev = map->dev;
    if (map->read_only)
        return E_RANGE;
    if (!count || start < 1 || (uint64_t)start + count > dev->sectors || overlaps(map, start, count))
        return E_RANGE;
    if (map->scheme == SCHEME_MBR) {
        static uint8_t mbr[BLOCK_SIZE];
        int err = block_read(dev, 0, 1, mbr);
        if (err)
            return err;
        int slot = -1;
        for (int i = 3; i >= 0; i--)
            if (!mbr[446 + i * 16 + 4])
                slot = i;
        if (slot < 0)
            return E_NOSPC;
        uint8_t *e = mbr + 446 + slot * 16;
        static const uint8_t chs[8] = {0x00, 0xfe, 0xff, 0xff, 0x00, 0xfe, 0xff, 0xff};
        memcpy(e, chs, 8);
        e[4] = kind == PART_EFI ? 0xef : kind == PART_SYNTHOS ? 0x7f : kind == PART_LINUX ? 0x83 : 0x07;
        wr32(e + 8, start);
        wr32(e + 12, count);
        if ((err = block_write(dev, 0, 1, mbr)) != 0)
            return err;
        return disk_map_read(dev, map);
    }
    if (map->scheme != SCHEME_GPT)
        return E_INVAL;
    struct gpt g;
    int err = gpt_load(dev, &g);
    if (err)
        return err;
    if (start < g.first || (uint64_t)start + count - 1 > g.last) {
        gpt_free(&g);
        return E_RANGE;
    }
    uint8_t *e = NULL;
    for (uint32_t i = 0; i < g.count && !e; i++)
        if (zero16(g.entries + i * g.size))
            e = g.entries + i * g.size;
    if (!e) {
        gpt_free(&g);
        return E_NOSPC;
    }
    memset(e, 0, g.size);
    const uint8_t *type = kind == PART_EFI ? guid_esp : kind == PART_SYNTHOS ? guid_synth
                                                    : kind == PART_LINUX     ? guid_linux
                                                                             : guid_basic;
    memcpy(e, type, 16);
    random_bytes(e + 16, 16);
    e[16 + 7] = (uint8_t)((e[16 + 7] & 0x0f) | 0x40);
    e[16 + 8] = (uint8_t)((e[16 + 8] & 0x3f) | 0x80);
    wr64(e + 32, start);
    wr64(e + 40, (uint64_t)start + count - 1);
    for (int k = 0; name && name[k] && k < 35; k++)
        e[56 + 2 * k] = (uint8_t)name[k];
    err = gpt_store(dev, &g);
    gpt_free(&g);
    return err ? err : disk_map_read(dev, map);
}

int disk_map_remove(struct disk_map *map, int slot)
{
    struct blockdev *dev = map->dev;
    if (slot < 0 || map->read_only)
        return E_INVAL;
    if (map->scheme == SCHEME_MBR) {
        static uint8_t mbr[BLOCK_SIZE];
        int err = block_read(dev, 0, 1, mbr);
        if (err)
            return err;
        if (slot > 3)
            return E_INVAL;
        memset(mbr + 446 + slot * 16, 0, 16);
        if ((err = block_write(dev, 0, 1, mbr)) != 0)
            return err;
        return disk_map_read(dev, map);
    }
    if (map->scheme != SCHEME_GPT)
        return E_INVAL;
    struct gpt g;
    int err = gpt_load(dev, &g);
    if (err)
        return err;
    if ((uint32_t)slot >= g.count) {
        gpt_free(&g);
        return E_INVAL;
    }
    memset(g.entries + slot * g.size, 0, g.size);
    err = gpt_store(dev, &g);
    gpt_free(&g);
    return err ? err : disk_map_read(dev, map);
}

int disk_map_init(struct blockdev *dev, enum part_scheme scheme)
{
    static uint8_t mbr[BLOCK_SIZE];
    memset(mbr, 0, sizeof(mbr));
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    if (scheme != SCHEME_GPT)
        return block_write(dev, 0, 1, mbr);
    if (dev->sectors < 4096)
        return E_RANGE;
    uint8_t *e = mbr + 446;
    static const uint8_t chs[8] = {0x00, 0x00, 0x02, 0x00, 0xee, 0xff, 0xff, 0xff};
    memcpy(e, chs, 8);
    wr32(e + 8, 1);
    wr32(e + 12, dev->sectors - 1);
    struct gpt g = {.count = 128, .size = 128, .sectors = 32, .entries_lba = 2};
    g.alternate = dev->sectors - 1;
    g.first = 34;
    g.last = dev->sectors - 34;
    if (!(g.entries = kzalloc(32 * BLOCK_SIZE)))
        return E_NOMEM;
    uint8_t *h = g.header;
    memcpy(h, "EFI PART", 8);
    wr32(h + 8, 0x00010000);
    wr32(h + 12, 92);
    wr64(h + 24, 1);
    wr64(h + 32, g.alternate);
    wr64(h + 40, g.first);
    wr64(h + 48, g.last);
    random_bytes(h + 56, 16);
    h[56 + 7] = (uint8_t)((h[56 + 7] & 0x0f) | 0x40);
    h[56 + 8] = (uint8_t)((h[56 + 8] & 0x3f) | 0x80);
    wr64(h + 72, 2);
    wr32(h + 80, 128);
    wr32(h + 84, 128);
    int err = gpt_store(dev, &g);
    gpt_free(&g);
    return err ? err : block_write(dev, 0, 1, mbr);
}
