#include <synth/config.h>
#include <synth/string.h>

#include "win32.h"
#if CONFIG_GUI
#include "w32gui.h"
#endif

struct data_entry {
    uint32_t rva, size, codepage, reserved;
};

static const uint8_t *module_base(HANDLE module)
{
    if (!module)
        return w32->main ? (const uint8_t *)w32->main->base : NULL;
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (img->base == (uint32_t)module && !img->builtin)
            return (const uint8_t *)img->base;
    return NULL;
}

static const uint8_t *resource_root(const uint8_t *base)
{
    if (!base)
        return NULL;
    uint32_t pe = *(const uint32_t *)(base + 0x3c);
    const uint8_t *opt = base + pe + 24;
    uint16_t magic = *(const uint16_t *)opt;
    const uint8_t *dirs = opt + (magic == 0x20b ? 112 : 96);
    uint32_t count = *(const uint32_t *)(opt + (magic == 0x20b ? 108 : 92));
    if (count < 3)
        return NULL;
    uint32_t rva = *(const uint32_t *)(dirs + 16);
    return rva ? base + rva : NULL;
}

static bool name_matches(const uint8_t *root, uint32_t offset, const void *id, bool wide)
{
    const uint16_t *s = (const uint16_t *)(root + (offset & 0x7fffffffu));
    uint16_t len = s[0];
    for (uint16_t i = 0; i < len; i++) {
        uint16_t a = s[1 + i];
        uint16_t b = wide ? ((const uint16_t *)id)[i] : (uint8_t)((const char *)id)[i];
        if (!b)
            return false;
        if (a >= 'a' && a <= 'z')
            a -= 32;
        if (b >= 'a' && b <= 'z')
            b -= 32;
        if (a != b)
            return false;
    }
    return wide ? ((const uint16_t *)id)[len] == 0 : ((const char *)id)[len] == 0;
}

static uintptr_t numeric(const void *id, bool wide)
{
    if ((uintptr_t)id < 0x10000)
        return (uintptr_t)id;
    uintptr_t v = 0;
    if (wide) {
        const uint16_t *s = id;
        if (*s != '#')
            return 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
            v = v * 10 + (*s - '0');
    } else {
        const char *s = id;
        if (*s != '#')
            return 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
            v = v * 10 + (uintptr_t)(*s - '0');
    }
    return v;
}

static const uint8_t *lookup(const uint8_t *root, const uint8_t *dir, const void *id, bool wide)
{
    uint16_t named = *(const uint16_t *)(dir + 12), ids = *(const uint16_t *)(dir + 14);
    const uint32_t *entries = (const uint32_t *)(dir + 16);
    uintptr_t want = numeric(id, wide);
    for (uint32_t i = 0; i < (uint32_t)named + ids; i++) {
        uint32_t name = entries[i * 2], target = entries[i * 2 + 1];
        bool hit = !id ? true
                   : (name & 0x80000000u) ? !want && name_matches(root, name, id, wide)
                                          : want && name == want;
        if (hit)
            return root + (target & 0x7fffffffu) + ((target & 0x80000000u) ? 0 : 0);
    }
    return NULL;
}

static const struct data_entry *find_entry(HANDLE module, const void *type, const void *name, bool wide)
{
    const uint8_t *root = resource_root(module_base(module));
    if (!root)
        return NULL;
    const uint8_t *types = lookup(root, root, type, wide);
    if (!types)
        return NULL;
    const uint8_t *names = lookup(root, types, name, wide);
    if (!names)
        return NULL;
    const uint8_t *langs = lookup(root, names, NULL, wide);
    return (const struct data_entry *)langs;
}

static void *entry_name(const uint8_t *root, uint32_t name, bool wide, void **copy)
{
    if (!(name & 0x80000000u))
        return (void *)(uintptr_t)name;
    const uint16_t *s = (const uint16_t *)(root + (name & 0x7fffffffu));
    if (!(*copy = w32_zalloc((size_t)(s[0] + 1) * 2)))
        return NULL;
    for (uint16_t k = 0; k < s[0]; k++) {
        if (wide)
            ((uint16_t *)*copy)[k] = s[1 + k];
        else
            ((char *)*copy)[k] = (char)(s[1 + k] < 0x80 ? s[1 + k] : '?');
    }
    return *copy;
}

typedef BOOL(WINAPI *enum_names_fn)(HANDLE, const void *, const void *, intptr_t);

