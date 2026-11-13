/* Photos: a viewer and a library. The library indexes the pictures in Pictures (and its folders), Desktop and
 * Downloads, and reads each one once: its size, its main colours, how bright it is and the faces in it. That
 * index is what search, People, Favorites and albums work from. Everything stays in the user's Pictures
 * folder: .library (the index), .albums ("album|path") and .people ("name|path|face"). */
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/command.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

#define MAX_SIDE 2400
#define MAX_ITEMS 512
#define MAX_ENTRIES 512
#define MAX_FACES 4
#define MAX_MARKS 512
#define SIDE 210
#define ROW 28
#define CELL 150
#define GAP 8
#define PAD 18
#define SMALL 128     /* pictures are read at this size to find colours and faces */
#define SAME_FACE 420 /* the largest descriptor distance that still counts as the same person */
#define RECENTS 48
#define FAVORITES "Favorites"

/* ---- the library ---- */

enum colour {
    C_RED, C_ORANGE, C_YELLOW, C_GREEN, C_CYAN, C_BLUE, C_PURPLE, C_PINK, C_BROWN, C_BLACK, C_WHITE, C_GRAY, C_COUNT
};
static const char *const colour_names[C_COUNT] = {"red",    "orange", "yellow", "green", "cyan",  "blue",
                                                  "purple", "pink",   "brown",  "black", "white", "gray"};

struct face {
    uint8_t x, y, w, h; /* in 1/255 of the picture's width and height */
    uint8_t d[16];      /* the face's light and shade on a 4 x 4 grid, normalised */
};

struct entry {
    char path[PATH_MAX_LEN];
    uint32_t mtime, size;
    uint16_t w, h;
    uint16_t colours;
    uint8_t bright, nfaces;
    bool done;
    struct face faces[MAX_FACES];
    int8_t who[MAX_FACES]; /* the person each face belongs to, -1 for nobody yet */
};

/* an album membership or a named face */
struct mark {
    char name[48];
    char path[PATH_MAX_LEN];
    int face;
};

static struct {
    char home[PATH_MAX_LEN];
    struct entry *e;
    int n;
    struct mark *albums, *people;
    int nalbums, npeople;
    char album_names[32][48];
    int nalbum_names;
    char person_names[32][48];
    int nperson_names;
    bool dirty;
} lib;

static void lib_file(const char *name, char *out, size_t cap)
{
    char dir[PATH_MAX_LEN];
    path_join(user_home(), "Pictures", dir, sizeof(dir));
    path_join(dir, name, out, cap);
}

static struct entry *lib_find(const char *path)
{
    for (int i = 0; i < lib.n; i++)
        if (!strcmp(lib.e[i].path, path))
            return &lib.e[i];
    return NULL;
}

static int hexval(int c)
{
    return is_digit(c) ? c - '0' : (to_lower(c) - 'a' + 10) & 15;
}

static void load_marks(const char *file, struct mark *out, int *n)
{
    char path[PATH_MAX_LEN], *text;
    size_t size;
    *n = 0;
    lib_file(file, path, sizeof(path));
    if (vfs_read_all(path, &text, &size) != 0)
        return;
    for (char *line = text; line < text + size && *n < MAX_MARKS;) {
        char *end = memchr(line, '\n', (size_t)(text + size - line));
        if (end)
            *end = 0;
        char *bar = strchr(line, '|');
        if (bar && bar > line) {
            *bar = 0;
            struct mark *m = &out[*n];
            str_copy(m->name, line, sizeof(m->name));
            char *bar2 = strchr(bar + 1, '|');
            m->face = 0;
            if (bar2) {
                *bar2 = 0;
                m->face = (int)str_to_long(bar2 + 1, NULL, 10);
            }
            str_copy(m->path, bar + 1, sizeof(m->path));
            ++*n;
        }
        if (!end)
            break;
        line = end + 1;
    }
    kfree(text);
}

static void save_marks(const char *file, const struct mark *m, int n, bool faces)
{
    char path[PATH_MAX_LEN];
    lib_file(file, path, sizeof(path));
    char *text = kmalloc((size_t)n * (PATH_MAX_LEN + 64) + 1);
    if (!text)
        return;
    size_t len = 0;
    for (int i = 0; i < n; i++)
        len += faces ? (size_t)ksnprintf(text + len, PATH_MAX_LEN + 64, "%s|%s|%d\n", m[i].name, m[i].path, m[i].face)
                     : (size_t)ksnprintf(text + len, PATH_MAX_LEN + 64, "%s|%s\n", m[i].name, m[i].path);
    char dir[PATH_MAX_LEN];
    path_dirname(path, dir, sizeof(dir));
    vfs_mkdirs(dir);
    vfs_write_all(path, text, len);
    kfree(text);
}

static int face_distance(const struct face *a, const struct face *b)
{
    int d = 0;
    for (int i = 0; i < 16; i++)
        d += ABS(a->d[i] - b->d[i]);
    return d;
}

static int person_index(const char *name)
{
    for (int i = 0; i < lib.nperson_names; i++)
        if (!str_casecmp(lib.person_names[i], name))
            return i;
    return -1;
}

/* Who is in each face: a face the user named is that person; any other face is the person whose named face
 * looks most like it, if one looks close enough. */
static void lib_match(void)
{
    lib.nperson_names = 0;
    for (int i = 0; i < lib.npeople; i++)
        if (person_index(lib.people[i].name) < 0 && lib.nperson_names < 32)
            str_copy(lib.person_names[lib.nperson_names++], lib.people[i].name, 48);
    lib.nalbum_names = 0;
    for (int i = 0; i < lib.nalbums; i++) {
        bool seen = false;
        for (int j = 0; j < lib.nalbum_names; j++)
            seen |= !strcmp(lib.album_names[j], lib.albums[i].name);
        if (!seen && strcmp(lib.albums[i].name, FAVORITES) && lib.nalbum_names < 32)
            str_copy(lib.album_names[lib.nalbum_names++], lib.albums[i].name, 48);
    }
    for (int i = 0; i < lib.n; i++)
        for (int f = 0; f < MAX_FACES; f++)
            lib.e[i].who[f] = -1;
    for (int i = 0; i < lib.npeople; i++) {
        struct entry *e = lib_find(lib.people[i].path);
        if (e && lib.people[i].face >= 0 && lib.people[i].face < e->nfaces)
            e->who[lib.people[i].face] = (int8_t)person_index(lib.people[i].name);
    }
    for (int i = 0; i < lib.n; i++) {
        struct entry *e = &lib.e[i];
        for (int f = 0; f < e->nfaces; f++) {
            if (e->who[f] >= 0)
                continue;
            int best = SAME_FACE + 1;
            for (int k = 0; k < lib.npeople; k++) {
                struct entry *o = lib_find(lib.people[k].path);
                int of = lib.people[k].face;
                if (!o || o == e || of < 0 || of >= o->nfaces)
                    continue;
                int d = face_distance(&e->faces[f], &o->faces[of]);
                if (d < best) {
                    best = d;
                    e->who[f] = (int8_t)person_index(lib.people[k].name);
                }
            }
        }
    }
}

static void lib_save(void)
{
    char path[PATH_MAX_LEN];
    lib_file(".library", path, sizeof(path));
    size_t cap = (size_t)lib.n * (PATH_MAX_LEN + 64 + MAX_FACES * 56) + 1;
    char *text = kmalloc(cap);
    if (!text)
        return;
    size_t len = 0;
    for (int i = 0; i < lib.n; i++) {
        const struct entry *e = &lib.e[i];
        if (!e->done)
            continue;
        len += (size_t)ksnprintf(text + len, cap - len, "%u %u %u %u %u %u %u", e->mtime, e->size, e->w, e->h,
                                 e->colours, e->bright, e->nfaces);
        for (int f = 0; f < e->nfaces; f++) {
            const struct face *fc = &e->faces[f];
            len += (size_t)ksnprintf(text + len, cap - len, " %u,%u,%u,%u,", fc->x, fc->y, fc->w, fc->h);
            for (int k = 0; k < 16; k++)
                len += (size_t)ksnprintf(text + len, cap - len, "%02x", fc->d[k]);
        }
        len += (size_t)ksnprintf(text + len, cap - len, " |%s\n", e->path);
    }
    char dir[PATH_MAX_LEN];
    path_dirname(path, dir, sizeof(dir));
    vfs_mkdirs(dir);
    vfs_write_all(path, text, len);
    kfree(text);
    lib.dirty = false;
}

static void parse_index(char *text, size_t size, struct entry *out, int *n)
{
    for (char *line = text; line < text + size && *n < MAX_ENTRIES;) {
        char *end = memchr(line, '\n', (size_t)(text + size - line));
        if (end)
            *end = 0;
        char *bar = strchr(line, '|');
        if (bar) {
            struct entry *e = &out[*n];
            memset(e, 0, sizeof(*e));
            const char *p = line;
            long v[7];
            for (int k = 0; k < 7; k++)
                v[k] = str_to_long(p, &p, 10);
            e->mtime = (uint32_t)v[0];
            e->size = (uint32_t)v[1];
            e->w = (uint16_t)v[2];
            e->h = (uint16_t)v[3];
            e->colours = (uint16_t)v[4];
            e->bright = (uint8_t)v[5];
            e->nfaces = (uint8_t)MIN(v[6], MAX_FACES);
            for (int f = 0; f < e->nfaces; f++) {
                struct face *fc = &e->faces[f];
                fc->x = (uint8_t)str_to_long(p, &p, 10);
                fc->y = (uint8_t)str_to_long(p + 1, &p, 10);
                fc->w = (uint8_t)str_to_long(p + 1, &p, 10);
                fc->h = (uint8_t)str_to_long(p + 1, &p, 10);
                p++;
                for (int k = 0; k < 16 && p[0] && p[1]; k++, p += 2)
                    fc->d[k] = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
            }
            str_copy(e->path, bar + 1, sizeof(e->path));
            e->done = true;
            ++*n;
        }
        if (!end)
            break;
        line = end + 1;
    }
}

struct scan {
    struct entry *old;
    int nold;
};

static void scan_add(struct scan *s, const char *path, const struct vstat *st)
{
    if (lib.n >= MAX_ENTRIES)
        return;
    struct entry *e = &lib.e[lib.n];
    for (int i = 0; i < s->nold; i++)
        if (s->old[i].mtime == st->mtime && s->old[i].size == st->size && !strcmp(s->old[i].path, path)) {
            *e = s->old[i];
            lib.n++;
            return;
        }
    memset(e, 0, sizeof(*e));
    str_copy(e->path, path, sizeof(e->path));
    e->mtime = st->mtime;
    e->size = st->size;
    lib.n++;
}

