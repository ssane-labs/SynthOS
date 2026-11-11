#include <synth/string.h>

#include "win64.h"

#ifdef __x86_64__

/* The native calls of ntcore.c for 64-bit programs: OBJECT_ATTRIBUTES, UNICODE_STRING, IO_STATUS_BLOCK and
   CLIENT_ID hold pointers or handles of 8 bytes, and the base address and size of the memory calls are in/out
   variables of 8 bytes. */

#define P(v) w64_ptr(v)
#define U(v) ((uint32_t)(uintptr_t)(v))

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

struct attr32 {
    uint32_t length, root, name, attributes, security, qos;
    struct {
        uint16_t length, max;
        uint32_t buffer;
    } text;
};

static uint32_t attr_to32(uint64_t at, struct attr32 *o)
{
    if (!at)
        return 0;
    const uint8_t *a = P(at);
    memset(o, 0, sizeof(*o));
    o->length = 24;
    o->root = (uint32_t)rd64(a + 8);
    o->attributes = rd32(a + 24);
    uint64_t us = rd64(a + 16);
    if (us) {
        const uint8_t *s = P(us);
        o->text.length = (uint16_t)(s[0] | s[1] << 8);
        o->text.max = (uint16_t)(s[2] | s[3] << 8);
        o->text.buffer = U(P(rd64(s + 8)));
        o->name = U(&o->text);
    }
    return U(o);
}

static uint64_t done(uint64_t handle_at, uint32_t h, uint64_t iosb_at, const uint32_t *io, uint32_t r)
{
    if (!r && handle_at)
        put64(P(handle_at), h);
    if (iosb_at) {
        uint8_t *o = P(iosb_at);
        put64(o, w64_sx(io[0]));
        put64(o + 8, io[1]);
    }
    return w64_sx(r);
}

static uint64_t h_create_file(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    uint32_t h = 0, io[2] = {0, 0};
    uint32_t r = (uint32_t)T32(t, U(&h), (uint32_t)a[1], attr_to32(a[2], &at), U(io), U(P(a[4])), (uint32_t)a[5],
                               (uint32_t)a[6], (uint32_t)a[7], (uint32_t)a[8], U(P(a[9])), (uint32_t)a[10]);
    return done(a[0], h, a[3], io, r);
}

static uint64_t h_open_file(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    uint32_t h = 0, io[2] = {0, 0};
    uint32_t r = (uint32_t)T32(t, U(&h), (uint32_t)a[1], attr_to32(a[2], &at), U(io), (uint32_t)a[4], (uint32_t)a[5]);
    return done(a[0], h, a[3], io, r);
}

static uint64_t h_query_attributes(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    return w64_sx((uint32_t)T32(t, attr_to32(a[0], &at), U(P(a[1]))));
}

/* (process, &base, x, &size, ...): base and size go in and come back */
static uint64_t memory_call(uint64_t *a, void *t, int size_at, int extra)
{
    uint32_t base = a[1] ? U(P(rd64(P(a[1])))) : 0, size = a[size_at] ? (uint32_t)rd64(P(a[size_at])) : 0;
    uint32_t r;
    if (size_at == 3)
        r = (uint32_t)T32(t, (uint32_t)a[0], a[1] ? U(&base) : 0, (uint32_t)a[2], a[3] ? U(&size) : 0,
                          (uint32_t)a[4], (uint32_t)a[5]);
    else if (extra)
        r = (uint32_t)T32(t, (uint32_t)a[0], a[1] ? U(&base) : 0, a[2] ? U(&size) : 0, (uint32_t)a[3],
                          U(P(a[4])));
    else
        r = (uint32_t)T32(t, (uint32_t)a[0], a[1] ? U(&base) : 0, a[2] ? U(&size) : 0, (uint32_t)a[3]);
    if (!r) {
        if (a[1])
            put64(P(a[1]), base);
        if (a[size_at])
            put64(P(a[size_at]), size);
    }
    return w64_sx(r);
}

static uint64_t h_allocate(uint64_t *a, const uint64_t *f, void *t)
{
    return memory_call(a, t, 3, 0);
}

static uint64_t h_free(uint64_t *a, const uint64_t *f, void *t)
{
    return memory_call(a, t, 2, 0);
}

static uint64_t h_protect(uint64_t *a, const uint64_t *f, void *t)
{
    return memory_call(a, t, 2, 1);
}

