#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <winhttp.h>

static LONG WINAPI filter(EXCEPTION_POINTERS *info)
{
    printf("filter saw 0x%08lx\n", info->ExceptionRecord->ExceptionCode);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int data_var = 1;

static volatile LONG wait_hits, wait_timeouts;

static void CALLBACK on_wait(void *ctx, BOOLEAN timed_out)
{
    (void)ctx;
    InterlockedIncrement(timed_out ? &wait_timeouts : &wait_hits);
}

/* What Firefox needs: Microsoft's _snwprintf when the text exactly fills the buffer, CPU sets, a view only at the
   address asked for, object type names and the token's owner */
static void gecko_test(void)
{
    wchar_t buf[4];
    int n = _snwprintf(buf, 4, L"%X", 0xabcd);
    int over = _snwprintf(buf, 4, L"%X", 0xabcde);
    printf("snwprintf exact=%d over=%d\n", n, over);

    typedef BOOL(WINAPI * cpuset_fn)(void *, ULONG, ULONG *, HANDLE, ULONG);
    cpuset_fn cpusets = (cpuset_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation");
    ULONG len = 0xdead;
    unsigned char sets[64];
    BOOL first = cpusets(NULL, 0, &len, NULL, 0);
    BOOL got = cpusets(sets, sizeof(sets), &len, NULL, 0);
    printf("cpusets len=%lu first=%d got=%d size=%u\n", len, first, got, sets[0]);

    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    void *any = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 0);
    void *same = MapViewOfFileEx(sec, FILE_MAP_WRITE, 0, 0, 0, any);
    DWORD err = GetLastError();
    UnmapViewOfFile(any);
    void *again = MapViewOfFileEx(sec, FILE_MAP_WRITE, 0, 0, 0, any);
    printf("view taken=%s err=%lu free=%s\n", same ? "mapped" : "refused", err, again == any ? "same" : "other");

    typedef LONG(WINAPI * query_fn)(HANDLE, int, void *, ULONG, ULONG *);
    query_fn query = (query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    struct {
        USHORT length, max;
        wchar_t *text;
        unsigned char rest[256];
    } info;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    LONG st = query(ev, 2, &info, sizeof(info), NULL);
    printf("object status=%ld type=%.*ls\n", st, info.length / 2, info.text);

    HANDLE token;
    DWORD need = 0;
    unsigned char owner[128];
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    BOOL ok = GetTokenInformation(token, TokenOwner, owner, sizeof(owner), &need);
    printf("owner ok=%d valid=%d\n", ok, ok && IsValidSid(*(PSID *)owner));

    /* a DLL never lands on memory a program only reserved (an allocator's arena) */
    void *arena = VirtualAlloc((void *)0x6e7c0000, 0x100000, MEM_RESERVE, PAGE_READWRITE);
    HMODULE dll = LoadLibraryA("greet.dll");
    printf("dll over reserved=%s\n",
           !arena || !dll ? "skipped" : (char *)dll >= (char *)arena && (char *)dll < (char *)arena + 0x100000 ? "yes" : "no");
}

static void compat(void)
{
    LOGFONTW lf = {0};
    lf.lfHeight = -13;
    lf.lfWeight = FW_BOLD;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    HFONT font = CreateFontIndirectW(&lf);
    LOGFONTW back = {0};
    int size = GetObjectW(font, sizeof(back), &back);
    DeleteObject(font);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    BOOL spi = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    printf("logfontw=%d face=%s ncm=%s\n", size, wcscmp(back.lfFaceName, L"Segoe UI") ? "bad" : "ok",
           spi && ncm.lfMessageFont.lfFaceName[0] ? "ok" : "bad");

    printf("lstrcmpW null=%d,%d,%d\n", lstrcmpW(NULL, L"a") < 0, lstrcmpW(L"a", NULL) > 0, lstrcmpW(NULL, NULL) == 0);

    HANDLE rw = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL), ro = NULL, w = NULL;
    BOOL made = DuplicateHandle(GetCurrentProcess(), rw, GetCurrentProcess(), &ro, FILE_MAP_READ, FALSE, 0);
    BOOL widened = DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &w, FILE_MAP_WRITE, FALSE, 0);
    void *view = MapViewOfFile(ro, FILE_MAP_WRITE, 0, 0, 0);
    printf("section read-only=%s write-dup=%s write-view=%s\n", made ? "ok" : "bad", widened ? "allowed" : "denied",
           view ? "allowed" : "denied");

    char path[MAX_PATH];
    GetTempPathA(sizeof(path) - 16, path);
    strcat(path, "mapped.txt");
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n;
    WriteFile(file, "aaaa", 4, &n, NULL);
    HANDLE map = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    const char *text = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    WriteFile(file, "bb", 2, &n, NULL);
    printf("mapped view=%.4s\n", text ? text : "none");
    UnmapViewOfFile(text);
    CloseHandle(map);
    CloseHandle(file);
    DeleteFileA(path);

    MEMORY_BASIC_INFORMATION mbi = {0};
    VirtualQuery((void *)compat, &mbi, sizeof(mbi));
    DWORD old = 0;
    VirtualProtect(&data_var, sizeof(data_var), PAGE_READONLY, &old);
    DWORD now = 0;
    VirtualProtect(&data_var, sizeof(data_var), old, &now);
    printf("image code=%lx data=%lx protected=%lx\n", mbi.Protect, old, now);

    typedef LONG(WINAPI * query_fn)(HANDLE, int, void *, ULONG, ULONG *);
    query_fn query = (query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    struct {
        USHORT length, max;
        WCHAR *buffer;
        BYTE rest[200];
    } type = {0};
    LONG st = query ? query(rw, 2, &type, sizeof(type), NULL) : -1;
    HANDLE self = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    printf("object type=%ls self=%s\n", st == 0 ? type.buffer : L"?",
           self && self != GetCurrentProcess() ? "handle" : "pseudo");

    HMODULE power = LoadLibraryA("api-ms-win-power-base-l1-1-0.dll"), acc = LoadLibraryA("oleacc.dll");
    printf("power=%s oleacc=%s\n", power && GetProcAddress(power, "CallNtPowerInformation") ? "ok" : "missing",
           acc && GetProcAddress(acc, "AccessibleObjectFromWindow") ? "ok" : "missing");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "wintest";
    RegisterClassA(&wc);
    HWND top = CreateWindowExA(0, "wintest", "", 0, 0, 0, 10, 10, HWND_MESSAGE, NULL, NULL, NULL);
    HWND a = CreateWindowExA(0, "wintest", "", WS_CHILD, 0, 0, 5, 5, top, NULL, NULL, NULL);
    HWND b = CreateWindowExA(0, "wintest", "", WS_CHILD, 0, 0, 5, 5, a, NULL, NULL, NULL);
    HWND loop = SetParent(a, b), moved = SetParent(b, top);
    int children = 0;
    for (HWND c = GetWindow(top, GW_CHILD); c && children < 10; c = GetWindow(c, GW_HWNDNEXT))
        children++;
    printf("reparent loop=%s move=%s children=%d\n", loop ? "allowed" : "refused", moved == a ? "ok" : "bad",
           children);
    DestroyWindow(top);

    HRGN ra = CreateRectRgn(0, 0, 10, 10), rb = CreateRectRgn(5, 5, 20, 20), u = CreateRectRgn(0, 0, 0, 0);
    int kind = CombineRgn(u, ra, rb, RGN_OR);
    RECT box;
    GetRgnBox(u, &box);
    CombineRgn(u, ra, rb, RGN_AND);
    RECT both;
    GetRgnBox(u, &both);
    HDC screen = CreateDCW(L"DISPLAY", NULL, NULL, NULL), mem = CreateCompatibleDC(screen);
    XFORM xf = {0};
    BOOL world = GetWorldTransform(mem, &xf);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB}};
    unsigned *src_px, *dst_px;
    HBITMAP sb = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&src_px, NULL, 0);
    HDC src = CreateCompatibleDC(screen);
    SelectObject(src, sb);
    HBITMAP db = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&dst_px, NULL, 0);
    SelectObject(mem, db);
    *src_px = 0x80800000u;
    *dst_px = 0x000000ffu;
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL blended = GdiAlphaBlend(mem, 0, 0, 1, 1, src, 0, 0, 1, 1, bf);
    printf("gdi region=%d box=%ld,%ld,%ld,%ld and=%ld,%ld,%ld,%ld dc=%s world=%s current=%s blend=%s %06x\n", kind,
           box.left, box.top, box.right, box.bottom, both.left, both.top, both.right, both.bottom, screen ? "ok" : "none",
           world && xf.eM11 == 1.0f ? "identity" : "bad", GetCurrentObject(mem, OBJ_BITMAP) == db ? "ok" : "bad",
           blended ? "ok" : "failed", *dst_px & 0xffffff);
    DeleteDC(src);
    DeleteDC(mem);
    DeleteDC(screen);

    HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT");
    HCERTSTORE banned = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Disallowed");
    HCERTSTORE all = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, NULL);
    BOOL linked = CertAddStoreToCollection(all, roots, 0, 0);
    int nroots = 0, nbanned = 0;
    PCCERT_CONTEXT c = NULL, first = NULL;
    while ((c = CertEnumCertificatesInStore(all, c)))
        if (!nroots++)
            first = CertDuplicateCertificateContext(c);
    while ((c = CertEnumCertificatesInStore(banned, c)))
        nbanned++;
    BYTE hash[20];
    DWORD hlen = sizeof(hash);
    CRYPT_HASH_BLOB blob = {hlen, hash};
    BOOL hashed = first && CryptHashCertificate(0, CALG_SHA1, 0, first->pbCertEncoded, first->cbCertEncoded, hash, &hlen);
    PCCERT_CONTEXT by_hash = hashed ? CertFindCertificateInStore(all, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob,
                                                                 NULL) : NULL;
    DWORD usage_size = 0;
    BOOL usage = first && CertGetEnhancedKeyUsage(first, 0, NULL, &usage_size) && GetLastError() == CRYPT_E_NOT_FOUND;
    printf("certs roots=%s disallowed=%d collection=%s sha1=%s usage=%s\n", nroots > 100 ? "many" : "few", nbanned,
           linked ? "live" : "bad", by_hash ? "found" : hashed ? "missing" : "nohash", usage ? "any" : "bad");
    CertCloseStore(all, 0);
    CertCloseStore(roots, 0);
    CertCloseStore(banned, 0);

    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL), once = NULL, timer = NULL;
    BOOL reg = RegisterWaitForSingleObject(&once, ev, on_wait, NULL, INFINITE, WT_EXECUTEONLYONCE);
    SetEvent(ev);
    for (int i = 0; i < 200 && !wait_hits; i++)
        Sleep(5);
    SetEvent(ev);
    Sleep(50);
    HANDLE never = CreateEventA(NULL, TRUE, FALSE, NULL);
    RegisterWaitForSingleObject(&timer, never, on_wait, NULL, 20, 0);
    for (int i = 0; i < 200 && wait_timeouts < 2; i++)
        Sleep(5);
    BOOL un = UnregisterWaitEx(once, INVALID_HANDLE_VALUE) && UnregisterWaitEx(timer, INVALID_HANDLE_VALUE);
    printf("registered wait=%s hits=%ld timeouts=%s unregister=%s\n", reg ? "ok" : "failed", wait_hits,
           wait_timeouts >= 2 ? "ok" : "missing", un ? "ok" : "failed");
}