static void scan_dir_into(struct scan *s, const char *dir, int depth)
{
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (lib.n < MAX_ENTRIES && vfs_readdir(&d, &ent) == 0) {
        if (ent.name[0] == '.')
            continue;
        char path[PATH_MAX_LEN];
        path_join(dir, ent.name, path, sizeof(path));
        struct vstat st;
        if (ent.type == VN_DIR) {
            if (depth > 0)
                scan_dir_into(s, path, depth - 1);
        } else if (picture_is_image(ent.name) && vfs_stat(path, &st) == 0) {
            scan_add(s, path, &st);
        }
    }
    vfs_closedir(&d);
}

/* Reads the index and brings it up to date with the folders: pictures that are new or changed are left to
 * be read again (done = false), ones that are gone drop out. Newest first. */
static bool lib_scan(void)
{
    if (!lib.e) {
        lib.e = kzalloc(sizeof(struct entry) * MAX_ENTRIES);
        lib.albums = kzalloc(sizeof(struct mark) * MAX_MARKS);
        lib.people = kzalloc(sizeof(struct mark) * MAX_MARKS);
        if (!lib.e || !lib.albums || !lib.people) {
            kfree(lib.e);
            kfree(lib.albums);
            kfree(lib.people);
            lib.e = NULL;
            return false;
        }
    }
    struct scan s = {kmalloc(sizeof(struct entry) * MAX_ENTRIES), 0};
    if (!s.old)
        return false;
    if (strcmp(lib.home, user_home())) {
        /* another user: start from their index */
        str_copy(lib.home, user_home(), sizeof(lib.home));
        char path[PATH_MAX_LEN], *text;
        size_t size;
        lib_file(".library", path, sizeof(path));
        if (vfs_read_all(path, &text, &size) == 0) {
            parse_index(text, size, s.old, &s.nold);
            kfree(text);
        }
    } else {
        memcpy(s.old, lib.e, sizeof(struct entry) * (size_t)lib.n);
        s.nold = lib.n;
    }
    lib.n = 0;
    char dir[PATH_MAX_LEN];
    path_join(user_home(), "Pictures", dir, sizeof(dir));
    scan_dir_into(&s, dir, 3);
    path_join(user_home(), "Desktop", dir, sizeof(dir));
    scan_dir_into(&s, dir, 0);
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    scan_dir_into(&s, dir, 0);
    kfree(s.old);
    for (int i = 1; i < lib.n; i++) {
        struct entry t = lib.e[i];
        int j = i;
        while (j > 0 && (lib.e[j - 1].mtime < t.mtime ||
                         (lib.e[j - 1].mtime == t.mtime && str_casecmp(lib.e[j - 1].path, t.path) > 0))) {
            lib.e[j] = lib.e[j - 1];
            j--;
        }
        lib.e[j] = t;
    }
    load_marks(".albums", lib.albums, &lib.nalbums);
    load_marks(".people", lib.people, &lib.npeople);
    lib_match();
    return true;
}

/* ---- reading a picture: colours, brightness, faces ---- */

static int luma(uint32_t p)
{
    return (int)((77 * (p >> 16 & 255) + 150 * (p >> 8 & 255) + 29 * (p & 255)) >> 8);
}

static int colour_of(uint32_t p)
{
    int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
    int mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn;
    if (mx < 48)
        return C_BLACK;
    if (d * 100 < mx * 18 || d < 20)
        return mx > 200 ? C_WHITE : mx < 70 ? C_BLACK : C_GRAY;
    int h; /* hue in degrees */
    if (mx == r)
        h = (60 * (g - b) / d + 360) % 360;
    else if (mx == g)
        h = 60 * (b - r) / d + 120;
    else
        h = 60 * (r - g) / d + 240;
    if (h < 15 || h >= 345)
        return C_RED;
    if (h < 45)
        return mx < 150 ? C_BROWN : C_ORANGE;
    if (h < 70)
        return mx < 110 ? C_BROWN : C_YELLOW;
    if (h < 165)
        return C_GREEN;
    if (h < 200)
        return C_CYAN;
    if (h < 260)
        return C_BLUE;
    if (h < 300)
        return C_PURPLE;
    return C_PINK;
}

static bool is_skin(uint32_t p)
{
    int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
    int y = luma(p);
    int cb = 128 + ((-43 * r - 85 * g + 128 * b) >> 8), cr = 128 + ((128 * r - 107 * g - 21 * b) >> 8);
    return y > 40 && cb >= 77 && cb <= 127 && cr >= 133 && cr <= 173;
}

/* Skin-coloured blobs that are face-shaped (a bit taller than wide, mostly filled) and have something
 * darker on both sides of their upper half, where eyes would be. */
static int find_faces(const uint32_t *px, int w, int h, struct face *out)
{
    int n = w * h, found = 0;
    int16_t *label = kmalloc((size_t)n * sizeof(int16_t));
    int *queue = kmalloc((size_t)n * sizeof(int));
    if (!label || !queue) {
        kfree(label);
        kfree(queue);
        return 0;
    }
    for (int i = 0; i < n; i++)
        label[i] = is_skin(px[i]) ? 0 : -1;
    struct cand {
        int x0, y0, x1, y1, area;
    } cands[MAX_FACES];
    int ncand = 0;
    int16_t next = 1;
    for (int start = 0; start < n && next < 30000; start++) {
        if (label[start] != 0)
            continue;
        int head = 0, tail = 0, area = 0;
        int x0 = w, y0 = h, x1 = 0, y1 = 0;
        label[start] = next;
        queue[tail++] = start;
        while (head < tail) {
            int i = queue[head++], x = i % w, y = i / w;
            area++;
            x0 = MIN(x0, x);
            x1 = MAX(x1, x);
            y0 = MIN(y0, y);
            y1 = MAX(y1, y);
            int nb[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
            for (int k = 0; k < 4; k++)
                if (nb[k] >= 0 && label[nb[k]] == 0) {
                    label[nb[k]] = next;
                    queue[tail++] = nb[k];
                }
        }
        next++;
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        if (area * 1000 < n * 15 || bw < 8 || bh * 10 < bw * 9 || bh * 10 > bw * 22 || area * 100 < bw * bh * 45)
            continue;
        /* eyes: darker pixels in the upper part, on both halves */
        int sum = 0, cnt = 0;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                if (label[y * w + x] == next - 1) {
                    sum += luma(px[y * w + x]);
                    cnt++;
                }
        int avg = sum / MAX(cnt, 1), left = 0, right = 0;
        for (int y = y0 + bh / 6; y <= y0 + bh * 55 / 100; y++)
            for (int x = x0 + bw / 10; x <= x1 - bw / 10; x++)
                if (luma(px[y * w + x]) < avg - 50) {
                    if (x < x0 + bw / 2)
                        left++;
                    else
                        right++;
                }
        if (left < 2 || right < 2)
            continue;
        struct cand c = {x0, y0, x1, y1, area};
        int at = ncand < MAX_FACES ? ncand++ : MAX_FACES;
        if (at == MAX_FACES) {
            if (cands[MAX_FACES - 1].area >= area)
                continue;
            at = MAX_FACES - 1;
        }
        while (at > 0 && cands[at - 1].area < area) {
            cands[at] = cands[at - 1];
            at--;
        }
        cands[at] = c;
    }
    /* left to right reads more naturally when naming */
    for (int i = 1; i < ncand; i++)
        for (int j = i; j > 0 && cands[j - 1].x0 > cands[j].x0; j--) {
            struct cand t = cands[j];
            cands[j] = cands[j - 1];
            cands[j - 1] = t;
        }
    for (int i = 0; i < ncand; i++) {
        struct cand *c = &cands[i];
        struct face *f = &out[found++];
        int bw = c->x1 - c->x0 + 1, bh = c->y1 - c->y0 + 1;
        f->x = (uint8_t)(c->x0 * 255 / w);
        f->y = (uint8_t)(c->y0 * 255 / h);
        f->w = (uint8_t)MIN(bw * 255 / w, 255);
        f->h = (uint8_t)MIN(bh * 255 / h, 255);
        int cell[16], lo = 255, hi = 0;
        for (int gy = 0; gy < 4; gy++)
            for (int gx = 0; gx < 4; gx++) {
                int s = 0, k = 0;
                for (int y = c->y0 + gy * bh / 4; y < c->y0 + (gy + 1) * bh / 4; y++)
                    for (int x = c->x0 + gx * bw / 4; x < c->x0 + (gx + 1) * bw / 4; x++, k++)
                        s += luma(px[y * w + x]);
                cell[gy * 4 + gx] = s / MAX(k, 1);
                lo = MIN(lo, cell[gy * 4 + gx]);
                hi = MAX(hi, cell[gy * 4 + gx]);
            }
        for (int k = 0; k < 16; k++)
            f->d[k] = (uint8_t)((cell[k] - lo) * 255 / MAX(hi - lo, 1));
    }
    kfree(label);
    kfree(queue);
    return found;
}

static void analyse(struct entry *e, const struct picture *pic)
{
    e->w = (uint16_t)MIN(pic->w, 65535);
    e->h = (uint16_t)MIN(pic->h, 65535);
    int side = MAX(pic->w, pic->h), w = MAX(pic->w * SMALL / MAX(side, 1), 1), h = MAX(pic->h * SMALL / MAX(side, 1), 1);
    if (side <= SMALL) {
        w = pic->w;
        h = pic->h;
    }
    uint32_t *px = kmalloc((size_t)w * (size_t)h * 4);
    if (!px)
        return;
    struct canvas dst = canvas_make(px, w, h), src = canvas_make(pic->px, pic->w, pic->h);
    gfx_clear(&dst, (struct rect){0, 0, w, h}, 0xff000000u);
    gfx_scale(&dst, (struct rect){0, 0, w, h}, &src, 255);
    int count[C_COUNT] = {0};
    long light = 0;
    for (int i = 0; i < w * h; i++) {
        count[colour_of(px[i])]++;
        light += luma(px[i]);
    }
    e->colours = 0;
    for (int c = 0; c < C_COUNT; c++)
        if (count[c] * 100 >= w * h * 12)
            e->colours |= (uint16_t)(1u << c);
    e->bright = (uint8_t)(light / (w * h));
    e->nfaces = (uint8_t)find_faces(px, w, h, e->faces);
    kfree(px);
    e->done = true;
    lib.dirty = true;
}

/* the real size of a picture whose file may have been read reduced */
static void real_size(struct entry *e, const struct picture *pic)
{
    if (!str_iends(e->path, ".jpg") && !str_iends(e->path, ".jpeg"))
        return;
    char *data;
    size_t size;
    if (vfs_read_all(e->path, &data, &size) != 0)
        return;
    const uint8_t *d = (const uint8_t *)data;
    for (size_t i = 2; i + 9 < size;) {
        if (d[i] != 0xff) {
            i++;
            continue;
        }
        uint8_t m = d[i + 1];
        if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            e->h = (uint16_t)(d[i + 5] << 8 | d[i + 6]);
            e->w = (uint16_t)(d[i + 7] << 8 | d[i + 8]);
            break;
        }
        i += 2 + (size_t)(d[i + 2] << 8 | d[i + 3]);
    }
    kfree(data);
    (void)pic;
}

