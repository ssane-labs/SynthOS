/* ACPI Machine Language interpreter (synth/aml.h). Methods run to the end on the caller's stack. A broken or hostile
   table stops only the method that met it: every read is bounded by the end of the term it belongs to, loops stop
   after aml_loop_limit turns, and nesting of terms and calls is limited (the kernel stack is 64 KiB). Objects are
   reference-counted; so are namespace nodes, because a method's own names (Name inside a method) go away when it
   returns while a reference to one may still be held. */
#include <synth/aml.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>

#define MAX_SEGS 16
#define MAX_NEST 48
#define MAX_CALLS 12
#define MAX_ITEMS 4096
#define MAX_BYTES 65536

enum { F_REGION, F_INDEX, F_BANK };
enum { REF_NODE, REF_ELEM, REF_BYTE };
enum { FLOW_NONE, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_ERROR };
enum { T_NONE, T_LOCAL, T_ARG, T_NODE, T_DEBUG, T_REF };

struct aml_node {
    char name[4];
    uint32_t refs;
    struct aml_node *parent, *child, *next;
    struct aml_obj *obj;
};

typedef bool (*native_fn)(struct aml_obj **args, int argc, struct aml_obj **ret);

struct aml_obj {
    uint8_t type;
    uint32_t refs;
    union {
        uint64_t ival;
        struct {
            uint32_t len;
            uint8_t *data;
        } buf;
        struct {
            uint32_t count;
            struct aml_obj **items;
        } pkg;
        struct {
            const uint8_t *code;
            uint32_t len;
            uint8_t flags;
            uint16_t active;
            native_fn native;
        } m;
        struct {
            uint8_t space;
            bool pci_known;
            uint64_t base, len, pci;
            struct aml_node *dev;
        } r;
        struct {
            uint8_t kind, flags;
            uint32_t bit_off, bit_len;
            struct aml_node *region, *index, *data;
            uint64_t bank_value;
        } f;
        struct {
            struct aml_obj *buf;
            uint32_t bit_off, bit_len;
        } bf;
        struct {
            uint8_t kind;
            uint32_t index;
            struct aml_node *node;
            struct aml_obj *target;
        } ref;
        struct {
            char *path;
            struct aml_node *scope;
        } nr;
    };
};

struct frame {
    struct aml_obj *args[7], *locals[8];
    struct aml_node *scope;
    struct aml_obj *ret;
    int flow;
    bool load;
};

struct cur {
    const uint8_t *p, *end;
};

struct name {
    bool root;
    uint8_t up, n;
    char seg[MAX_SEGS][4];
};

struct target {
    int kind, i;
    struct aml_node *node;
    struct aml_obj *ref;
};

struct aml_os aml_os;
uint32_t aml_loop_limit = 1000000;
uint32_t aml_errors;
char aml_last_error[64];

static struct aml_node *root;
static int int_bits = 64;
static int nest, calls;

static struct aml_obj *eval(struct frame *f, struct cur *c);
static void exec_list(struct frame *f, struct cur *c);
static struct aml_obj *read_node(struct frame *f, struct aml_node *n);
static void store_node(struct frame *f, struct aml_node *n, struct aml_obj *v);
static struct aml_obj *invoke(struct aml_node *n, struct aml_obj **args, int argc, bool *ok);

static uint64_t ones(void)
{
    return int_bits == 32 ? 0xffffffffu : ~0ull;
}

static void fail(struct frame *f, const char *why)
{
    if (f->flow == FLOW_ERROR)
        return;
    f->flow = FLOW_ERROR;
    str_copy(aml_last_error, why, sizeof(aml_last_error));
}

/* ---- objects ---- */

static struct aml_obj *obj_new(int type)
{
    struct aml_obj *o = kzalloc(sizeof(*o));
    if (o) {
        o->type = (uint8_t)type;
        o->refs = 1;
    }
    return o;
}

static struct aml_obj *obj_get(struct aml_obj *o)
{
    if (o)
        o->refs++;
    return o;
}

static void node_put(struct aml_node *n);

static struct aml_node *node_get(struct aml_node *n)
{
    if (n)
        n->refs++;
    return n;
}

void aml_put(struct aml_obj *o)
{
    if (!o || --o->refs)
        return;
    switch (o->type) {
    case AML_STR:
    case AML_BUF:
        kfree(o->buf.data);
        break;
    case AML_PKG:
        for (uint32_t i = 0; i < o->pkg.count; i++)
            aml_put(o->pkg.items[i]);
        kfree(o->pkg.items);
        break;
    case AML_REGION:
        node_put(o->r.dev);
        break;
    case AML_FIELD:
        node_put(o->f.region);
        node_put(o->f.index);
        node_put(o->f.data);
        break;
    case AML_BUFFIELD:
        aml_put(o->bf.buf);
        break;
    case AML_REF:
        node_put(o->ref.node);
        aml_put(o->ref.target);
        break;
    case AML_NAMEREF:
        kfree(o->nr.path);
        node_put(o->nr.scope);
        break;
    default:
        break;
    }
    kfree(o);
}

struct aml_obj *aml_int(uint64_t v)
{
    struct aml_obj *o = obj_new(AML_INT);
    if (o)
        o->ival = v;
    return o;
}

/* a string (with its NUL) or buffer of len bytes, copied from data when it is not NULL */
static struct aml_obj *obj_bytes(int type, const void *data, uint32_t len)
{
    struct aml_obj *o = obj_new(type);
    if (!o)
        return NULL;
    o->buf.data = kzalloc(len + 1);
    if (!o->buf.data) {
        kfree(o);
        return NULL;
    }
    if (data)
        memcpy(o->buf.data, data, len);
    o->buf.len = len;
    return o;
}

static struct aml_obj *obj_str(const char *s)
{
    return obj_bytes(AML_STR, s, (uint32_t)strlen(s));
}

static struct aml_obj *obj_pkg(uint32_t count)
{
    struct aml_obj *o = obj_new(AML_PKG);
    if (!o)
        return NULL;
    o->pkg.items = kzalloc((count ? count : 1) * sizeof(*o->pkg.items));
    if (!o->pkg.items) {
        kfree(o);
        return NULL;
    }
    o->pkg.count = count;
    return o;
}

struct aml_obj *aml_buf(const void *data, uint32_t len)
{
    return obj_bytes(AML_BUF, data, len);
}

struct aml_obj *aml_pkg(uint32_t count)
{
    return obj_pkg(count);
}

/* what Store puts in its target: strings, buffers and packages are copied, the rest is shared */
static struct aml_obj *obj_copy(struct aml_obj *v)
{
    if (!v)
        return NULL;
    if (v->type == AML_STR || v->type == AML_BUF)
        return obj_bytes(v->type, v->buf.data, v->buf.len);
    if (v->type == AML_PKG) {
        struct aml_obj *p = obj_pkg(v->pkg.count);
        if (!p)
            return NULL;
        for (uint32_t i = 0; i < v->pkg.count; i++)
            p->pkg.items[i] = obj_copy(v->pkg.items[i]);
        return p;
    }
    return obj_get(v);
}

/* ---- namespace ---- */

static struct aml_node *node_new(struct aml_node *parent, const char seg[4], struct aml_obj *obj)
{
    struct aml_node *n = kzalloc(sizeof(*n));
    if (!n) {
        aml_put(obj);
        return NULL;
    }
    memcpy(n->name, seg, 4);
    n->refs = 1;
    n->obj = obj;
    n->parent = parent;
    if (parent) {
        struct aml_node **link = &parent->child;
        while (*link)
            link = &(*link)->next;
        *link = n;
    }
    return n;
}

static void node_put(struct aml_node *n)
{
    if (!n || --n->refs)
        return;
    aml_put(n->obj);
    kfree(n);
}

/* takes n (and what is under it) out of the namespace */
static void node_detach(struct aml_node *n)
{
    while (n->child)
        node_detach(n->child);
    if (n->parent) {
        struct aml_node **link = &n->parent->child;
        while (*link && *link != n)
            link = &(*link)->next;
        if (*link)
            *link = n->next;
    }
    n->parent = n->next = NULL;
    /* its value goes now: a value that refers to the node itself would otherwise keep it forever */
    struct aml_obj *o = n->obj;
    n->obj = NULL;
    aml_put(o);
    node_put(n);
}

static struct aml_node *child_of(const struct aml_node *parent, const char seg[4])
{
    for (struct aml_node *c = parent ? parent->child : NULL; c; c = c->next)
        if (!memcmp(c->name, seg, 4))
            return c;
    return NULL;
}

static struct aml_node *lookup(struct aml_node *scope, const struct name *nm, bool search)
{
    struct aml_node *s = nm->root ? root : scope;
    for (int i = 0; i < nm->up; i++) {
        if (!s || !s->parent)
            return NULL;
        s = s->parent;
    }
    if (!nm->n)
        return s;
    if (nm->n == 1 && search && !nm->root && !nm->up) {
        for (struct aml_node *t = s; t; t = t->parent) {
            struct aml_node *x = child_of(t, nm->seg[0]);
            if (x)
                return x;
        }
        return NULL;
    }
    for (int i = 0; i < nm->n && s; i++)
        s = child_of(s, nm->seg[i]);
    return s;
}

/* a named object: the last segment is created under the scope the others name; obj is taken over */
static struct aml_node *create(struct frame *f, const struct name *nm, struct aml_obj *obj)
{
    if (!obj) {
        fail(f, "out of memory");
        return NULL;
    }
    if (!nm->n) {
        aml_put(obj);
        fail(f, "empty name");
        return NULL;
    }
    struct name parent = *nm;
    parent.n--;
    struct aml_node *p = lookup(f->scope, &parent, false);
    if (!p) {
        aml_put(obj);
        if (!f->load)
            fail(f, "no scope for a name");
        return NULL;
    }
    struct aml_node *old = child_of(p, nm->seg[nm->n - 1]);
    if (old) {
        /* a second definition: the first stays, as ACPICA keeps it, except inside a method that runs again */
        if (!f->load || !old->obj || old->obj->type == AML_NONE) {
            aml_put(old->obj);
            old->obj = obj;
        } else {
            aml_put(obj);
        }
        return old;
    }
    return node_new(p, nm->seg[nm->n - 1], obj);
}

/* ---- reading the byte code ---- */

static bool take(struct cur *c, uint8_t *b)
{
    if (c->p >= c->end)
        return false;
    *b = *c->p++;
    return true;
}

static bool take_le(struct cur *c, int bytes, uint64_t *v)
{
    if (c->end - c->p < bytes)
        return false;
    *v = 0;
    for (int i = 0; i < bytes; i++)
        *v |= (uint64_t)c->p[i] << (8 * i);
    c->p += bytes;
    return true;
}

/* the value of a PkgLength encoding */
static bool pkg_value(struct cur *c, uint32_t *len)
{
    uint8_t lead;
    if (!take(c, &lead))
        return false;
    int n = lead >> 6;
    if (!n) {
        *len = lead & 0x3f;
        return true;
    }
    if (c->end - c->p < n)
        return false;
    *len = lead & 0x0f;
    for (int i = 0; i < n; i++)
        *len |= (uint32_t)c->p[i] << (4 + 8 * i);
    c->p += n;
    return true;
}

