#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

/* Which engine a program is, told from the files it ships with rather than its name, so every Firefox and Chromium
   derivative is recognized: Gecko keeps xul.dll next to the program (Firefox, LibreWolf, Waterfox, Floorp, Zen, Tor
   and Mullvad Browser), Chromium its chrome.dll/<name>_elf.dll or a <name>_proxy.exe launcher (Chrome, Edge, Brave,
   Vivaldi, Opera, Yandex, Comet, Thorium...), CEF its libcef.dll (Steam's web helper). */

static const char *const chromium_names[] = {"chrome.exe",  "msedge.exe",  "brave.exe",   "vivaldi.exe",
                                             "opera.exe",   "browser.exe", "comet.exe",   "thorium.exe",
                                             "chromium.exe", "arc.exe",    "dia.exe",     "steamwebhelper.exe"};

static bool suffix(const char *s, const char *end)
{
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !str_casecmp(s + n - m, end);
}

enum w32_engine w32_browser_engine(const char *path)
{
    const char *exe = path_basename(path);
    if (!str_casecmp(exe, "obscura.exe"))
        return W32_ENGINE_OBSCURA;
    for (size_t i = 0; i < ARRAY_SIZE(chromium_names); i++)
        if (!str_casecmp(exe, chromium_names[i]))
            return W32_ENGINE_CHROMIUM;
    char dir[PATH_MAX_LEN];
    path_dirname(path, dir, sizeof(dir));
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return W32_ENGINE_NONE;
    enum w32_engine e = W32_ENGINE_NONE;
    struct vfs_dirent ent;
    while (e == W32_ENGINE_NONE && vfs_readdir(&d, &ent) == 0) {
        if (!str_casecmp(ent.name, "xul.dll"))
            e = W32_ENGINE_GECKO;
        else if (!str_casecmp(ent.name, "chrome.dll") || !str_casecmp(ent.name, "libcef.dll") ||
                 suffix(ent.name, "_elf.dll") || suffix(ent.name, "_proxy.exe"))
            e = W32_ENGINE_CHROMIUM;
    }
    vfs_closedir(&d);
    return e;
}

/* Obscura, the desktop's browser engine, may open pages on the local network like any browser. Chromium and Gecko
   run with their sandboxes: the system offers what their brokers need (restricted and AppContainer tokens, job
   objects, desktops, ntdll system call stubs a broker can patch in its child, memory of other processes). */
const char *w32_engine_environment(enum w32_engine e)
{
    return e == W32_ENGINE_OBSCURA ? "OBSCURA_ALLOW_PRIVATE_NETWORK=1\n" : "";
}