static bool analyse_path(struct entry *e)
{
    struct picture pic;
    if (picture_load_fit(e->path, SMALL * 2, SMALL * 2, &pic) != 0) {
        e->done = true; /* unreadable: do not try again until it changes */
        lib.dirty = true;
        return false;
    }
    analyse(e, &pic);
    real_size(e, &pic);
    picture_free(&pic);
    return true;
}

/* ---- albums and people ---- */

static bool in_album(const char *album, const char *path)
{
    for (int i = 0; i < lib.nalbums; i++)
        if (!strcmp(lib.albums[i].name, album) && !strcmp(lib.albums[i].path, path))
            return true;
    return false;
}

static void album_add(const char *album, const char *path)
{
    if (!album[0] || in_album(album, path) || lib.nalbums >= MAX_MARKS)
        return;
    struct mark *m = &lib.albums[lib.nalbums++];
    str_copy(m->name, album, sizeof(m->name));
    str_copy(m->path, path, sizeof(m->path));
    m->face = 0;
    save_marks(".albums", lib.albums, lib.nalbums, false);
    lib_match();
}

static void album_remove(const char *album, const char *path)
{
    for (int i = 0; i < lib.nalbums; i++)
        if (!strcmp(lib.albums[i].name, album) && (!path || !strcmp(lib.albums[i].path, path))) {
            memmove(&lib.albums[i], &lib.albums[i + 1], sizeof(struct mark) * (size_t)(lib.nalbums - i - 1));
            lib.nalbums--;
            i--;
        }
    save_marks(".albums", lib.albums, lib.nalbums, false);
    lib_match();
}

static void name_face(const char *path, int face, const char *name)
{
    for (int i = 0; i < lib.npeople; i++)
        if (!strcmp(lib.people[i].path, path) && lib.people[i].face == face) {
            memmove(&lib.people[i], &lib.people[i + 1], sizeof(struct mark) * (size_t)(lib.npeople - i - 1));
            lib.npeople--;
            i--;
        }
    if (name && name[0] && lib.npeople < MAX_MARKS) {
        struct mark *m = &lib.people[lib.npeople++];
        str_copy(m->name, name, sizeof(m->name));
        str_copy(m->path, path, sizeof(m->path));
        m->face = face;
    }
    save_marks(".people", lib.people, lib.npeople, true);
    lib_match();
}

static bool has_person(const struct entry *e, int person)
{
    for (int f = 0; f < e->nfaces; f++)
        if (e->who[f] == person)
            return true;
    return false;
}

static bool has_unnamed(const struct entry *e)
{
    return has_person(e, -1);
}

/* ---- search ---- */

static const char *const months[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec"};

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!str_ncasecmp(hay, needle, n))
            return true;
    return false;
}

static bool word_matches(const struct entry *e, const char *w)
{
    if (contains_ci(path_basename(e->path), w))
        return true;
    for (int c = 0; c < C_COUNT; c++)
        if (!str_casecmp(w, colour_names[c]) || (c == C_GRAY && !str_casecmp(w, "grey")))
            return e->colours & (1u << c);
    struct rtc_time t;
    time_to_rtc(e->mtime, &t);
    long year;
    if (strlen(w) == 4 && str_parse_long(w, &year) && year > 1900)
        return t.year == year;
    if (strlen(w) >= 3)
        for (int m = 0; m < 12; m++)
            if (!str_ncasecmp(w, months[m], 3))
                return t.month == m + 1;
    if (!str_casecmp(w, "dark") || !str_casecmp(w, "night"))
        return e->done && e->bright < 80;
    if (!str_casecmp(w, "bright") || !str_casecmp(w, "light"))
        return e->bright > 170;
    if (!str_casecmp(w, "portrait") || !str_casecmp(w, "tall"))
        return e->h * 10 > e->w * 11;
    if (!str_casecmp(w, "landscape") || !str_casecmp(w, "wide"))
        return e->w * 10 > e->h * 11;
    if (!str_casecmp(w, "square"))
        return e->w && e->w * 10 <= e->h * 11 && e->h * 10 <= e->w * 11;
    if (!str_casecmp(w, "people") || !str_casecmp(w, "faces") || !str_casecmp(w, "face") || !str_casecmp(w, "person"))
        return e->nfaces > 0;
    if (!str_casecmp(w, "screenshot") || !str_casecmp(w, "screenshots"))
        return contains_ci(path_basename(e->path), "screen");
    if (!str_casecmp(w, "favorite") || !str_casecmp(w, "favorites") || !str_casecmp(w, "favourite"))
        return in_album(FAVORITES, e->path);
    for (int i = 0; i < lib.nalbums; i++)
        if (contains_ci(lib.albums[i].name, w) && !strcmp(lib.albums[i].path, e->path))
            return true;
    for (int f = 0; f < e->nfaces; f++)
        if (e->who[f] >= 0 && contains_ci(lib.person_names[e->who[f]], w))
            return true;
    return false;
}

static bool query_matches(const struct entry *e, const char *query)
{
    char word[64];
    const char *p = query;
    bool any = false;
    while (*p) {
        while (*p == ' ')
            p++;
        int n = 0;
        while (*p && *p != ' ') {
            if (n < 63)
                word[n++] = *p;
            p++;
        }
        word[n] = 0;
        if (!n)
            continue;
        any = true;
        if (!word_matches(e, word))
            return false;
    }
    return any;
}

/* ---- the window ---- */

enum {
    CMD_OPEN,
    CMD_PREV,
    CMD_NEXT,
    CMD_ZOOM_IN,
    CMD_ZOOM_OUT,
    CMD_FIT,
    CMD_ACTUAL,
    CMD_ROTATE,
    CMD_WALLPAPER,
    CMD_EDIT,
    CMD_SHOW,
    CMD_SLIDESHOW,
    CMD_LIBRARY,
    CMD_FAVORITE,
    CMD_ALBUM,
    CMD_NAME,
    CMD_REFRESH,
    CMD_NEW_ALBUM,
    CMD_LAST = CMD_NEW_ALBUM,
};

static const struct menu_item file_items[] = {
    {"Open…", KEY_COMMAND(CMD_OPEN), "⌘O"},
    {"Show in Files", KEY_COMMAND(CMD_SHOW), NULL},
    {"Edit in Paint", KEY_COMMAND(CMD_EDIT), NULL},
    {"Set as Wallpaper", KEY_COMMAND(CMD_WALLPAPER), NULL},
    {"", 0, NULL},
    {"Favorite", KEY_COMMAND(CMD_FAVORITE), "F"},
    {"Add to Album…", KEY_COMMAND(CMD_ALBUM), NULL},
    {"Name People…", KEY_COMMAND(CMD_NAME), NULL},
    {"", 0, NULL},
    {"New Album…", KEY_COMMAND(CMD_NEW_ALBUM), NULL},
    {"Refresh Library", KEY_COMMAND(CMD_REFRESH), NULL},
};

static const struct menu_item view_items[] = {
    {"Library", KEY_COMMAND(CMD_LIBRARY), "Esc"},    {"", 0, NULL},
    {"Previous", KEY_COMMAND(CMD_PREV), "←"},       {"Next", KEY_COMMAND(CMD_NEXT), "→"},
    {"", 0, NULL},                                   {"Zoom In", KEY_COMMAND(CMD_ZOOM_IN), "+"},
    {"Zoom Out", KEY_COMMAND(CMD_ZOOM_OUT), "-"},    {"Fit to Window", KEY_COMMAND(CMD_FIT), "0"},
    {"Actual Size", KEY_COMMAND(CMD_ACTUAL), "1"},   {"Rotate", KEY_COMMAND(CMD_ROTATE), "R"},
    {"", 0, NULL},                                   {"Slideshow", KEY_COMMAND(CMD_SLIDESHOW), "Space"},
};

