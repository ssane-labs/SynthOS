#include <synth/heap.h>
#include <synth/string.h>

#include "win64.h"

#define P(v) w64_ptr(v)
#define U(v) ((uint32_t)(uintptr_t)(v))
#define WM_CREATE 0x0001
#define WM_NCCREATE 0x0081
#define GWLP_WNDPROC (-4)
#define CS_MAGIC 0x32335343u

struct thunk {
    uint8_t code[16];
    uint64_t fn;
    int argc;
    struct thunk *next;
};

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void put32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static uint32_t WINAPI bridge4(struct thunk *t, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp)
{
    uint64_t args[4] = {h, msg, w64_high_back(wp), w64_high_back(lp)};
    uint8_t cs[88];
    if ((msg == WM_CREATE || msg == WM_NCCREATE) && lp) {
        const uint32_t *src = P(lp);
        memset(cs, 0, sizeof(cs));
        for (int i = 0; i < 4; i++)
            put64(cs + i * 8, w64_high_back(src[i]));
        memcpy(cs + 32, src + 4, 20);
        put64(cs + 56, src[9]);
        put64(cs + 64, src[10]);
        put32(cs + 72, src[11]);
        put32(cs + 80, CS_MAGIC);
        put32(cs + 84, lp);
        args[3] = U(cs);
    }
    return (uint32_t)w64_call(t->fn, 4, args);
}

static uint32_t WINAPI bridge2(struct thunk *t, uint32_t a, uint32_t b)
{
    uint64_t args[2] = {a, b};
    return (uint32_t)w64_call(t->fn, 2, args);
}

static bool is_stub_for(uint64_t addr, w64_handler fn, void **target);
static uint64_t h_proc32(uint64_t *a, const uint64_t *f, void *t);

static uint32_t to32(uint64_t fn, int argc)
{
    if (!fn)
        return 0;
    void *target;
    if (is_stub_for(fn, h_proc32, &target))
        return U(target);
    for (struct thunk *t = w32->w64_thunks; t; t = t->next)
        if (t->fn == fn && t->argc == argc)
            return U(t);
    struct thunk *t = w32_zalloc(sizeof(*t));
    if (!t)
        return 0;
    uint32_t self = U(t), entry = U(argc == 2 ? (void *)bridge2 : (void *)bridge4);
    int32_t rel = (int32_t)(entry - (self + 12));
    t->code[0] = 0x59;
    t->code[1] = 0x68;
    memcpy(t->code + 2, &self, 4);
    t->code[6] = 0x51;
    t->code[7] = 0xe9;
    memcpy(t->code + 8, &rel, 4);
    t->fn = fn;
    t->argc = argc;
    t->next = w32->w64_thunks;
    w32->w64_thunks = t;
    return self;
}

static uint64_t to64(uint32_t fn)
{
    if (!fn)
        return 0;
    for (struct thunk *t = w32->w64_thunks; t; t = t->next)
        if (U(t) == fn)
            return t->fn;
    return U(w64_target_stub(h_proc32, (void *)(uintptr_t)fn));
}

static bool is_stub_for(uint64_t addr, w64_handler fn, void **target)
{
    return !(addr >> 32) && w64_stub_info(addr, fn, target);
}

static uint32_t lparam32(uint64_t msg, uint64_t lp)
{
    if (((uint32_t)msg == WM_CREATE || (uint32_t)msg == WM_NCCREATE) && lp && rd32(P(lp + 80)) == CS_MAGIC)
        return rd32(P(lp + 84));
    return U(P(lp));
}

static uint64_t h_proc32(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_ret((uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], U(P(a[2])), lparam32(a[1], a[3])));
}

static void class_to32(const uint8_t *c, uint32_t *o, uint32_t style)
{
    o[0] = style;
    o[1] = to32(rd64(c + 8), 4);
    memcpy(o + 2, c + 16, 8);
    for (int i = 0; i < 6; i++)
        o[4 + i] = (uint32_t)rd64(c + 24 + i * 8);
}

static uint64_t h_register_class(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t wc[10];
    class_to32(P(a[0]), wc, rd32(P(a[0])));
    return T32(t, U(wc)) & 0xffff;
}

static uint64_t h_register_class_ex(uint64_t *a, const uint64_t *f, void *t)
{
    const uint8_t *c = P(a[0]);
    uint32_t wc[12];
    wc[0] = 48;
    class_to32(c, wc + 1, rd32(c + 4));
    wc[11] = (uint32_t)rd64(c + 72);
    return T32(t, U(wc)) & 0xffff;
}

static uint64_t h_class_info_ex(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t wc[12] = {48};
    uint32_t r = (uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], U(wc));
    if (r) {
        uint8_t *o = P(a[2]);
        put32(o, 80);
        put32(o + 4, wc[1]);
        put64(o + 8, to64(wc[2]));
        memcpy(o + 16, wc + 3, 8);
        for (int i = 0; i < 7; i++)
            put64(o + 24 + i * 8, wc[5 + i]);
    }
    return r;
}