/* a PkgLength: where the package that starts at it ends */
static bool pkg_end(struct cur *c, const uint8_t **end)
{
    const uint8_t *start = c->p;
    uint32_t len;
    if (!pkg_value(c, &len) || len > (size_t)(c->end - start) || start + len < c->p)
        return false;
    *end = start + len;
    return true;
}

static bool name_char(uint8_t b)
{
    return (b >= 'A' && b <= 'Z') || b == '_' || (b >= '0' && b <= '9');
}

static bool name_start(uint8_t b)
{
    return (b >= 'A' && b <= 'Z') || b == '_' || b == '\\' || b == '^' || b == 0x2e || b == 0x2f;
}

static bool parse_name(struct frame *f, struct cur *c, struct name *nm)
{
    nm->root = false;
    nm->up = 0;
    nm->n = 0;
    if (c->p < c->end && *c->p == '\\') {
        nm->root = true;
        c->p++;
    } else {
        while (c->p < c->end && *c->p == '^' && nm->up < 255) {
            nm->up++;
            c->p++;
        }
    }
    uint8_t b;
    if (!take(c, &b)) {
        fail(f, "name cut off");
        return false;
    }
    int n;
    if (b == 0x00) {
        n = 0;
    } else if (b == 0x2e) {
        n = 2;
    } else if (b == 0x2f) {
        if (!take(c, &b)) {
            fail(f, "name cut off");
            return false;
        }
        n = b;
    } else {
        c->p--;
        n = 1;
    }
    if (n > MAX_SEGS || c->end - c->p < 4 * n) {
        fail(f, "bad name");
        return false;
    }
    for (int i = 0; i < n; i++) {
        memcpy(nm->seg[i], c->p, 4);
        for (int k = 0; k < 4; k++)
            if (!name_char(c->p[k])) {
                fail(f, "bad name");
                return false;
            }
        c->p += 4;
    }
    nm->n = (uint8_t)n;
    return true;
}

/* the text of a name, for a reference to it inside a package */
static void name_text(const struct name *nm, char *buf, size_t cap)
{
    size_t at = 0;
    if (nm->root && at + 1 < cap)
        buf[at++] = '\\';
    for (int i = 0; i < nm->up && at + 1 < cap; i++)
        buf[at++] = '^';
    for (int i = 0; i < nm->n; i++) {
        if (i && at + 1 < cap)
            buf[at++] = '.';
        for (int k = 0; k < 4 && at + 1 < cap; k++)
            buf[at++] = nm->seg[i][k];
    }
    buf[at] = 0;
}

/* a path written as text ("\_SB.PCI0", "^EC0", "_LID"); short segments are padded with '_' */
static bool text_name(const char *s, struct name *nm)
{
    memset(nm, 0, sizeof(*nm));
    if (*s == '\\') {
        nm->root = true;
        s++;
    }
    while (*s == '^') {
        nm->up++;
        s++;
    }
    while (*s) {
        if (nm->n == MAX_SEGS)
            return false;
        int k = 0;
        while (*s && *s != '.') {
            if (k == 4 || !name_char((uint8_t)*s))
                return false;
            nm->seg[nm->n][k++] = *s++;
        }
        if (!k)
            return false;
        while (k < 4)
            nm->seg[nm->n][k++] = '_';
        nm->n++;
        if (*s == '.')
            s++;
    }
    return true;
}

/* ---- conversions ---- */

static void deref(struct frame *f, struct aml_obj **v);

static bool to_int(struct frame *f, struct aml_obj *v, uint64_t *out)
{
    *out = 0;
    if (!v) {
        fail(f, "no value");
        return false;
    }
    switch (v->type) {
    case AML_INT:
        *out = v->ival;
        return true;
    case AML_BUF: {
        uint32_t n = MIN(v->buf.len, (uint32_t)(int_bits / 8));
        for (uint32_t i = 0; i < n; i++)
            *out |= (uint64_t)v->buf.data[i] << (8 * i);
        return true;
    }
    case AML_STR: {
        const char *s = (const char *)v->buf.data;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
        for (; is_xdigit(*s); s++)
            *out = *out << 4 | (uint64_t)(is_digit(*s) ? *s - '0' : (to_lower(*s) - 'a' + 10));
        *out &= ones();
        return true;
    }
    case AML_REF: {
        struct aml_obj *d = obj_get(v);
        deref(f, &d);
        bool ok = d && d->type != AML_REF && to_int(f, d, out);
        aml_put(d);
        return ok;
    }
    default:
        fail(f, "not an integer");
        return false;
    }
}

static void hex(char *out, uint64_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--, v >>= 4)
        out[i] = "0123456789ABCDEF"[v & 15];
    out[digits] = 0;
}

/* the bytes of v as a buffer (integers little-endian, strings with their NUL) */
static struct aml_obj *to_buffer(struct frame *f, struct aml_obj *v)
{
    if (v && v->type == AML_INT) {
        uint8_t b[8];
        for (int i = 0; i < 8; i++)
            b[i] = (uint8_t)(v->ival >> (8 * i));
        return obj_bytes(AML_BUF, b, (uint32_t)int_bits / 8);
    }
    if (v && v->type == AML_BUF)
        return obj_get(v);
    if (v && v->type == AML_STR)
        return obj_bytes(AML_BUF, v->buf.data, v->buf.len ? v->buf.len + 1 : 0);
    fail(f, "not a buffer");
    return NULL;
}

/* v as text, as Concatenate converts it: integers and buffers in hexadecimal */
static struct aml_obj *to_string(struct frame *f, struct aml_obj *v)
{
    if (v && v->type == AML_STR)
        return obj_get(v);
    if (v && v->type == AML_INT) {
        char s[17];
        hex(s, v->ival, int_bits / 4);
        return obj_str(s);
    }
    if (v && v->type == AML_BUF) {
        uint32_t n = MIN(v->buf.len, (uint32_t)MAX_BYTES / 3);
        struct aml_obj *s = obj_bytes(AML_STR, NULL, n ? n * 3 - 1 : 0);
        if (!s)
            return NULL;
        for (uint32_t i = 0; i < n; i++) {
            char h[3];
            hex(h, v->buf.data[i], 2);
            memcpy(s->buf.data + i * 3, h, 2);
            if (i + 1 < n)
                s->buf.data[i * 3 + 2] = ' ';
        }
        return s;
    }
    fail(f, "not a string");
    return NULL;
}

static struct aml_obj *eval_value(struct frame *f, struct cur *c)
{
    struct aml_obj *v = eval(f, c);
    if (v && v->type == AML_REF)
        deref(f, &v);
    return v;
}

static bool eval_int(struct frame *f, struct cur *c, uint64_t *out)
{
    struct aml_obj *v = eval(f, c);
    bool ok = v && to_int(f, v, out);
    aml_put(v);
    return ok && !f->flow;
}

/* ---- operation regions and fields ---- */

static int access_bits(uint8_t flags)
{
    switch (flags & 15) {
    case 2:
        return 16;
    case 3:
        return 32;
    case 4:
        return 64;
    default:
        return 8;
    }
}

static struct aml_node *ancestor_bridge(struct aml_node *n)
{
    for (; n; n = n->parent)
        if (aml_hid_is(n, "PNP0A03") || aml_hid_is(n, "PNP0A08"))
            return n;
    return NULL;
}

/* bus, device and function of the PCI device a PCI_Config region belongs to */
static uint64_t region_pci(struct aml_obj *r)
{
    if (!r->r.pci_known) {
        uint64_t adr = 0, bus = 0;
        r->r.pci_known = true;
        if (r->r.dev && !aml_eval_int(r->r.dev, "_ADR", &adr))
            adr = 0;
        struct aml_node *bridge = ancestor_bridge(r->r.dev);
        if (bridge && !aml_eval_int(bridge, "_BBN", &bus))
            bus = 0;
        r->r.pci = (bus & 0xff) << 32 | (adr >> 16 & 0x1f) << 24 | (adr & 7) << 16;
    }
    return r->r.pci;
}

static uint64_t field_get(struct frame *f, struct aml_node *n);
static void field_set(struct frame *f, struct aml_node *n, uint64_t v);

static bool region_of(struct aml_obj *fo, struct aml_obj **r)
{
    *r = fo->f.region && fo->f.region->obj && fo->f.region->obj->type == AML_REGION ? fo->f.region->obj : NULL;
    return *r != NULL;
}

/* one access of bits at byte offset off of the field's region (or through its index and data fields) */
static uint64_t unit_read(struct frame *f, struct aml_obj *fo, uint64_t off, int bits)
{
    if (fo->f.kind == F_INDEX) {
        field_set(f, fo->f.index, off);
        return field_get(f, fo->f.data);
    }
    if (fo->f.kind == F_BANK)
        field_set(f, fo->f.index, fo->f.bank_value);
    struct aml_obj *r;
    if (!region_of(fo, &r) || off + (uint64_t)bits / 8 > r->r.len || !aml_os.read)
        return 0;
    uint64_t addr = r->r.base + off;
    if (r->r.space == AML_SPACE_PCI)
        addr = region_pci(r) | ((r->r.base + off) & 0xffff);
    if (r->r.space == AML_SPACE_MEMORY || r->r.space == AML_SPACE_IO || r->r.space == AML_SPACE_PCI ||
        r->r.space == AML_SPACE_EC || r->r.space == AML_SPACE_CMOS)
        return aml_os.read(r->r.space, addr, bits);
    return 0;
}

static void unit_write(struct frame *f, struct aml_obj *fo, uint64_t off, int bits, uint64_t v)
{
    if (fo->f.kind == F_INDEX) {
        field_set(f, fo->f.index, off);
        field_set(f, fo->f.data, v);
        return;
    }
    if (fo->f.kind == F_BANK)
        field_set(f, fo->f.index, fo->f.bank_value);
    struct aml_obj *r;
    if (!region_of(fo, &r) || off + (uint64_t)bits / 8 > r->r.len || !aml_os.write)
        return;
    uint64_t addr = r->r.base + off;
    if (r->r.space == AML_SPACE_PCI)
        addr = region_pci(r) | ((r->r.base + off) & 0xffff);
    if (r->r.space == AML_SPACE_MEMORY || r->r.space == AML_SPACE_IO || r->r.space == AML_SPACE_PCI ||
        r->r.space == AML_SPACE_EC || r->r.space == AML_SPACE_CMOS)
        aml_os.write(r->r.space, addr, bits, v);
}

/* the field's bits into out (zeroed, (bit_len + 7) / 8 bytes) */
static void field_read_bits(struct frame *f, struct aml_obj *fo, uint8_t *out)
{
    int w = access_bits(fo->f.flags);
    uint64_t first = fo->f.bit_off, last = first + fo->f.bit_len;
    for (uint64_t u = first / (unsigned)w * (unsigned)w; u < last && !f->flow; u += (unsigned)w) {
        uint64_t v = unit_read(f, fo, u / 8, w);
        for (uint64_t b = MAX(u, first); b < MIN(u + (unsigned)w, last); b++)
            if (v >> (b - u) & 1)
                out[(b - first) / 8] |= (uint8_t)(1u << ((b - first) & 7));
    }
}