static const struct app_menu menus[] = {
    {"Picture", file_items, ARRAY_SIZE(file_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
};

enum { COL_ALL, COL_FAVORITES, COL_RECENTS, COL_PERSON, COL_UNNAMED, COL_ALBUM, COL_FOLDER, COL_SEARCH };

struct photos {
    char path[PATH_MAX_LEN];
    char dir[PATH_MAX_LEN];
    int col, col_arg; /* the collection shown, and which person or album */
    char (*items)[PATH_MAX_LEN];
    struct canvas *thumbs;
    bool *thumb_tried;
    int count, index, selected;
    bool grid; /* the library grid, not one picture */
    int grid_scroll, side_scroll;
    struct ui_text query;
    char shown_query[256];
    struct picture pic;
    int frame;
    uint64_t frame_due;
    struct canvas fitted;
    int fitted_w, fitted_h;
    struct spring zoom, slide, fade, bar;
    uint64_t last_move;
    bool fit;
    int pan_x, pan_y;
    bool panning;
    int drag_x, drag_y;
    uint64_t tick;
    bool slideshow;
    uint64_t slide_due;
    char error[96];
    struct ui_state st;
    int list_scroll;
    char target[PATH_MAX_LEN]; /* the picture a menu or prompt is about */
    int naming;                /* the face being named, -1 for none */
};

static const struct {
    enum symbol_id sym;
    const char *label, *dir;
} places[] = {
    {SYM_PHOTO, "Pictures", "Pictures"},
    {SYM_DESKTOP, "Desktop", "Desktop"},
    {SYM_DOWNLOAD, "Downloads", "Downloads"},
    {SYM_DISPLAY, "Wallpapers", "/system/wallpapers"},
};

static void place_dir(int i, char *out, size_t size)
{
    if (places[i].dir[0] == '/')
        str_copy(out, places[i].dir, size);
    else
        path_join(user_home(), places[i].dir, out, size);
}

static int side_width(int W)
{
    return W >= 560 ? SIDE : 0;
}

static struct rect view_rect(int W, int H)
{
    int x = side_width(W);
    return (struct rect){x, TOOLBAR_HEIGHT, W - x, MAX(H - TOOLBAR_HEIGHT, 1)};
}

static struct canvas sub_canvas(struct canvas *c, struct rect r)
{
    r = rect_intersect(r, (struct rect){0, 0, c->w, c->h});
    struct canvas s = {c->px + (size_t)r.y * (size_t)c->stride + (size_t)r.x, r.w, r.h, c->stride, {0, 0, 0, 0}};
    struct rect cl = rect_intersect(c->clip, r);
    if (!rect_empty(cl))
        s.clip = (struct rect){cl.x - r.x, cl.y - r.y, cl.w, cl.h};
    return s;
}

static void shrink(struct picture *p)
{
    int side = MAX(p->w, p->h);
    if (side <= MAX_SIDE || p->frames != 1)
        return;
    int w = (int)((int64_t)p->w * MAX_SIDE / side), h = (int)((int64_t)p->h * MAX_SIDE / side);
    uint32_t *px = kmalloc((size_t)w * (size_t)h * 4);
    if (!px)
        return;
    struct canvas dst = canvas_make(px, w, h), src = canvas_make(p->px, p->w, p->h);
    gfx_clear(&dst, (struct rect){0, 0, w, h}, 0);
    gfx_scale(&dst, (struct rect){0, 0, w, h}, &src, 255);
    kfree(p->px);
    p->px = px;
    p->w = w;
    p->h = h;
}

static void rotate(struct photos *ph)
{
    struct picture *p = &ph->pic;
    size_t n = (size_t)p->w * (size_t)p->h;
    uint32_t *px = kmalloc(n * 4 * (size_t)p->frames);
    if (!px)
        return;
    for (int f = 0; f < p->frames; f++)
        for (int y = 0; y < p->h; y++)
            for (int x = 0; x < p->w; x++)
                px[f * n + (size_t)x * (size_t)p->h + (size_t)(p->h - 1 - y)] = p->px[f * n + (size_t)y * (size_t)p->w + (size_t)x];
    kfree(p->px);
    p->px = px;
    int t = p->w;
    p->w = p->h;
    p->h = t;
    canvas_free(&ph->fitted);
}

static void clear_thumbs(struct photos *ph)
{
    for (int i = 0; i < MAX_ITEMS; i++) {
        canvas_free(&ph->thumbs[i]);
        ph->thumb_tried[i] = false;
    }
}

static void add_item(struct photos *ph, const char *path)
{
    if (ph->count < MAX_ITEMS)
        str_copy(ph->items[ph->count++], path, PATH_MAX_LEN);
}

static void collect_dir(struct photos *ph)
{
    struct vdir d;
    if (vfs_opendir(ph->dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (ph->count < MAX_ITEMS && vfs_readdir(&d, &ent) == 0)
        if (ent.type != VN_DIR && ent.name[0] != '.' && picture_is_image(ent.name)) {
            char path[PATH_MAX_LEN];
            path_join(ph->dir, ent.name, path, sizeof(path));
            int i = ph->count++;
            while (i > 0 && str_casecmp(path_basename(ph->items[i - 1]), ent.name) > 0) {
                str_copy(ph->items[i], ph->items[i - 1], PATH_MAX_LEN);
                i--;
            }
            str_copy(ph->items[i], path, PATH_MAX_LEN);
        }
    vfs_closedir(&d);
}

/* fills the item list from the collection the sidebar (or the search field) chose */
static void collect(struct photos *ph)
{
    char keep[PATH_MAX_LEN];
    str_copy(keep, ph->count && ph->selected < ph->count ? ph->items[ph->selected] : "", sizeof(keep));
    clear_thumbs(ph);
    ph->count = 0;
    ph->grid_scroll = 0;
    ph->list_scroll = 0;
    if (ph->col == COL_FOLDER) {
        collect_dir(ph);
    } else if (ph->col == COL_FAVORITES || ph->col == COL_ALBUM) {
        const char *album = ph->col == COL_FAVORITES ? FAVORITES : lib.album_names[ph->col_arg];
        for (int i = 0; i < lib.nalbums; i++)
            if (!strcmp(lib.albums[i].name, album) && vfs_exists(lib.albums[i].path))
                add_item(ph, lib.albums[i].path);
    } else {
        for (int i = 0; i < lib.n; i++) {
            struct entry *e = &lib.e[i];
            bool in = ph->col == COL_ALL || (ph->col == COL_RECENTS && i < RECENTS) ||
                      (ph->col == COL_PERSON && has_person(e, ph->col_arg)) ||
                      (ph->col == COL_UNNAMED && has_unnamed(e)) ||
                      (ph->col == COL_SEARCH && query_matches(e, ph->query.buf));
            if (in)
                add_item(ph, e->path);
        }
    }
    ph->selected = 0;
    for (int i = 0; i < ph->count; i++)
        if (!strcmp(ph->items[i], keep))
            ph->selected = i;
}

static const char *collection_title(struct photos *ph)
{
    switch (ph->col) {
    case COL_ALL: return "All Photos";
    case COL_FAVORITES: return FAVORITES;
    case COL_RECENTS: return "Recents";
    case COL_PERSON: return lib.person_names[ph->col_arg];
    case COL_UNNAMED: return "Unnamed Faces";
    case COL_ALBUM: return lib.album_names[ph->col_arg];
    case COL_SEARCH: return "Search";
    default: return path_basename(ph->dir)[0] ? path_basename(ph->dir) : ph->dir;
    }
}

static void show(struct window *w, struct photos *ph, const char *path, int direction)
{
    picture_free(&ph->pic);
    canvas_free(&ph->fitted);
    ph->error[0] = 0;
    ph->grid = false;
    ph->naming = -1;
    str_copy(ph->path, path, sizeof(ph->path));
    int err = picture_load(path, &ph->pic);
    if (err) {
        ksnprintf(ph->error, sizeof(ph->error), "Cannot open this picture (%s)",
                  err == E_INVAL ? "unknown format" : err == E_NOSYS ? "unsupported kind of JPEG" : err_str(err));
    } else {
        shrink(&ph->pic);
    }
    ph->frame = 0;
    ph->frame_due = hal_ticks_ms() + (ph->pic.delays ? (uint64_t)ph->pic.delays[0] : 0);
    ph->fit = true;
    ph->pan_x = ph->pan_y = 0;
    spring_set(&ph->zoom, 1024);
    spring_set(&ph->slide, direction * 160);
    ph->slide.target = 0;
    spring_set(&ph->fade, direction ? 200 : 0);
    ph->fade.target = 1024;
    char title[96];
    str_copy(title, path_basename(path), sizeof(title));
    wm_set_title(w, title);
    wm_invalidate(w);
}

static void open_index(struct window *w, struct photos *ph, int i, int direction)
{
    if (!ph->count)
        return;
    ph->index = ph->selected = (i + ph->count) % ph->count;
    show(w, ph, ph->items[ph->index], direction);
}

static void to_grid(struct window *w, struct photos *ph)
{
    ph->grid = true;
    ph->slideshow = false;
    ph->naming = -1;
    wm_set_title(w, "Photos");
    wm_invalidate(w);
}

static void choose(struct window *w, struct photos *ph, int col, int arg)
{
    ph->col = col;
    ph->col_arg = arg;
    if (col != COL_SEARCH)
        ui_text_set(&ph->query, "");
    ph->shown_query[0] = 0;
    if (col != COL_FOLDER && col != COL_SEARCH)
        lib_scan();
    collect(ph);
    to_grid(w, ph);
}

static void open_path(struct window *w, struct photos *ph, const char *path)
{
    if (vfs_is_dir(path)) {
        str_copy(ph->dir, path, sizeof(ph->dir));
        choose(w, ph, COL_FOLDER, 0);
        return;
    }
    path_dirname(path, ph->dir, sizeof(ph->dir));
    ph->col = COL_FOLDER;
    ui_text_set(&ph->query, "");
    collect(ph);
    ph->index = 0;
    for (int i = 0; i < ph->count; i++)
        if (!strcmp(ph->items[i], path))
            ph->index = i;
    show(w, ph, path, 0);
}

static struct rect image_rect(struct photos *ph, int W, int H, int *scale)
{
    int pw = ph->pic.w, ph_ = ph->pic.h;
    int fit = MIN(W * 1024 / MAX(pw, 1), H * 1024 / MAX(ph_, 1));
    fit = MIN(fit, 1024 * 4);
    if (pw <= W && ph_ <= H)
        fit = MIN(fit, 1024);
    int s = (int)((int64_t)fit * ph->zoom.pos / 1024);
    *scale = s;
    int w = (int)((int64_t)pw * s / 1024), h = (int)((int64_t)ph_ * s / 1024);
    return (struct rect){(W - w) / 2 + ph->pan_x + ph->slide.pos * W / 1024 / 4, (H - h) / 2 + ph->pan_y, w, h};
}

static const int bar_cmds[] = {CMD_PREV, CMD_ZOOM_OUT, CMD_ZOOM_IN, CMD_ROTATE, CMD_NEXT};
static const enum symbol_id bar_syms[] = {SYM_LEFT, SYM_MINUS, SYM_PLUS, SYM_RESTART, SYM_RIGHT};
#define BAR_BUTTONS 5
#define BAR_BUTTON 40

static struct rect bar_rect(int W, int H, int show)
{
    int w = BAR_BUTTONS * BAR_BUTTON + 12;
    return (struct rect){(W - w) / 2, H - 16 - 44 + (1024 - show) * 64 / 1024, w, 44};
}

static struct rect bar_button(struct rect b, int i)
{
    return (struct rect){b.x + 6 + i * BAR_BUTTON, b.y + 2, BAR_BUTTON, b.h - 4};
}

static void paint_bar(struct photos *ph, struct canvas *c)
{
    int show = CLAMP(ph->bar.pos, 0, 1024);
    if (show < 16 || !ph->pic.px)
        return;
    struct rect b = bar_rect(c->w, c->h, show);
    int a = show * 255 / 1024;
    gfx_round(c, b, b.h / 2, RGBA(24, 24, 30, 200 * a / 255));
    gfx_round_frame(c, b, b.h / 2, ALPHA(0xffffffffu, 36 * a / 255));
    for (int i = 0; i < BAR_BUTTONS; i++) {
        struct rect r = bar_button(b, i);
        bool off = (bar_cmds[i] == CMD_PREV || bar_cmds[i] == CMD_NEXT) && ph->count < 2;
        gfx_symbol(c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, bar_syms[i], 16,
                   ALPHA(0xffffffffu, (off ? 90 : 235) * a / 255));
    }
}

/* ---- the grid ---- */

static int grid_cols(int W)
{
    return MAX((W - 2 * PAD + GAP) / (CELL + GAP), 1);
}

static struct rect cell_rect(int W, int i, int scroll)
{
    int cols = grid_cols(W), used = cols * (CELL + GAP) - GAP, x0 = (W - used) / 2;
    return (struct rect){x0 + i % cols * (CELL + GAP), PAD + i / cols * (CELL + GAP) - scroll, CELL, CELL};
}

static int grid_height(int W, int count)
{
    int cols = grid_cols(W);
    return PAD * 2 + (count + cols - 1) / cols * (CELL + GAP);
}

static int grid_hit(struct photos *ph, int W, int x, int y)
{
    for (int i = 0; i < ph->count; i++)
        if (rect_contains(cell_rect(W, i, ph->grid_scroll), x, y))
            return i;
    return -1;
}

static void make_thumb(struct photos *ph, int i)
{
    ph->thumb_tried[i] = true;
    struct picture pic;
    struct entry *e = lib_find(ph->items[i]);
    if (picture_load_fit(ph->items[i], CELL, CELL, &pic) != 0) {
        if (e && !e->done) {
            e->done = true;
            lib.dirty = true;
        }
        return;
    }
    if (e && !e->done) {
        analyse(e, &pic);
        real_size(e, &pic);
    }
    int side = MIN(pic.w, pic.h), x0 = (pic.w - side) / 2, y0 = (pic.h - side) / 2;
    struct canvas src = canvas_make(pic.px + (size_t)y0 * (size_t)pic.w + (size_t)x0, side, side);
    src.stride = pic.w;
    if (canvas_alloc(&ph->thumbs[i], CELL, CELL)) {
        gfx_clear(&ph->thumbs[i], (struct rect){0, 0, CELL, CELL}, 0);
        gfx_scale(&ph->thumbs[i], (struct rect){0, 0, CELL, CELL}, &src, 255);
    }
    picture_free(&pic);
}

static void paint_grid(struct photos *ph, struct canvas *c)
{
    int W = c->w, H = c->h;
    gfx_clear(c, (struct rect){0, 0, W, H}, style.content);
    if (!ph->count) {
        const char *msg = ph->col == COL_SEARCH ? "No Results"
                        : ph->col == COL_FAVORITES ? "No Favorites Yet"
                        : ph->col == COL_ALBUM ? "This Album Is Empty"
                        : ph->col == COL_UNNAMED ? "No Unnamed Faces"
                        : "No Pictures Here";
        const char *hint = ph->col == COL_SEARCH ? "Try a name, a colour, a year, a month, a person or an album"
                         : ph->col == COL_FAVORITES || ph->col == COL_ALBUM
                             ? "Right-click a picture to add it"
                             : "Put pictures in Pictures, Desktop or Downloads";
        gfx_symbol(c, W / 2 - 16, H / 2 - 48, ph->col == COL_SEARCH ? SYM_SEARCH : SYM_PHOTO, 32, style.tertiary);
        gfx_text_box(c, (struct rect){0, H / 2 - 8, W, 22}, &font_bold, msg, style.secondary, ALIGN_CENTER);
        gfx_text_box(c, (struct rect){0, H / 2 + 16, W, 18}, &font_small, hint, style.tertiary, ALIGN_CENTER);
        return;
    }
    for (int i = 0; i < ph->count; i++) {
        struct rect r = cell_rect(W, i, ph->grid_scroll);
        if (r.y + r.h < 0)
            continue;
        if (r.y > H)
            break;
        if (ph->thumbs[i].px) {
            gfx_blit_round(c, r, &ph->thumbs[i], 10, true);
        } else {
            gfx_round(c, r, 10, style.zebra | 0xff000000u);
            gfx_symbol(c, r.x + (r.w - 20) / 2, r.y + (r.h - 20) / 2, SYM_PHOTO, 20,
                       ph->thumb_tried[i] ? style.error : style.tertiary);
        }
        if (in_album(FAVORITES, ph->items[i])) {
            gfx_circle(c, r.x + 16, r.y + r.h - 16, 11, RGBA(0, 0, 0, 110));
            gfx_symbol(c, r.x + 9, r.y + r.h - 23, SYM_STAR, 14, 0xffffffffu);
        }
        if (i == ph->selected) {
            gfx_round_frame(c, (struct rect){r.x - 3, r.y - 3, r.w + 6, r.h + 6}, 13, style.accent);
            gfx_round_frame(c, (struct rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 12, style.accent);
        }
    }
}

static void paint_faces(struct photos *ph, struct canvas *c)
{
    struct entry *e = lib_find(ph->path);
    if (!e || ph->naming < 0 || ph->naming >= e->nfaces || spring_busy(&ph->zoom))
        return;
    int scale;
    struct rect r = image_rect(ph, c->w, c->h, &scale);
    const struct face *f = &e->faces[ph->naming];
    struct rect fr = {r.x + f->x * r.w / 255 - 4, r.y + f->y * r.h / 255 - 4, f->w * r.w / 255 + 8,
                      f->h * r.h / 255 + 8};
    gfx_round_frame(c, fr, 8, 0xffffffffu);
    gfx_round_frame(c, (struct rect){fr.x - 1, fr.y - 1, fr.w + 2, fr.h + 2}, 9, style.accent);
}

static void paint_picture(struct window *w, struct canvas *c);
static void frame(struct ui *ui, void *data);

static void paint(struct window *w, struct canvas *c)
{
    struct photos *ph = w->data;
    struct canvas view = sub_canvas(c, view_rect(c->w, c->h));
    if (view.w > 0 && view.h > 0) {
        if (ph->grid) {
            paint_grid(ph, &view);
        } else {
            paint_picture(w, &view);
            paint_faces(ph, &view);
            paint_bar(ph, &view);
        }
    }
    ui_run(w, &ph->st, c, NULL, frame, ph);
}

static void paint_picture(struct window *w, struct canvas *c)
{
    struct photos *ph = w->data;
    int W = c->w, H = c->h;
    gfx_clear(c, (struct rect){0, 0, W, H}, style.dark ? RGB(12, 12, 14) : color_mix(RGB(24, 24, 28), style.tint | 0xff000000u, 20));
    if (!ph->pic.px) {
        const char *msg = ph->error[0] ? ph->error : "No pictures here";
        gfx_symbol(c, W / 2 - 16, H / 2 - 40, SYM_PHOTO, 32, ALPHA(0xffffffffu, 120));
        gfx_text_box(c, (struct rect){0, H / 2, W, 22}, &font_bold, msg, ALPHA(0xffffffffu, 200), ALIGN_CENTER);
        gfx_text_box(c, (struct rect){0, H / 2 + 24, W, 18}, &font_small, "Click to open a PNG, JPEG, GIF or BMP picture",
                     ALPHA(0xffffffffu, 120), ALIGN_CENTER);
        return;
    }
    int scale;
    struct rect r = image_rect(ph, W, H, &scale);
    struct canvas src = canvas_make(ph->pic.px + (size_t)ph->frame * (size_t)ph->pic.w * (size_t)ph->pic.h, ph->pic.w,
                                    ph->pic.h);
    int alpha = CLAMP(ph->fade.pos, 0, 1024) * 255 / 1024;
    bool still = !spring_busy(&ph->zoom) && !spring_busy(&ph->slide) && ph->pic.frames == 1;
    if (still && r.w <= ph->pic.w) {
        if (!ph->fitted.px || ph->fitted_w != r.w || ph->fitted_h != r.h) {
            canvas_free(&ph->fitted);
            if (canvas_alloc(&ph->fitted, MAX(r.w, 1), MAX(r.h, 1))) {
                gfx_clear(&ph->fitted, (struct rect){0, 0, r.w, r.h}, 0);
                gfx_scale(&ph->fitted, (struct rect){0, 0, r.w, r.h}, &src, 255);
                ph->fitted_w = r.w;
                ph->fitted_h = r.h;
            }
        }
        if (ph->fitted.px) {
            gfx_drop_shadow(c, r, 4, 8, 60 * alpha / 255, 2);
            gfx_blit_alpha(c, r.x, r.y, &ph->fitted, (struct rect){0, 0, r.w, r.h}, alpha);
            return;
        }
    }
    gfx_drop_shadow(c, r, 4, 8, 60 * alpha / 255, 2);
    gfx_scale_fast(c, r, &src, alpha);
}

static void command(struct window *w, struct photos *ph, int cmd);

static bool side_item(struct ui *ui, int side, int *y, enum symbol_id sym, const char *label, bool sel)
{
    bool hit = ui_sidebar_item(ui, (struct rect){14, *y, side - 26, ROW - 2}, sym, label, sel);
    *y += ROW;
    return hit;
}

static void side_header(struct ui *ui, int side, int *y, const char *label)
{
    ui_sidebar_header(ui, (struct rect){22, *y + 6, side - 32, 20}, label);
    *y += 30;
}

static void sidebar(struct ui *ui, struct photos *ph, int side, int H)
{
    struct window *w = ui->win;
    ui_sidebar_panel(ui, side, H, style.window);
    ui_search(ui, (struct rect){14, 44, side - 28, 28}, &ph->query, "Search Photos");
    if (strcmp(ph->query.buf, ph->shown_query)) {
        str_copy(ph->shown_query, ph->query.buf, sizeof(ph->shown_query));
        if (ph->query.buf[0]) {
            ph->col = COL_SEARCH;
            collect(ph);
            to_grid(w, ph);
        } else if (ph->col == COL_SEARCH) {
            choose(w, ph, COL_ALL, 0);
        }
        ui->changed = true;
    }
    struct rect list = {0, 80, side, H - 80};
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(list, ui->e->x, ui->e->y)) {
        ph->side_scroll = MAX(ph->side_scroll + ui->e->wheel * ROW, 0);
        ui->consumed = true;
        ui->changed = true;
    }
    struct canvas *c = ui->c;
    struct rect saved = c ? c->clip : (struct rect){0, 0, 0, 0};
    if (c)
        c->clip = rect_intersect(c->clip, list);
    int y = 80 - ph->side_scroll, top = y;
    bool lib_col = ph->col != COL_SEARCH;
    side_header(ui, side, &y, "Library");
    if (side_item(ui, side, &y, SYM_GRID, "All Photos", lib_col && ph->col == COL_ALL))
        choose(w, ph, COL_ALL, 0);
    if (side_item(ui, side, &y, SYM_STAR, FAVORITES, ph->col == COL_FAVORITES))
        choose(w, ph, COL_FAVORITES, 0);
    if (side_item(ui, side, &y, SYM_CLOCK, "Recents", ph->col == COL_RECENTS))
        choose(w, ph, COL_RECENTS, 0);
    bool unnamed = false;
    for (int i = 0; i < lib.n && !unnamed; i++)
        unnamed = has_unnamed(&lib.e[i]);
    if (lib.nperson_names || unnamed) {
        side_header(ui, side, &y, "People");
        for (int i = 0; i < lib.nperson_names; i++)
            if (side_item(ui, side, &y, SYM_USER, lib.person_names[i], ph->col == COL_PERSON && ph->col_arg == i))
                choose(w, ph, COL_PERSON, i);
        if (unnamed && side_item(ui, side, &y, SYM_USERS, "Unnamed Faces", ph->col == COL_UNNAMED))
            choose(w, ph, COL_UNNAMED, 0);
    }
    side_header(ui, side, &y, "Albums");
    for (int i = 0; i < lib.nalbum_names; i++)
        if (side_item(ui, side, &y, SYM_COPY, lib.album_names[i], ph->col == COL_ALBUM && ph->col_arg == i))
            choose(w, ph, COL_ALBUM, i);
    if (side_item(ui, side, &y, SYM_PLUS, "New Album…", false))
        command(w, ph, CMD_NEW_ALBUM);
    side_header(ui, side, &y, "Places");
    for (int i = 0; i < (int)ARRAY_SIZE(places); i++) {
        char dir[PATH_MAX_LEN];
        place_dir(i, dir, sizeof(dir));
        bool sel = ph->col == COL_FOLDER && !strcmp(dir, ph->dir);
        if (side_item(ui, side, &y, places[i].sym, places[i].label, sel) && !sel)
            open_path(w, ph, dir);
    }
    int content = y - top + 12;
    ph->side_scroll = CLAMP(ph->side_scroll, 0, MAX(content - list.h, 0));
    if (c)
        c->clip = saved;
}

static void frame(struct ui *ui, void *data)
{
    struct photos *ph = data;
    int W = ui->win->surface.w, H = ui->win->surface.h, side = side_width(W);
    if (side)
        sidebar(ui, ph, side, H);
    int x = side ? side + 18 : 84;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){side, 0, W - side, TOOLBAR_HEIGHT - 1}, style.content);
        gfx_fill(ui->c, (struct rect){side, TOOLBAR_HEIGHT - 1, W - side, 1}, style.separator);
    }
    if (!ph->grid) {
        if (ui_symbol_button(ui, (struct rect){x, 12, 28, 28}, SYM_GRID, false))
            command(ui->win, ph, CMD_LIBRARY);
        x += 36;
    }
    char sub[80];
    const char *title;
    if (ph->grid) {
        title = collection_title(ph);
        ksnprintf(sub, sizeof(sub), "%d picture%s", ph->count, ph->count == 1 ? "" : "s");
    } else {
        title = ph->pic.px || ph->error[0] ? path_basename(ph->path) : "Photos";
        sub[0] = 0;
        if (ph->pic.px) {
            struct entry *e = lib_find(ph->path);
            int len = ksnprintf(sub, sizeof(sub), "%d of %d  -  %d x %d", ph->index + 1, ph->count, ph->pic.w,
                                ph->pic.h);
            for (int f = 0; e && f < e->nfaces && len < (int)sizeof(sub) - 4; f++)
                if (e->who[f] >= 0)
                    len += ksnprintf(sub + len, sizeof(sub) - (size_t)len, "%s%s", len && f == 0 ? "  -  " : ", ",
                                     lib.person_names[e->who[f]]);
        }
    }
    int tw = MIN(gfx_text_width(&font_bold, title), MAX(W - x - 220, 40));
    ui_label(ui, (struct rect){x, 0, tw + 4, TOOLBAR_HEIGHT}, title, &font_bold, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){x + tw + 14, 2, MAX(W - x - tw - 130, 10), TOOLBAR_HEIGHT}, sub, &font_small,
             style.tertiary, ALIGN_LEFT);
    int bx = W - 46 - 30;
    if (ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, SYM_FOLDER, false))
        command(ui->win, ph, CMD_OPEN);
    bx -= 32;
    if (ph->count > 1 &&
        ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, ph->slideshow ? SYM_PAUSE : SYM_PLAY, ph->slideshow))
        command(ui->win, ph, CMD_SLIDESHOW);
    bx -= 32;
    const char *cur = ph->grid ? (ph->count ? ph->items[ph->selected] : NULL) : ph->path;
    if (cur && cur[0] && ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, SYM_STAR, in_album(FAVORITES, cur)))
        command(ui->win, ph, CMD_FAVORITE);
    bx -= 32;
    if (!ph->grid && ph->pic.px && ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, SYM_RESTART, false))
        command(ui->win, ph, CMD_ROTATE);
}