static BOOL enum_names(HANDLE module, const void *type, enum_names_fn fn, intptr_t param, bool wide)
{
    const uint8_t *root = resource_root(module_base(module));
    const uint8_t *dir = root ? lookup(root, root, type, wide) : NULL;
    if (!dir || !fn) {
        w32_set_error(1813);
        return FALSE;
    }
    uint32_t count = (uint32_t)*(const uint16_t *)(dir + 12) + *(const uint16_t *)(dir + 14);
    const uint32_t *entries = (const uint32_t *)(dir + 16);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t name = entries[i * 2];
        void *copy = NULL, *arg = entry_name(root, name, wide, &copy);
        if (!arg)
            return FALSE;
        BOOL go = W32_CALL(__typeof__(fn), fn, module, type, arg, param);
        w32_free(copy);
        if (!go)
            break;
    }
    return TRUE;
}

typedef BOOL(WINAPI *enum_types_fn)(HANDLE, const void *, intptr_t);

static BOOL enum_types(HANDLE module, enum_types_fn fn, intptr_t param, bool wide)
{
    const uint8_t *root = resource_root(module_base(module));
    if (!root || !fn) {
        w32_set_error(1813);
        return FALSE;
    }
    uint32_t count = (uint32_t)*(const uint16_t *)(root + 12) + *(const uint16_t *)(root + 14);
    const uint32_t *entries = (const uint32_t *)(root + 16);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t name = entries[i * 2];
        void *copy = NULL, *arg = entry_name(root, name, wide, &copy);
        if (!arg)
            return FALSE;
        BOOL go = W32_CALL(__typeof__(fn), fn, module, arg, param);
        w32_free(copy);
        if (!go)
            break;
    }
    return TRUE;
}

static BOOL WINAPI EnumResourceTypesW(HANDLE module, enum_types_fn fn, intptr_t param)
{
    return enum_types(module, fn, param, true);
}

static BOOL WINAPI EnumResourceTypesA(HANDLE module, enum_types_fn fn, intptr_t param)
{
    return enum_types(module, fn, param, false);
}

static BOOL WINAPI EnumResourceNamesW(HANDLE module, const WCHAR *type, enum_names_fn fn, intptr_t param)
{
    return enum_names(module, type, fn, param, true);
}

static BOOL WINAPI EnumResourceNamesA(HANDLE module, const char *type, enum_names_fn fn, intptr_t param)
{
    return enum_names(module, type, fn, param, false);
}

const void *w32_find_resource(HANDLE module, const void *type, const void *name, uint32_t *size)
{
    const struct data_entry *e = find_entry(module, type, name, false);
    if (!e)
        return NULL;
    if (size)
        *size = e->size;
    return module_base(module) + e->rva;
}

static HANDLE WINAPI FindResourceA(HANDLE module, LPCSTR name, LPCSTR type)
{
    const struct data_entry *e = find_entry(module, type, name, false);
    if (!e)
        w32_set_error(1814);
    return (HANDLE)e;
}

static HANDLE WINAPI FindResourceW(HANDLE module, LPCWSTR name, LPCWSTR type)
{
    const struct data_entry *e = find_entry(module, type, name, true);
    if (!e)
        w32_set_error(1814);
    return (HANDLE)e;
}

static HANDLE WINAPI FindResourceExA(HANDLE module, LPCSTR type, LPCSTR name, WORD lang)
{
    UNUSED(lang);
    return FindResourceA(module, name, type);
}

static HANDLE WINAPI FindResourceExW(HANDLE module, LPCWSTR type, LPCWSTR name, WORD lang)
{
    UNUSED(lang);
    return FindResourceW(module, name, type);
}

static HANDLE WINAPI LoadResource(HANDLE module, HANDLE res)
{
    const struct data_entry *e = res;
    const uint8_t *base = module_base(module);
    return e && base ? (HANDLE)(base + e->rva) : NULL;
}

static void *WINAPI LockResource(HANDLE data)
{
    return data;
}

static DWORD WINAPI SizeofResource(HANDLE module, HANDLE res)
{
    UNUSED(module);
    const struct data_entry *e = res;
    return e ? e->size : 0;
}

static BOOL WINAPI FreeResource(HANDLE data)
{
    UNUSED(data);
    return FALSE;
}

static const uint16_t *string_resource(HANDLE inst, UINT id, int *len)
{
    uint32_t size;
    const uint16_t *p = w32_find_resource(inst, (const void *)6, (const void *)(uintptr_t)((id >> 4) + 1), &size);
    if (!p)
        return NULL;
    for (UINT i = 0; i < (id & 15); i++)
        p += 1 + *p;
    *len = *p;
    return p + 1;
}

static int WINAPI LoadStringW(HANDLE inst, UINT id, LPWSTR buf, int cap)
{
    int len = 0;
    const uint16_t *s = string_resource(inst, id, &len);
    if (cap == 0) {
        if (buf)
            *(const uint16_t **)buf = s;
        return s ? len : 0;
    }
    if (!buf || cap < 1)
        return 0;
    int n = s ? MIN(len, cap - 1) : 0;
    if (n)
        memcpy(buf, s, (size_t)n * 2);
    buf[n] = 0;
    return n;
}