/* Installers make shortcuts through IShellLinkW + IPersistFile; SynthOS writes real .lnk files the desktop opens. */
static void shortcut_test(void)
{
    static const GUID clsid_link = {0x00021401, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    static const GUID iid_link = {0x000214f9, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    static const GUID iid_file = {0x0000010b, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    HMODULE ole = LoadLibraryA("ole32.dll"), aut = LoadLibraryA("oleaut32.dll");
    HRESULT(WINAPI * create)(const GUID *, void *, DWORD, const GUID *, void **) =
        (HRESULT(WINAPI *)(const GUID *, void *, DWORD, const GUID *, void **))GetProcAddress(ole, "CoCreateInstance");
    INT(WINAPI * to_variant)(const SYSTEMTIME *, double *) =
        (INT(WINAPI *)(const SYSTEMTIME *, double *))GetProcAddress(aut, "SystemTimeToVariantTime");
    INT(WINAPI * from_variant)(double, SYSTEMTIME *) =
        (INT(WINAPI *)(double, SYSTEMTIME *))GetProcAddress(aut, "VariantTimeToSystemTime");
    IShellLinkW *link = NULL, *back = NULL;
    IPersistFile *file = NULL, *file2 = NULL;
    HRESULT hr = create(&clsid_link, NULL, CLSCTX_INPROC_SERVER, &iid_link, (void **)&link);
    BOOL saved = FALSE;
    if (SUCCEEDED(hr)) {
        link->lpVtbl->SetPath(link, L"C:\\Windows\\notepad.exe");
        link->lpVtbl->SetArguments(link, L"--flag \"a b\"");
        link->lpVtbl->SetDescription(link, L"Test link");
        saved = SUCCEEDED(link->lpVtbl->QueryInterface(link, &iid_file, (void **)&file)) &&
                SUCCEEDED(file->lpVtbl->Save(file, L"C:\\Temp\\test.lnk", TRUE));
    }
    WCHAR path[MAX_PATH] = L"", args[MAX_PATH] = L"";
    if (SUCCEEDED(create(&clsid_link, NULL, CLSCTX_INPROC_SERVER, &iid_link, (void **)&back)) &&
        SUCCEEDED(back->lpVtbl->QueryInterface(back, &iid_file, (void **)&file2)) &&
        SUCCEEDED(file2->lpVtbl->Load(file2, L"C:\\Temp\\test.lnk", STGM_READ))) {
        back->lpVtbl->GetPath(back, path, MAX_PATH, NULL, 0);
        back->lpVtbl->GetArguments(back, args, MAX_PATH);
    }
    printf("shortcut saved=%s path=%ls args=%ls\n", saved ? "ok" : "failed", path, args);
    if (file)
        file->lpVtbl->Release(file);
    if (file2)
        file2->lpVtbl->Release(file2);
    if (link)
        link->lpVtbl->Release(link);
    if (back)
        back->lpVtbl->Release(back);

    SYSTEMTIME st = {2026, 10, 5, 16, 12, 0, 0, 0}, out;
    double v = 0;
    BOOL vt = to_variant(&st, &v) && from_variant(v, &out);
    URL_COMPONENTS uc = {sizeof(uc)};
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwSchemeLength = (DWORD)-1;
    BOOL cracked = WinHttpCrackUrl(L"https://dl.google.com:8443/chrome/install?x=1", 0, 0, &uc);
    printf("misc muldiv=%d,%d vartime=%d %d-%d-%d %d:%02d url=%s:%.*ls:%d%.*ls\n", MulDiv(7, 3, 2), MulDiv(-7, 3, 2),
           (int)v, vt ? out.wYear : 0, out.wMonth, out.wDay, out.wHour, out.wMinute, cracked ? "ok" : "failed",
           (int)uc.dwHostNameLength, uc.lpszHostName, uc.nPort, (int)uc.dwUrlPathLength, uc.lpszUrlPath);
}

/* DirectWrite the way Chromium's renderer uses it: a font collection and font files supplied by the program's own
   loaders (here a one-glyph font built in memory), then glyph coverage through IDWriteGlyphRunAnalysis. */
typedef HRESULT(WINAPI *create_factory_fn)(int, const GUID *, void **);

static unsigned char font[1024];
static int font_size;
static void *factory;

#define CALL(obj, slot, type) ((type)(*(void ***)(obj))[slot])

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char *p, unsigned v)
{
    put16(p, v >> 16);
    put16(p + 2, v & 0xffff);
}

/* Glyph 1 is a square from (100, 0) to (600, 700) in a 1000-unit em, mapped from 'A'. */
static void build_font(void)
{
    static const char *const tags[] = {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name"};
    unsigned char t[8][128];
    int len[8];
    memset(t, 0, sizeof(t));
    unsigned char *cmap = t[0];
    put16(cmap + 2, 1);
    put16(cmap + 4, 3);
    put16(cmap + 6, 1);
    put32(cmap + 8, 12);
    unsigned char *f4 = cmap + 12;
    put16(f4, 4);
    put16(f4 + 2, 32);
    put16(f4 + 6, 4);
    put16(f4 + 8, 4);
    put16(f4 + 10, 1);
    put16(f4 + 14, 'A');
    put16(f4 + 16, 0xffff);
    put16(f4 + 20, 'A');
    put16(f4 + 22, 0xffff);
    put16(f4 + 24, (1 - 'A') & 0xffff);
    put16(f4 + 26, 1);
    len[0] = 12 + 32;
    unsigned char *g = t[1];
    put16(g, 1);
    put16(g + 2, 100);
    put16(g + 6, 600);
    put16(g + 8, 700);
    put16(g + 10, 3);
    memset(g + 14, 1, 4);
    static const short xs[] = {100, 0, 500, 0}, ys[] = {0, 700, 0, -700};
    for (int i = 0; i < 4; i++) {
        put16(g + 18 + i * 2, (unsigned)xs[i] & 0xffff);
        put16(g + 26 + i * 2, (unsigned)ys[i] & 0xffff);
    }
    len[1] = 36;
    put32(t[2], 0x10000);
    put16(t[2] + 18, 1000);
    put16(t[2] + 50, 1);
    len[2] = 54;
    put32(t[3], 0x10000);
    put16(t[3] + 4, 800);
    put16(t[3] + 6, (unsigned)-200 & 0xffff);
    put16(t[3] + 34, 2);
    len[3] = 36;
    put16(t[4], 500);
    put16(t[4] + 4, 700);
    put16(t[4] + 6, 100);
    len[4] = 8;
    put32(t[5] + 8, 36);
    len[5] = 12;
    put32(t[6], 0x5000);
    put16(t[6] + 4, 2);
    len[6] = 6;
    unsigned char *n = t[7];
    put16(n + 2, 1);
    put16(n + 4, 18);
    put16(n + 6, 3);
    put16(n + 8, 1);
    put16(n + 10, 0x409);
    put16(n + 12, 1);
    put16(n + 14, 8);
    for (int i = 0; i < 4; i++)
        put16(n + 18 + i * 2, (unsigned)"Tiny"[i]);
    len[7] = 26;
    put32(font, 0x10000);
    put16(font + 4, 8);
    int pos = 12 + 8 * 16;
    for (int i = 0; i < 8; i++) {
        unsigned char *r = font + 12 + i * 16;
        memcpy(r, tags[i], 4);
        put32(r + 8, (unsigned)pos);
        put32(r + 12, (unsigned)len[i]);
        memcpy(font + pos, t[i], (size_t)len[i]);
        pos += (len[i] + 3) & ~3;
    }
    font_size = pos;
}

struct com {
    void **vtbl;
};

static HRESULT WINAPI qi(void *self, const GUID *iid, void **out)
{
    (void)iid;
    *out = self;
    return S_OK;
}

static ULONG WINAPI ref(void *self)
{
    (void)self;
    return 1;
}

static HRESULT WINAPI stream_read(void *self, const void **start, UINT64 offset, UINT64 size, void **ctx)
{
    (void)self;
    *ctx = NULL;
    if (offset + size > (UINT64)font_size)
        return E_FAIL;
    *start = font + offset;
    return S_OK;
}

static void WINAPI stream_release(void *self, void *ctx)
{
    (void)self;
    (void)ctx;
}

static HRESULT WINAPI stream_size(void *self, UINT64 *size)
{
    (void)self;
    *size = (UINT64)font_size;
    return S_OK;
}

static HRESULT WINAPI stream_time(void *self, UINT64 *time)
{
    (void)self;
    *time = 0;
    return S_OK;
}

static void *stream_vtbl[] = {qi, ref, ref, stream_read, stream_release, stream_size, stream_time};
static struct com stream = {stream_vtbl};

static HRESULT WINAPI loader_stream(void *self, const void *key, UINT32 size, void **out)
{
    (void)self;
    (void)key;
    (void)size;
    *out = &stream;
    return S_OK;
}

static void *loader_vtbl[] = {qi, ref, ref, loader_stream};
static struct com loader = {loader_vtbl};
static int enum_pos;

static HRESULT WINAPI enum_next(void *self, BOOL *more)
{
    (void)self;
    *more = enum_pos++ == 0;
    return S_OK;
}

static HRESULT WINAPI enum_file(void *self, void **out)
{
    (void)self;
    static const int key = 7;
    return CALL(factory, 8, HRESULT(WINAPI *)(void *, const void *, UINT32, void *, void **))(factory, &key,
                                                                                            sizeof(key), &loader, out);
}

static void *enum_vtbl[] = {qi, ref, ref, enum_next, enum_file};
static struct com enumerator = {enum_vtbl};

static HRESULT WINAPI collection_enum(void *self, void *f, const void *key, UINT32 size, void **out)
{
    (void)self;
    (void)f;
    (void)key;
    (void)size;
    enum_pos = 0;
    *out = &enumerator;
    return S_OK;
}

static void *collection_loader_vtbl[] = {qi, ref, ref, collection_enum};
static struct com collection_loader = {collection_loader_vtbl};

struct glyph_run {
    void *face;
    float em;
    UINT32 count;
    const UINT16 *glyphs;
    const float *advances;
    const void *offsets;
    BOOL sideways;
    UINT32 bidi;
};

static int font_test(void)
{
    static const GUID iid_factory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
    build_font();
    create_factory_fn create = (create_factory_fn)GetProcAddress(LoadLibraryA("dwrite.dll"), "DWriteCreateFactory");
    if (!create || create(0, &iid_factory, &factory) || !factory) {
        printf("no dwrite\n");
        return 1;
    }
    CALL(factory, 5, HRESULT(WINAPI *)(void *, void *))(factory, &collection_loader);
    CALL(factory, 13, HRESULT(WINAPI *)(void *, void *))(factory, &loader);
    static const int key = 1;
    void *collection = NULL, *family = NULL, *match = NULL, *face = NULL, *analysis = NULL;
    HRESULT hr = CALL(factory, 4, HRESULT(WINAPI *)(void *, void *, const void *, UINT32, void **))(
        factory, &collection_loader, &key, sizeof(key), &collection);
    UINT32 families = collection ? CALL(collection, 3, UINT32(WINAPI *)(void *))(collection) : 0, index = 0;
    BOOL exists = FALSE;
    if (collection)
        CALL(collection, 5, HRESULT(WINAPI *)(void *, const WCHAR *, UINT32 *, BOOL *))(collection, L"Tiny", &index,
                                                                                         &exists);
    if (exists)
        CALL(collection, 4, HRESULT(WINAPI *)(void *, UINT32, void **))(collection, index, &family);
    if (family)
        CALL(family, 7, HRESULT(WINAPI *)(void *, int, int, int, void **))(family, 400, 5, 0, &match);
    if (match)
        CALL(match, 13, HRESULT(WINAPI *)(void *, void **))(match, &face);
    UINT32 cp = 'A';
    UINT16 glyph = 0;
    if (face)
        CALL(face, 11, HRESULT(WINAPI *)(void *, const UINT32 *, UINT32, UINT16 *))(face, &cp, 1, &glyph);
    printf("collection hr=%lx families=%u found=%s glyph=%u\n", (unsigned long)hr, families, exists ? "yes" : "no",
           glyph);
    if (!face)
        return 1;
    struct glyph_run run = {face, 20.0f, 1, &glyph, NULL, NULL, FALSE, 0};
    CALL(factory, 23, HRESULT(WINAPI *)(void *, const struct glyph_run *, float, const void *, int, int, float, float,
                                        void **))(factory, &run, 1.0f, NULL, 4, 0, 0.0f, 0.0f, &analysis);
    RECT r = {0};
    if (analysis)
        CALL(analysis, 3, HRESULT(WINAPI *)(void *, int, RECT *))(analysis, 1, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    unsigned char bits[3 * 32 * 32] = {0};
    if (analysis && w > 0 && h > 0 && w * h <= 32 * 32)
        CALL(analysis, 4, HRESULT(WINAPI *)(void *, int, const RECT *, BYTE *, UINT32))(analysis, 1, &r, bits,
                                                                                         sizeof(bits));
    int edge = w > 0 ? bits[((h / 2) * w + w - 1) * 3] : -1, center = w > 0 ? bits[((h / 2) * w + w / 2) * 3] : -1;
    printf("glyph bounds=%ld,%ld,%ld,%ld center=%d outside=%d\n", r.left, r.top, r.right, r.bottom, center, edge);
    return 0;
}

static const BYTE self_signed[] = {
    0x30, 0x82, 0x01, 0x84, 0x30, 0x82, 0x01, 0x29, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14, 0x24,
    0xfa, 0xdb, 0xb0, 0x45, 0x69, 0xa7, 0x12, 0x14, 0x32, 0x7d, 0xef, 0x9d, 0x89, 0xe9, 0xcb, 0x2f,
    0xc8, 0x77, 0xb2, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
    0x17, 0x31, 0x15, 0x30, 0x13, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0c, 0x77, 0x69, 0x6e, 0x74,
    0x65, 0x73, 0x74, 0x2e, 0x73, 0x65, 0x6c, 0x66, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x31, 0x30,
    0x32, 0x30, 0x32, 0x30, 0x33, 0x39, 0x34, 0x34, 0x5a, 0x17, 0x0d, 0x33, 0x36, 0x31, 0x30, 0x31,
    0x37, 0x32, 0x30, 0x33, 0x39, 0x34, 0x34, 0x5a, 0x30, 0x17, 0x31, 0x15, 0x30, 0x13, 0x06, 0x03,
    0x55, 0x04, 0x03, 0x0c, 0x0c, 0x77, 0x69, 0x6e, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x73, 0x65, 0x6c,
    0x66, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08,
    0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x83, 0x67, 0x82, 0xc7,
    0x9a, 0x8f, 0xbb, 0x4d, 0xa0, 0xf1, 0x28, 0xd6, 0xb5, 0xa0, 0x89, 0x43, 0x1e, 0x7d, 0xbc, 0x3a,
    0x3d, 0x21, 0x85, 0x26, 0x6d, 0xba, 0xdc, 0x1e, 0xf0, 0x6b, 0x59, 0x83, 0xfc, 0xa8, 0x19, 0x89,
    0xe6, 0x87, 0xc8, 0xa8, 0x01, 0xdb, 0xe6, 0x85, 0x30, 0x4b, 0x96, 0x16, 0xf1, 0x20, 0xbc, 0xcd,
    0xdc, 0x11, 0x38, 0xf6, 0x8e, 0x55, 0xf5, 0xc5, 0xc6, 0xc7, 0x6f, 0x2d, 0xa3, 0x53, 0x30, 0x51,
    0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x26, 0x9c, 0x4e, 0x0c, 0x9c,
    0x5e, 0xc0, 0x7e, 0xe1, 0xd4, 0x3f, 0x66, 0x67, 0x2b, 0x85, 0xa0, 0xa6, 0x0d, 0x71, 0xff, 0x30,
    0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x26, 0x9c, 0x4e, 0x0c,
    0x9c, 0x5e, 0xc0, 0x7e, 0xe1, 0xd4, 0x3f, 0x66, 0x67, 0x2b, 0x85, 0xa0, 0xa6, 0x0d, 0x71, 0xff,
    0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01,
    0xff, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x49, 0x00,
    0x30, 0x46, 0x02, 0x21, 0x00, 0xe0, 0xc0, 0xd5, 0x1a, 0x97, 0xe6, 0x72, 0x30, 0xa3, 0xa7, 0x1b,
    0x7f, 0xd3, 0x4a, 0x93, 0x5c, 0x86, 0x85, 0x8d, 0x02, 0x63, 0x31, 0xda, 0x9b, 0xe9, 0x07, 0x0c,
    0x3d, 0x69, 0x16, 0xb0, 0xf0, 0x02, 0x21, 0x00, 0x97, 0x15, 0x7f, 0x4e, 0x7d, 0x98, 0x17, 0x4f,
    0xc0, 0x93, 0x62, 0x37, 0x89, 0x45, 0xfe, 0xb4, 0x68, 0x5e, 0x79, 0x49, 0x5a, 0xbd, 0x0d, 0x9a,
    0x95, 0x1d, 0x06, 0xc3, 0x09, 0x1d, 0x30, 0x7b,
};

/* a trusted root chains to itself; a self-signed certificate nobody trusts reports an untrusted root */
static void chain_one(const char *label, PCCERT_CONTEXT cert)
{
    CERT_CHAIN_PARA para = {sizeof(para)};
    PCCERT_CHAIN_CONTEXT chain = NULL;
    if (!CertGetCertificateChain(NULL, cert, NULL, NULL, &para, 0, NULL, &chain)) {
        printf("%s chain failed %lu\n", label, GetLastError());
        return;
    }
    CERT_CHAIN_POLICY_PARA policy = {sizeof(policy)};
    CERT_CHAIN_POLICY_STATUS status = {sizeof(status)};
    CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE, chain, &policy, &status);
    printf("%s chain elements=%lu errors=%lx policy=%lx\n", label, chain->rgpChain[0]->cElement,
           chain->TrustStatus.dwErrorStatus, status.dwError);
    CertFreeCertificateChain(chain);
}

static void chain_test(void)
{
    HCERTSTORE roots = CertOpenSystemStoreA(0, "ROOT");
    PCCERT_CONTEXT root = roots ? CertEnumCertificatesInStore(roots, NULL) : NULL;
    PCCERT_CONTEXT copy = root ? CertCreateCertificateContext(X509_ASN_ENCODING, root->pbCertEncoded,
                                                              root->cbCertEncoded) : NULL;
    if (copy)
        chain_one("root", copy);
    PCCERT_CONTEXT self = CertCreateCertificateContext(X509_ASN_ENCODING, self_signed, sizeof(self_signed));
    if (self)
        chain_one("self", self);
    PCCERT_CONTEXT junk = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE *)"junk", 4);
    printf("junk context=%d info=%d\n", junk != NULL, self && self->pCertInfo && self->pCertInfo->Subject.cbData > 0);
    if (roots)
        CertCloseStore(roots, 0);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "info";
    if (!strcmp(mode, "crash")) {
        volatile int *p = (int *)(uintptr_t)(argc - 2);
        printf("about to crash\n");
        fflush(stdout);
        *p = 1;
    } else if (!strcmp(mode, "loop")) {
        printf("looping forever, press Ctrl+C\n");
        fflush(stdout);
        for (volatile unsigned i = 0;; i++)
            ;
    } else if (!strcmp(mode, "raise")) {
        SetUnhandledExceptionFilter(filter);
        RaiseException(0xE0001234, 0, 0, NULL);
    } else if (!strcmp(mode, "system")) {
        int rc = system("echo inside system; winhello nested; wintest info");
        printf("system returned %d\n", rc);
    } else if (!strcmp(mode, "compat")) {
        compat();
    } else if (!strcmp(mode, "gecko")) {
        gecko_test();
    } else if (!strcmp(mode, "shortcut")) {
        shortcut_test();
    } else if (!strcmp(mode, "chain")) {
        chain_test();
    } else if (!strcmp(mode, "font")) {
        return font_test();
    } else if (!strcmp(mode, "box")) {
        int r = MessageBoxA(NULL, "SynthOS runs Windows programs.\nPress Y or N.", "Question", MB_YESNO);
        printf("answer=%s\n", r == IDYES ? "yes" : "no");
    } else {
        OSVERSIONINFOA v = {sizeof(v)};
        GetVersionExA(&v);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        MEMORYSTATUSEX mem = {sizeof(mem)};
        GlobalMemoryStatusEx(&mem);
        char name[64], user[64], dir[MAX_PATH];
        DWORD n = sizeof(name), u = sizeof(user);
        GetComputerNameA(name, &n);
        GetUserNameA(user, &u);
        GetWindowsDirectoryA(dir, sizeof(dir));
        SYSTEMTIME t;
        GetLocalTime(&t);
        printf("Windows %lu.%lu build %lu, %lu CPU, page %lu\n", v.dwMajorVersion, v.dwMinorVersion,
               v.dwBuildNumber, si.dwNumberOfProcessors, si.dwPageSize);
        printf("memory: %llu MB total, %llu MB free\n", mem.ullTotalPhys >> 20, mem.ullAvailPhys >> 20);
        printf("computer=%s user=%s windir=%s\n", name, user, dir);
        printf("date %04d-%02d-%02d, ticks %lu\n", t.wYear, t.wMonth, t.wDay, GetTickCount() > 0 ? 1UL : 0UL);
    }
    return 0;
}