static void picked(void *ctx, const char *value)
{
    struct window *owner = ctx;
    if (wm_window_alive(owner) && value && value[0])
        open_path(owner, owner->data, value);
}

static void zoom_to(struct photos *ph, int z)
{
    ph->zoom.target = CLAMP(z, 256, 8192);
    if (ph->zoom.target == 1024) {
        ph->pan_x = ph->pan_y = 0;
    }
}

/* the picture a command is about: the one shown, or the one selected in the grid */
static const char *current(struct photos *ph)
{
    if (ph->target[0])
        return ph->target;
    if (!ph->grid)
        return ph->path[0] ? ph->path : NULL;
    return ph->count ? ph->items[ph->selected] : NULL;
}

static void refresh(struct window *w, struct photos *ph)
{
    if (ph->col != COL_FOLDER) {
        int keep = ph->grid_scroll;
        collect(ph);
        ph->grid_scroll = keep;
    }
    wm_invalidate(w);
}

static void album_named(struct window *owner, const char *value)
{
    if (!wm_window_alive(owner) || !value || !value[0])
        return;
    struct photos *ph = owner->data;
    char name[48];
    str_copy(name, value, sizeof(name));
    str_trim(name);
    for (char *p = name; *p; p++)
        if (*p == '|')
            *p = '-';
    if (!name[0])
        return;
    if (ph->target[0]) {
        album_add(name, ph->target);
        ph->target[0] = 0;
        refresh(owner, ph);
    } else {
        /* an empty album is kept as a line with no picture */
        if (lib.nalbums < MAX_MARKS && !in_album(name, "")) {
            str_copy(lib.albums[lib.nalbums].name, name, 48);
            lib.albums[lib.nalbums++].path[0] = 0;
            save_marks(".albums", lib.albums, lib.nalbums, false);
            lib_match();
        }
        for (int i = 0; i < lib.nalbum_names; i++)
            if (!strcmp(lib.album_names[i], name))
                choose(owner, ph, COL_ALBUM, i);
    }
}