static void msg_to64(const uint32_t *m, uint8_t *o)
{
    memset(o, 0, 48);
    put64(o, m[0]);
    put32(o + 8, m[1]);
    put64(o + 16, w64_high_back(m[2]));
    put64(o + 24, w64_high_back(m[3]));
    put32(o + 32, m[4]);
    memcpy(o + 36, m + 5, 8);
}

static void msg_to32(const uint8_t *o, uint32_t *m)
{
    m[0] = (uint32_t)rd64(o);
    m[1] = rd32(o + 8);
    m[2] = U(P(rd64(o + 16)));
    m[3] = U(P(rd64(o + 24)));
    m[4] = rd32(o + 32);
    memcpy(m + 5, o + 36, 8);
}

static uint64_t h_get_message(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t m[7] = {0};
    uint32_t r = (uint32_t)T32(t, U(m), (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]);
    msg_to64(m, P(a[0]));
    return w64_sx(r);
}

static uint64_t h_msg_in0(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t m[7];
    msg_to32(P(a[0]), m);
    return w64_ret((uint32_t)T32(t, U(m)));
}

static uint64_t h_msg_in1(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t m[7];
    msg_to32(P(a[1]), m);
    return w64_ret((uint32_t)T32(t, (uint32_t)a[0], U(m)));
}

static uint64_t h_msg_in2(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t m[7];
    msg_to32(P(a[2]), m);
    return w64_ret((uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], U(m)));
}

static uint64_t h_begin_paint(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t ps[64];
    uint32_t r = (uint32_t)T32(t, (uint32_t)a[0], U(ps));
    uint8_t *o = P(a[1]);
    memset(o, 0, 72);
    put64(o, rd32(ps));
    memcpy(o + 8, ps + 4, 60);
    return r;
}

static uint64_t h_end_paint(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t ps[64];
    const uint8_t *o = P(a[1]);
    put32(ps, (uint32_t)rd64(o));
    memcpy(ps + 4, o + 8, 60);
    return (uint32_t)T32(t, (uint32_t)a[0], U(ps));
}

static uint64_t h_get_long(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t r = (uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1]);
    return (int32_t)a[1] == GWLP_WNDPROC ? to64(r) : w64_high_back(w64_ret(r));
}

static uint64_t h_set_long(uint64_t *a, const uint64_t *f, void *t)
{
    bool proc = (int32_t)a[1] == GWLP_WNDPROC;
    uint32_t r = (uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], proc ? to32(a[2], 4) : U(P(a[2])));
    return proc ? to64(r) : w64_high_back(w64_ret(r));
}

static uint64_t h_call_window_proc(uint64_t *a, const uint64_t *f, void *t)
{
    void *target;
    if (is_stub_for(a[0], h_proc32, &target))
        return w64_ret((uint32_t)T32(target, (uint32_t)a[1], (uint32_t)a[2], U(P(a[3])), lparam32(a[2], a[4])));
    for (struct thunk *th = w32->w64_thunks; th; th = th->next)
        if (U(th) == a[0])
            a[0] = th->fn;
    if (a[0] >> 32 == 0 && !w64_is_code64(a[0]))
        return w64_ret((uint32_t)T32(P(a[0]), (uint32_t)a[1], (uint32_t)a[2], U(P(a[3])), lparam32(a[2], a[4])));
    return w64_call(a[0], 4, a + 1);
}

static uint64_t h_def_proc(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_ret((uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], U(P(a[2])), lparam32(a[1], a[3])));
}

static uint64_t h_dialog(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_ret((uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], to32(a[3], 4), (uint32_t)a[4]));
}

static uint64_t h_set_timer(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_ret((uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], to32(a[3], 4)));
}

static uint64_t h_enum_windows(uint64_t *a, const uint64_t *f, void *t)
{
    return (uint32_t)T32(t, to32(a[0], 2), (uint32_t)a[1]);
}

static uint64_t h_enum_monitors(uint64_t *a, const uint64_t *f, void *t)
{
    return (uint32_t)T32(t, (uint32_t)a[0], U(P(a[1])), to32(a[2], 4), U(P(a[3])));
}

static uint64_t h_enum_children(uint64_t *a, const uint64_t *f, void *t)
{
    return (uint32_t)T32(t, (uint32_t)a[0], to32(a[1], 2), (uint32_t)a[2]);
}

static uint64_t h_msg_wait(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t n = (uint32_t)MIN(a[0], 64u), handles[64];
    for (uint32_t i = 0; i < n; i++)
        handles[i] = (uint32_t)rd64(P(a[1] + i * 8));
    return w64_ret((uint32_t)T32(t, n, U(handles), (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]));
}