static int WINAPI LoadStringA(HANDLE inst, UINT id, LPSTR buf, int cap)
{
    if (!buf || cap < 1)
        return 0;
    int len = 0;
    const uint16_t *s = string_resource(inst, id, &len);
    if (!s) {
        buf[0] = 0;
        return 0;
    }
    size_t n = w32_wide_to_cp(s, len, buf, (size_t)cap - 1, w32_acp());
    n = MIN(n, (size_t)cap - 1);
    buf[n] = 0;
    return (int)n;
}

#if CONFIG_GUI
static const uint16_t *menu_items(HANDLE menu, const uint16_t *p, int depth)
{
    for (int guard = 0; guard < 256; guard++) {
        uint16_t flags = *p++;
        uint16_t id = 0;
        if (!(flags & 0x10))
            id = *p++;
        const uint16_t *text = p;
        while (*p)
            p++;
        p++;
        char *label = w32_narrow((LPCWSTR)text);
        if (flags & 0x10) {
            HANDLE sub = w32_menu_new();
            p = menu_items(sub, p, depth + 1);
            w32_menu_append(menu, 0x10, (uintptr_t)sub, label);
        } else {
            w32_menu_append(menu, label && *label ? (flags & 0x0b) : 0x800, id, label);
        }
        w32_free(label);
        if (flags & 0x80)
            break;
    }
    return p;
}

/* MENUEX items: DWORD type, state, id; WORD flags; text; DWORD-aligned, popups add a help id before their items */
static const uint8_t *menuex_items(HANDLE menu, const uint8_t *base, const uint8_t *p, int depth)
{
    for (int guard = 0; guard < 256 && depth < 8; guard++) {
        p = base + (((size_t)(p - base) + 3) & ~(size_t)3);
        uint32_t type, state, id;
        uint16_t res;
        memcpy(&type, p, 4);
        memcpy(&state, p + 4, 4);
        memcpy(&id, p + 8, 4);
        memcpy(&res, p + 12, 2);
        const uint16_t *text = (const uint16_t *)(p + 14);
        const uint16_t *t = text;
        while (*t)
            t++;
        p = (const uint8_t *)(t + 1);
        char *label = w32_narrow((LPCWSTR)text);
        if (res & 1) {
            p = base + (((size_t)(p - base) + 3) & ~(size_t)3) + 4;
            HANDLE sub = w32_menu_new();
            p = menuex_items(sub, base, p, depth + 1);
            w32_menu_append(menu, 0x10 | (state & 0xb), (uintptr_t)sub, label);
        } else {
            w32_menu_append(menu, (type & 0x800) || !label || !*label ? 0x800 : (state & 0xb), id, label);
        }
        w32_free(label);
        if (res & 0x80)
            break;
    }
    return p;
}

static HANDLE load_menu(HANDLE inst, const void *name, bool wide)
{
    const struct data_entry *e = find_entry(inst, (const void *)4, name, wide);
    const uint8_t *base = module_base(inst);
    if (!e || !base)
        return NULL;
    const uint16_t *p = (const uint16_t *)(base + e->rva);
    if (p[0] > 1)
        return NULL;
    HANDLE menu = w32_menu_new();
    if (menu && p[0] == 1)
        menuex_items(menu, (const uint8_t *)p, (const uint8_t *)p + 4 + p[1], 0);
    else if (menu)
        menu_items(menu, p + 2 + p[1] / 2, 0);
    return menu;
}

static HANDLE WINAPI LoadMenuA(HANDLE inst, LPCSTR name)
{
    return load_menu(inst, name, false);
}

static HANDLE WINAPI LoadMenuW(HANDLE inst, LPCWSTR name)
{
    return load_menu(inst, name, true);
}

static HANDLE WINAPI LoadMenuIndirectA(const void *templ)
{
    const uint16_t *p = templ;
    HANDLE menu = w32_menu_new();
    if (menu && p[0] == 1)
        menuex_items(menu, (const uint8_t *)p, (const uint8_t *)p + 4 + p[1], 0);
    else if (menu && p[0] == 0)
        menu_items(menu, p + 2 + p[1] / 2, 0);
    return menu;
}

struct accel {
    uint16_t flags, key, cmd, pad;
};

static HANDLE load_accel(HANDLE inst, const void *name, bool wide)
{
    const struct data_entry *e = find_entry(inst, (const void *)9, name, wide);
    const uint8_t *base = module_base(inst);
    return e && base ? (HANDLE)(base + e->rva) : NULL;
}

static HANDLE WINAPI LoadAcceleratorsA(HANDLE inst, LPCSTR name)
{
    return load_accel(inst, name, false);
}