static void album_picked(struct window *owner, int index)
{
    if (!wm_window_alive(owner) || index < 0)
        return;
    struct photos *ph = owner->data;
    if (index < lib.nalbum_names) {
        album_add(lib.album_names[index], ph->target);
        ph->target[0] = 0;
        refresh(owner, ph);
    } else {
        ui_prompt(owner, "New Album", "Album name", "", album_named);
    }
}

static void ask_name(struct window *w, struct photos *ph);

static void face_named(struct window *owner, const char *value)
{
    if (!wm_window_alive(owner))
        return;
    struct photos *ph = owner->data;
    if (value && ph->naming >= 0) {
        char name[48];
        str_copy(name, value, sizeof(name));
        str_trim(name);
        for (char *p = name; *p; p++)
            if (*p == '|')
                *p = '-';
        name_face(ph->target, ph->naming, name);
        ph->naming++;
        ask_name(owner, ph);
    } else {
        ph->naming = -1;
        ph->target[0] = 0;
    }
    wm_invalidate(owner);
}

/* names the faces in the picture one at a time, outlining the one being asked about */
static void ask_name(struct window *w, struct photos *ph)
{
    struct entry *e = lib_find(ph->target);
    if (!e || ph->naming >= e->nfaces) {
        ph->naming = -1;
        ph->target[0] = 0;
        refresh(w, ph);
        return;
    }
    if (ph->grid || strcmp(ph->path, ph->target)) {
        for (int i = 0; i < ph->count; i++)
            if (!strcmp(ph->items[i], ph->target))
                ph->index = i;
        int keep = ph->naming;
        char path[PATH_MAX_LEN];
        str_copy(path, ph->target, sizeof(path));
        show(w, ph, path, 0);
        ph->naming = keep;
    }
    char label[64];
    ksnprintf(label, sizeof(label), e->nfaces > 1 ? "Who is this? (face %d of %d)" : "Who is this?", ph->naming + 1,
              e->nfaces);
    const char *now = e->who[ph->naming] >= 0 ? lib.person_names[e->who[ph->naming]] : "";
    wm_invalidate(w);
    ui_prompt(w, "Name People", label, now, face_named);
}

static void context_pick(struct window *owner, int index);

static const char *ctx_items[6];
static int ctx_cmds[6];

static void context_menu(struct window *w, struct photos *ph, int x, int y)
{
    const char *path = current(ph);
    if (!path)
        return;
    str_copy(ph->target, path, sizeof(ph->target));
    struct entry *e = lib_find(path);
    int n = 0;
    ctx_items[n] = in_album(FAVORITES, path) ? "Remove from Favorites" : "Add to Favorites";
    ctx_cmds[n++] = CMD_FAVORITE;
    ctx_items[n] = "Add to Album…";
    ctx_cmds[n++] = CMD_ALBUM;
    if (ph->col == COL_ALBUM) {
        ctx_items[n] = "Remove from Album";
        ctx_cmds[n++] = -1;
    }
    if (e && e->nfaces) {
        ctx_items[n] = "Name People…";
        ctx_cmds[n++] = CMD_NAME;
    }
    ctx_items[n] = "Show in Files";
    ctx_cmds[n++] = CMD_SHOW;
    ctx_items[n] = "Edit in Paint";
    ctx_cmds[n++] = CMD_EDIT;
    ui_menu(w, x, y, ctx_items, n, context_pick);
}

static void context_pick(struct window *owner, int index)
{
    if (!wm_window_alive(owner))
        return;
    struct photos *ph = owner->data;
    if (index < 0) {
        ph->target[0] = 0;
        return;
    }
    if (ctx_cmds[index] < 0) {
        album_remove(lib.album_names[ph->col_arg], ph->target);
        ph->target[0] = 0;
        refresh(owner, ph);
        return;
    }
    command(owner, ph, ctx_cmds[index]);
}

