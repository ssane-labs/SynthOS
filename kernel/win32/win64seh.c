#include <synth/fmt.h>
#include <synth/string.h>

#include "win64.h"

#define CTX_SIZE 0x4d0
#define C_RAX 0x78
#define C_RSP 0x98
#define C_RIP 0xf8
#define C_XMM 0x1a0
#define DC_SIZE 80
#define MAX_RECORDS 16
#define MAX_FRAMES 4096

enum { UNW_EHANDLER = 1, UNW_UHANDLER = 2, UNW_CHAININFO = 4 };
enum { EH_NONCONTINUABLE = 1, EH_UNWINDING = 2, EH_EXIT_UNWIND = 4, EH_TARGET_UNWIND = 0x20, EH_COLLIDED = 0x40 };
enum { DISPOSITION_CONTINUE_EXECUTION = 0, DISPOSITION_CONTINUE_SEARCH = 1 };

#define STATUS_GCC_THROW 0x20474343u
#define STATUS_UNWIND 0xc0000027u

extern char w64_capture_context[], w64_restore_context[], w64_raise_exception[], w64_unwind_ex[];
extern uint64_t w64_raise_hook, w64_unwind_hook;

struct active_handler {
    bool unwind;
    uint32_t boundary;
    const uint8_t *resume;
    const uint8_t *frame_ctx;
    const uint8_t *dc;
};

static struct active_handler *active[MAX_RECORDS];
static int active_count;

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

static void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static uint64_t *reg(uint8_t *ctx, int index)
{
    return (uint64_t *)(ctx + C_RAX + index * 8);
}

static uint64_t rip(const uint8_t *ctx)
{
    return rd64(ctx + C_RIP);
}

static uint64_t rsp(const uint8_t *ctx)
{
    return rd64(ctx + C_RSP);
}

static struct w32_image *image_of(uint64_t pc)
{
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (img->pe64 && !img->builtin && pc >= img->base && pc < (uint64_t)img->base + img->size)
            return img;
    return NULL;
}

static const uint8_t *function_entry(uint64_t pc, uint64_t *base)
{
    struct w32_image *img = image_of(pc);
    if (!img)
        return NULL;
    const uint8_t *mem = (const uint8_t *)img->base;
    uint32_t opt = rd32(mem + 0x3c) + 24;
    uint32_t rva = rd32(mem + opt + 112 + 3 * 8), size = rd32(mem + opt + 112 + 3 * 8 + 4);
    *base = img->base;
    if (!rva || rva + size > img->size)
        return NULL;
    uint32_t rel = (uint32_t)(pc - img->base), lo = 0, hi = size / 12;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const uint8_t *e = mem + rva + mid * 12;
        if (rel < rd32(e))
            hi = mid;
        else if (rel >= rd32(e + 4))
            lo = mid + 1;
        else {
            for (int depth = 0; depth < 8 && (rd32(e + 8) & 1); depth++)
                e = mem + (rd32(e + 8) & ~1u);
            return e;
        }
    }
    return NULL;
}

static int code_slots(const uint8_t *code)
{
    switch (code[1] & 0xf) {
    case 1: return (code[1] >> 4) ? 3 : 2;
    case 4: case 8: return 2;
    case 5: case 9: return 3;
    default: return 1;
    }
}