static void field_write_bits(struct frame *f, struct aml_obj *fo, const uint8_t *in, uint32_t in_len)
{
    int w = access_bits(fo->f.flags);
    uint64_t first = fo->f.bit_off, last = first + fo->f.bit_len;
    int update = fo->f.flags >> 5 & 3;
    for (uint64_t u = first / (unsigned)w * (unsigned)w; u < last && !f->flow; u += (unsigned)w) {
        uint64_t lo = MAX(u, first), hi = MIN(u + (unsigned)w, last);
        uint64_t v = 0;
        if (lo != u || hi != u + (unsigned)w)
            v = update == 1 ? ~0ull : update == 2 ? 0 : unit_read(f, fo, u / 8, w);
        for (uint64_t b = lo; b < hi; b++) {
            uint64_t i = b - first;
            bool bit = i / 8 < in_len && (in[i / 8] >> (i & 7) & 1);
            v = bit ? v | 1ull << (b - u) : v & ~(1ull << (b - u));
        }
        unit_write(f, fo, u / 8, w, v);
    }
}

static struct aml_obj *bits_value(const uint8_t *bits, uint32_t bit_len)
{
    uint32_t n = (bit_len + 7) / 8;
    if (bit_len <= (uint32_t)int_bits) {
        uint64_t v = 0;
        for (uint32_t i = 0; i < n; i++)
            v |= (uint64_t)bits[i] << (8 * i);
        return aml_int(v);
    }
    return obj_bytes(AML_BUF, bits, n);
}

static struct aml_obj *field_read(struct frame *f, struct aml_obj *fo)
{
    if (fo->f.bit_len > MAX_BYTES * 8) {
        fail(f, "field too long");
        return NULL;
    }
    uint8_t small[8] = {0};
    uint32_t n = (fo->f.bit_len + 7) / 8;
    uint8_t *bits = n <= 8 ? small : kzalloc(n);
    if (!bits)
        return NULL;
    field_read_bits(f, fo, bits);
    struct aml_obj *v = bits_value(bits, fo->f.bit_len);
    if (bits != small)
        kfree(bits);
    return v;
}

/* the bytes of v for writing into a field */
static const uint8_t *value_bytes(struct aml_obj *v, uint8_t tmp[8], uint32_t *len)
{
    if (v->type == AML_STR || v->type == AML_BUF) {
        *len = v->buf.len;
        return v->buf.data;
    }
    for (int i = 0; i < 8; i++)
        tmp[i] = (uint8_t)(v->ival >> (8 * i));
    *len = v->type == AML_INT ? 8 : 0;
    return tmp;
}

static void field_write(struct frame *f, struct aml_obj *fo, struct aml_obj *v)
{
    uint8_t tmp[8];
    uint32_t len;
    const uint8_t *in = value_bytes(v, tmp, &len);
    field_write_bits(f, fo, in, len);
}

static uint64_t field_get(struct frame *f, struct aml_node *n)
{
    uint64_t v = 0;
    if (!n || !n->obj || n->obj->type != AML_FIELD || ++nest > MAX_NEST) {
        nest -= n && n->obj && n->obj->type == AML_FIELD;
        fail(f, "bad index field");
        return 0;
    }
    struct aml_obj *r = field_read(f, n->obj);
    nest--;
    if (r && r->type == AML_INT)
        v = r->ival;
    aml_put(r);
    return v;
}

static void field_set(struct frame *f, struct aml_node *n, uint64_t v)
{
    if (!n || !n->obj || n->obj->type != AML_FIELD || ++nest > MAX_NEST) {
        nest -= n && n->obj && n->obj->type == AML_FIELD;
        fail(f, "bad index field");
        return;
    }
    struct aml_obj tmp = {.type = AML_INT, .refs = 1, .ival = v};
    field_write(f, n->obj, &tmp);
    nest--;
}

static bool bfield_ok(struct aml_obj *bf)
{
    struct aml_obj *b = bf->bf.buf;
    return (b->type == AML_BUF || b->type == AML_STR) &&
           (uint64_t)bf->bf.bit_off + bf->bf.bit_len <= (uint64_t)b->buf.len * 8;
}

static struct aml_obj *bfield_read(struct aml_obj *bf)
{
    if (!bfield_ok(bf))
        return aml_int(0);
    uint32_t n = (bf->bf.bit_len + 7) / 8;
    uint8_t *bits = kzalloc(n + 1);
    if (!bits)
        return NULL;
    const uint8_t *src = bf->bf.buf->buf.data;
    for (uint32_t i = 0; i < bf->bf.bit_len; i++) {
        uint32_t b = bf->bf.bit_off + i;
        if (src[b / 8] >> (b & 7) & 1)
            bits[i / 8] |= (uint8_t)(1u << (i & 7));
    }
    struct aml_obj *v = bits_value(bits, bf->bf.bit_len);
    kfree(bits);
    return v;
}

static void bfield_write(struct aml_obj *bf, struct aml_obj *v)
{
    if (!bfield_ok(bf))
        return;
    uint8_t tmp[8];
    uint32_t len;
    const uint8_t *in = value_bytes(v, tmp, &len);
    uint8_t *dst = bf->bf.buf->buf.data;
    for (uint32_t i = 0; i < bf->bf.bit_len; i++) {
        uint32_t b = bf->bf.bit_off + i;
        bool bit = i / 8 < len && (in[i / 8] >> (i & 7) & 1);
        dst[b / 8] = bit ? (uint8_t)(dst[b / 8] | 1u << (b & 7)) : (uint8_t)(dst[b / 8] & ~(1u << (b & 7)));
    }
}

/* ---- values of names, references and targets ---- */

static struct aml_obj *read_node(struct frame *f, struct aml_node *n)
{
    struct aml_obj *o = n->obj;
    if (!o) {
        fail(f, "name without a value");
        return NULL;
    }
    if (o->type == AML_FIELD) {
        if (++nest > MAX_NEST) {
            nest--;
            fail(f, "fields nest too deep");
            return NULL;
        }
        struct aml_obj *v = field_read(f, o);
        nest--;
        return v;
    }
    if (o->type == AML_BUFFIELD)
        return bfield_read(o);
    return obj_get(o);
}

static struct aml_node *resolve_nameref(struct aml_obj *nr)
{
    struct name nm;
    if (!text_name(nr->nr.path, &nm))
        return NULL;
    return lookup(nr->nr.scope, &nm, true);
}

/* replaces *v, a reference, by what it refers to */
static void deref(struct frame *f, struct aml_obj **v)
{
    struct aml_obj *r = *v;
    if (!r || r->type != AML_REF)
        return;
    struct aml_obj *out = NULL;
    if (r->ref.kind == REF_NODE) {
        out = read_node(f, r->ref.node);
    } else if (r->ref.kind == REF_BYTE) {
        struct aml_obj *b = r->ref.target;
        out = aml_int(r->ref.index < b->buf.len ? b->buf.data[r->ref.index] : 0);
    } else {
        struct aml_obj *p = r->ref.target;
        struct aml_obj *e = r->ref.index < p->pkg.count ? p->pkg.items[r->ref.index] : NULL;
        if (e && e->type == AML_NAMEREF) {
            struct aml_node *n = resolve_nameref(e);
            out = n ? read_node(f, n) : NULL;
            if (!n)
                fail(f, "unknown name in a package");
        } else {
            out = e ? obj_get(e) : aml_int(0);
        }
    }
    aml_put(r);
    *v = out;
}

static void store_ref(struct frame *f, struct aml_obj *r, struct aml_obj *v)
{
    if (r->ref.kind == REF_NODE) {
        store_node(f, r->ref.node, v);
    } else if (r->ref.kind == REF_BYTE) {
        uint64_t x;
        if (to_int(f, v, &x) && r->ref.index < r->ref.target->buf.len)
            r->ref.target->buf.data[r->ref.index] = (uint8_t)x;
    } else if (r->ref.index < r->ref.target->pkg.count) {
        /* an element gets the value of an Index reference, never the reference: it could point into the package
           itself, a cycle no count frees */
        struct aml_obj *val = obj_get(v);
        if (val->type == AML_REF && val->ref.kind != REF_NODE)
            deref(f, &val);
        if (!val)
            return;
        struct aml_obj **slot = &r->ref.target->pkg.items[r->ref.index];
        aml_put(*slot);
        *slot = obj_copy(val);
        aml_put(val);
    }
}

static void store_node(struct frame *f, struct aml_node *n, struct aml_obj *v)
{
    struct aml_obj *o = n->obj;
    int type = o ? o->type : AML_NONE;
    if (v->type == AML_REF && (type == AML_FIELD || type == AML_BUFFIELD || type == AML_INT || type == AML_STR ||
                               type == AML_BUF)) {
        struct aml_obj *d = obj_get(v);
        deref(f, &d);
        if (d && d->type != AML_REF)
            store_node(f, n, d);
        aml_put(d);
        return;
    }
    if (type == AML_FIELD) {
        if (++nest > MAX_NEST) {
            fail(f, "fields nest too deep");
        } else if (v->type == AML_INT || v->type == AML_BUF || v->type == AML_STR) {
            field_write(f, o, v);
        } else {
            fail(f, "bad value for a field");
        }
        nest--;
        return;
    }
    if (type == AML_BUFFIELD) {
        if (v->type == AML_INT || v->type == AML_BUF || v->type == AML_STR)
            bfield_write(o, v);
        return;
    }
    struct aml_obj *nv = NULL;
    if (type == AML_INT) {
        uint64_t x;
        if (!to_int(f, v, &x))
            return;
        nv = aml_int(x);
    } else if (type == AML_BUF && (v->type == AML_BUF || v->type == AML_INT || v->type == AML_STR)) {
        /* in place, at the same length: buffer fields made on it keep pointing at it */
        uint8_t tmp[8];
        uint32_t len;
        const uint8_t *in = value_bytes(v, tmp, &len);
        if (v->type == AML_INT)
            len = (uint32_t)int_bits / 8;
        if (!o->buf.len) {
            uint8_t *d = kzalloc(len + 1);
            if (!d)
                return;
            kfree(o->buf.data);
            o->buf.data = d;
            o->buf.len = len;
        }
        uint32_t n = MIN(len, o->buf.len);
        memmove(o->buf.data, in, n);
        memset(o->buf.data + n, 0, o->buf.len - n);
        return;
    } else if (type == AML_STR && v->type != AML_PKG) {
        nv = to_string(f, v);
        if (nv == v) {
            aml_put(nv);
            nv = obj_copy(v);
        }
    } else if (type == AML_INT || type == AML_STR || type == AML_BUF || type == AML_PKG || type == AML_NONE ||
               type == AML_REF || type == AML_NAMEREF) {
        nv = obj_copy(v);
    } else {
        return; /* devices, methods, regions: Store leaves them alone */
    }
    if (!nv)
        return;
    aml_put(n->obj);
    n->obj = nv;
}

