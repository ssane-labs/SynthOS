#include <synth/arch_win32.h>
#include <synth/string.h>

#include "crt.h"

/* C++ exceptions the way MSVC builds them on x86: throw raises SEH exception 0xe06d7363 carrying the object and its
   ThrowInfo; each function with try blocks or destructors registers __CxxFrameHandler with its FuncInfo, and the
   handler finds a matching catch, runs the destructors of the frames in between and of its own inner states, copies
   the object into the catch variable, runs the catch funclet and resumes at the address it returns. */

#define CXX_EXCEPTION 0xe06d7363u
#define CXX_MAGIC_VC6 0x19930520u
#define CXX_MAGIC_VC8 0x19930522u
#define EH_NONCONTINUABLE 1
#define EH_UNWINDING 0x6
#define FUNC_SYNCHRONOUS 1
#define TYPE_CONST 1
#define TYPE_VOLATILE 2
#define TYPE_REFERENCE 8
#define CLASS_SIMPLE 1
#define CLASS_VIRTUAL_BASE 4

struct cxx_type_descriptor {
    void *vtable;
    void *spare;
    char name[];
};

struct cxx_offsets {
    int this_offset, vbase_descr, vbase_offset;
};

struct cxx_catchable {
    uint32_t flags;
    const struct cxx_type_descriptor *type;
    struct cxx_offsets offsets;
    uint32_t size;
    void *copy_ctor;
};

struct cxx_catchable_table {
    uint32_t count;
    const struct cxx_catchable *types[];
};

struct cxx_throw_info {
    uint32_t flags;
    void *destructor;
    void *forward_compat;
    const struct cxx_catchable_table *table;
};

struct cxx_catch {
    uint32_t flags;
    const struct cxx_type_descriptor *type;
    int offset;
    void *handler;
};

struct cxx_try {
    int start, end, catch_level;
    int catch_count;
    const struct cxx_catch *catches;
};

struct cxx_unwind {
    int prev;
    void *action;
};

struct cxx_func_info {
    uint32_t magic;
    int unwind_count;
    const struct cxx_unwind *unwind;
    int try_count;
    const struct cxx_try *tries;
    uint32_t ip_count;
    void *ip_map;
    void *es_types;
    uint32_t flags;
};

struct cxx_frame {
    void *prev;
    void *handler;
    int state;
};

static uint32_t frame_ebp(struct cxx_frame *f)
{
    return (uint32_t)(uintptr_t)f + 12;
}

static void local_unwind(struct cxx_frame *f, const struct cxx_func_info *fi, int to)
{
    int state = f->state;
    while (state != to && state >= 0 && state < fi->unwind_count) {
        const struct cxx_unwind *u = &fi->unwind[state];
        f->state = u->prev;
        if (u->action)
            arch_call_funclet(u->action, frame_ebp(f));
        state = u->prev;
    }
    f->state = to;
}

static void *this_pointer(const struct cxx_offsets *off, void *object)
{
    if (!object)
        return NULL;
    char *p = object;
    if (off->vbase_descr >= 0) {
        char *table = *(char **)(p + off->vbase_descr);
        p += *(int *)(table + off->vbase_offset);
    }
    return p + off->this_offset;
}

static bool matches(const struct cxx_throw_info *ti, const struct cxx_catch *c, const struct cxx_catchable *t)
{
    if (!c->type)
        return true;
    if (!t->type || (c->type != t->type && strcmp(c->type->name, t->type->name)))
        return false;
    if ((ti->flags & TYPE_CONST) && !(c->flags & TYPE_CONST))
        return false;
    return !(ti->flags & TYPE_VOLATILE) || (c->flags & TYPE_VOLATILE);
}

static void copy_object(void *object, void *dest, const struct cxx_catch *c, const struct cxx_catchable *t)
{
    if (c->flags & TYPE_REFERENCE) {
        *(void **)dest = this_pointer(&t->offsets, object);
    } else if (t->flags & CLASS_SIMPLE) {
        memmove(dest, object, t->size);
        if (t->size == sizeof(void *))
            *(void **)dest = this_pointer(&t->offsets, *(void **)dest);
    } else if (t->copy_ctor) {
        bool vbase = t->flags & CLASS_VIRTUAL_BASE;
        arch_thiscall(t->copy_ctor, dest, vbase ? 2 : 1, (uint32_t)(uintptr_t)this_pointer(&t->offsets, object), 1);
    } else {
        memmove(dest, this_pointer(&t->offsets, object), t->size);
    }
}