static uint64_t virtual_unwind(int type, uint64_t base, uint64_t pc, const uint8_t *entry, uint8_t *ctx,
                               uint64_t *data, uint64_t *frame)
{
    uint64_t handler = 0;
    bool first = true, machine_frame = false;
    *frame = rsp(ctx);
    for (int chain = 0; chain < 32; chain++) {
        const uint8_t *info = (const uint8_t *)(uintptr_t)(base + rd32(entry + 8));
        uint32_t prolog_offset = (uint32_t)(pc - base - rd32(entry));
        uint8_t flags = info[0] >> 3, prolog = info[1], count = info[2];
        uint8_t frame_reg = info[3] & 0xf, frame_offset = info[3] >> 4;
        const uint8_t *codes = info + 4;
        uint64_t base_frame = rsp(ctx);
        if (frame_reg) {
            bool frame_set = prolog_offset >= prolog;
            for (int i = 0; !frame_set && i < count; i += code_slots(codes + i * 2))
                if ((codes[i * 2 + 1] & 0xf) == 3 && prolog_offset >= codes[i * 2])
                    frame_set = true;
            if (frame_set)
                base_frame = *reg(ctx, frame_reg) - frame_offset * 16u;
        }
        if (first)
            *frame = base_frame;
        for (int i = 0; i < count; i += code_slots(codes + i * 2)) {
            const uint8_t *c = codes + i * 2;
            if (prolog_offset < c[0])
                continue;
            int op = c[1] & 0xf, op_info = c[1] >> 4;
            uint16_t slot1 = (uint16_t)(c[2] | c[3] << 8);
            uint32_t slot12 = slot1 | (uint32_t)(c[4] | c[5] << 8) << 16;
            switch (op) {
            case 0:
                *reg(ctx, op_info) = rd64(w64_ptr(rsp(ctx)));
                put64(ctx + C_RSP, rsp(ctx) + 8);
                break;
            case 1:
                put64(ctx + C_RSP, rsp(ctx) + (op_info ? slot12 : slot1 * 8u));
                break;
            case 2:
                put64(ctx + C_RSP, rsp(ctx) + op_info * 8u + 8);
                break;
            case 3:
                put64(ctx + C_RSP, base_frame);
                break;
            case 4:
                *reg(ctx, op_info) = rd64(w64_ptr(base_frame + slot1 * 8u));
                break;
            case 5:
                *reg(ctx, op_info) = rd64(w64_ptr(base_frame + slot12));
                break;
            case 8:
                memcpy(ctx + C_XMM + op_info * 16, w64_ptr(base_frame + slot1 * 16u), 16);
                break;
            case 9:
                memcpy(ctx + C_XMM + op_info * 16, w64_ptr(base_frame + slot12), 16);
                break;
            case 10: {
                uint64_t sp = rsp(ctx) + (op_info ? 8 : 0);
                put64(ctx + C_RIP, rd64(w64_ptr(sp)));
                put64(ctx + C_RSP, rd64(w64_ptr(sp + 24)));
                machine_frame = true;
                break;
            }
            default:
                break;
            }
        }
        if (first && (flags & type)) {
            const uint8_t *h = codes + ((count + 1u) & ~1u) * 2;
            handler = base + rd32(h);
            *data = (uint64_t)(uintptr_t)(h + 4);
        }
        first = false;
        if (!(flags & UNW_CHAININFO))
            break;
        entry = codes + ((count + 1u) & ~1u) * 2;
    }
    if (!machine_frame) {
        put64(ctx + C_RIP, rd64(w64_ptr(rsp(ctx))));
        put64(ctx + C_RSP, rsp(ctx) + 8);
    }
    return handler;
}

static uint64_t step(int type, uint8_t *dc, uint8_t *ctx)
{
    uint64_t pc = rip(ctx), base = 0, data = 0, frame = rsp(ctx), handler = 0;
    const uint8_t *entry = function_entry(pc, &base);
    memset(dc, 0, DC_SIZE);
    put64(dc, pc);
    put64(dc + 8, base);
    put64(dc + 16, (uintptr_t)entry);
    if (entry) {
        handler = virtual_unwind(type, base, pc, entry, ctx, &data, &frame);
    } else {
        put64(ctx + C_RIP, rd64(w64_ptr(rsp(ctx))));
        put64(ctx + C_RSP, rsp(ctx) + 8);
    }
    put64(dc + 24, frame);
    put64(dc + 48, handler);
    put64(dc + 56, data);
    return handler;
}

static uint32_t call_handler(struct active_handler *h, uint64_t handler, uint8_t *rec, uint64_t frame, uint8_t *ctx,
                             uint8_t *dc)
{
    h->boundary = (uint32_t)(uintptr_t)__builtin_frame_address(0);
    if (active_count < MAX_RECORDS)
        active[active_count] = h;
    active_count++;
    uint64_t args[4] = {(uintptr_t)rec, frame, (uintptr_t)ctx, (uintptr_t)dc};
    uint32_t r = (uint32_t)w64_call(handler, 4, args);
    active_count--;
    return r;
}

static NORETURN void restore(uint8_t *ctx)
{
    uint64_t target = rsp(ctx);
    while (active_count > 0 && active_count <= MAX_RECORDS && active[active_count - 1]->boundary < target)
        active_count--;
    uint64_t arg = (uintptr_t)ctx;
    arch_w64_call((uint32_t)(uintptr_t)w64_restore_context, 1, &arg);
    for (;;)
        ;
}

static NORETURN void fail(const char *what, uint32_t code)
{
    ksnprintf(w32->message, sizeof(w32->message), "%s 0x%08x", what, code);
    w32_exit(code);
}

static uint64_t dispatch(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t *rec = w64_ptr(a[0]), *orig = w64_ptr(a[1]);
    uint8_t ctx[CTX_SIZE], dc[DC_SIZE];
    memcpy(ctx, orig, CTX_SIZE);
    int level = MIN(active_count, MAX_RECORDS);
    for (int n = 0; n < MAX_FRAMES && !w32->exiting; n++) {
        if (!image_of(rip(ctx))) {
            if (--level < 0)
                break;
            memcpy(ctx, active[level]->resume, CTX_SIZE);
            continue;
        }
        uint64_t handler = step(UNW_EHANDLER, dc, ctx);
        if (!handler)
            continue;
        put64(dc + 40, (uintptr_t)orig);
        struct active_handler h = {.unwind = false, .resume = orig};
        uint32_t r = call_handler(&h, handler, rec, rd64(dc + 24), orig, dc);
        if (r == DISPOSITION_CONTINUE_EXECUTION) {
            if (rd32(rec + 4) & EH_NONCONTINUABLE)
                fail("crashed: noncontinuable exception", 0xc0000025u);
            restore(orig);
        }
    }
    uint32_t code = rd32(rec);
    if (code == STATUS_GCC_THROW || w32->exiting)
        return 0;
    fail("crashed: unhandled exception", code);
}