static bool parse_target(struct frame *f, struct cur *c, struct target *t)
{
    memset(t, 0, sizeof(*t));
    if (c->p >= c->end) {
        fail(f, "target cut off");
        return false;
    }
    uint8_t op = *c->p;
    if (op == 0x00) {
        c->p++;
        t->kind = T_NONE;
        return true;
    }
    if (op >= 0x60 && op <= 0x67) {
        c->p++;
        t->kind = T_LOCAL;
        t->i = op - 0x60;
        return true;
    }
    if (op >= 0x68 && op <= 0x6e) {
        c->p++;
        t->kind = T_ARG;
        t->i = op - 0x68;
        return true;
    }
    if (op == 0x5b && c->end - c->p >= 2 && c->p[1] == 0x31) {
        c->p += 2;
        t->kind = T_DEBUG;
        return true;
    }
    if (name_start(op)) {
        const uint8_t *save = c->p;
        struct name nm;
        if (!parse_name(f, c, &nm))
            return false;
        t->node = lookup(f->scope, &nm, true);
        if (!t->node) {
            fail(f, "unknown name");
            return false;
        }
        if (t->node->obj && t->node->obj->type == AML_METHOD) {
            c->p = save; /* a method that returns a reference */
        } else {
            t->kind = T_NODE;
            return true;
        }
    }
    struct aml_obj *r = eval(f, c);
    if (!r || r->type != AML_REF) {
        aml_put(r);
        fail(f, "bad target");
        return false;
    }
    t->kind = T_REF;
    t->ref = r;
    return true;
}

static void store(struct frame *f, struct target *t, struct aml_obj *v)
{
    if (!v || f->flow)
        return;
    struct aml_obj **slot = NULL;
    switch (t->kind) {
    case T_LOCAL:
        slot = &f->locals[t->i];
        break;
    case T_ARG:
        if (f->args[t->i] && f->args[t->i]->type == AML_REF) {
            store_ref(f, f->args[t->i], v);
            return;
        }
        slot = &f->args[t->i];
        break;
    case T_NODE:
        store_node(f, t->node, v);
        return;
    case T_REF:
        store_ref(f, t->ref, v);
        return;
    default:
        return;
    }
    struct aml_obj *nv = obj_copy(v);
    aml_put(*slot);
    *slot = nv;
}

static struct aml_obj *read_target(struct frame *f, struct target *t)
{
    struct aml_obj *v = NULL;
    switch (t->kind) {
    case T_LOCAL:
    case T_ARG:
        v = obj_get(t->kind == T_LOCAL ? f->locals[t->i] : f->args[t->i]);
        break;
    case T_NODE:
        return read_node(f, t->node);
    case T_REF:
        v = obj_get(t->ref);
        break;
    default:
        break;
    }
    if (!v) {
        fail(f, "no value");
        return NULL;
    }
    deref(f, &v);
    return v;
}

static void target_done(struct target *t)
{
    aml_put(t->ref);
    t->ref = NULL;
}

/* the result goes to the target that follows, and is returned too */
static struct aml_obj *to_target(struct frame *f, struct cur *c, struct aml_obj *v)
{
    struct target t;
    if (!v) {
        fail(f, "out of memory");
        return NULL;
    }
    if (!parse_target(f, c, &t)) {
        aml_put(v);
        return NULL;
    }
    store(f, &t, v);
    target_done(&t);
    return v;
}

/* ---- data ---- */

static struct aml_obj *parse_buffer(struct frame *f, struct cur *c)
{
    const uint8_t *end;
    if (!pkg_end(c, &end)) {
        fail(f, "bad buffer");
        return NULL;
    }
    struct cur in = {c->p, end};
    uint64_t size;
    c->p = end;
    if (!eval_int(f, &in, &size))
        return NULL;
    uint32_t given = (uint32_t)(end - in.p);
    if (size > MAX_BYTES) {
        fail(f, "buffer too large");
        return NULL;
    }
    struct aml_obj *b = obj_bytes(AML_BUF, NULL, MAX((uint32_t)size, given));
    if (b)
        memcpy(b->buf.data, in.p, given);
    return b;
}

/* a name inside a package, kept as text and resolved when it is used */
static __attribute__((noinline)) struct aml_obj *parse_nameref(struct frame *f, struct cur *c)
{
    struct name nm;
    char text[MAX_SEGS * 5 + 40];
    if (!parse_name(f, c, &nm))
        return NULL;
    name_text(&nm, text, sizeof(text));
    struct aml_obj *e = obj_new(AML_NAMEREF);
    if (e) {
        size_t n = strlen(text) + 1;
        e->nr.path = kzalloc(n);
        if (e->nr.path)
            str_copy(e->nr.path, text, n);
        e->nr.scope = node_get(f->scope);
    }
    if (!e || !e->nr.path) {
        aml_put(e);
        fail(f, "out of memory");
        return NULL;
    }
    return e;
}

static struct aml_obj *parse_package(struct frame *f, struct cur *c, bool var)
{
    const uint8_t *end;
    if (!pkg_end(c, &end)) {
        fail(f, "bad package");
        return NULL;
    }
    struct cur in = {c->p, end};
    c->p = end;
    uint64_t count;
    uint8_t b;
    if (var) {
        if (!eval_int(f, &in, &count))
            return NULL;
    } else {
        if (!take(&in, &b)) {
            fail(f, "bad package");
            return NULL;
        }
        count = b;
    }
    if (count > MAX_ITEMS) {
        fail(f, "package too large");
        return NULL;
    }
    struct aml_obj *p = obj_pkg((uint32_t)count);
    if (!p)
        return NULL;
    for (uint32_t i = 0; in.p < in.end && !f->flow; i++) {
        struct aml_obj *e;
        if (name_start(*in.p)) {
            e = parse_nameref(f, &in);
            if (!e)
                break;
        } else {
            e = eval(f, &in);
        }
        if (i < p->pkg.count)
            p->pkg.items[i] = e;
        else
            aml_put(e); /* more elements than the count says: the count wins */
    }
    if (f->flow) {
        aml_put(p);
        return NULL;
    }
    return p;
}

/* ---- operators ---- */

static bool same_bytes_type(struct aml_obj *a)
{
    return a->type == AML_STR || a->type == AML_BUF;
}

/* a compared with b, as LEqual, LGreater and LLess do: <0, 0 or >0 */
static int compare(struct frame *f, struct aml_obj *a, struct aml_obj *b)
{
    if (same_bytes_type(a)) {
        struct aml_obj *bb = a->type == AML_STR ? to_string(f, b) : to_buffer(f, b);
        if (!bb)
            return 0;
        uint32_t n = MIN(a->buf.len, bb->buf.len);
        int r = memcmp(a->buf.data, bb->buf.data, n);
        if (!r)
            r = a->buf.len < bb->buf.len ? -1 : a->buf.len > bb->buf.len;
        aml_put(bb);
        return r;
    }
    uint64_t x, y;
    if (!to_int(f, a, &x) || !to_int(f, b, &y))
        return 0;
    return x < y ? -1 : x > y;
}

static struct aml_obj *concat(struct frame *f, struct aml_obj *a, struct aml_obj *b)
{
    struct aml_obj *x, *y, *r = NULL;
    if (a->type == AML_STR) {
        x = obj_get(a);
        y = to_string(f, b);
    } else {
        x = to_buffer(f, a);
        y = a->type == AML_INT ? NULL : to_buffer(f, b);
        if (a->type == AML_INT) {
            uint64_t v;
            if (to_int(f, b, &v)) {
                struct aml_obj t = {.type = AML_INT, .refs = 1, .ival = v};
                y = to_buffer(f, &t);
            }
        }
    }
    if (x && y && (uint64_t)x->buf.len + y->buf.len <= MAX_BYTES) {
        r = obj_bytes(x->type, NULL, x->buf.len + y->buf.len);
        if (r) {
            memcpy(r->buf.data, x->buf.data, x->buf.len);
            memcpy(r->buf.data + x->buf.len, y->buf.data, y->buf.len);
        }
    } else if (x && y) {
        fail(f, "concatenation too long");
    }
    aml_put(x);
    aml_put(y);
    return r;
}

static uint32_t template_len(const struct aml_obj *b)
{
    uint32_t n = b->buf.len;
    return n >= 2 && b->buf.data[n - 2] == 0x79 ? n - 2 : n;
}

static struct aml_obj *concat_res(struct frame *f, struct aml_obj *a, struct aml_obj *b)
{
    if (a->type != AML_BUF || b->type != AML_BUF) {
        fail(f, "ConcatenateResTemplate of a non-buffer");
        return NULL;
    }
    uint32_t la = template_len(a), lb = template_len(b);
    if ((uint64_t)la + lb + 2 > MAX_BYTES) {
        fail(f, "template too long");
        return NULL;
    }
    struct aml_obj *r = obj_bytes(AML_BUF, NULL, la + lb + 2);
    if (r) {
        memcpy(r->buf.data, a->buf.data, la);
        memcpy(r->buf.data + la, b->buf.data, lb);
        r->buf.data[la + lb] = 0x79;
    }
    return r;
}

static struct aml_obj *mid(struct frame *f, struct aml_obj *s, uint64_t at, uint64_t len)
{
    if (!same_bytes_type(s)) {
        fail(f, "Mid of a non-string");
        return NULL;
    }
    uint32_t start = (uint32_t)MIN(at, (uint64_t)s->buf.len);
    uint32_t n = (uint32_t)MIN(len, (uint64_t)(s->buf.len - start));
    return obj_bytes(s->type, s->buf.data + start, n);
}

static bool match_one(int op, uint64_t a, uint64_t b)
{
    switch (op) {
    case 0:
        return true;
    case 1:
        return a == b;
    case 2:
        return a <= b;
    case 3:
        return a < b;
    case 4:
        return a >= b;
    case 5:
        return a > b;
    default:
        return false;
    }
}

static struct aml_obj *op_match(struct frame *f, struct cur *c)
{
    struct aml_obj *p = eval_value(f, c);
    uint8_t op1 = 0, op2 = 0;
    uint64_t v1 = 0, v2 = 0, start = 0;
    bool ok = p && take(c, &op1) && eval_int(f, c, &v1) && take(c, &op2) && eval_int(f, c, &v2) &&
              eval_int(f, c, &start);
    if (!ok || p->type != AML_PKG) {
        aml_put(p);
        fail(f, "bad Match");
        return NULL;
    }
    uint64_t found = ones();
    for (uint64_t i = start; i < p->pkg.count; i++) {
        struct aml_obj *e = p->pkg.items[i];
        if (!e || e->type != AML_INT)
            continue;
        if (match_one(op1, e->ival, v1) && match_one(op2, e->ival, v2)) {
            found = i;
            break;
        }
    }
    aml_put(p);
    return aml_int(found);
}

static struct aml_obj *op_index(struct frame *f, struct cur *c)
{
    struct aml_obj *src = eval_value(f, c);
    uint64_t i;
    if (!src || !eval_int(f, c, &i)) {
        aml_put(src);
        return NULL;
    }
    int kind = src->type == AML_PKG ? REF_ELEM : same_bytes_type(src) ? REF_BYTE : -1;
    uint64_t size = kind == REF_ELEM ? src->pkg.count : kind == REF_BYTE ? src->buf.len : 0;
    if (kind < 0 || i >= size) {
        aml_put(src);
        fail(f, kind < 0 ? "Index of a non-package" : "Index past the end");
        return NULL;
    }
    struct aml_obj *r = obj_new(AML_REF);
    if (!r) {
        aml_put(src);
        return NULL;
    }
    r->ref.kind = (uint8_t)kind;
    r->ref.target = src;
    r->ref.index = (uint32_t)i;
    return to_target(f, c, r);
}