static uint64_t h_dib_section(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t bits = 0;
    uint32_t r = (uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], a[3] ? U(&bits) : 0,
                               (uint32_t)a[4], (uint32_t)a[5]);
    if (a[3])
        put64(P(a[3]), bits);
    return w64_ret(r);
}

static uint64_t h_get_object(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t buf[128];
    uint32_t size = (uint32_t)MIN(a[1], (uint64_t)sizeof(buf));
    uint32_t r = (uint32_t)T32(t, (uint32_t)a[0], a[2] ? size : 0, a[2] ? U(buf) : 0);
    if (r == 24 && (!a[2] || a[1] >= 32)) {
        if (a[2]) {
            uint8_t *o = P(a[2]);
            memset(o, 0, 32);
            memcpy(o, buf, 20);
            put64(o + 24, rd32(buf + 20));
        }
        return 32;
    }
    if (a[2] && r)
        memcpy(P(a[2]), buf, MIN(r, size));
    return r;
}

static const struct w64_export user32_exports[] = {
    W64_FN("RegisterClassA", h_register_class),
    W64_FN("RegisterClassW", h_register_class),
    W64_FN("RegisterClassExA", h_register_class_ex),
    W64_FN("RegisterClassExW", h_register_class_ex),
    W64_FN("GetClassInfoExA", h_class_info_ex),
    W64_FN("GetClassInfoExW", h_class_info_ex),
    W64_FN("GetMessageA", h_get_message),
    W64_FN("GetMessageW", h_get_message),
    W64_FN("PeekMessageA", h_get_message),
    W64_FN("PeekMessageW", h_get_message),
    W64_FN("TranslateMessage", h_msg_in0),
    W64_FN("DispatchMessageA", h_msg_in0),
    W64_FN("DispatchMessageW", h_msg_in0),
    W64_FN("IsDialogMessageA", h_msg_in1),
    W64_FN("IsDialogMessageW", h_msg_in1),
    W64_FN("TranslateAcceleratorA", h_msg_in2),
    W64_FN("TranslateAcceleratorW", h_msg_in2),
    W64_FN("BeginPaint", h_begin_paint),
    W64_FN("EndPaint", h_end_paint),
    W64_FN("GetWindowLongPtrA", h_get_long),
    W64_FN("GetWindowLongPtrW", h_get_long),
    W64_FN("GetWindowLongA", h_get_long),
    W64_FN("GetWindowLongW", h_get_long),
    W64_FN("SetWindowLongPtrA", h_set_long),
    W64_FN("SetWindowLongPtrW", h_set_long),
    W64_FN("SetWindowLongA", h_set_long),
    W64_FN("SetWindowLongW", h_set_long),
    W64_FN("CallWindowProcA", h_call_window_proc),
    W64_FN("CallWindowProcW", h_call_window_proc),
    W64_FN("DialogBoxParamA", h_dialog),
    W64_FN("DialogBoxParamW", h_dialog),
    W64_FN("DialogBoxIndirectParamA", h_dialog),
    W64_FN("DialogBoxIndirectParamW", h_dialog),
    W64_FN("CreateDialogParamA", h_dialog),
    W64_FN("CreateDialogParamW", h_dialog),
    W64_FN("CreateDialogIndirectParamA", h_dialog),
    W64_FN("CreateDialogIndirectParamW", h_dialog),
    W64_FN("DefWindowProcA", h_def_proc),
    W64_FN("DefWindowProcW", h_def_proc),
    W64_FN("DefDlgProcA", h_def_proc),
    W64_FN("DefDlgProcW", h_def_proc),
    W64_FN("DefFrameProcA", h_def_proc),
    W64_FN("DefMDIChildProcA", h_def_proc),
    W64_FN("SetTimer", h_set_timer),
    W64_FN("SetCoalescableTimer", h_set_timer),
    W64_FN("EnumWindows", h_enum_windows),
    W64_FN("EnumChildWindows", h_enum_children),
    W64_FN("EnumDisplayMonitors", h_enum_monitors),
    W64_FN("MsgWaitForMultipleObjects", h_msg_wait),
};
W64_EXPORTS(user32_module, "user32.dll", user32_exports);

/* arrays of handles: 8 bytes each */
static const struct w64_export wait_exports[] = {
    W64_FN("MsgWaitForMultipleObjectsEx", h_msg_wait),
    W64_FN("WSAWaitForMultipleEvents", h_msg_wait),
};
W64_EXPORTS(wait_module, "*", wait_exports);

static const struct w64_export gdi32_exports[] = {
    W64_FN("CreateDIBSection", h_dib_section),
    W64_FN("GetObjectA", h_get_object),
    W64_FN("GetObjectW", h_get_object),
};
W64_EXPORTS(gdi32_module, "gdi32.dll", gdi32_exports);