static NORETURN void run_catch(uint8_t *rec, struct cxx_frame *f, const struct cxx_func_info *fi,
                               const struct cxx_try *tb, const struct cxx_catch *c, const struct cxx_catchable *t)
{
    uint32_t params[3];
    memcpy(params, rec + 20, sizeof(params));
    void *object = (void *)(uintptr_t)params[1];
    const struct cxx_throw_info *ti = (const void *)(uintptr_t)params[2];
    if (t && c->type && c->offset)
        copy_object(object, (char *)(uintptr_t)frame_ebp(f) + c->offset, c, t);
    w32_seh_unwind(f, rec);
    local_unwind(f, fi, tb->start);
    f->state = tb->end + 1;
    struct w32_thread *th = w32->current;
    void *saved_object = th ? th->cxx_object : NULL, *saved_type = th ? th->cxx_type : NULL;
    if (th) {
        th->cxx_object = ti ? object : NULL;
        th->cxx_type = (void *)ti;
    }
    void *resume = (void *)(uintptr_t)arch_call_funclet(c->handler, frame_ebp(f));
    if (th) {
        th->cxx_object = saved_object;
        th->cxx_type = saved_type;
    }
    if (ti && ti->destructor && object)
        arch_thiscall(ti->destructor, object, 0, 0, 0);
    arch_continue_at(resume, frame_ebp(f), ((uint32_t *)f)[-1]);
}

int cxx_frame_handler(const struct cxx_func_info *fi, uint8_t *rec, struct cxx_frame *f, void *ctx, void *dispatch);
int cxx_frame_handler(const struct cxx_func_info *fi, uint8_t *rec, struct cxx_frame *f, void *ctx, void *dispatch)
{
    UNUSED(ctx);
    UNUSED(dispatch);
    uint32_t code, flags, argc, params[3] = {0};
    memcpy(&code, rec, 4);
    memcpy(&flags, rec + 4, 4);
    memcpy(&argc, rec + 16, 4);
    memcpy(params, rec + 20, sizeof(params));
    if (fi->magic < CXX_MAGIC_VC6 || fi->magic > CXX_MAGIC_VC8)
        return 1;
    if (flags & EH_UNWINDING) {
        if (fi->unwind_count)
            local_unwind(f, fi, -1);
        return 1;
    }
    bool cxx = code == CXX_EXCEPTION && argc >= 3 && params[0] >= CXX_MAGIC_VC6 && params[0] <= CXX_MAGIC_VC8;
    if (!cxx && fi->magic >= CXX_MAGIC_VC8 && (fi->flags & FUNC_SYNCHRONOUS))
        return 1;
    const struct cxx_throw_info *ti = cxx ? (const void *)(uintptr_t)params[2] : NULL;
    for (int i = 0; i < fi->try_count; i++) {
        const struct cxx_try *tb = &fi->tries[i];
        if (f->state < tb->start || f->state > tb->end)
            continue;
        for (int j = 0; j < tb->catch_count; j++) {
            const struct cxx_catch *c = &tb->catches[j];
            if (!cxx) {
                if (!c->type)
                    run_catch(rec, f, fi, tb, c, NULL);
                continue;
            }
            for (uint32_t k = 0; ti && ti->table && k < ti->table->count; k++)
                if (matches(ti, c, ti->table->types[k]))
                    run_catch(rec, f, fi, tb, c, ti->table->types[k]);
            if (!c->type)
                run_catch(rec, f, fi, tb, c, NULL);
        }
    }
    return 1;
}
W32_GATE(cxx_frame_handler);

/* throw; inside a catch throws the object being handled again */
static void WINAPI cxx_throw(void *object, const struct cxx_throw_info *ti)
{
    struct w32_thread *th = w32->current;
    if (!object && !ti && th && th->cxx_type) {
        object = th->cxx_object;
        ti = th->cxx_type;
    }
    uint32_t args[3] = {CXX_MAGIC_VC6, (uint32_t)(uintptr_t)object, (uint32_t)(uintptr_t)ti};
    w32_seh_raise(CXX_EXCEPTION, EH_NONCONTINUABLE, 3, args);
}

static int uncaught_exception(void)
{
    return 0;
}

static void type_info_destructor(void)
{
}

static W32_STATE int adjust_fdiv;

extern char crt_cxx_frame_handler[], crt_eh_prolog[];

static const struct w32_export cxxeh_exports[] = {
    W32_AS("_CxxThrowException", cxx_throw),
    W32_AS("__CxxFrameHandler", crt_cxx_frame_handler),
    W32_AS("__CxxFrameHandler2", crt_cxx_frame_handler),
    W32_AS("__CxxFrameHandler3", crt_cxx_frame_handler),
    W32_AS("_EH_prolog", crt_eh_prolog),
    W32_AS("??1type_info@@UAE@XZ", type_info_destructor),
    W32_AS("_adjust_fdiv", &adjust_fdiv),
    W32_AS("__uncaught_exception", uncaught_exception),
    W32_AS("?uncaught_exception@@YA_NXZ", uncaught_exception),
};
CRT_EXPORTS(cxxeh_module, cxxeh_exports);