/* Name, Device, Field and the other terms that add names */
static bool named_op(struct frame *f, struct cur *c, uint8_t op, uint8_t ext);

static struct aml_obj *op_ref(struct frame *f, struct cur *c, bool cond)
{
    if (c->p < c->end && name_start(*c->p)) {
        struct name nm;
        if (!parse_name(f, c, &nm))
            return NULL;
        struct aml_node *n = lookup(f->scope, &nm, true);
        if (!n && !cond) {
            fail(f, "RefOf an unknown name");
            return NULL;
        }
        struct aml_obj *r = n ? obj_new(AML_REF) : NULL;
        if (r) {
            r->ref.kind = REF_NODE;
            r->ref.node = node_get(n);
        }
        if (!cond)
            return r;
        if (r)
            aml_put(to_target(f, c, r));
        else
            aml_put(to_target(f, c, aml_int(0)));
        return aml_int(n ? ones() : 0);
    }
    struct target t;
    if (!parse_target(f, c, &t))
        return NULL;
    struct aml_obj *r = t.kind == T_REF ? obj_get(t.ref) : NULL;
    if (!r && (t.kind == T_LOCAL || t.kind == T_ARG)) {
        struct aml_obj *v = t.kind == T_LOCAL ? f->locals[t.i] : f->args[t.i];
        r = v && v->type == AML_REF ? obj_get(v) : NULL;
    }
    target_done(&t);
    if (!cond) {
        if (!r)
            fail(f, "RefOf a local");
        return r;
    }
    aml_put(to_target(f, c, r ? obj_get(r) : aml_int(0)));
    aml_put(r);
    return aml_int(r ? ones() : 0);
}

static int type_of(struct aml_obj *o)
{
    if (!o)
        return 0;
    if (o->type == AML_SCOPE || o->type == AML_NAMEREF || o->type == AML_REF)
        return 0;
    return o->type;
}

static struct aml_obj *op_type_size(struct frame *f, struct cur *c, bool size)
{
    struct target t;
    if (!parse_target(f, c, &t))
        return NULL;
    struct aml_obj *v = NULL;
    if (!size && t.kind == T_NODE) {
        target_done(&t);
        return aml_int((uint64_t)type_of(t.node->obj));
    }
    v = read_target(f, &t);
    target_done(&t);
    if (!v)
        return NULL;
    uint64_t r = 0;
    if (!size)
        r = (uint64_t)type_of(v);
    else if (same_bytes_type(v))
        r = v->buf.len;
    else if (v->type == AML_PKG)
        r = v->pkg.count;
    else
        fail(f, "SizeOf a non-string");
    aml_put(v);
    return f->flow ? NULL : aml_int(r);
}

static struct aml_obj *op_incdec(struct frame *f, struct cur *c, int delta)
{
    struct target t;
    if (!parse_target(f, c, &t))
        return NULL;
    struct aml_obj *v = read_target(f, &t);
    uint64_t x;
    struct aml_obj *r = NULL;
    if (v && to_int(f, v, &x)) {
        r = aml_int((x + (uint64_t)(int64_t)delta) & ones());
        store(f, &t, r);
    }
    aml_put(v);
    target_done(&t);
    return r;
}

static uint64_t from_bcd(uint64_t v)
{
    uint64_t r = 0, m = 1;
    for (; v; v >>= 4, m *= 10)
        r += (v & 15) * m;
    return r;
}

static uint64_t to_bcd(uint64_t v)
{
    uint64_t r = 0;
    for (int s = 0; v && s < 64; v /= 10, s += 4)
        r |= (v % 10) << s;
    return r;
}

/* the integer operators that take two operands and a target */
static struct aml_obj *op_binary(struct frame *f, struct cur *c, uint8_t op)
{
    uint64_t a, b, r = 0;
    if (!eval_int(f, c, &a) || !eval_int(f, c, &b))
        return NULL;
    switch (op) {
    case 0x72:
        r = a + b;
        break;
    case 0x74:
        r = a - b;
        break;
    case 0x77:
        r = a * b;
        break;
    case 0x79:
        r = b >= 64 ? 0 : a << b;
        break;
    case 0x7a:
        r = b >= 64 ? 0 : a >> b;
        break;
    case 0x7b:
        r = a & b;
        break;
    case 0x7c:
        r = ~(a & b);
        break;
    case 0x7d:
        r = a | b;
        break;
    case 0x7e:
        r = ~(a | b);
        break;
    case 0x7f:
        r = a ^ b;
        break;
    case 0x85:
        if (!b) {
            fail(f, "Mod by zero");
            return NULL;
        }
        r = a % b;
        break;
    default:
        break;
    }
    return to_target(f, c, aml_int(r & ones()));
}

static struct aml_obj *op_unary(struct frame *f, struct cur *c, uint8_t op)
{
    uint64_t a, r = 0;
    if (!eval_int(f, c, &a))
        return NULL;
    if (op == 0x80) {
        r = ~a;
    } else if (op == 0x81) {
        for (int i = 63; i >= 0; i--)
            if (a >> i & 1) {
                r = (uint64_t)i + 1;
                break;
            }
    } else if (op == 0x82) {
        for (int i = 0; i < 64; i++)
            if (a >> i & 1) {
                r = (uint64_t)i + 1;
                break;
            }
    } else if (op == 0x28) {
        r = from_bcd(a);
    } else {
        r = to_bcd(a);
    }
    return to_target(f, c, aml_int(r & ones()));
}

static struct aml_obj *op_divide(struct frame *f, struct cur *c)
{
    uint64_t a, b;
    if (!eval_int(f, c, &a) || !eval_int(f, c, &b))
        return NULL;
    if (!b) {
        fail(f, "divide by zero");
        return NULL;
    }
    aml_put(to_target(f, c, aml_int(a % b)));
    if (f->flow)
        return NULL;
    return to_target(f, c, aml_int(a / b));
}

static struct aml_obj *op_logic(struct frame *f, struct cur *c, uint8_t op)
{
    if (op == 0x92) {
        uint64_t a;
        return eval_int(f, c, &a) ? aml_int(a ? 0 : ones()) : NULL;
    }
    if (op == 0x90 || op == 0x91) {
        uint64_t a, b;
        if (!eval_int(f, c, &a) || !eval_int(f, c, &b))
            return NULL;
        return aml_int((op == 0x90 ? a && b : a || b) ? ones() : 0);
    }
    struct aml_obj *a = eval_value(f, c), *b = a ? eval_value(f, c) : NULL;
    if (!a || !b) {
        aml_put(a);
        aml_put(b);
        return NULL;
    }
    int r = compare(f, a, b);
    aml_put(a);
    aml_put(b);
    if (f->flow)
        return NULL;
    bool t = op == 0x93 ? r == 0 : op == 0x94 ? r > 0 : r < 0;
    return aml_int(t ? ones() : 0);
}

static struct aml_obj *op_convert(struct frame *f, struct cur *c, uint8_t op)
{
    struct aml_obj *v = eval_value(f, c), *r = NULL;
    if (!v)
        return NULL;
    if (op == 0x96) {
        r = to_buffer(f, v);
    } else if (op == 0x99) {
        uint64_t x = 0;
        if (v->type == AML_STR) {
            const char *s = (const char *)v->buf.data;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                to_int(f, v, &x);
            } else {
                for (; is_digit(*s); s++)
                    x = x * 10 + (uint64_t)(*s - '0');
            }
            r = aml_int(x & ones());
        } else if (to_int(f, v, &x)) {
            r = aml_int(x);
        }
    } else if (op == 0x97) {
        char s[24];
        uint64_t x;
        if (v->type == AML_INT) {
            ksnprintf(s, sizeof(s), "%llu", (unsigned long long)v->ival);
            r = obj_str(s);
        } else if (v->type == AML_BUF) {
            r = obj_bytes(AML_STR, NULL, 0);
            for (uint32_t i = 0; r && i < v->buf.len && i < 4096; i++) {
                ksnprintf(s, sizeof(s), i ? ",%u" : "%u", v->buf.data[i]);
                struct aml_obj t = {.type = AML_STR, .refs = 1, .buf = {(uint32_t)strlen(s), (uint8_t *)s}};
                struct aml_obj *n = concat(f, r, &t);
                aml_put(r);
                r = n;
            }
        } else if (to_int(f, v, &x)) {
            r = obj_get(v);
        }
    } else {
        r = to_string(f, v);
    }
    aml_put(v);
    if (!r) {
        if (!f->flow)
            fail(f, "conversion failed");
        return NULL;
    }
    return to_target(f, c, r);
}

static struct aml_obj *op_tostring(struct frame *f, struct cur *c)
{
    struct aml_obj *v = eval_value(f, c);
    uint64_t len;
    if (!v || !eval_int(f, c, &len) || v->type != AML_BUF) {
        if (v && !f->flow)
            fail(f, "ToString of a non-buffer");
        aml_put(v);
        return NULL;
    }
    uint32_t n = 0;
    while (n < v->buf.len && n < len && v->buf.data[n])
        n++;
    struct aml_obj *r = obj_bytes(AML_STR, v->buf.data, n);
    aml_put(v);
    return to_target(f, c, r);
}

/* a method call: the arguments follow its name */
static struct aml_obj *call(struct frame *f, struct cur *c, struct aml_node *n)
{
    struct aml_obj *args[7] = {0};
    int argc = n->obj->m.flags & 7;
    for (int i = 0; i < argc && !f->flow; i++)
        args[i] = eval(f, c);
    struct aml_obj *r = NULL;
    if (!f->flow) {
        bool ok;
        r = invoke(n, args, argc, &ok);
        if (!ok)
            f->flow = FLOW_ERROR; /* aml_last_error says why */
    }
    for (int i = 0; i < argc; i++)
        aml_put(args[i]);
    if (f->flow) {
        aml_put(r);
        return NULL;
    }
    return r ? r : aml_int(0);
}

/* out of line: its buffer stays off the stack of the recursion */
static __attribute__((noinline)) void fail_name(struct frame *f, const struct name *nm)
{
    char text[48];
    name_text(nm, text, sizeof(text));
    if (f->flow != FLOW_ERROR)
        ksnprintf(aml_last_error, sizeof(aml_last_error), "unknown name %s", text);
    f->flow = FLOW_ERROR;
}

static struct aml_obj *eval_name(struct frame *f, struct cur *c)
{
    struct name nm;
    if (!parse_name(f, c, &nm))
        return NULL;
    struct aml_node *n = lookup(f->scope, &nm, true);
    if (!n) {
        fail_name(f, &nm);
        return NULL;
    }
    if (n->obj && n->obj->type == AML_METHOD)
        return call(f, c, n);
    return read_node(f, n);
}