static uint64_t unwind(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t end_frame = a[0], target_ip = a[1], retval = a[3];
    uint8_t *rec = w64_ptr(a[2]), *caller = w64_ptr(a[4]);
    uint8_t local[0x98], ctx[CTX_SIZE], next[CTX_SIZE], dc[DC_SIZE];
    if (!rec) {
        memset(local, 0, sizeof(local));
        uint32_t code = STATUS_UNWIND;
        memcpy(local, &code, 4);
        put64(local + 16, rip(caller));
        rec = local;
    }
    uint32_t flags = rd32(rec + 4) | EH_UNWINDING | (end_frame ? 0 : EH_EXIT_UNWIND);
    memcpy(ctx, caller, CTX_SIZE);
    memcpy(next, ctx, CTX_SIZE);
    int level = MIN(active_count, MAX_RECORDS);
    for (int n = 0; n < MAX_FRAMES && !w32->exiting; n++) {
        uint64_t handler;
        if (!image_of(rip(next))) {
            if (--level < 0)
                fail("crashed: bad unwind target", 0xc0000029u);
            struct active_handler *outer = active[level];
            if (!outer->unwind) {
                memcpy(ctx, outer->resume, CTX_SIZE);
                memcpy(next, ctx, CTX_SIZE);
                continue;
            }
            memcpy(ctx, outer->frame_ctx, CTX_SIZE);
            memcpy(next, ctx, CTX_SIZE);
            uint8_t scratch[DC_SIZE];
            step(UNW_UHANDLER, scratch, next);
            memcpy(dc, outer->dc, DC_SIZE);
            handler = rd64(dc + 48);
            flags |= EH_COLLIDED;
        } else {
            handler = step(UNW_UHANDLER, dc, next);
        }
        uint64_t frame = rd64(dc + 24);
        put64(dc + 32, target_ip);
        put64(dc + 40, (uintptr_t)ctx);
        if (handler) {
            if (end_frame && frame > end_frame)
                fail("crashed: bad unwind target", 0xc0000029u);
            if (frame == end_frame)
                flags |= EH_TARGET_UNWIND;
            memcpy(rec + 4, &flags, 4);
            struct active_handler h = {.unwind = true, .resume = caller, .frame_ctx = ctx, .dc = dc};
            call_handler(&h, handler, rec, frame, ctx, dc);
            flags = rd32(rec + 4) & ~EH_COLLIDED;
        }
        if (frame == end_frame)
            break;
        memcpy(ctx, next, CTX_SIZE);
    }
    put64(ctx + C_RAX, retval);
    put64(ctx + C_RIP, target_ip);
    restore(ctx);
}

static uint64_t h_lookup_function_entry(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t base = 0;
    const uint8_t *e = function_entry(a[0], &base);
    if (a[1])
        put64(w64_ptr(a[1]), e ? base : 0);
    return (uintptr_t)e;
}

static uint64_t h_virtual_unwind(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t data = 0, frame = 0;
    uint64_t handler = virtual_unwind((int)a[0], a[1], a[2], w64_ptr(a[3]), w64_ptr(a[4]), &data, &frame);
    if (a[5])
        put64(w64_ptr(a[5]), data);
    if (a[6])
        put64(w64_ptr(a[6]), frame);
    return handler;
}

void w64_seh_setup(void)
{
    if (!w64_raise_hook)
        w64_raise_hook = (uintptr_t)w64_kernel_stub(dispatch, "RaiseException");
    if (!w64_unwind_hook)
        w64_unwind_hook = (uintptr_t)w64_kernel_stub(unwind, "RtlUnwindEx");
}

static const struct w64_export seh_exports[] = {
    W64_NATIVE("RtlCaptureContext", w64_capture_context),
    W64_NATIVE("RtlRestoreContext", w64_restore_context),
    W64_NATIVE("RaiseException", w64_raise_exception),
    W64_NATIVE("RtlUnwindEx", w64_unwind_ex),
    W64_FN("RtlLookupFunctionEntry", h_lookup_function_entry),
    W64_FN("RtlVirtualUnwind", h_virtual_unwind),
};
W64_EXPORTS(seh_kernel32, "kernel32.dll", seh_exports);
W64_EXPORTS(seh_ntdll, "ntdll.dll", seh_exports);
