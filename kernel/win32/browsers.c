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

/* Gecko's sandboxes need restricted tokens and job objects, which SynthOS does not offer: its processes run
   unsandboxed, as with these variables on Windows. */
const char *w32_engine_environment(enum w32_engine e)
{
    if (e != W32_ENGINE_GECKO)
        return "";
    return "MOZ_DISABLE_CONTENT_SANDBOX=1\n"
           "MOZ_DISABLE_GMP_SANDBOX=1\n"
           "MOZ_DISABLE_RDD_SANDBOX=1\n"
           "MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1\n"
           "MOZ_DISABLE_GPU_SANDBOX=1\n"
           "MOZ_DISABLE_UTILITY_SANDBOX=1\n"
           "MOZ_DISABLE_VR_SANDBOX=1\n";
}