static struct aml_obj *op_ext(struct frame *f, struct cur *c, uint8_t op)
{
    uint64_t a;
    struct target t;
    switch (op) {
    case 0x12:
        return op_ref(f, c, true);
    case 0x21:
    case 0x22:
        if (!eval_int(f, c, &a))
            return NULL;
        if (op == 0x21 && aml_os.stall_us)
            aml_os.stall_us((uint32_t)MIN(a, 255u));
        if (op == 0x22 && aml_os.sleep_ms)
            aml_os.sleep_ms((uint32_t)MIN(a, 2000u));
        return aml_int(0);
    case 0x23: /* Acquire: one thread runs AML, so a mutex is always free */
        if (!parse_target(f, c, &t))
            return NULL;
        target_done(&t);
        if (c->end - c->p < 2) {
            fail(f, "Acquire cut off");
            return NULL;
        }
        c->p += 2;
        return aml_int(0);
    case 0x24:
    case 0x26:
    case 0x27:
        if (!parse_target(f, c, &t))
            return NULL;
        target_done(&t);
        return aml_int(0);
    case 0x25:
        if (!parse_target(f, c, &t))
            return NULL;
        target_done(&t);
        return eval_int(f, c, &a) ? aml_int(0) : NULL;
    case 0x28:
    case 0x29:
        return op_unary(f, c, op);
    case 0x30:
        return aml_int(2);
    case 0x31:
        return aml_int(0);
    case 0x32: {
        uint64_t code = 0;
        if (c->end - c->p < 5) {
            fail(f, "Fatal cut off");
            return NULL;
        }
        c->p++;
        take_le(c, 4, &code);
        if (!eval_int(f, c, &a))
            return NULL;
        klog("acpi: Fatal %llx (%llx)", (unsigned long long)code, (unsigned long long)a);
        return aml_int(0);
    }
    case 0x33:
        return aml_int(aml_os.timer ? aml_os.timer() : 0);
    case 0x20: /* Load: tables from memory are not loaded */
        if (!parse_target(f, c, &t))
            return NULL;
        target_done(&t);
        return to_target(f, c, aml_int(0));
    case 0x1f:
        for (int i = 0; i < 6; i++) {
            struct aml_obj *v = eval(f, c);
            if (!v)
                return NULL;
            aml_put(v);
        }
        return aml_int(0);
    case 0x2a:
        if (!parse_target(f, c, &t))
            return NULL;
        target_done(&t);
        return aml_int(0);
    default:
        fail(f, "unknown extended opcode");
        return NULL;
    }
}

static struct aml_obj *eval_op(struct frame *f, struct cur *c)
{
    uint8_t op = *c->p++;
    uint64_t v;
    switch (op) {
    case 0x00:
        return aml_int(0);
    case 0x01:
        return aml_int(1);
    case 0xff:
        return aml_int(ones());
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0e:
        if (!take_le(c, op == 0x0a ? 1 : op == 0x0b ? 2 : op == 0x0c ? 4 : 8, &v)) {
            fail(f, "constant cut off");
            return NULL;
        }
        return aml_int(v);
    case 0x0d: {
        const uint8_t *z = memchr(c->p, 0, (size_t)(c->end - c->p));
        if (!z) {
            fail(f, "string cut off");
            return NULL;
        }
        struct aml_obj *s = obj_bytes(AML_STR, c->p, (uint32_t)(z - c->p));
        c->p = z + 1;
        return s;
    }
    case 0x11:
        return parse_buffer(f, c);
    case 0x12:
    case 0x13:
        return parse_package(f, c, op == 0x13);
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: {
        struct aml_obj *l = f->locals[op - 0x60];
        return l ? obj_get(l) : aml_int(0);
    }
    case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: {
        struct aml_obj *a = f->args[op - 0x68];
        return a ? obj_get(a) : aml_int(0);
    }
    case 0x70: {
        struct aml_obj *val = eval(f, c);
        return val ? to_target(f, c, val) : NULL;
    }
    case 0x9d: { /* CopyObject: the target takes the object as it is */
        struct aml_obj *val = eval_value(f, c);
        struct target t;
        if (!val || !parse_target(f, c, &t)) {
            aml_put(val);
            return NULL;
        }
        if (t.kind == T_NODE) {
            aml_put(t.node->obj);
            t.node->obj = obj_copy(val);
        } else {
            store(f, &t, val);
        }
        target_done(&t);
        return val;
    }
    case 0x71:
        return op_ref(f, c, false);
    case 0x73:
    case 0x84: {
        struct aml_obj *a = eval_value(f, c), *b = a ? eval_value(f, c) : NULL, *r = NULL;
        if (a && b)
            r = op == 0x73 ? concat(f, a, b) : concat_res(f, a, b);
        aml_put(a);
        aml_put(b);
        return r ? to_target(f, c, r) : NULL;
    }
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
    case 0x85:
        return op_binary(f, c, op);
    case 0x75:
    case 0x76:
        return op_incdec(f, c, op == 0x75 ? 1 : -1);
    case 0x78:
        return op_divide(f, c);
    case 0x80:
    case 0x81:
    case 0x82:
        return op_unary(f, c, op);
    case 0x83: {
        struct aml_obj *r = eval(f, c);
        if (r && r->type == AML_STR) {
            struct name nm;
            struct aml_node *n = text_name((const char *)r->buf.data, &nm) ? lookup(f->scope, &nm, true) : NULL;
            aml_put(r);
            if (!n) {
                fail(f, "DerefOf an unknown name");
                return NULL;
            }
            return read_node(f, n);
        }
        deref(f, &r); /* DerefOf of a plain value gives the value, as ACPICA lets it */
        return r;
    }
    case 0x86: {
        struct target t;
        if (!parse_target(f, c, &t) || !eval_int(f, c, &v)) {
            target_done(&t);
            return NULL;
        }
        struct aml_node *n = t.kind == T_NODE ? t.node : NULL;
        if (t.kind == T_REF && t.ref->ref.kind == REF_NODE)
            n = t.ref->ref.node;
        if (n && aml_os.notify)
            aml_os.notify(n, v);
        target_done(&t);
        return aml_int(0);
    }
    case 0x87:
    case 0x8e:
        return op_type_size(f, c, op == 0x87);
    case 0x88:
        return op_index(f, c);
    case 0x89:
        return op_match(f, c);
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
        return op_logic(f, c, op);
    case 0x96: case 0x97: case 0x98: case 0x99:
        return op_convert(f, c, op);
    case 0x9c:
        return op_tostring(f, c);
    case 0x9e: {
        struct aml_obj *s = eval_value(f, c), *r = NULL;
        uint64_t at, len;
        if (s && eval_int(f, c, &at) && eval_int(f, c, &len))
            r = mid(f, s, at, len);
        aml_put(s);
        return r ? to_target(f, c, r) : NULL;
    }
    case 0x5b: {
        uint8_t ext;
        if (!take(c, &ext)) {
            fail(f, "opcode cut off");
            return NULL;
        }
        return op_ext(f, c, ext);
    }
    default:
        fail(f, "unknown opcode");
        return NULL;
    }
}

static struct aml_obj *eval(struct frame *f, struct cur *c)
{
    if (f->flow)
        return NULL;
    if (c->p >= c->end) {
        fail(f, "operand cut off");
        return NULL;
    }
    if (++nest > MAX_NEST) {
        nest--;
        fail(f, "terms nest too deep");
        return NULL;
    }
    struct aml_obj *r = name_start(*c->p) ? eval_name(f, c) : eval_op(f, c);
    nest--;
    if (f->flow) {
        aml_put(r);
        return NULL;
    }
    if (!r)
        fail(f, "out of memory");
    return r;
}

/* ---- named objects ---- */

static bool make_field(struct frame *f, const char seg[4], int kind, uint8_t flags, uint32_t bit_off,
                       uint32_t bit_len, struct aml_node *a, struct aml_node *b, uint64_t bank)
{
    struct aml_obj *o = obj_new(AML_FIELD);
    if (!o) {
        fail(f, "out of memory");
        return false;
    }
    o->f.kind = (uint8_t)kind;
    o->f.flags = flags;
    o->f.bit_off = bit_off;
    o->f.bit_len = bit_len;
    if (kind == F_INDEX) {
        o->f.index = node_get(a);
        o->f.data = node_get(b);
    } else {
        o->f.region = node_get(a);
        o->f.index = node_get(b);
        o->f.bank_value = bank;
    }
    struct name nm = {.n = 1};
    memcpy(nm.seg[0], seg, 4);
    return create(f, &nm, o) != NULL || f->load;
}

static void field_list(struct frame *f, struct cur *c, int kind, struct aml_node *a, struct aml_node *b,
                       uint64_t bank)
{
    uint8_t flags;
    if (!take(c, &flags)) {
        fail(f, "field cut off");
        return;
    }
    uint64_t bit = 0;
    while (c->p < c->end && !f->flow) {
        uint8_t k = *c->p;
        uint32_t len;
        if (k == 0x00) {
            c->p++;
            if (!pkg_value(c, &len))
                break;
            bit += len;
        } else if (k == 0x01 || k == 0x03) {
            if (c->end - c->p < (k == 0x01 ? 3 : 4))
                break;
            flags = (uint8_t)((flags & ~15) | (c->p[1] & 15));
            c->p += k == 0x01 ? 3 : 4;
        } else if (k == 0x02) {
            c->p++;
            if (c->p < c->end && *c->p == 0x11) {
                aml_put(eval(f, c));
            } else {
                struct name nm;
                parse_name(f, c, &nm);
            }
        } else {
            if (c->end - c->p < 4)
                break;
            char seg[4];
            memcpy(seg, c->p, 4);
            c->p += 4;
            if (!pkg_value(c, &len))
                break;
            if (bit + len > 0xffffffffu) {
                fail(f, "field past 4 GiB");
                return;
            }
            if (!make_field(f, seg, kind, flags, (uint32_t)bit, len, a, b, bank))
                return;
            bit += len;
        }
    }
    if (c->p < c->end && !f->flow)
        fail(f, "bad field list");
}

/* a region, index or bank field term: names to resolve, then the list */
static void op_field(struct frame *f, struct cur *c, uint8_t ext)
{
    const uint8_t *end;
    if (!pkg_end(c, &end)) {
        fail(f, "bad field");
        return;
    }
    struct cur in = {c->p, end};
    c->p = end;
    struct name n1, n2;
    if (!parse_name(f, &in, &n1))
        return;
    struct aml_node *a = lookup(f->scope, &n1, true), *b = NULL;
    uint64_t bank = 0;
    if (ext != 0x81) {
        if (!parse_name(f, &in, &n2))
            return;
        b = lookup(f->scope, &n2, true);
    }
    if (ext == 0x87 && !eval_int(f, &in, &bank))
        return;
    if (!a || (ext != 0x81 && !b)) {
        fail(f, "field of an unknown region");
        return;
    }
    field_list(f, &in, ext == 0x81 ? F_REGION : ext == 0x86 ? F_INDEX : F_BANK, a, b, bank);
}

/* CreateBitField and the others: a field over the bytes of a buffer */
static void op_create_field(struct frame *f, struct cur *c, uint8_t op, bool any)
{
    struct aml_obj *src = eval(f, c);
    uint64_t index = 0, bits = 0;
    struct name nm;
    bool ok = src && eval_int(f, c, &index) && (!any || eval_int(f, c, &bits)) && parse_name(f, c, &nm);
    if (ok && src->type == AML_REF)
        deref(f, &src);
    if (!ok || !src || !same_bytes_type(src)) {
        aml_put(src);
        fail(f, "buffer field of a non-buffer");
        return;
    }
    if (!any) {
        bits = op == 0x8d ? 1 : op == 0x8c ? 8 : op == 0x8b ? 16 : op == 0x8a ? 32 : 64;
        if (op != 0x8d)
            index *= 8;
    }
    struct aml_obj *o = obj_new(AML_BUFFIELD);
    if (!o || index > 0xffffffffu || bits > 0xffffffffu) {
        aml_put(o);
        aml_put(src);
        fail(f, "bad buffer field");
        return;
    }
    o->bf.buf = src;
    o->bf.bit_off = (uint32_t)index;
    o->bf.bit_len = (uint32_t)bits;
    create(f, &nm, o);
}