static uint64_t h_create_section(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    uint32_t h = 0;
    uint32_t r = (uint32_t)T32(t, U(&h), (uint32_t)a[1], attr_to32(a[2], &at), U(P(a[3])), (uint32_t)a[4],
                               (uint32_t)a[5], (uint32_t)a[6]);
    return done(a[0], h, 0, NULL, r);
}

/* NtOpenProcess/NtOpenThread: CLIENT_ID is two 8-byte handles */
static uint64_t h_open_client(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    uint32_t h = 0, id[2] = {0, 0};
    if (a[3]) {
        const uint8_t *c = P(a[3]);
        id[0] = (uint32_t)rd64(c);
        id[1] = (uint32_t)rd64(c + 8);
    }
    uint32_t r = (uint32_t)T32(t, U(&h), (uint32_t)a[1], attr_to32(a[2], &at), a[3] ? U(id) : 0);
    return done(a[0], h, 0, NULL, r);
}

static uint64_t h_open_object(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    uint32_t h = 0;
    uint32_t r = (uint32_t)T32(t, U(&h), (uint32_t)a[1], attr_to32(a[2], &at));
    return done(a[0], h, 0, NULL, r);
}

static uint64_t h_create_event(uint64_t *a, const uint64_t *f, void *t)
{
    struct attr32 at;
    uint32_t h = 0;
    uint32_t r = (uint32_t)T32(t, U(&h), (uint32_t)a[1], attr_to32(a[2], &at), (uint32_t)a[3], (uint32_t)a[4] & 0xff);
    return done(a[0], h, 0, NULL, r);
}

/* ThreadImpersonationToken carries an 8-byte handle */
static uint64_t h_set_thread_info(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t cls = (uint32_t)a[1], token = 0;
    if (cls == 5 && a[2] && (uint32_t)a[3] >= 8) {
        token = (uint32_t)rd64(P(a[2]));
        return w64_sx((uint32_t)T32(t, (uint32_t)a[0], cls, U(&token), 4u));
    }
    return w64_sx((uint32_t)T32(t, (uint32_t)a[0], cls, U(P(a[2])), (uint32_t)a[3]));
}

static const struct w64_export nt_exports[] = {
    W64_FN("NtCreateFile", h_create_file),
    W64_FN("ZwCreateFile", h_create_file),
    W64_FN("NtOpenFile", h_open_file),
    W64_FN("ZwOpenFile", h_open_file),
    W64_FN("NtQueryAttributesFile", h_query_attributes),
    W64_FN("ZwQueryAttributesFile", h_query_attributes),
    W64_FN("NtQueryFullAttributesFile", h_query_attributes),
    W64_FN("ZwQueryFullAttributesFile", h_query_attributes),
    W64_FN("NtAllocateVirtualMemory", h_allocate),
    W64_FN("ZwAllocateVirtualMemory", h_allocate),
    W64_FN("NtFreeVirtualMemory", h_free),
    W64_FN("ZwFreeVirtualMemory", h_free),
    W64_FN("NtProtectVirtualMemory", h_protect),
    W64_FN("ZwProtectVirtualMemory", h_protect),
    W64_FN("NtCreateSection", h_create_section),
    W64_FN("ZwCreateSection", h_create_section),
    W64_FN("NtOpenProcess", h_open_client),
    W64_FN("ZwOpenProcess", h_open_client),
    W64_FN("NtOpenThread", h_open_client),
    W64_FN("ZwOpenThread", h_open_client),
    W64_FN("NtOpenEvent", h_open_object),
    W64_FN("ZwOpenEvent", h_open_object),
    W64_FN("NtCreateEvent", h_create_event),
    W64_FN("ZwCreateEvent", h_create_event),
    W64_FN("NtSetInformationThread", h_set_thread_info),
    W64_FN("ZwSetInformationThread", h_set_thread_info),
    W64_OUT("NtDuplicateObject", 1u << 3),
    W64_OUT("ZwDuplicateObject", 1u << 3),
    W64_OUT("NtOpenProcessToken", 1u << 2),
    W64_OUT("ZwOpenProcessToken", 1u << 2),
    W64_OUT("NtOpenProcessTokenEx", 1u << 3),
    W64_OUT("ZwOpenProcessTokenEx", 1u << 3),
    W64_OUT("NtOpenThreadToken", 1u << 3),
    W64_OUT("ZwOpenThreadToken", 1u << 3),
    W64_OUT("NtOpenThreadTokenEx", 1u << 4),
    W64_OUT("ZwOpenThreadTokenEx", 1u << 4),
};
W64_EXPORTS(nt_module, "ntdll.dll", nt_exports);

#endif