static void command(struct window *w, struct photos *ph, int cmd)
{
    const char *cur = current(ph);
    switch (cmd) {
    case CMD_OPEN: {
        struct gui_picker o = {.title = "Open Picture", .dir = ph->dir, .exts = ".png;.jpg;.jpeg;.gif;.bmp",
                               .button = "Open"};
        gui_file_picker(w, &o, picked, w);
        break;
    }
    case CMD_PREV:
        if (!ph->grid)
            open_index(w, ph, ph->index - 1, -1);
        break;
    case CMD_NEXT:
        if (!ph->grid)
            open_index(w, ph, ph->index + 1, 1);
        break;
    case CMD_ZOOM_IN: zoom_to(ph, ph->zoom.target * 3 / 2); break;
    case CMD_ZOOM_OUT: zoom_to(ph, ph->zoom.target * 2 / 3); break;
    case CMD_FIT: zoom_to(ph, 1024); break;
    case CMD_ACTUAL: {
        int scale;
        struct rect v = view_rect(w->surface.w, w->surface.h);
        image_rect(ph, v.w, v.h, &scale);
        int fit = scale * 1024 / MAX(ph->zoom.pos, 1);
        zoom_to(ph, fit ? 1024 * 1024 / fit : 1024);
        break;
    }
    case CMD_ROTATE:
        if (ph->pic.px && !ph->grid)
            rotate(ph);
        break;
    case CMD_WALLPAPER:
        if (cur) {
            char line[PATH_MAX_LEN + 24];
            ksnprintf(line, sizeof(line), "config wallpaper '%s'", cur);
            gui_terminal_run(line);
        }
        break;
    case CMD_EDIT:
        if (cur)
            gui_app_launch(gui_app_find("Paint"), cur);
        break;
    case CMD_SHOW:
        if (cur) {
            char dir[PATH_MAX_LEN];
            path_dirname(cur, dir, sizeof(dir));
            gui_app_launch(gui_app_find("Files"), dir);
        }
        break;
    case CMD_SLIDESHOW:
        if (ph->grid && ph->count)
            open_index(w, ph, ph->selected, 0);
        ph->slideshow = !ph->slideshow;
        ph->slide_due = hal_ticks_ms() + 3000;
        break;
    case CMD_LIBRARY:
        if (!ph->grid)
            to_grid(w, ph);
        break;
    case CMD_FAVORITE:
        if (cur) {
            char path[PATH_MAX_LEN];
            str_copy(path, cur, sizeof(path));
            if (in_album(FAVORITES, path))
                album_remove(FAVORITES, path);
            else
                album_add(FAVORITES, path);
            if (ph->col == COL_FAVORITES && ph->grid)
                refresh(w, ph);
        }
        break;
    case CMD_ALBUM:
        if (cur) {
            str_copy(ph->target, cur, sizeof(ph->target));
            static const char *names[33];
            int n = 0;
            for (int i = 0; i < lib.nalbum_names; i++)
                names[n++] = lib.album_names[i];
            names[n++] = "New Album…";
            struct rect r = view_rect(w->surface.w, w->surface.h);
            ui_menu(w, r.x + r.w / 2 - 80, r.y + 20, names, n, album_picked);
            wm_invalidate(w);
            return;
        }
        break;
    case CMD_NEW_ALBUM:
        ph->target[0] = 0;
        ui_prompt(w, "New Album", "Album name", "", album_named);
        break;
    case CMD_NAME:
        if (cur) {
            struct entry *e = lib_find(cur);
            if (e && !e->done)
                analyse_path(e);
            if (!e || !e->nfaces) {
                ui_alert(w, "Name People", "No faces were found in this picture.", ICON_PHOTOS, NULL, 0, NULL);
                break;
            }
            str_copy(ph->target, cur, sizeof(ph->target));
            ph->naming = 0;
            ask_name(w, ph);
            return;
        }
        break;
    case CMD_REFRESH:
        lib_scan();
        refresh(w, ph);
        break;
    }
    if (cmd != CMD_NAME)
        ph->target[0] = 0;
    wm_invalidate(w);
}

static void grid_event(struct window *w, struct photos *ph, struct gui_event *e, struct rect v)
{
    int ex = e->x - v.x, ey = e->y - v.y;
    int cols = grid_cols(v.w);
    switch (e->type) {
    case EV_WHEEL:
        if (rect_contains(v, e->x, e->y)) {
            ph->grid_scroll = CLAMP(ph->grid_scroll + e->wheel * 60, 0, MAX(grid_height(v.w, ph->count) - v.h, 0));
            wm_invalidate(w);
        }
        break;
    case EV_MOUSE_DOWN: {
        if (!rect_contains(v, e->x, e->y))
            break;
        int i = grid_hit(ph, v.w, ex, ey);
        if (i >= 0) {
            ph->selected = i;
            if (e->button == BUTTON_LEFT && e->clicks >= 2)
                open_index(w, ph, i, 0);
            else if (e->button == BUTTON_RIGHT)
                context_menu(w, ph, e->x, e->y);
        }
        wm_invalidate(w);
        e->handled = true;
        break;
    }
    case EV_KEY: {
        int k = e->key, s = ph->selected;
        if (k == KEY_LEFT)
            s--;
        else if (k == KEY_RIGHT)
            s++;
        else if (k == KEY_UP)
            s -= cols;
        else if (k == KEY_DOWN)
            s += cols;
        else if ((k == '\n' || k == '\r' || k == ' ') && ph->count) {
            if (k == ' ')
                command(w, ph, CMD_SLIDESHOW);
            else
                open_index(w, ph, ph->selected, 0);
            break;
        } else if (k == 'f' || k == 'F') {
            command(w, ph, CMD_FAVORITE);
            break;
        } else {
            break;
        }
        if (ph->count) {
            ph->selected = CLAMP(s, 0, ph->count - 1);
            struct rect r = cell_rect(v.w, ph->selected, ph->grid_scroll);
            if (r.y < 0)
                ph->grid_scroll += r.y - PAD;
            else if (r.y + r.h > v.h)
                ph->grid_scroll += r.y + r.h - v.h + PAD;
            ph->grid_scroll = MAX(ph->grid_scroll, 0);
        }
        wm_invalidate(w);
        break;
    }
    default: break;
    }
}

/* background work while the window is open: thumbnails for what is on screen, then reading the rest */
static void work(struct window *w, struct photos *ph, struct rect v)
{
    uint64_t start = hal_ticks_ms();
    bool did = false;
    while (hal_ticks_ms() - start < 24) {
        int pick = -1;
        if (ph->grid)
            for (int i = 0; i < ph->count && pick < 0; i++) {
                struct rect r = cell_rect(v.w, i, ph->grid_scroll);
                if (r.y + r.h >= -CELL && r.y <= v.h + CELL && !ph->thumb_tried[i])
                    pick = i;
            }
        if (pick >= 0) {
            make_thumb(ph, pick);
            did = true;
            continue;
        }
        struct entry *e = NULL;
        for (int i = 0; i < lib.n && !e; i++)
            if (!lib.e[i].done)
                e = &lib.e[i];
        if (!e)
            break;
        analyse_path(e);
        did = true;
    }
    if (!did && lib.dirty) {
        lib_save();
        lib_match();
        if (ph->col == COL_SEARCH || ph->col == COL_UNNAMED || ph->col == COL_PERSON) {
            int keep = ph->grid_scroll;
            collect(ph);
            ph->grid_scroll = keep;
        }
        did = true;
    }
    if (did)
        wm_invalidate(w);
}

static void event(struct window *w, struct gui_event *e)
{
    struct photos *ph = w->data;
    if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP || e->type == EV_WHEEL ||
        e->type == EV_LEAVE || e->type == EV_KEY) {
        ui_run(w, &ph->st, NULL, e, frame, ph);
        if (e->handled)
            return;
    }
    struct rect v = view_rect(w->surface.w, w->surface.h);
    int ex = e->x - v.x, ey = e->y - v.y;
    bool inside = rect_contains(v, e->x, e->y);
    if (e->type == EV_KEY) {
        int k = e->key;
        if (k >= KEY_COMMAND(0) && k <= KEY_COMMAND(CMD_LAST)) {
            command(w, ph, k - KEY_COMMAND(0));
            return;
        }
        if (k == KEY_CTRL('o')) {
            command(w, ph, CMD_OPEN);
            return;
        }
    }
    if (ph->grid && e->type != EV_TIMER && e->type != EV_RESIZE) {
        grid_event(w, ph, e, v);
        return;
    }
    switch (e->type) {
    case EV_KEY: {
        int k = e->key;
        if (k == KEY_LEFT)
            command(w, ph, CMD_PREV);
        else if (k == KEY_RIGHT)
            command(w, ph, CMD_NEXT);
        else if (k == '+' || k == '=')
            command(w, ph, CMD_ZOOM_IN);
        else if (k == '-')
            command(w, ph, CMD_ZOOM_OUT);
        else if (k == '0')
            command(w, ph, CMD_FIT);
        else if (k == '1')
            command(w, ph, CMD_ACTUAL);
        else if (k == 'r' || k == 'R')
            command(w, ph, CMD_ROTATE);
        else if (k == 'f' || k == 'F')
            command(w, ph, CMD_FAVORITE);
        else if (k == ' ')
            command(w, ph, CMD_SLIDESHOW);
        else if (k == KEY_CTRL('c') && ph->pic.px)
            gui_clipboard_set_image(ph->pic.px + (size_t)ph->frame * (size_t)ph->pic.w * (size_t)ph->pic.h, ph->pic.w,
                                    ph->pic.h, ph->pic.w);
        else if (k == KEY_ESC && ph->slideshow)
            ph->slideshow = false;
        else if (k == KEY_ESC)
            command(w, ph, CMD_LIBRARY);
        break;
    }
    case EV_WHEEL:
        if (inside)
            command(w, ph, e->wheel < 0 ? CMD_ZOOM_IN : CMD_ZOOM_OUT);
        break;
    case EV_MOUSE_DOWN:
        ph->last_move = hal_ticks_ms();
        if (e->button == BUTTON_LEFT && !inside) {
            break;
        } else if (e->button == BUTTON_LEFT && !ph->pic.px) {
            command(w, ph, CMD_OPEN);
        } else if (e->button == BUTTON_LEFT && ph->bar.pos > 512 && rect_contains(bar_rect(v.w, v.h, 1024), ex, ey)) {
            struct rect b = bar_rect(v.w, v.h, 1024);
            for (int i = 0; i < BAR_BUTTONS; i++)
                if (rect_contains(bar_button(b, i), ex, ey))
                    command(w, ph, bar_cmds[i]);
        } else if (e->button == BUTTON_LEFT && e->clicks >= 2) {
            command(w, ph, ph->zoom.target == 1024 ? CMD_ACTUAL : CMD_FIT);
        } else if (e->button == BUTTON_LEFT) {
            ph->panning = true;
            ph->drag_x = ex;
            ph->drag_y = ey;
        } else if (e->button == BUTTON_RIGHT && inside) {
            context_menu(w, ph, e->x, e->y);
        }
        e->handled = true;
        break;
    case EV_MOUSE_MOVE:
        ph->last_move = hal_ticks_ms();
        if (ph->panning && ph->zoom.target > 1024) {
            ph->pan_x += ex - ph->drag_x;
            ph->pan_y += ey - ph->drag_y;
            wm_invalidate(w);
        }
        ph->drag_x = ex;
        ph->drag_y = ey;
        break;
    case EV_MOUSE_UP: ph->panning = false; break;
    case EV_TIMER: {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - ph->tick, 64u);
        ph->tick = now;
        bool busy = spring_step(&ph->zoom, 360, 34, dt);
        busy |= spring_step(&ph->slide, 260, 30, dt);
        busy |= spring_step(&ph->fade, 300, 40, dt);
        bool over = rect_contains(bar_rect(v.w, v.h, 1024), ph->drag_x, ph->drag_y);
        ph->bar.target = now - ph->last_move < 2500 || over ? 1024 : 0;
        busy |= spring_step(&ph->bar, 320, 32, dt);
        if (!ph->grid && ph->pic.frames > 1 && now >= ph->frame_due) {
            ph->frame = (ph->frame + 1) % ph->pic.frames;
            ph->frame_due = now + (uint64_t)ph->pic.delays[ph->frame];
            busy = true;
        }
        if (ph->slideshow && !ph->grid && now >= ph->slide_due) {
            ph->slide_due = now + 3000;
            command(w, ph, CMD_NEXT);
        }
        if (busy)
            wm_invalidate(w);
        if (!busy)
            work(w, ph, v);
        break;
    }
    case EV_RESIZE: canvas_free(&ph->fitted); break;
    default: break;
    }
}