/* a term list under the scope n (Device, Scope, ...) */
static void scope_body(struct frame *f, struct cur *body, struct aml_node *n)
{
    struct aml_node *save = f->scope;
    if (++nest > MAX_NEST) {
        nest--;
        fail(f, "scopes nest too deep");
        return;
    }
    f->scope = n;
    exec_list(f, body);
    f->scope = save;
    nest--;
    if (f->flow == FLOW_ERROR && f->load) {
        /* a bad term loses the rest of its scope, not the rest of the table */
        aml_errors++;
        f->flow = FLOW_NONE;
    }
}

static bool named_op(struct frame *f, struct cur *c, uint8_t op, uint8_t ext)
{
    struct name nm;
    const uint8_t *end;
    uint8_t b;
    uint64_t v1, v2;
    if (op == 0x08) {
        if (!parse_name(f, c, &nm))
            return true;
        create(f, &nm, eval(f, c));
        return true;
    }
    if (op == 0x06) {
        struct name to;
        if (!parse_name(f, c, &nm) || !parse_name(f, c, &to))
            return true;
        struct aml_node *src = lookup(f->scope, &nm, true);
        if (src && src->obj)
            create(f, &to, obj_get(src->obj));
        return true;
    }
    if (op == 0x15) {
        if (parse_name(f, c, &nm) && c->end - c->p >= 2)
            c->p += 2;
        else
            fail(f, "External cut off");
        return true;
    }
    if (op >= 0x8a && op <= 0x8f && op != 0x8e) {
        op_create_field(f, c, op, false);
        return true;
    }
    if (op == 0x10 || op == 0x14 || (op == 0x5b && (ext == 0x82 || ext == 0x83 || ext == 0x84 || ext == 0x85))) {
        if (!pkg_end(c, &end)) {
            fail(f, "bad package length");
            return true;
        }
        struct cur in = {c->p, end};
        c->p = end;
        if (!parse_name(f, &in, &nm))
            return true;
        if (op == 0x14) {
            if (!take(&in, &b)) {
                fail(f, "method cut off");
                return true;
            }
            struct aml_obj *m = obj_new(AML_METHOD);
            if (m) {
                m->m.code = in.p;
                m->m.len = (uint32_t)(in.end - in.p);
                m->m.flags = b;
            }
            create(f, &nm, m);
            return true;
        }
        struct aml_node *n;
        if (op == 0x10) {
            n = lookup(f->scope, &nm, true);
            if (!n) {
                if (f->load)
                    aml_errors++;
                else
                    fail(f, "Scope of an unknown name");
                return true;
            }
        } else {
            int type = ext == 0x82 ? AML_DEVICE : ext == 0x83 ? AML_PROCESSOR : ext == 0x84 ? AML_POWER : AML_THERMAL;
            int skip = ext == 0x83 ? 6 : ext == 0x84 ? 3 : 0;
            if (in.end - in.p < skip) {
                fail(f, "object cut off");
                return true;
            }
            in.p += skip;
            n = create(f, &nm, obj_new(type));
            if (!n)
                return true;
        }
        scope_body(f, &in, n);
        return true;
    }
    if (op != 0x5b)
        return false;
    switch (ext) {
    case 0x01:
    case 0x02:
        if (!parse_name(f, c, &nm))
            return true;
        if (ext == 0x01 && !take(c, &b)) {
            fail(f, "Mutex cut off");
            return true;
        }
        create(f, &nm, obj_new(ext == 0x01 ? AML_MUTEX : AML_EVENT));
        return true;
    case 0x13:
        op_create_field(f, c, 0, true);
        return true;
    case 0x80: {
        if (!parse_name(f, c, &nm) || !take(c, &b) || !eval_int(f, c, &v1) || !eval_int(f, c, &v2)) {
            fail(f, "bad OperationRegion");
            return true;
        }
        struct aml_obj *r = obj_new(AML_REGION);
        if (r) {
            r->r.space = b;
            r->r.base = v1;
            r->r.len = v2;
            struct name parent = nm;
            parent.n--;
            struct aml_node *dev = lookup(f->scope, &parent, false);
            while (dev && dev->obj && dev->obj->type == AML_METHOD)
                dev = dev->parent;
            r->r.dev = node_get(dev);
        }
        create(f, &nm, r);
        return true;
    }
    case 0x88: {
        if (!parse_name(f, c, &nm))
            return true;
        for (int i = 0; i < 3; i++)
            aml_put(eval(f, c));
        struct aml_obj *r = obj_new(AML_REGION);
        if (r)
            r->r.space = 0x80; /* a table's bytes: not mapped, reads give 0 */
        create(f, &nm, r);
        return true;
    }
    case 0x81:
    case 0x86:
    case 0x87:
        op_field(f, c, ext);
        return true;
    default:
        return false;
    }
}

/* ---- statements ---- */

static void exec_term(struct frame *f, struct cur *c)
{
    uint8_t op = *c->p;
    const uint8_t *end;
    if (op == 0xa0 || op == 0xa2) {
        c->p++;
        if (!pkg_end(c, &end)) {
            fail(f, "bad If");
            return;
        }
        const uint8_t *start = c->p;
        c->p = end;
        if (op == 0xa2) {
            for (uint32_t turns = 0; !f->flow; turns++) {
                struct cur body = {start, end};
                uint64_t pred;
                if (turns >= aml_loop_limit) {
                    fail(f, "While runs too long");
                    return;
                }
                if (!eval_int(f, &body, &pred) || !pred)
                    return;
                exec_list(f, &body);
                if (f->flow == FLOW_BREAK || f->flow == FLOW_CONTINUE) {
                    bool stop = f->flow == FLOW_BREAK;
                    f->flow = FLOW_NONE;
                    if (stop)
                        return;
                }
            }
            return;
        }
        struct cur body = {start, end};
        uint64_t pred;
        if (!eval_int(f, &body, &pred))
            return;
        if (pred)
            exec_list(f, &body);
        if (c->p < c->end && *c->p == 0xa1) {
            c->p++;
            if (!pkg_end(c, &end)) {
                fail(f, "bad Else");
                return;
            }
            struct cur other = {c->p, end};
            c->p = end;
            if (!pred && !f->flow)
                exec_list(f, &other);
        }
        return;
    }
    switch (op) {
    case 0xa1: /* an Else without its If */
        c->p++;
        if (!pkg_end(c, &end))
            fail(f, "bad Else");
        else
            c->p = end;
        return;
    case 0xa3:
    case 0xcc:
        c->p++;
        return;
    case 0xa4: {
        c->p++;
        struct aml_obj *v = c->p < c->end ? eval(f, c) : aml_int(0);
        if (!v)
            return;
        if (v->type == AML_REF && v->ref.kind != REF_NODE)
            deref(f, &v);
        aml_put(f->ret);
        f->ret = v;
        f->flow = FLOW_RETURN;
        return;
    }
    case 0xa5:
        c->p++;
        f->flow = FLOW_BREAK;
        return;
    case 0x9f:
        c->p++;
        f->flow = FLOW_CONTINUE;
        return;
    default:
        break;
    }
    if (op == 0x08 || op == 0x06 || op == 0x10 || op == 0x14 || op == 0x15 ||
        (op >= 0x8a && op <= 0x8f && op != 0x8e)) {
        c->p++;
        named_op(f, c, op, 0);
        return;
    }
    if (op == 0x5b && c->end - c->p >= 2) {
        uint8_t ext = c->p[1];
        if (ext == 0x01 || ext == 0x02 || ext == 0x13 || (ext >= 0x80 && ext <= 0x88)) {
            c->p += 2;
            named_op(f, c, op, ext);
            return;
        }
    }
    aml_put(eval(f, c));
}

static void exec_list(struct frame *f, struct cur *c)
{
    while (c->p < c->end && !f->flow)
        exec_term(f, c);
}

/* ---- methods ---- */

static const char *const osi_names[] = {
    "Windows 2000", "Windows 2001", "Windows 2001 SP1", "Windows 2001.1", "Windows 2001 SP2", "Windows 2001.1 SP1",
    "Windows 2006", "Windows 2006 SP1", "Windows 2006.1", "Windows 2009", "Windows 2012", "Windows 2013",
    "Windows 2015", "Windows 2016", "Windows 2017", "Windows 2017.2", "Windows 2018", "Windows 2018.2",
    "Windows 2019", "Windows 2020", "Windows 2021", "Windows 2022", "Module Device", "Processor Device",
    "3.0 Thermal Model", "3.0 _SCP Extensions", "Processor Aggregator Device", "Extended Address Space Descriptor",
};

/* \_OSI: firmware tests for the Windows versions it was tested with, so the answer is the one Windows gives */
static bool osi(struct aml_obj **args, int argc, struct aml_obj **ret)
{
    bool yes = false;
    if (argc >= 1 && args[0] && args[0]->type == AML_STR)
        for (size_t i = 0; i < sizeof(osi_names) / sizeof(osi_names[0]); i++)
            if (!strcmp((const char *)args[0]->buf.data, osi_names[i]))
                yes = true;
    *ret = aml_int(yes ? ones() : 0);
    return *ret != NULL;
}

static struct aml_obj *invoke(struct aml_node *n, struct aml_obj **args, int argc, bool *ok)
{
    struct aml_obj *m = n->obj;
    *ok = false;
    if (m->m.native) {
        struct aml_obj *r = NULL;
        *ok = m->m.native(args, argc, &r);
        return r;
    }
    if (calls >= MAX_CALLS) {
        str_copy(aml_last_error, "methods nest too deep", sizeof(aml_last_error));
        return NULL;
    }
    struct frame f;
    memset(&f, 0, sizeof(f));
    f.scope = n;
    for (int i = 0; i < argc && i < 7; i++)
        f.args[i] = obj_get(args[i]);
    struct cur c = {m->m.code, m->m.code + m->m.len};
    calls++;
    m->m.active++;
    node_get(n);
    obj_get(m);
    exec_list(&f, &c);
    calls--;
    /* the names the method made go with its last running copy */
    if (!--m->m.active && n->obj == m)
        while (n->child)
            node_detach(n->child);
    aml_put(m);
    node_put(n);
    for (int i = 0; i < 7; i++)
        aml_put(f.args[i]);
    for (int i = 0; i < 8; i++)
        aml_put(f.locals[i]);
    if (f.flow == FLOW_ERROR) {
        aml_put(f.ret);
        return NULL;
    }
    *ok = true;
    return f.ret;
}

/* ---- the interface ---- */

