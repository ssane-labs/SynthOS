#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "w32gui.h"

/* Open and save dialogs: IFileOpenDialog/IFileSaveDialog (what Chromium and most current programs use),
   GetOpenFileName/GetSaveFileName and IShellItem; all of them show the desktop's own panel. */

#define S_OK 0u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define CANCELLED 0x800704c7u
#define FOS_PICKFOLDERS 0x20u
#define SFGAO_FOLDER 0x20000000u
#define SFGAO_FILESYSTEM 0x40000000u

#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                                      \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),               \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), a, b, c, d, e, f, g, h}

static const uint8_t iid_unknown[16] = GUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_modal[16] = GUID(0xb4db1657, 0x70d7, 0x485e, 0x8e, 0x3e, 0x6f, 0xcb, 0x5a, 0x5c, 0x18, 0x02);
static const uint8_t iid_file_dialog[16] =
    GUID(0x42f85136, 0xdb7e, 0x439c, 0x85, 0xf1, 0xe4, 0x07, 0x5d, 0x13, 0x5f, 0xc8);
static const uint8_t iid_open_dialog[16] =
    GUID(0xd57c7288, 0xd4ad, 0x4768, 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60);
static const uint8_t iid_save_dialog[16] =
    GUID(0x84bccd23, 0x5fde, 0x4cdb, 0xae, 0xa4, 0xaf, 0x64, 0xb8, 0x3d, 0x78, 0xab);
static const uint8_t iid_shell_item[16] =
    GUID(0x43826d1e, 0xe718, 0x42ee, 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe);
static const uint8_t iid_shell_item_array[16] =
    GUID(0xb63ea76d, 0x1f85, 0x456f, 0xa1, 0x9c, 0x48, 0x15, 0x9e, 0xfa, 0x85, 0x8b);
static const uint8_t clsid_open_dialog[16] =
    GUID(0xdc1c5a9c, 0xe88a, 0x4dde, 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7);
static const uint8_t clsid_save_dialog[16] =
    GUID(0xc0b4e2f3, 0xba21, 0x4773, 0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b);

static WCHAR *task_string(const char *s)
{
    void *(WINAPI *alloc)(SIZE_T) = (void *(WINAPI *)(SIZE_T))w32_builtin_lookup("ole32.dll", "CoTaskMemAlloc");
    size_t n = strlen(s) + 1;
    WCHAR *w = alloc ? alloc(n * sizeof(WCHAR)) : NULL;
    if (w)
        w32_cp_to_wide(s, -1, w, n, 65001);
    return w;
}

static void to_vfs(const WCHAR *win, char *out, size_t cap)
{
    char *s = w32_narrow(win);
    if (!s || winpath_to_vfs("/", s, out, cap) != 0)
        str_copy(out, s ? s : "", cap);
    w32_free(s);
}

/* Specs like "*.png;*.jpg" become ".png;.jpg"; "*.*" or "*" means any file. */
static void spec_to_exts(const char *spec, char *out, size_t cap)
{
    out[0] = 0;
    for (const char *s = spec; s && *s;) {
        while (*s == ' ' || *s == ';')
            s++;
        const char *end = s;
        while (*end && *end != ';')
            end++;
        if (end - s >= 1 && (!strncmp(s, "*.*", 3) || (end - s == 1 && *s == '*'))) {
            out[0] = 0;
            return;
        }
        if (*s == '*')
            s++;
        if (end > s && strlen(out) + (size_t)(end - s) + 2 < cap) {
            if (out[0])
                str_append(out, ";", cap);
            size_t n = strlen(out);
            memcpy(out + n, s, (size_t)(end - s));
            out[n + (size_t)(end - s)] = 0;
        }
        s = end;
    }
}

struct shell_item {
    const void *vt;
    LONG refs;
    char path[PATH_MAX_LEN];
};

static const void *const item_vt[];