static HANDLE WINAPI LoadAcceleratorsW(HANDLE inst, LPCWSTR name)
{
    return load_accel(inst, name, true);
}

static int WINAPI TranslateAcceleratorA(HANDLE hwnd, HANDLE table, MSG *msg)
{
    const struct accel *a = table;
    if (!a || !msg)
        return 0;
    bool keydown = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
    bool chr = msg->message == WM_CHAR || msg->message == WM_SYSCHAR;
    if (!keydown && !chr)
        return 0;
    typedef short WINAPI (*state_fn)(int);
    struct w32_image *u = w32_find_module("user32.dll");
    state_fn state = u ? (state_fn)w32_get_proc(u, "GetKeyState", 0) : NULL;
    bool shift = state && W32_CALL(state_fn, state, 0x10) < 0, ctrl = state && W32_CALL(state_fn, state, 0x11) < 0,
         alt = msg->message == WM_SYSKEYDOWN;
    for (int i = 0; i < 256; i++) {
        bool virt = a[i].flags & 1;
        if (virt && keydown && a[i].key == msg->wParam && !!(a[i].flags & 4) == shift &&
            !!(a[i].flags & 8) == ctrl && !!(a[i].flags & 0x10) == alt) {
            w32_send(w32_win(hwnd), WM_COMMAND, (WPARAM)(a[i].cmd | 1u << 16), 0);
            return 1;
        }
        if (!virt && chr && a[i].key == msg->wParam) {
            w32_send(w32_win(hwnd), WM_COMMAND, (WPARAM)(a[i].cmd | 1u << 16), 0);
            return 1;
        }
        if (a[i].flags & 0x80)
            break;
    }
    return 0;
}

static HANDLE WINAPI CreateAcceleratorTableW(const uint8_t *list, int n)
{
    if (!list || n <= 0)
        return NULL;
    struct accel *a = w32_zalloc((size_t)n * sizeof(*a) + 8);
    if (!a)
        return NULL;
    memcpy(a, "SYACCEL", 8);
    struct accel *t = (struct accel *)((uint8_t *)a + 8);
    for (int i = 0; i < n; i++, list += 6) {
        t[i].flags = list[0];
        memcpy(&t[i].key, list + 2, 2);
        memcpy(&t[i].cmd, list + 4, 2);
    }
    t[n - 1].flags |= 0x80;
    return t;
}

static int WINAPI CopyAcceleratorTableW(HANDLE table, uint8_t *out, int n)
{
    const struct accel *a = table;
    if (!a)
        return 0;
    int count = 0;
    while (count < 256 && !(a[count++].flags & 0x80))
        ;
    if (!out)
        return count;
    int i = 0;
    for (; i < n && i < count; i++, out += 6) {
        out[0] = (uint8_t)(a[i].flags & 0x7f);
        out[1] = 0;
        memcpy(out + 2, &a[i].key, 2);
        memcpy(out + 4, &a[i].cmd, 2);
    }
    return i;
}

static BOOL WINAPI DestroyAcceleratorTable(HANDLE table)
{
    uint8_t *p = table ? (uint8_t *)table - 8 : NULL;
    if (p && !memcmp(p, "SYACCEL", 8))
        w32_free(p);
    return TRUE;
}
#endif

static const struct w32_export resource_exports[] = {
    W32_FN(FindResourceA), W32_FN(FindResourceW), W32_FN(FindResourceExA), W32_FN(FindResourceExW),
    W32_FN(EnumResourceNamesW), W32_FN(EnumResourceNamesA),
    W32_FN(EnumResourceTypesW), W32_FN(EnumResourceTypesA),
    W32_FN(LoadResource),  W32_FN(LockResource),  W32_FN(SizeofResource),  W32_FN(FreeResource),
};

static const struct w32_export resource_user_exports[] = {
    W32_FN(LoadStringA), W32_FN(LoadStringW),
#if CONFIG_GUI
    W32_FN(LoadMenuA), W32_FN(LoadMenuW), W32_FN(LoadMenuIndirectA), W32_AS("LoadMenuIndirectW", LoadMenuIndirectA),
    W32_FN(LoadAcceleratorsA), W32_FN(LoadAcceleratorsW), W32_FN(TranslateAcceleratorA),
    W32_AS("TranslateAcceleratorW", TranslateAcceleratorA), W32_FN(DestroyAcceleratorTable),
    W32_FN(CreateAcceleratorTableW), W32_AS("CreateAcceleratorTableA", CreateAcceleratorTableW), W32_FN(CopyAcceleratorTableW),
    W32_AS("CopyAcceleratorTableA", CopyAcceleratorTableW),
#endif
};

W32_EXPORTS(resource_module, "kernel32.dll", resource_exports);
W32_EXPORTS(resource_user_module, "user32.dll", resource_user_exports);