static void make_root(void)
{
    static const char *const scopes[] = {"_GPE", "_PR_", "_SB_", "_SI_", "_TZ_"};
    root = node_new(NULL, "\\___", obj_new(AML_SCOPE));
    if (!root)
        return;
    for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); i++)
        node_new(root, scopes[i], obj_new(AML_SCOPE));
    struct aml_obj *m = obj_new(AML_METHOD);
    if (m) {
        m->m.flags = 1;
        m->m.native = osi;
    }
    node_new(root, "_OSI", m);
    node_new(root, "_OS_", obj_str("Microsoft Windows NT"));
    node_new(root, "_REV", aml_int(2));
    node_new(root, "_GL_", obj_new(AML_MUTEX));
}

bool aml_load(const uint8_t *table, size_t len)
{
    if (len < 36)
        return false;
    uint32_t tlen = (uint32_t)table[4] | (uint32_t)table[5] << 8 | (uint32_t)table[6] << 16 |
                    (uint32_t)table[7] << 24;
    if (tlen < 36 || tlen > len)
        return false;
    if (!root)
        make_root();
    if (!root)
        return false;
    if (!memcmp(table, "DSDT", 4))
        int_bits = table[8] >= 2 ? 64 : 32;
    struct frame f;
    memset(&f, 0, sizeof(f));
    f.scope = root;
    f.load = true;
    struct cur c = {table + 36, table + tlen};
    exec_list(&f, &c);
    bool ok = f.flow != FLOW_ERROR;
    if (!ok)
        aml_errors++;
    for (int i = 0; i < 8; i++)
        aml_put(f.locals[i]);
    for (int i = 0; i < 7; i++)
        aml_put(f.args[i]);
    aml_put(f.ret);
    return ok;
}

/* Objects first: while the tree holds every node it is in, no node goes away, so an object that refers to one
   never meets freed memory. Then the nodes. */
static void put_objects(struct aml_node *n)
{
    for (struct aml_node *c = n->child; c; c = c->next)
        put_objects(c);
    aml_put(n->obj);
    n->obj = NULL;
}

static void free_nodes(struct aml_node *n)
{
    while (n->child) {
        struct aml_node *c = n->child;
        n->child = c->next;
        free_nodes(c);
    }
    kfree(n);
}

void aml_reset(void)
{
    if (!root)
        return;
    put_objects(root);
    free_nodes(root);
    root = NULL;
    int_bits = 64;
    nest = calls = 0;
}

struct aml_node *aml_root(void)
{
    return root;
}

struct aml_node *aml_find(struct aml_node *scope, const char *path)
{
    struct name nm;
    if (!root || !text_name(path, &nm))
        return NULL;
    return lookup(scope ? scope : root, &nm, false);
}

struct aml_node *aml_walk(struct aml_node *n)
{
    if (!n)
        return root;
    if (n->child)
        return n->child;
    for (; n; n = n->parent)
        if (n->next)
            return n->next;
    return NULL;
}

struct aml_node *aml_parent(const struct aml_node *n)
{
    return n ? n->parent : NULL;
}

enum aml_type aml_node_type(const struct aml_node *n)
{
    return n && n->obj ? (enum aml_type)n->obj->type : AML_NONE;
}

void aml_path(const struct aml_node *n, char *buf, size_t cap)
{
    const struct aml_node *chain[64];
    int depth = 0;
    for (; n && n->parent && depth < 64; n = n->parent)
        chain[depth++] = n;
    size_t at = 0;
    if (at + 1 < cap)
        buf[at++] = '\\';
    for (int i = depth - 1; i >= 0; i--) {
        if (i != depth - 1 && at + 1 < cap)
            buf[at++] = '.';
        for (int k = 0; k < 4 && at + 1 < cap; k++)
            buf[at++] = chain[i]->name[k];
    }
    if (cap)
        buf[MIN(at, cap - 1)] = 0;
}

bool aml_eval(struct aml_node *node, const char *path, struct aml_obj **args, int argc, struct aml_obj **ret)
{
    if (ret)
        *ret = NULL;
    struct aml_node *n = path ? aml_find(node, path) : node;
    if (!n || !n->obj)
        return false;
    struct aml_obj *r = NULL;
    bool ok;
    if (n->obj->type == AML_METHOD) {
        r = invoke(n, args, argc, &ok);
        if (!ok) {
            aml_errors++;
            char where[80];
            aml_path(n, where, sizeof(where));
            klog("acpi: %s failed: %s", where, aml_last_error);
        }
    } else {
        struct frame f;
        memset(&f, 0, sizeof(f));
        f.scope = n->parent;
        r = read_node(&f, n);
        ok = r != NULL;
    }
    if (ok && r && r->type == AML_REF) {
        struct frame f;
        memset(&f, 0, sizeof(f));
        deref(&f, &r);
        ok = r != NULL;
    }
    if (ret)
        *ret = r;
    else
        aml_put(r);
    return ok;
}

bool aml_eval_int(struct aml_node *node, const char *path, uint64_t *out)
{
    struct aml_obj *r;
    if (!aml_eval(node, path, NULL, 0, &r))
        return false;
    bool ok = aml_obj_int(r, out);
    aml_put(r);
    return ok;
}

uint64_t aml_status(struct aml_node *dev)
{
    uint64_t sta;
    if (!aml_find(dev, "_STA"))
        return 0x0f;
    return aml_eval_int(dev, "_STA", &sta) ? sta : 0;
}

static bool id_matches(const struct aml_obj *o, const char *id)
{
    if (!o)
        return false;
    if (o->type == AML_STR)
        return !strcmp((const char *)o->buf.data, id);
    if (o->type == AML_INT) {
        /* a compressed EISA id: three letters in 15 bits, then four hex digits */
        uint32_t v = (uint32_t)o->ival;
        uint8_t b0 = (uint8_t)v, b1 = (uint8_t)(v >> 8), b2 = (uint8_t)(v >> 16), b3 = (uint8_t)(v >> 24);
        char s[8];
        s[0] = (char)('@' + (b0 >> 2 & 31));
        s[1] = (char)('@' + ((b0 & 3) << 3 | b1 >> 5));
        s[2] = (char)('@' + (b1 & 31));
        hex(s + 3, b2, 2);
        hex(s + 5, b3, 2);
        return !strcmp(s, id);
    }
    if (o->type == AML_PKG)
        for (uint32_t i = 0; i < o->pkg.count; i++)
            if (id_matches(o->pkg.items[i], id))
                return true;
    return false;
}

bool aml_hid_is(struct aml_node *dev, const char *id)
{
    static const char *const names[] = {"_HID", "_CID"};
    if (!dev || !dev->obj || (dev->obj->type != AML_DEVICE && dev->obj->type != AML_SCOPE))
        return false;
    for (int i = 0; i < 2; i++) {
        struct aml_obj *r;
        if (!aml_find(dev, names[i]) || !aml_eval(dev, names[i], NULL, 0, &r))
            continue;
        bool yes = id_matches(r, id);
        aml_put(r);
        if (yes)
            return true;
    }
    return false;
}

struct aml_node *aml_find_device(const char *id, struct aml_node *after)
{
    for (struct aml_node *n = aml_walk(after); n; n = aml_walk(n))
        if (n->obj && n->obj->type == AML_DEVICE && aml_hid_is(n, id) && (aml_status(n) & 1))
            return n;
    return NULL;
}

static void run_reg(int space)
{
    struct aml_node *done[32];
    int ndone = 0;
    for (struct aml_node *n = aml_walk(NULL); n; n = aml_walk(n)) {
        if (!n->obj || n->obj->type != AML_REGION || n->obj->r.space != space || !n->parent)
            continue;
        struct aml_node *dev = n->parent;
        bool seen = false;
        for (int i = 0; i < ndone; i++)
            seen |= done[i] == dev;
        if (seen || !aml_find(dev, "_REG"))
            continue;
        if (ndone < 32)
            done[ndone++] = dev;
        struct aml_obj *args[2] = {aml_int((uint64_t)space), aml_int(1)};
        if (args[0] && args[1])
            aml_eval(dev, "_REG", args, 2, NULL);
        aml_put(args[0]);
        aml_put(args[1]);
    }
}

void aml_region_ready(int space)
{
    run_reg(space);
}

static void init_scope(struct aml_node *scope, int depth)
{
    if (depth > 32)
        return;
    for (struct aml_node *n = scope->child; n; n = n->next) {
        int type = n->obj ? n->obj->type : AML_NONE;
        if (type == AML_SCOPE) {
            init_scope(n, depth + 1);
        } else if (type == AML_DEVICE || type == AML_PROCESSOR || type == AML_THERMAL) {
            uint64_t sta = aml_status(n);
            if (sta & 1) {
                if (aml_find(n, "_INI"))
                    aml_eval(n, "_INI", NULL, 0, NULL);
                init_scope(n, depth + 1);
            } else if (sta & 8) {
                init_scope(n, depth + 1);
            }
        }
    }
}

void aml_init_devices(void)
{
    if (!root)
        return;
    for (int space = AML_SPACE_MEMORY; space <= AML_SPACE_PCI; space++)
        run_reg(space);
    if (aml_find(NULL, "\\_SB._INI"))
        aml_eval(NULL, "\\_SB._INI", NULL, 0, NULL);
    init_scope(root, 0);
}

enum aml_type aml_type_of(const struct aml_obj *o)
{
    return o ? (enum aml_type)o->type : AML_NONE;
}

bool aml_obj_int(const struct aml_obj *o, uint64_t *out)
{
    struct frame f;
    memset(&f, 0, sizeof(f));
    if (!o || (o->type != AML_INT && o->type != AML_BUF && o->type != AML_STR))
        return false;
    return to_int(&f, (struct aml_obj *)o, out);
}

const struct aml_obj *aml_pkg_item(const struct aml_obj *pkg, uint32_t i)
{
    return pkg && pkg->type == AML_PKG && i < pkg->pkg.count ? pkg->pkg.items[i] : NULL;
}

const uint8_t *aml_buffer(const struct aml_obj *o, uint32_t *len)
{
    if (!o || (o->type != AML_BUF && o->type != AML_STR))
        return NULL;
    *len = o->buf.len;
    return o->buf.data;
}

uint32_t aml_pkg_count(const struct aml_obj *pkg)
{
    return pkg && pkg->type == AML_PKG ? pkg->pkg.count : 0;
}

bool aml_pkg_int(const struct aml_obj *pkg, uint32_t i, uint64_t *out)
{
    if (!pkg || pkg->type != AML_PKG || i >= pkg->pkg.count || !pkg->pkg.items[i] ||
        pkg->pkg.items[i]->type != AML_INT)
        return false;
    *out = pkg->pkg.items[i]->ival;
    return true;
}

bool aml_pkg_str(const struct aml_obj *pkg, uint32_t i, char *buf, size_t cap)
{
    if (!cap || !pkg || pkg->type != AML_PKG || i >= pkg->pkg.count || !pkg->pkg.items[i])
        return false;
    const struct aml_obj *e = pkg->pkg.items[i];
    if (e->type != AML_STR && e->type != AML_BUF)
        return false;
    size_t n = 0;
    while (n < e->buf.len && n + 1 < cap && e->buf.data[n] >= 0x20 && e->buf.data[n] < 0x7f) {
        buf[n] = (char)e->buf.data[n];
        n++;
    }
    buf[n] = 0;
    return true;
}
