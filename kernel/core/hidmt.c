/* HID multi-touch: the report descriptor of a Precision Touchpad or a touch screen (HID Usage Tables, Digitizers
   page 0x0D) and its input reports. A touch application collection (Touch Screen 0x04 or Touch Pad 0x05) holds one
   logical collection per finger (0x22) with Tip Switch, Confidence, Contact Identifier and X/Y, and once Contact
   Count and the pad's button. In hybrid mode a report carries fewer fingers than are down: the first report of a
   frame has the count, the following ones a count of 0. The Device Configuration collection (0x0E) has the
   features the host sets: Input Mode (3 for a touchpad, 2 for a touch screen), Surface Switch and Button Switch. */
#ifdef __x86_64__

#include <synth/hidmt.h>
#include <synth/string.h>

#define U(page, id) ((uint32_t)(page) << 16 | (id))
#define MAX_USAGES 16
#define MAX_DEPTH 8

enum {
    MAIN_INPUT = 8,
    MAIN_OUTPUT = 9,
    MAIN_COLLECTION = 10,
    MAIN_FEATURE = 11,
    MAIN_END = 12,
};

struct globals {
    uint16_t page;
    int32_t min, max;
    uint32_t size, count;
    uint8_t id;
};

struct offsets {
    uint8_t type, id;
    uint32_t bits;
};

struct parser {
    struct globals g, stack[4];
    int depth;
    uint32_t usages[MAX_USAGES];
    int usage_count;
    uint32_t usage_min, usage_max;
    uint32_t collections[MAX_DEPTH];
    int nest;
    int finger; /* the finger collection the items are in, -1 outside one */
    int fingers_seen;
    bool input_found;
    struct offsets off[16];
    int off_count;
    int32_t min_x, max_x, min_y, max_y;
};

