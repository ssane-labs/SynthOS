#include <synth/crypto.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

#define KEY_PATH "/home/.dpapi"
#define MAGIC "SDP1"
#define HEADER (4 + 12 + 16)
#define ERROR_INVALID_DATA 13

struct blob {
    DWORD size;
    uint8_t *data;
};

static bool user_key(uint8_t key[32])
{
    char *data = NULL;
    size_t size = 0;
    if (vfs_read_all(KEY_PATH, &data, &size) == 0 && size == 32) {
        memcpy(key, data, 32);
        kfree(data);
        return true;
    }
    kfree(data);
    random_bytes(key, 32);
    return vfs_write_all(KEY_PATH, key, 32) == 0;
}

static void *local_alloc(size_t n)
{
    void *(WINAPI * fn)(UINT, SIZE_T) = (void *(WINAPI *)(UINT, SIZE_T))w32_builtin_lookup("kernel32.dll", "LocalAlloc");
    return fn ? fn(0x40, n ? n : 1) : NULL;
}

static struct gcm *user_cipher(void)
{
    uint8_t key[32];
    if (!user_key(key))
        return NULL;
    struct gcm *g = kmalloc(sizeof(*g));
    if (g)
        gcm_init(g, key, sizeof(key));
    memset(key, 0, sizeof(key));
    return g;
}

static BOOL WINAPI CryptProtectData(const struct blob *in, const WCHAR *desc, const struct blob *entropy, void *reserved,
                                    void *prompt, DWORD flags, struct blob *out)
{
    UNUSED(desc);
    UNUSED(reserved);
    UNUSED(prompt);
    UNUSED(flags);
    if (!in || !out) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct gcm *g = user_cipher();
    uint8_t *buf = g ? local_alloc(HEADER + (size_t)in->size) : NULL;
    if (!buf) {
        kfree(g);
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    memcpy(buf, MAGIC, 4);
    random_bytes(buf + 4, 12);
    gcm_seal(g, buf + 4, entropy ? entropy->data : NULL, entropy ? entropy->size : 0, in->data, in->size,
             buf + HEADER, buf + 16);
    kfree(g);
    out->size = HEADER + in->size;
    out->data = buf;
    return TRUE;
}

static BOOL WINAPI CryptUnprotectData(const struct blob *in, WCHAR **desc, const struct blob *entropy, void *reserved,
                                      void *prompt, DWORD flags, struct blob *out)
{
    UNUSED(reserved);
    UNUSED(prompt);
    UNUSED(flags);
    if (desc)
        *desc = NULL;
    if (!in || !out || in->size < HEADER || memcmp(in->data, MAGIC, 4)) {
        w32_set_error(ERROR_INVALID_DATA);
        return FALSE;
    }
    size_t n = in->size - HEADER;
    struct gcm *g = user_cipher();
    uint8_t *buf = g ? local_alloc(n) : NULL;
    if (!buf) {
        kfree(g);
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    bool ok = gcm_open(g, in->data + 4, entropy ? entropy->data : NULL, entropy ? entropy->size : 0,
                       in->data + HEADER, n, buf, in->data + 16);
    kfree(g);
    if (!ok) {
        void *(WINAPI * local_free)(void *) = (void *(WINAPI *)(void *))w32_builtin_lookup("kernel32.dll", "LocalFree");
        if (local_free)
            local_free(buf);
        w32_set_error(ERROR_INVALID_DATA);
        return FALSE;
    }
    out->size = (DWORD)n;
    out->data = buf;
    return TRUE;
}

static struct aes *memory_key;

static BOOL WINAPI CryptProtectMemory(uint8_t *data, DWORD size, DWORD flags)
{
    UNUSED(flags);
    if (size % 16) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!memory_key) {
        uint8_t key[32];
        random_bytes(key, sizeof(key));
        if (!(memory_key = kmalloc(sizeof(*memory_key))))
            return FALSE;
        aes_init(memory_key, key, sizeof(key));
    }
    uint8_t counter[16] = {0}, stream[16];
    for (DWORD off = 0; off < size; off += 16) {
        memcpy(counter, &off, 4);
        aes_encrypt(memory_key, counter, stream);
        for (int i = 0; i < 16; i++)
            data[off + i] ^= stream[i];
    }
    return TRUE;
}

static const struct w32_export dpapi_exports[] = {
    W32_FN(CryptProtectData),
    W32_FN(CryptUnprotectData),
    W32_FN(CryptProtectMemory),
    W32_AS("CryptUnprotectMemory", CryptProtectMemory),
};

W32_EXPORTS(dpapi_module, "crypt32.dll", dpapi_exports);
