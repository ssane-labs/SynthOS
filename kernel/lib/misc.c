#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/keys.h>
#include <synth/string.h>

const char *err_str(int code)
{
    static const char *const messages[] = {
        [0] = "success",
        [-E_NOENT] = "no such file or directory",
        [-E_EXIST] = "already exists",
        [-E_NOTDIR] = "not a directory",
        [-E_ISDIR] = "is a directory",
        [-E_NOTEMPTY] = "directory not empty",
        [-E_NOMEM] = "out of memory",
        [-E_INVAL] = "invalid argument",
        [-E_IO] = "input/output error",
        [-E_NOSPC] = "no space left",
        [-E_PERM] = "permission denied",
        [-E_NOEXEC] = "not an executable format",
        [-E_BUSY] = "resource busy",
        [-E_RANGE] = "value out of range",
        [-E_NOSYS] = "not supported",
        [-E_BADF] = "bad file handle",
        [-E_MFILE] = "too many open files",
        [-E_NAMETOOLONG] = "name too long",
        [-E_XDEV] = "cross-device operation",
        [-E_FAULT] = "bad address",
        [-E_INTR] = "interrupted",
        [-E_TIMEDOUT] = "timed out",
        [-E_PIPE] = "endpoint stalled",
        [-E_NODEV] = "no such device",
    };
    int idx = -code;
    if (idx >= 0 && idx < (int)ARRAY_SIZE(messages) && messages[idx])
        return messages[idx];
    return "unknown error";
}

const char *key_name(int key, char *buf, int cap)
{
    static const char *const special[] = {
        "Up", "Down", "Left", "Right", "Home", "End", "PgUp", "PgDn", "Insert", "Delete",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Resize", "EOF",
    };
    char base[16];
    int k = KEY_BASE(key);
    if (k >= KEY_UP && k <= KEY_EOF)
        str_copy(base, special[k - KEY_UP], sizeof(base));
    else if (k == KEY_ENTER)
        str_copy(base, "Enter", sizeof(base));
    else if (k == KEY_TAB)
        str_copy(base, "Tab", sizeof(base));
    else if (k == KEY_BACKSPACE)
        str_copy(base, "Backspace", sizeof(base));
    else if (k == KEY_ESC)
        str_copy(base, "Esc", sizeof(base));
    else if (k == ' ')
        str_copy(base, "Space", sizeof(base));
    else if (k < 0x20)
        ksnprintf(base, sizeof(base), "Ctrl+%c", k + '@');
    else
        ksnprintf(base, sizeof(base), "%c", k);
    ksnprintf(buf, (size_t)cap, "%s%s%s%s", (key & KEY_MOD_CTRL) ? "Ctrl+" : "",
              (key & KEY_MOD_ALT) ? "Alt+" : "", (key & KEY_MOD_SHIFT) ? "Shift+" : "", base);
    return buf;
}