static uint32_t item_value(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_signed(const uint8_t *p, int size)
{
    uint32_t v = item_value(p, size);
    return size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
}

static uint32_t *bits_of(struct parser *ps, uint8_t type, uint8_t id)
{
    for (int i = 0; i < ps->off_count; i++)
        if (ps->off[i].type == type && ps->off[i].id == id)
            return &ps->off[i].bits;
    if (ps->off_count == (int)ARRAY_SIZE(ps->off))
        return NULL;
    ps->off[ps->off_count] = (struct offsets){type, id, 0};
    return &ps->off[ps->off_count++].bits;
}

/* the application collection the parser is in (the outermost), 0 outside every collection */
static uint32_t application(const struct parser *ps)
{
    return ps->nest ? ps->collections[0] : 0;
}

static void set(struct hid_field *f, uint32_t offset, uint32_t size)
{
    if (f->present || offset > 0xffff)
        return;
    *f = (struct hid_field){(uint16_t)offset, (uint8_t)MIN(size, 32u), true};
}

static uint32_t usage_at(const struct parser *ps, uint32_t i)
{
    if (ps->usage_count)
        return ps->usages[MIN(i, (uint32_t)ps->usage_count - 1)];
    if (ps->usage_max >= ps->usage_min && ps->usage_min + i <= ps->usage_max)
        return ps->usage_min + i;
    return 0;
}

static void input(struct hidmt *m, struct parser *ps, uint32_t flags, uint32_t start)
{
    uint32_t app = application(ps);
    if ((app != U(0x0d, 0x04) && app != U(0x0d, 0x05)) || (flags & 1))
        return;
    if (ps->input_found && ps->g.id != m->report)
        return;
    if (!ps->input_found) {
        ps->input_found = true;
        m->report = ps->g.id;
        m->direct = app == U(0x0d, 0x04);
    }
    for (uint32_t i = 0; i < ps->g.count && i < 64; i++) {
        uint32_t usage = usage_at(ps, i), at = start + i * ps->g.size;
        if (usage == U(0x0d, 0x54))
            set(&m->count, at, ps->g.size);
        else if (usage == U(0x09, 0x01))
            set(&m->button, at, 1);
        if (ps->finger < 0 || ps->finger >= TOUCH_MAX)
            continue;
        if (usage == U(0x0d, 0x42))
            set(&m->f[ps->finger].tip, at, 1);
        else if (usage == U(0x0d, 0x47))
            set(&m->f[ps->finger].confidence, at, 1);
        else if (usage == U(0x0d, 0x51))
            set(&m->f[ps->finger].id, at, ps->g.size);
        else if (usage == U(0x01, 0x30) && !m->f[ps->finger].x.present) {
            set(&m->f[ps->finger].x, at, ps->g.size);
            if (ps->finger == 0)
                ps->min_x = ps->g.min, ps->max_x = ps->g.max;
        } else if (usage == U(0x01, 0x31) && !m->f[ps->finger].y.present) {
            set(&m->f[ps->finger].y, at, ps->g.size);
            if (ps->finger == 0)
                ps->min_y = ps->g.min, ps->max_y = ps->g.max;
        }
    }
}

static void feature(struct hidmt *m, struct parser *ps, uint32_t flags, uint32_t start)
{
    if (application(ps) != U(0x0d, 0x0e) || (flags & 1))
        return;
    for (uint32_t i = 0; i < ps->g.count && i < 64; i++) {
        uint32_t usage = usage_at(ps, i);
        uint8_t value = usage == U(0x0d, 0x52) ? 3 : usage == U(0x0d, 0x57) || usage == U(0x0d, 0x58) ? 1 : 0;
        if (!value || m->setting_count == (int)ARRAY_SIZE(m->settings))
            continue;
        struct hidmt_setting *s = &m->settings[m->setting_count++];
        s->report = ps->g.id;
        s->value = value;
        set(&s->field, start + i * ps->g.size, ps->g.size);
    }
}

static void main_item(struct hidmt *m, struct parser *ps, int tag, uint32_t value)
{
    if (tag == MAIN_COLLECTION) {
        uint32_t usage = usage_at(ps, 0);
        if (ps->nest < MAX_DEPTH)
            ps->collections[ps->nest] = usage;
        ps->nest++;
        uint32_t app = application(ps);
        if (usage == U(0x0d, 0x22) && ps->finger < 0 && (app == U(0x0d, 0x04) || app == U(0x0d, 0x05)))
            ps->finger = ps->fingers_seen++;
    } else if (tag == MAIN_END) {
        if (ps->nest > 0 && --ps->nest < MAX_DEPTH && ps->collections[ps->nest] == U(0x0d, 0x22))
            ps->finger = -1;
    } else if (tag == MAIN_INPUT || tag == MAIN_OUTPUT || tag == MAIN_FEATURE) {
        uint32_t *bits = bits_of(ps, (uint8_t)tag, ps->g.id);
        if (!bits)
            return;
        uint32_t start = *bits;
        if (tag == MAIN_INPUT)
            input(m, ps, value, start);
        else if (tag == MAIN_FEATURE)
            feature(m, ps, value, start);
        uint64_t end = (uint64_t)*bits + (uint64_t)ps->g.size * ps->g.count;
        *bits = end > 0xffff ? 0xffff : (uint32_t)end;
    }
}

static void global_item(struct parser *ps, int tag, const uint8_t *v, int size)
{
    switch (tag) {
    case 0: ps->g.page = (uint16_t)item_value(v, size); break;
    case 1: ps->g.min = item_signed(v, size); break;
    case 2: ps->g.max = ps->g.min < 0 ? item_signed(v, size) : (int32_t)item_value(v, size); break;
    case 7: ps->g.size = item_value(v, size); break;
    case 8: ps->g.id = (uint8_t)item_value(v, size); break;
    case 9: ps->g.count = item_value(v, size); break;
    case 10:
        if (ps->depth < (int)ARRAY_SIZE(ps->stack))
            ps->stack[ps->depth++] = ps->g;
        break;
    case 11:
        if (ps->depth > 0)
            ps->g = ps->stack[--ps->depth];
        break;
    }
}

static void local_item(struct parser *ps, int tag, const uint8_t *v, int size)
{
    uint32_t u = item_value(v, size);
    if (size < 4)
        u = U(ps->g.page, u);
    if (tag == 0 && ps->usage_count < MAX_USAGES)
        ps->usages[ps->usage_count++] = u;
    else if (tag == 1)
        ps->usage_min = u;
    else if (tag == 2)
        ps->usage_max = u;
}

bool hidmt_parse(struct hidmt *m, const uint8_t *desc, size_t length)
{
    memset(m, 0, sizeof(*m));
    struct parser ps = {.finger = -1};
    for (size_t at = 0; at < length;) {
        uint8_t b = desc[at];
        if (b == 0xfe) { /* a long item */
            at += 3 + (at + 1 < length ? desc[at + 1] : 0);
            continue;
        }
        int size = (b & 3) == 3 ? 4 : (b & 3);
        if (at + 1 + (size_t)size > length)
            break;
        const uint8_t *v = desc + at + 1;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (type == 0) {
            main_item(m, &ps, tag, item_value(v, size));
            ps.usage_count = 0;
            ps.usage_min = ps.usage_max = 0;
        } else if (type == 1) {
            global_item(&ps, tag, v, size);
        } else if (type == 2) {
            local_item(&ps, tag, v, size);
        }
        at += 1 + (size_t)size;
    }
    while (m->fingers < MIN(ps.fingers_seen, TOUCH_MAX) && m->f[m->fingers].tip.present &&
           m->f[m->fingers].x.present && m->f[m->fingers].y.present)
        m->fingers++;
    if (!m->fingers || ps.max_x <= ps.min_x || ps.max_y <= ps.min_y)
        return false;
    for (int i = 0; i < ps.off_count; i++)
        if (ps.off[i].type == MAIN_INPUT && ps.off[i].id == m->report)
            m->bytes = (uint16_t)((ps.off[i].bits + 7) / 8);
    for (int s = 0; s < m->setting_count; s++) {
        if (m->settings[s].value == 3 && m->direct)
            m->settings[s].value = 2;
        for (int i = 0; i < ps.off_count; i++)
            if (ps.off[i].type == MAIN_FEATURE && ps.off[i].id == m->settings[s].report)
                m->feature_bytes[s] = (uint16_t)((ps.off[i].bits + 7) / 8);
    }
    m->min_x = ps.min_x;
    m->min_y = ps.min_y;
    m->pad.max_x = ps.max_x - ps.min_x;
    m->pad.max_y = ps.max_y - ps.min_y;
    m->pad.direct = m->direct;
    return true;
}

static uint32_t get(const uint8_t *data, size_t length, const struct hid_field *f)
{
    uint32_t v = 0;
    for (int i = 0; f->present && i < f->size; i++) {
        size_t bit = (size_t)f->offset + (size_t)i;
        if (bit / 8 < length && (data[bit / 8] >> (bit % 8) & 1))
            v |= 1u << i;
    }
    return v;
}

void hidmt_input(struct hidmt *m, const uint8_t *r, size_t length, uint64_t now_ms)
{
    if (m->report) {
        if (!length || r[0] != m->report)
            return;
        r++;
        length--;
    }
    bool hybrid = m->count.present;
    if (hybrid) {
        int count = (int)get(r, length, &m->count);
        if (count > 0 || m->received >= m->expected) { /* the first report of a frame */
            memset(&m->pending, 0, sizeof(m->pending));
            m->expected = count;
            m->received = 0;
        }
    } else {
        memset(&m->pending, 0, sizeof(m->pending));
    }
    if (m->button.present && get(r, length, &m->button))
        m->pending.buttons = 1;
    for (int i = 0; i < m->fingers && (!hybrid || m->received < m->expected); i++) {
        m->received++;
        const typeof(m->f[0]) *f = &m->f[i];
        if (!get(r, length, &f->tip) || (f->confidence.present && !get(r, length, &f->confidence)))
            continue;
        if (m->pending.count == TOUCH_MAX)
            continue;
        int x = (int)get(r, length, &f->x) - m->min_x, y = (int)get(r, length, &f->y) - m->min_y;
        m->pending.c[m->pending.count++] = (struct touch_contact){
            f->id.present ? (int)get(r, length, &f->id) : i, CLAMP(x, 0, m->pad.max_x), CLAMP(y, 0, m->pad.max_y)};
    }
    if (!hybrid || m->received >= m->expected)
        touch_frame(&m->pad, &m->pending, now_ms);
}

int hidmt_setting_report(const struct hidmt *m, int i, uint8_t *buf, int cap)
{
    /* the i-th distinct report among the settings */
    int seen = 0;
    for (int s = 0; s < m->setting_count; s++) {
        bool first = true;
        for (int t = 0; t < s; t++)
            if (m->settings[t].report == m->settings[s].report)
                first = false;
        if (!first || seen++ != i)
            continue;
        int length = 1 + m->feature_bytes[s];
        if (length > cap)
            return 0;
        memset(buf, 0, (size_t)length);
        buf[0] = m->settings[s].report;
        for (int t = s; t < m->setting_count; t++) {
            const struct hidmt_setting *st = &m->settings[t];
            if (st->report != m->settings[s].report)
                continue;
            for (int b = 0; b < st->field.size && b < 8; b++) {
                int bit = st->field.offset + b;
                if (st->value >> b & 1 && 1 + bit / 8 < length)
                    buf[1 + bit / 8] |= (uint8_t)(1u << (bit % 8));
            }
        }
        return length;
    }
    return 0;
}

#endif
