#include <synth/lnk.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define E_OUTOFMEMORY 0x8007000eu
#define REGDB_E_CLASSNOTREG 0x80040154u
#define CO_E_OBJNOTCONNECTED 0x800401fdu
#define RPC_E_CALL_COMPLETE 0x80010117u
#define STG_E_FILENOTFOUND 0x80030002u
#define MAX_CLASSES 16

#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                                      \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),               \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), a, b, c, d, e, f, g, h}

static const uint8_t iid_unknown[16] = GUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_class_factory[16] = GUID(1, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t clsid_shell_link[16] = GUID(0x00021401, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_shell_link_w[16] = GUID(0x000214f9, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_persist[16] = GUID(0x0000010c, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_persist_file[16] = GUID(0x0000010b, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_property_store[16] =
    GUID(0x886d8eeb, 0x8cf2, 0x4446, 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99);

static uint32_t com_call(void *obj, int slot, uint32_t a, uint32_t b, uint32_t c)
{
    typedef uint32_t(WINAPI * fn3)(void *, uint32_t, uint32_t, uint32_t);
    return W32_CALL(fn3, (*(void ***)obj)[slot], obj, a, b, c);
}

/* Class objects a program registered with CoRegisterClassObject: CoCreateInstance finds them in that process. */
static W32_STATE struct registered_class {
    uint8_t clsid[16];
    void *factory;
    DWORD cookie;
} classes[MAX_CLASSES];
static W32_STATE DWORD next_cookie;
static W32_STATE LONG server_refs;

static uint32_t WINAPI CoRegisterClassObject(const uint8_t *clsid, void *unk, DWORD ctx, DWORD flags, DWORD *cookie)
{
    UNUSED(ctx);
    UNUSED(flags);
    if (!clsid || !unk || !cookie)
        return E_POINTER;
    for (int i = 0; i < MAX_CLASSES; i++)
        if (!classes[i].factory) {
            com_call(unk, 1, 0, 0, 0);
            memcpy(classes[i].clsid, clsid, 16);
            classes[i].factory = unk;
            classes[i].cookie = *cookie = ++next_cookie;
            return S_OK;
        }
    return E_OUTOFMEMORY;
}

static uint32_t WINAPI CoRevokeClassObject(DWORD cookie)
{
    for (int i = 0; i < MAX_CLASSES; i++)
        if (classes[i].factory && classes[i].cookie == cookie) {
            void *f = classes[i].factory;
            classes[i].factory = NULL;
            com_call(f, 2, 0, 0, 0);
            return S_OK;
        }
    return CO_E_OBJNOTCONNECTED;
}

static ULONG WINAPI CoAddRefServerProcess(void)
{
    return (ULONG)++server_refs;
}

static ULONG WINAPI CoReleaseServerProcess(void)
{
    return server_refs > 0 ? (ULONG)--server_refs : 0;
}

static uint32_t WINAPI CoIncrementMTAUsage(void **cookie)
{
    if (!cookie)
        return E_POINTER;
    *cookie = (void *)(uintptr_t)++server_refs;
    return S_OK;
}

static uint32_t WINAPI CoGetCallContext(const void *iid, void **out)
{
    UNUSED(iid);
    if (out)
        *out = NULL;
    return RPC_E_CALL_COMPLETE;
}

struct shell_link {
    void *link_vt;
    void *persist_vt;
    void *props_vt;
    LONG refs;
    struct lnk l;
    WCHAR app_id[128];
};

#define LINK_OF(p, field) ((struct shell_link *)((uint8_t *)(p) - __builtin_offsetof(struct shell_link, field)))

static uint32_t link_query(struct shell_link *s, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!memcmp(iid, iid_unknown, 16) || !memcmp(iid, iid_shell_link_w, 16))
        *out = &s->link_vt;
    else if (!memcmp(iid, iid_persist_file, 16) || !memcmp(iid, iid_persist, 16))
        *out = &s->persist_vt;
    else if (!memcmp(iid, iid_property_store, 16))
        *out = &s->props_vt;
    else {
        *out = NULL;
        return E_NOINTERFACE;
    }
    s->refs++;
    return S_OK;
}

static ULONG link_release(struct shell_link *s)
{
    if (--s->refs > 0)
        return (ULONG)s->refs;
    w32_free(s);
    return 0;
}

static void to_utf8(const WCHAR *w, char *out, size_t cap)
{
    if (!w) {
        out[0] = 0;
        return;
    }
    size_t n = w32_wide_to_cp(w, -1, out, cap, 65001);
    out[n < cap ? n : cap - 1] = 0;
}

static uint32_t to_wide(const char *s, WCHAR *out, int cap)
{
    if (!out || cap <= 0)
        return E_POINTER;
    size_t n = w32_cp_to_wide(s, -1, out, (size_t)cap, 65001);
    out[n < (size_t)cap ? n : (size_t)cap - 1] = 0;
    return S_OK;
}

static uint32_t WINAPI sl_query(void *p, const uint8_t *iid, void **out) { return link_query(LINK_OF(p, link_vt), iid, out); }
static ULONG WINAPI sl_add_ref(void *p) { return (ULONG)++LINK_OF(p, link_vt)->refs; }
static ULONG WINAPI sl_release(void *p) { return link_release(LINK_OF(p, link_vt)); }

static uint32_t WINAPI sl_get_path(void *p, WCHAR *out, int cap, void *find, DWORD flags)
{
    UNUSED(find);
    UNUSED(flags);
    struct shell_link *s = LINK_OF(p, link_vt);
    to_wide(s->l.target, out, cap);
    return s->l.target[0] ? S_OK : S_FALSE;
}

static uint32_t WINAPI sl_get_id_list(void *p, void **out)
{
    UNUSED(p);
    *out = NULL;
    return S_FALSE;
}

static uint32_t WINAPI sl_set_id_list(void *p, const void *pidl)
{
    UNUSED(p);
    UNUSED(pidl);
    return E_NOTIMPL;
}

#define STRING_PROPERTY(name, field)                                                                                   \
    static uint32_t WINAPI sl_get_##name(void *p, WCHAR *out, int cap)                                                \
    {                                                                                                                  \
        return to_wide(LINK_OF(p, link_vt)->l.field, out, cap);                                                       \
    }                                                                                                                  \
    static uint32_t WINAPI sl_set_##name(void *p, const WCHAR *text)                                                  \
    {                                                                                                                  \
        struct shell_link *s = LINK_OF(p, link_vt);                                                                    \
        to_utf8(text, s->l.field, sizeof(s->l.field));                                                                 \
        return S_OK;                                                                                                   \
    }

STRING_PROPERTY(description, desc)
STRING_PROPERTY(working_dir, dir)
STRING_PROPERTY(arguments, args)

static uint32_t WINAPI sl_get_hotkey(void *p, uint16_t *key)
{
    UNUSED(p);
    *key = 0;
    return S_OK;
}

static uint32_t WINAPI sl_set_hotkey(void *p, uint16_t key)
{
    UNUSED(p);
    UNUSED(key);
    return S_OK;
}

static uint32_t WINAPI sl_get_show(void *p, int *show)
{
    struct shell_link *s = LINK_OF(p, link_vt);
    *show = s->l.show ? s->l.show : 1;
    return S_OK;
}

static uint32_t WINAPI sl_set_show(void *p, int show)
{
    LINK_OF(p, link_vt)->l.show = show;
    return S_OK;
}

static uint32_t WINAPI sl_get_icon(void *p, WCHAR *out, int cap, int *index)
{
    struct shell_link *s = LINK_OF(p, link_vt);
    if (index)
        *index = s->l.icon_index;
    return to_wide(s->l.icon, out, cap);
}

static uint32_t WINAPI sl_set_icon(void *p, const WCHAR *path, int index)
{
    struct shell_link *s = LINK_OF(p, link_vt);
    to_utf8(path, s->l.icon, sizeof(s->l.icon));
    s->l.icon_index = index;
    return S_OK;
}

static uint32_t WINAPI sl_set_relative(void *p, const WCHAR *path, DWORD reserved)
{
    UNUSED(p);
    UNUSED(path);
    UNUSED(reserved);
    return S_OK;
}

static uint32_t WINAPI sl_resolve(void *p, HANDLE wnd, DWORD flags)
{
    UNUSED(p);
    UNUSED(wnd);
    UNUSED(flags);
    return S_OK;
}

static uint32_t WINAPI sl_set_path(void *p, const WCHAR *path)
{
    struct shell_link *s = LINK_OF(p, link_vt);
    to_utf8(path, s->l.target, sizeof(s->l.target));
    return S_OK;
}

static const void *const shell_link_vt[] W32_VTABLE = {
    sl_query,          sl_add_ref,       sl_release,     sl_get_path,     sl_get_id_list,  sl_set_id_list,
    sl_get_description, sl_set_description, sl_get_working_dir, sl_set_working_dir, sl_get_arguments,
    sl_set_arguments,  sl_get_hotkey,    sl_set_hotkey,  sl_get_show,     sl_set_show,     sl_get_icon,
    sl_set_icon,       sl_set_relative,  sl_resolve,     sl_set_path,
};

static uint32_t WINAPI pf_query(void *p, const uint8_t *iid, void **out) { return link_query(LINK_OF(p, persist_vt), iid, out); }
static ULONG WINAPI pf_add_ref(void *p) { return (ULONG)++LINK_OF(p, persist_vt)->refs; }
static ULONG WINAPI pf_release(void *p) { return link_release(LINK_OF(p, persist_vt)); }

static uint32_t WINAPI pf_class_id(void *p, uint8_t *out)
{
    UNUSED(p);
    memcpy(out, clsid_shell_link, 16);
    return S_OK;
}

static uint32_t WINAPI pf_is_dirty(void *p)
{
    UNUSED(p);
    return S_OK;
}

static uint32_t WINAPI pf_load(void *p, const WCHAR *name, DWORD mode)
{
    UNUSED(mode);
    struct shell_link *s = LINK_OF(p, persist_vt);
    char win[PATH_MAX_LEN], path[PATH_MAX_LEN];
    to_utf8(name, win, sizeof(win));
    if (w32_path(win, path, sizeof(path)) != 0 || lnk_read(path, &s->l) != 0)
        return STG_E_FILENOTFOUND;
    return S_OK;
}

static uint32_t WINAPI pf_save(void *p, const WCHAR *name, BOOL remember)
{
    UNUSED(remember);
    struct shell_link *s = LINK_OF(p, persist_vt);
    char win[PATH_MAX_LEN], path[PATH_MAX_LEN];
    to_utf8(name, win, sizeof(win));
    uint8_t *buf = w32_alloc(8192);
    if (!buf)
        return E_OUTOFMEMORY;
    size_t n = lnk_encode(&s->l, buf, 8192);
    int err = !n || w32_path(win, path, sizeof(path)) != 0 ? -1 : vfs_write_all(path, buf, n);
    w32_free(buf);
    return err ? E_FAIL : S_OK;
}

static uint32_t WINAPI pf_save_completed(void *p, const WCHAR *name)
{
    UNUSED(p);
    UNUSED(name);
    return S_OK;
}

static uint32_t WINAPI pf_cur_file(void *p, WCHAR **out)
{
    UNUSED(p);
    *out = NULL;
    return S_FALSE;
}

static const void *const persist_file_vt[] W32_VTABLE = {pf_query, pf_add_ref, pf_release, pf_class_id, pf_is_dirty,
                                              pf_load,  pf_save,    pf_save_completed, pf_cur_file};

static uint32_t WINAPI ps_query(void *p, const uint8_t *iid, void **out) { return link_query(LINK_OF(p, props_vt), iid, out); }
static ULONG WINAPI ps_add_ref(void *p) { return (ULONG)++LINK_OF(p, props_vt)->refs; }
static ULONG WINAPI ps_release(void *p) { return link_release(LINK_OF(p, props_vt)); }

static uint32_t WINAPI ps_count(void *p, DWORD *count)
{
    UNUSED(p);
    *count = 0;
    return S_OK;
}

static uint32_t WINAPI ps_at(void *p, DWORD index, void *key)
{
    UNUSED(p);
    UNUSED(index);
    UNUSED(key);
    return 0x80070057u;
}

/* PKEY_AppUserModel_ID is {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5; strings are VT_LPWSTR (31). */
static const uint8_t pkey_app_id[16] = GUID(0x9f4c2855, 0x9f79, 0x4b39, 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3);

static bool is_app_id(const uint8_t *key)
{
    uint32_t pid;
    memcpy(&pid, key + 16, 4);
    return !memcmp(key, pkey_app_id, 16) && pid == 5;
}

static uint32_t WINAPI ps_get(void *p, const uint8_t *key, uint16_t *value)
{
    struct shell_link *s = LINK_OF(p, props_vt);
    memset(value, 0, 16);
    if (is_app_id(key) && s->app_id[0]) {
        size_t n = (w32_wcslen(s->app_id) + 1) * sizeof(WCHAR);
        void *(WINAPI *alloc)(SIZE_T) = (void *(WINAPI *)(SIZE_T))w32_builtin_lookup("ole32.dll", "CoTaskMemAlloc");
        WCHAR *copy = alloc ? alloc(n) : NULL;
        if (copy) {
            memcpy(copy, s->app_id, n);
            value[0] = 31;
            memcpy(value + 4, &copy, sizeof(copy));
        }
    }
    return S_OK;
}

static uint32_t WINAPI ps_set(void *p, const uint8_t *key, const uint16_t *value)
{
    struct shell_link *s = LINK_OF(p, props_vt);
    if (is_app_id(key) && value[0] == 31) {
        const WCHAR *text;
        memcpy(&text, value + 4, sizeof(text));
        size_t n = MIN(w32_wcslen(text), ARRAY_SIZE(s->app_id) - 1);
        memcpy(s->app_id, text, n * sizeof(WCHAR));
        s->app_id[n] = 0;
    }
    return S_OK;
}

static uint32_t WINAPI ps_commit(void *p)
{
    UNUSED(p);
    return S_OK;
}

static const void *const property_store_vt[] W32_VTABLE = {ps_query, ps_add_ref, ps_release, ps_count,
                                                ps_at,    ps_get,     ps_set,     ps_commit};

static uint32_t new_shell_link(const uint8_t *iid, void **out)
{
    struct shell_link *s = w32_alloc(sizeof(*s));
    if (!s)
        return E_OUTOFMEMORY;
    memset(s, 0, sizeof(*s));
    s->link_vt = (void *)shell_link_vt;
    s->persist_vt = (void *)persist_file_vt;
    s->props_vt = (void *)property_store_vt;
    s->refs = 1;
    uint32_t hr = link_query(s, iid, out);
    link_release(s);
    return hr;
}

static uint32_t WINAPI CoGetClassObject(const uint8_t *clsid, DWORD ctx, void *info, const uint8_t *iid, void **out)
{
    UNUSED(ctx);
    UNUSED(info);
    if (!out)
        return E_POINTER;
    *out = NULL;
    for (int i = 0; clsid && i < MAX_CLASSES; i++)
        if (classes[i].factory && !memcmp(classes[i].clsid, clsid, 16))
            return com_call(classes[i].factory, 0, (uint32_t)(uintptr_t)iid, (uint32_t)(uintptr_t)out, 0);
    return REGDB_E_CLASSNOTREG;
}

static uint32_t WINAPI CoCreateInstance(const uint8_t *clsid, void *outer, DWORD ctx, const uint8_t *iid, void **out)
{
    if (!out || !clsid || !iid)
        return E_POINTER;
    *out = NULL;
    if (!memcmp(clsid, clsid_shell_link, 16))
        return outer ? 0x80040110u : new_shell_link(iid, out);
    if (w32_is_file_dialog(clsid))
        return outer ? 0x80040110u : w32_new_file_dialog(clsid, iid, out);
    void *factory;
    uint32_t hr = CoGetClassObject(clsid, ctx, NULL, iid_class_factory, &factory);
    if (hr)
        return hr;
    hr = com_call(factory, 3, (uint32_t)(uintptr_t)outer, (uint32_t)(uintptr_t)iid, (uint32_t)(uintptr_t)out);
    com_call(factory, 2, 0, 0, 0);
    return hr;
}

/* PSCreateMemoryPropertyStore: an IPropertyStore that only keeps values (jump lists and AppUserModel IDs) */
#define STORE_MAX 16

struct memory_store {
    const void *vt;
    LONG refs;
    int count;
    struct {
        uint8_t key[20];
        uint16_t value[8];
    } items[STORE_MAX];
};

#define STORE(p) ((struct memory_store *)(p))

static uint32_t variant_copy(uint16_t *to, const uint16_t *from)
{
    uint32_t(WINAPI * fn)(uint16_t *, const uint16_t *) =
        (uint32_t(WINAPI *)(uint16_t *, const uint16_t *))w32_builtin_lookup("ole32.dll", "PropVariantCopy");
    return fn ? fn(to, from) : E_POINTER;
}

static void variant_clear(uint16_t *v)
{
    uint32_t(WINAPI * fn)(uint16_t *) = (uint32_t(WINAPI *)(uint16_t *))w32_builtin_lookup("ole32.dll", "PropVariantClear");
    if (fn)
        fn(v);
}

static uint32_t WINAPI ms_query(void *p, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (memcmp(iid, iid_unknown, 16) && memcmp(iid, iid_property_store, 16)) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    STORE(p)->refs++;
    *out = p;
    return S_OK;
}

static ULONG WINAPI ms_add_ref(void *p)
{
    return (ULONG)++STORE(p)->refs;
}

static ULONG WINAPI ms_release(void *p)
{
    struct memory_store *m = STORE(p);
    if (--m->refs > 0)
        return (ULONG)m->refs;
    for (int i = 0; i < m->count; i++)
        variant_clear(m->items[i].value);
    w32_free(m);
    return 0;
}

static uint32_t WINAPI ms_count(void *p, DWORD *count)
{
    if (!count)
        return E_POINTER;
    *count = (DWORD)STORE(p)->count;
    return S_OK;
}

static uint32_t WINAPI ms_at(void *p, DWORD index, void *key)
{
    if ((int)index >= STORE(p)->count || !key)
        return 0x80070057u;
    memcpy(key, STORE(p)->items[index].key, 20);
    return S_OK;
}

static int ms_find(struct memory_store *m, const uint8_t *key)
{
    for (int i = 0; i < m->count; i++)
        if (!memcmp(m->items[i].key, key, 20))
            return i;
    return -1;
}

static uint32_t WINAPI ms_get(void *p, const uint8_t *key, uint16_t *value)
{
    if (!key || !value)
        return E_POINTER;
    int i = ms_find(STORE(p), key);
    memset(value, 0, 16);
    return i < 0 ? S_OK : variant_copy(value, STORE(p)->items[i].value);
}

static uint32_t WINAPI ms_set(void *p, const uint8_t *key, const uint16_t *value)
{
    struct memory_store *m = STORE(p);
    if (!key || !value)
        return E_POINTER;
    int i = ms_find(m, key);
    if (i < 0) {
        if (m->count == STORE_MAX)
            return E_OUTOFMEMORY;
        i = m->count++;
        memcpy(m->items[i].key, key, 20);
    } else {
        variant_clear(m->items[i].value);
    }
    return variant_copy(m->items[i].value, value);
}

static const void *const memory_store_vt[] W32_VTABLE = {ms_query, ms_add_ref, ms_release, ms_count,
                                              ms_at,    ms_get,     ms_set,     ps_commit};

static uint32_t WINAPI PSCreateMemoryPropertyStore(const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    struct memory_store *m = w32_alloc(sizeof(*m));
    if (!m)
        return E_OUTOFMEMORY;
    memset(m, 0, sizeof(*m));
    m->vt = memory_store_vt;
    m->refs = 1;
    uint32_t hr = ms_query(m, iid, out);
    ms_release(m);
    return hr;
}

static const struct w32_export memory_store_exports[] = {
    W32_FN(PSCreateMemoryPropertyStore),
};

W32_EXPORTS(memory_store_module, "propsys.dll", memory_store_exports);

static const struct w32_export shelllink_exports[] = {
    W32_FN(CoCreateInstance),
    W32_FN(CoGetClassObject),
    W32_FN(CoRegisterClassObject),
    W32_FN(CoRevokeClassObject),
    W32_FN(CoAddRefServerProcess),
    W32_FN(CoReleaseServerProcess),
    W32_FN(CoGetCallContext),
    W32_FN(CoIncrementMTAUsage),
    W32_AS("CoDecrementMTAUsage", w32_zero1),
    W32_AS("CoResumeClassObjects", w32_zero0),
    W32_AS("CoSuspendClassObjects", w32_zero0),
};

W32_EXPORTS(shelllink_module, "ole32.dll", shelllink_exports);