static void destroy(struct window *w)
{
    struct photos *ph = w->data;
    if (lib.dirty)
        lib_save();
    picture_free(&ph->pic);
    canvas_free(&ph->fitted);
    clear_thumbs(ph);
    kfree(ph->thumbs);
    kfree(ph->thumb_tried);
    kfree(ph->items);
    kfree(ph);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

/* arg: a picture or a folder to open; nothing opens the library */
static void launch(const char *arg)
{
    struct photos *ph = kzalloc(sizeof(*ph));
    if (!ph)
        return;
    ph->items = kzalloc(sizeof(*ph->items) * MAX_ITEMS);
    ph->thumbs = kzalloc(sizeof(struct canvas) * MAX_ITEMS);
    ph->thumb_tried = kzalloc(sizeof(bool) * MAX_ITEMS);
    if (!ph->items || !ph->thumbs || !ph->thumb_tried || !lib_scan()) {
        kfree(ph->items);
        kfree(ph->thumbs);
        kfree(ph->thumb_tried);
        kfree(ph);
        return;
    }
    ph->tick = ph->last_move = hal_ticks_ms();
    ph->naming = -1;
    spring_set(&ph->zoom, 1024);
    spring_set(&ph->fade, 1024);
    struct window *w =
        wm_open("Photos", ICON_PHOTOS, 960, 620, &ops, ph, WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!w) {
        kfree(ph->items);
        kfree(ph->thumbs);
        kfree(ph->thumb_tried);
        kfree(ph);
        return;
    }
    w->toolbar_h = TOOLBAR_HEIGHT;
    w->min_w = 360;
    w->min_h = 260;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    wm_set_timer(w, 16);
    path_join(user_home(), "Pictures", ph->dir, sizeof(ph->dir));
    if (arg && *arg && vfs_exists(arg))
        open_path(w, ph, arg);
    else
        choose(w, ph, COL_ALL, 0);
}

static const struct gui_app photos_app = {"Photos", "Your pictures: albums, favorites, people and search",
                                          ICON_PHOTOS, launch, ".png;.jpg;.jpeg;.gif;.bmp", NULL};

/* ---- the command ---- */

static void print_entry(struct shell *sh, const struct entry *e)
{
    sh_printf(sh, "%s  %ux%u", e->path, e->w, e->h);
    if (e->nfaces) {
        sh_printf(sh, "  faces:");
        for (int f = 0; f < e->nfaces; f++)
            sh_printf(sh, " %s", e->who[f] >= 0 ? lib.person_names[e->who[f]] : "?");
    }
    sh_printf(sh, "\n");
}

static void index_all(void)
{
    lib_scan();
    for (int i = 0; i < lib.n; i++)
        if (!lib.e[i].done)
            analyse_path(&lib.e[i]);
    if (lib.dirty)
        lib_save();
    lib_match();
}

static const char *full_path(struct shell *sh, const char *arg, char *buf, size_t cap)
{
    if (path_resolve(sh->cwd, arg, buf, cap) != 0)
        str_copy(buf, arg, cap);
    return buf;
}

COMMAND(photos, NULL, "media",
        "photos index | search WORDS | album [NAME [add|remove PATH]] | favorite PATH | faces | people | "
        "name PATH NAME [FACE]",
        "the Photos library: index pictures, search them, albums, favorites and the people in them")
{
    if (argc < 2)
        return command_usage(sh, command_find("photos"));
    if (!lib_scan())
        return sh_error(sh, "photos", "out of memory");
    const char *cmd = argv[1];
    char buf[PATH_MAX_LEN];
    if (!strcmp(cmd, "index")) {
        index_all();
        int faces = 0, done = 0;
        for (int i = 0; i < lib.n; i++) {
            faces += lib.e[i].nfaces;
            done += lib.e[i].done;
        }
        sh_printf(sh, "%d picture%s indexed, %d face%s found\n", done, done == 1 ? "" : "s", faces,
                  faces == 1 ? "" : "s");
    } else if (!strcmp(cmd, "search") && argc > 2) {
        index_all();
        char query[256] = "";
        for (int i = 2; i < argc; i++) {
            str_append(query, argv[i], sizeof(query));
            str_append(query, " ", sizeof(query));
        }
        int found = 0;
        for (int i = 0; i < lib.n; i++)
            if (query_matches(&lib.e[i], query)) {
                print_entry(sh, &lib.e[i]);
                found++;
            }
        if (!found)
            sh_printf(sh, "no pictures match\n");
    } else if (!strcmp(cmd, "album") || !strcmp(cmd, "albums")) {
        if (argc == 2) {
            int favs = 0;
            for (int i = 0; i < lib.nalbums; i++)
                favs += !strcmp(lib.albums[i].name, FAVORITES) && lib.albums[i].path[0];
            sh_printf(sh, "%-24s %d\n", FAVORITES, favs);
            for (int a = 0; a < lib.nalbum_names; a++) {
                int n = 0;
                for (int i = 0; i < lib.nalbums; i++)
                    n += !strcmp(lib.albums[i].name, lib.album_names[a]) && lib.albums[i].path[0];
                sh_printf(sh, "%-24s %d\n", lib.album_names[a], n);
            }
        } else if (argc == 3) {
            int n = 0;
            for (int i = 0; i < lib.nalbums; i++)
                if (!str_casecmp(lib.albums[i].name, argv[2]) && lib.albums[i].path[0]) {
                    sh_printf(sh, "%s\n", lib.albums[i].path);
                    n++;
                }
            if (!n)
                sh_printf(sh, "the album is empty\n");
        } else if (argc == 5 && !strcmp(argv[3], "add")) {
            const char *p = full_path(sh, argv[4], buf, sizeof(buf));
            if (!vfs_exists(p))
                return sh_error(sh, "photos", "%s: no such picture", argv[4]);
            album_add(argv[2], p);
            sh_printf(sh, "added to %s\n", argv[2]);
        } else if (argc >= 4 && !strcmp(argv[3], "remove")) {
            album_remove(argv[2], argc > 4 ? full_path(sh, argv[4], buf, sizeof(buf)) : NULL);
            sh_printf(sh, "removed from %s\n", argv[2]);
        } else {
            return command_usage(sh, command_find("photos"));
        }
    } else if ((!strcmp(cmd, "favorite") || !strcmp(cmd, "fav")) && argc > 2) {
        const char *p = full_path(sh, argv[2], buf, sizeof(buf));
        if (!vfs_exists(p))
            return sh_error(sh, "photos", "%s: no such picture", argv[2]);
        album_add(FAVORITES, p);
        sh_printf(sh, "added to %s\n", FAVORITES);
    } else if (!strcmp(cmd, "faces")) {
        index_all();
        int n = 0;
        for (int i = 0; i < lib.n; i++)
            if (lib.e[i].nfaces) {
                print_entry(sh, &lib.e[i]);
                n++;
            }
        if (!n)
            sh_printf(sh, "no faces found\n");
    } else if (!strcmp(cmd, "people")) {
        index_all();
        if (!lib.nperson_names)
            sh_printf(sh, "nobody is named yet: photos name PATH NAME\n");
        for (int p = 0; p < lib.nperson_names; p++) {
            int n = 0;
            for (int i = 0; i < lib.n; i++)
                n += has_person(&lib.e[i], p);
            sh_printf(sh, "%-24s %d picture%s\n", lib.person_names[p], n, n == 1 ? "" : "s");
        }
    } else if (!strcmp(cmd, "name") && argc >= 4) {
        index_all();
        const char *p = full_path(sh, argv[2], buf, sizeof(buf));
        struct entry *e = lib_find(p);
        if (!e)
            return sh_error(sh, "photos", "%s is not in the library (Pictures, Desktop or Downloads)", argv[2]);
        long face = 1;
        if (argc > 4 && !str_parse_long(argv[4], &face))
            return command_usage(sh, command_find("photos"));
        if (face < 1 || face > e->nfaces)
            return sh_error(sh, "photos", "%s has %d face%s", argv[2], e->nfaces, e->nfaces == 1 ? "" : "s");
        name_face(p, (int)face - 1, argv[3]);
        sh_printf(sh, "face %ld is %s\n", face, argv[3]);
    } else {
        return command_usage(sh, command_find("photos"));
    }
    return 0;
}

static const struct command *const commands[] = {&cmddef_photos};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&photos_app, NULL, &ext);