static void *new_item(const char *vfs_path)
{
    struct shell_item *i = w32_zalloc(sizeof(*i));
    if (!i)
        return NULL;
    i->vt = item_vt;
    i->refs = 1;
    str_copy(i->path, vfs_path, sizeof(i->path));
    return i;
}

static uint32_t WINAPI item_query(struct shell_item *i, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (memcmp(iid, iid_unknown, 16) && memcmp(iid, iid_shell_item, 16)) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    i->refs++;
    *out = i;
    return S_OK;
}

static ULONG WINAPI item_add_ref(struct shell_item *i)
{
    return (ULONG)++i->refs;
}

static ULONG WINAPI item_release(struct shell_item *i)
{
    if (--i->refs > 0)
        return (ULONG)i->refs;
    w32_free(i);
    return 0;
}

static uint32_t WINAPI item_bind(struct shell_item *i, void *bc, const void *bhid, const void *iid, void **out)
{
    UNUSED(i);
    UNUSED(bc);
    UNUSED(bhid);
    UNUSED(iid);
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI item_parent(struct shell_item *i, void **out)
{
    char dir[PATH_MAX_LEN];
    path_dirname(i->path, dir, sizeof(dir));
    *out = new_item(dir);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static uint32_t WINAPI item_name(struct shell_item *i, DWORD sigdn, WCHAR **out)
{
    char win[PATH_MAX_LEN];
    if (sigdn == 0 || sigdn == 0x80001001u)
        str_copy(win, path_basename(i->path), sizeof(win));
    else
        w32_path_to_win(i->path, win, sizeof(win));
    *out = task_string(win);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static uint32_t WINAPI item_attributes(struct shell_item *i, DWORD mask, DWORD *out)
{
    DWORD a = SFGAO_FILESYSTEM | (vfs_is_dir(i->path) ? SFGAO_FOLDER : 0);
    *out = a & mask;
    return *out == mask ? S_OK : 1u;
}

static uint32_t WINAPI item_compare(struct shell_item *i, struct shell_item *other, DWORD hint, int *order)
{
    UNUSED(hint);
    *order = other && other->vt == item_vt ? strcmp(i->path, other->path) : 1;
    return *order ? 1u : S_OK;
}

static const void *const item_vt[] = {item_query, item_add_ref, item_release,    item_bind,
                                      item_parent, item_name,   item_attributes, item_compare};

/* The path of any IShellItem, ours or the program's: GetDisplayName(SIGDN_FILESYSPATH). */
static bool item_path(void *item, char *out, size_t cap)
{
    if (!item)
        return false;
    typedef uint32_t(WINAPI * name_fn)(void *, DWORD, WCHAR **);
    WCHAR *w = NULL;
    if (((name_fn)(*(void ***)item)[5])(item, 0x80058000u, &w) || !w)
        return false;
    to_vfs(w, out, cap);
    void(WINAPI * task_free)(void *) = (void(WINAPI *)(void *))w32_builtin_lookup("ole32.dll", "CoTaskMemFree");
    if (task_free)
        task_free(w);
    return true;
}

struct item_array {
    const void *vt;
    LONG refs;
    void *item;
};

static uint32_t WINAPI array_query(struct item_array *a, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (memcmp(iid, iid_unknown, 16) && memcmp(iid, iid_shell_item_array, 16)) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    a->refs++;
    *out = a;
    return S_OK;
}

static ULONG WINAPI array_add_ref(struct item_array *a)
{
    return (ULONG)++a->refs;
}

static ULONG WINAPI array_release(struct item_array *a)
{
    if (--a->refs > 0)
        return (ULONG)a->refs;
    item_release(a->item);
    w32_free(a);
    return 0;
}

static uint32_t WINAPI array_bind(void *a, void *bc, const void *bhid, const void *iid, void **out)
{
    UNUSED(a);
    UNUSED(bc);
    UNUSED(bhid);
    UNUSED(iid);
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI array_notimpl3(void *a, uint32_t b, const void *c, void *d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    if (d)
        *(void **)d = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI array_count(struct item_array *a, DWORD *out)
{
    UNUSED(a);
    *out = 1;
    return S_OK;
}

static uint32_t WINAPI array_at(struct item_array *a, DWORD index, void **out)
{
    if (index)
        return E_INVALIDARG;
    item_add_ref(a->item);
    *out = a->item;
    return S_OK;
}

static uint32_t WINAPI array_enum(struct item_array *a, void **out)
{
    UNUSED(a);
    *out = NULL;
    return E_NOTIMPL;
}

static const void *const array_vt[] = {array_query,    array_add_ref, array_release, array_bind, array_notimpl3,
                                       array_notimpl3, array_notimpl3, array_count,  array_at,   array_enum};

struct file_dialog {
    const void *vt;
    LONG refs;
    bool save, chosen;
    DWORD options;
    UINT type_index;
    char title[80], button[32], name[NAME_MAX_LEN], ext[16];
    char dir[PATH_MAX_LEN], result[PATH_MAX_LEN];
    char specs[8][200];
    int spec_count;
};

#define DIALOG(p) ((struct file_dialog *)(p))

static uint32_t WINAPI fd_query(void *p, const uint8_t *iid, void **out)
{
    struct file_dialog *d = DIALOG(p);
    if (!out)
        return E_POINTER;
    if (memcmp(iid, iid_unknown, 16) && memcmp(iid, iid_modal, 16) && memcmp(iid, iid_file_dialog, 16) &&
        memcmp(iid, d->save ? iid_save_dialog : iid_open_dialog, 16)) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    d->refs++;
    *out = d;
    return S_OK;
}

static ULONG WINAPI fd_add_ref(void *p)
{
    return (ULONG)++DIALOG(p)->refs;
}

static ULONG WINAPI fd_release(void *p)
{
    struct file_dialog *d = DIALOG(p);
    if (--d->refs > 0)
        return (ULONG)d->refs;
    w32_free(d);
    return 0;
}

static uint32_t WINAPI fd_show(void *p, HANDLE owner)
{
    UNUSED(owner);
    struct file_dialog *d = DIALOG(p);
    char exts[200] = "";
    if (d->spec_count)
        str_copy(exts, d->specs[MIN((int)MAX(d->type_index, 1u), d->spec_count) - 1], sizeof(exts));
    struct gui_picker o = {.save = d->save, .folders = (d->options & FOS_PICKFOLDERS) != 0,
                           .title = d->title[0] ? d->title : NULL, .dir = d->dir[0] ? d->dir : NULL,
                           .name = d->name[0] ? d->name : NULL, .exts = d->save ? NULL : exts,
                           .button = d->button[0] ? d->button : NULL};
    d->chosen = w32_gui_pick_file(&o, d->result, sizeof(d->result));
    if (!d->chosen)
        return CANCELLED;
    if (d->save && d->ext[0] && !strchr(path_basename(d->result), '.')) {
        str_append(d->result, ".", sizeof(d->result));
        str_append(d->result, d->ext, sizeof(d->result));
    }
    str_copy(d->name, path_basename(d->result), sizeof(d->name));
    return S_OK;
}

typedef struct {
    const WCHAR *name, *spec;
} FILTERSPEC;

static uint32_t WINAPI fd_set_file_types(void *p, UINT count, const FILTERSPEC *specs)
{
    struct file_dialog *d = DIALOG(p);
    d->spec_count = 0;
    for (UINT i = 0; specs && i < count && d->spec_count < (int)ARRAY_SIZE(d->specs); i++) {
        char *s = w32_narrow(specs[i].spec);
        spec_to_exts(s, d->specs[d->spec_count++], sizeof(d->specs[0]));
        w32_free(s);
    }
    return S_OK;
}

static uint32_t WINAPI fd_set_type_index(void *p, UINT index)
{
    DIALOG(p)->type_index = index;
    return S_OK;
}

static uint32_t WINAPI fd_get_type_index(void *p, UINT *index)
{
    *index = MAX(DIALOG(p)->type_index, 1u);
    return S_OK;
}

static uint32_t WINAPI fd_advise(void *p, void *events, DWORD *cookie)
{
    UNUSED(p);
    UNUSED(events);
    *cookie = 1;
    return S_OK;
}

static uint32_t WINAPI fd_ok0(void *p)
{
    UNUSED(p);
    return S_OK;
}

static uint32_t WINAPI fd_ok1(void *p, uint32_t a)
{
    UNUSED(p);
    UNUSED(a);
    return S_OK;
}

static uint32_t WINAPI fd_ok2(void *p, uint32_t a, uint32_t b)
{
    UNUSED(p);
    UNUSED(a);
    UNUSED(b);
    return S_OK;
}

static uint32_t WINAPI fd_set_options(void *p, DWORD options)
{
    DIALOG(p)->options = options;
    return S_OK;
}

static uint32_t WINAPI fd_get_options(void *p, DWORD *options)
{
    *options = DIALOG(p)->options;
    return S_OK;
}

static uint32_t WINAPI fd_set_folder(void *p, void *item)
{
    struct file_dialog *d = DIALOG(p);
    char path[PATH_MAX_LEN];
    if (item_path(item, path, sizeof(path)) && vfs_is_dir(path))
        str_copy(d->dir, path, sizeof(d->dir));
    return S_OK;
}

static uint32_t WINAPI fd_get_folder(void *p, void **out)
{
    struct file_dialog *d = DIALOG(p);
    char dir[PATH_MAX_LEN];
    if (d->chosen)
        path_dirname(d->result, dir, sizeof(dir));
    else
        str_copy(dir, d->dir[0] ? d->dir : "/home", sizeof(dir));
    *out = new_item(dir);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static uint32_t WINAPI fd_get_result(void *p, void **out)
{
    struct file_dialog *d = DIALOG(p);
    if (!out)
        return E_POINTER;
    *out = d->chosen ? new_item(d->result) : NULL;
    return *out ? S_OK : 0x8000ffffu;
}

static uint32_t WINAPI fd_set_file_name(void *p, const WCHAR *name)
{
    struct file_dialog *d = DIALOG(p);
    char *s = name ? w32_narrow(name) : NULL;
    char *base = s ? strrchr(s, '\\') : NULL;
    str_copy(d->name, base ? base + 1 : s ? s : "", sizeof(d->name));
    w32_free(s);
    return S_OK;
}

static uint32_t WINAPI fd_get_file_name(void *p, WCHAR **out)
{
    *out = task_string(DIALOG(p)->name);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static void narrow_into(const WCHAR *w, char *out, size_t cap)
{
    char *s = w ? w32_narrow(w) : NULL;
    str_copy(out, s ? s : "", cap);
    w32_free(s);
}

static uint32_t WINAPI fd_set_title(void *p, const WCHAR *title)
{
    narrow_into(title, DIALOG(p)->title, sizeof(DIALOG(p)->title));
    return S_OK;
}

static uint32_t WINAPI fd_set_ok_label(void *p, const WCHAR *label)
{
    struct file_dialog *d = DIALOG(p);
    narrow_into(label, d->button, sizeof(d->button));
    char *amp = strchr(d->button, '&');
    if (amp)
        memmove(amp, amp + 1, strlen(amp));
    return S_OK;
}

static uint32_t WINAPI fd_set_default_ext(void *p, const WCHAR *ext)
{
    struct file_dialog *d = DIALOG(p);
    narrow_into(ext, d->ext, sizeof(d->ext));
    if (d->ext[0] == '.')
        memmove(d->ext, d->ext + 1, strlen(d->ext));
    return S_OK;
}

static uint32_t WINAPI fd_get_results(void *p, void **out)
{
    struct file_dialog *d = DIALOG(p);
    if (!d->chosen) {
        *out = NULL;
        return 0x8000ffffu;
    }
    struct item_array *a = w32_zalloc(sizeof(*a));
    if (!a || !(a->item = new_item(d->result))) {
        w32_free(a);
        *out = NULL;
        return E_OUTOFMEMORY;
    }
    a->vt = array_vt;
    a->refs = 1;
    *out = a;
    return S_OK;
}

static uint32_t WINAPI fd_set_save_as_item(void *p, void *item)
{
    struct file_dialog *d = DIALOG(p);
    char path[PATH_MAX_LEN];
    if (item_path(item, path, sizeof(path))) {
        path_dirname(path, d->dir, sizeof(d->dir));
        str_copy(d->name, path_basename(path), sizeof(d->name));
    }
    return S_OK;
}

static uint32_t WINAPI fd_notimpl1(void *p, void **out)
{
    UNUSED(p);
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI fd_notimpl4(void *p, void *a, void *b, void *c, void *d)
{
    UNUSED(p);
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return E_NOTIMPL;
}

/* IUnknown, IModalWindow, IFileDialog (23 methods), then IFileOpenDialog's two or IFileSaveDialog's five. */
#define DIALOG_SLOTS                                                                                                   \
    fd_query, fd_add_ref, fd_release, fd_show, fd_set_file_types, fd_set_type_index, fd_get_type_index, fd_advise,    \
        fd_ok1, fd_set_options, fd_get_options, fd_set_folder, fd_set_folder, fd_get_folder, fd_get_result,          \
        fd_set_file_name, fd_get_file_name, fd_set_title, fd_set_ok_label, fd_ok1, fd_get_result, fd_ok2,            \
        fd_set_default_ext, fd_ok1, fd_ok1, fd_ok0, fd_ok1

static const void *const open_vt[] = {DIALOG_SLOTS, fd_get_results, fd_get_results};
static const void *const save_vt[] = {DIALOG_SLOTS, fd_set_save_as_item, fd_ok1, fd_ok2, fd_notimpl1, fd_notimpl4};

bool w32_is_file_dialog(const uint8_t *clsid)
{
    return !memcmp(clsid, clsid_open_dialog, 16) || !memcmp(clsid, clsid_save_dialog, 16);
}

uint32_t w32_new_file_dialog(const uint8_t *clsid, const uint8_t *iid, void **out)
{
    struct file_dialog *d = w32_zalloc(sizeof(*d));
    if (!d)
        return E_OUTOFMEMORY;
    d->save = !memcmp(clsid, clsid_save_dialog, 16);
    d->vt = d->save ? save_vt : open_vt;
    d->refs = 1;
    uint32_t hr = fd_query(d, iid, out);
    fd_release(d);
    return hr;
}

static uint32_t WINAPI SHCreateItemFromParsingName(const WCHAR *path, void *bc, const uint8_t *iid, void **out)
{
    UNUSED(bc);
    if (!out || !path)
        return E_POINTER;
    *out = NULL;
    char vfs[PATH_MAX_LEN];
    to_vfs(path, vfs, sizeof(vfs));
    if (!vfs_exists(vfs))
        return 0x80070002u;
    struct shell_item *i = new_item(vfs);
    if (!i)
        return E_OUTOFMEMORY;
    uint32_t hr = item_query(i, iid, out);
    item_release(i);
    return hr;
}

/* OPENFILENAME is the same for A and W apart from the string type. */
typedef struct {
    DWORD size;
    HANDLE owner, instance;
    const void *filter;
    void *custom_filter;
    DWORD custom_max, filter_index;
    void *file;
    DWORD file_max;
    void *file_title;
    DWORD file_title_max;
    const void *initial_dir, *title;
    DWORD flags;
    WORD file_offset, file_extension;
    const void *default_ext;
} OPENFILENAME_COMMON;

static void common_string(const void *s, bool wide, char *out, size_t cap)
{
    if (!s)
        out[0] = 0;
    else if (wide)
        narrow_into(s, out, cap);
    else
        str_copy(out, s, cap);
}

static void put_string(void *dst, DWORD cap, bool wide, const char *s)
{
    if (!dst || !cap)
        return;
    if (wide)
        w32_cp_to_wide(s, -1, dst, cap, 65001);
    else
        str_copy(dst, s, cap);
}

static BOOL file_name_dialog(OPENFILENAME_COMMON *o, bool wide, bool save)
{
    if (!o || !o->file)
        return FALSE;
    char exts[200] = "", dir[PATH_MAX_LEN] = "", name[PATH_MAX_LEN], title[80], ext[16];
    /* The filter is pairs of strings ("Text", "*.txt", ...), ended by an empty one; nFilterIndex counts from 1. */
    const uint8_t *f = o->filter;
    for (DWORD i = 1; f && (wide ? *(const WCHAR *)f : *f); i++) {
        char label[120], spec[200];
        common_string(f, wide, label, sizeof(label));
        f += (wide ? (w32_wcslen((const WCHAR *)f) + 1) * 2 : strlen((const char *)f) + 1);
        common_string(f, wide, spec, sizeof(spec));
        f += (wide ? (w32_wcslen((const WCHAR *)f) + 1) * 2 : strlen((const char *)f) + 1);
        if (i == MAX(o->filter_index, 1u))
            spec_to_exts(spec, exts, sizeof(exts));
    }
    char win[PATH_MAX_LEN];
    common_string(o->initial_dir, wide, win, sizeof(win));
    if (win[0] && winpath_to_vfs("/", win, dir, sizeof(dir)) != 0)
        dir[0] = 0;
    common_string(o->file, wide, win, sizeof(win));
    char *base = strrchr(win, '\\');
    str_copy(name, base ? base + 1 : win, sizeof(name));
    if (base && !dir[0]) {
        *base = 0;
        if (winpath_to_vfs("/", win, dir, sizeof(dir)) != 0)
            dir[0] = 0;
    }
    common_string(o->title, wide, title, sizeof(title));
    common_string(o->default_ext, wide, ext, sizeof(ext));
    struct gui_picker p = {.save = save, .title = title[0] ? title : NULL, .dir = dir[0] ? dir : NULL,
                           .name = name[0] ? name : NULL, .exts = save ? NULL : exts};
    char chosen[PATH_MAX_LEN];
    if (!w32_gui_pick_file(&p, chosen, sizeof(chosen)))
        return FALSE;
    if (save && ext[0] && !strchr(path_basename(chosen), '.')) {
        str_append(chosen, ".", sizeof(chosen));
        str_append(chosen, ext, sizeof(chosen));
    }
    w32_path_to_win(chosen, win, sizeof(win));
    put_string(o->file, o->file_max, wide, win);
    const char *slash = strrchr(win, '\\'), *dot = strrchr(win, '.');
    o->file_offset = (WORD)(slash ? slash - win + 1 : 0);
    o->file_extension = (WORD)(dot && dot > slash ? dot - win + 1 : 0);
    put_string(o->file_title, o->file_title_max, wide, slash ? slash + 1 : win);
    return TRUE;
}

static BOOL WINAPI GetOpenFileNameW(OPENFILENAME_COMMON *o)
{
    return file_name_dialog(o, true, false);
}

static BOOL WINAPI GetSaveFileNameW(OPENFILENAME_COMMON *o)
{
    return file_name_dialog(o, true, true);
}

static BOOL WINAPI GetOpenFileNameA(OPENFILENAME_COMMON *o)
{
    return file_name_dialog(o, false, false);
}

static BOOL WINAPI GetSaveFileNameA(OPENFILENAME_COMMON *o)
{
    return file_name_dialog(o, false, true);
}

static const struct w32_export dialog_shell_exports[] = {
    W32_FN(SHCreateItemFromParsingName),
};

static const struct w32_export dialog_comdlg_exports[] = {
    W32_FN(GetOpenFileNameW),
    W32_FN(GetSaveFileNameW),
    W32_FN(GetOpenFileNameA),
    W32_FN(GetSaveFileNameA),
};

W32_EXPORTS(dialog_shell_module, "shell32.dll", dialog_shell_exports);
W32_EXPORTS(dialog_comdlg_module, "comdlg32.dll", dialog_comdlg_exports);
