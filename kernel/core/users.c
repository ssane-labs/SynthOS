#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/lineedit.h>
#include <synth/path.h>
#include <synth/sha256.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#define ITERATIONS 2000

static struct user users[USERS_MAX];
static int count;
static int current;
static char autologin[USER_NAME_MAX];
static bool logged_in;

static void hex_encode(const uint8_t *data, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    out[n * 2] = 0;
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool hex_decode(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hex_value(s[i * 2]), lo = hi < 0 ? -1 : hex_value(s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static void set_root(void)
{
    count = 1;
    current = 0;
    memset(&users[0], 0, sizeof(users[0]));
    str_copy(users[0].name, "root", sizeof(users[0].name));
    str_copy(users[0].full, "System Administrator", sizeof(users[0].full));
    str_copy(users[0].home, "/home", sizeof(users[0].home));
    users[0].admin = true;
}

static char *field(char **p)
{
    char *start = *p, *sep = strchr(start, ':');
    if (sep) {
        *sep = 0;
        *p = sep + 1;
    } else {
        *p = start + strlen(start);
    }
    return start;
}

static void parse_line(char *line)
{
    if (count >= USERS_MAX || !*line || *line == '#')
        return;
    struct user u = {0};
    char *p = line;
    str_copy(u.name, field(&p), sizeof(u.name));
    long uid = 0;
    str_parse_long(field(&p), &uid);
    u.uid = (uint32_t)uid;
    u.admin = !strcmp(field(&p), "admin");
    str_copy(u.full, field(&p), sizeof(u.full));
    str_copy(u.home, field(&p), sizeof(u.home));
    char *salt = field(&p), *hash = field(&p);
    if (strlen(salt) == 32 && strlen(hash) == 64 && hex_decode(salt, u.salt, 16) && hex_decode(hash, u.hash, 32))
        u.has_password = true;
    if (!user_name_valid(u.name) || !u.home[0])
        return;
    users[count++] = u;
}

void users_init(void)
{
    set_root();
    char *data;
    size_t size;
    if (vfs_read_all(USERS_FILE, &data, &size) != 0)
        return;
    count = 0;
    data[size] = 0;
    char *line = data;
    for (size_t i = 0; i < size; i++)
        if (data[i] == '\n') {
            data[i] = 0;
            parse_line(line);
            line = data + i + 1;
        }
    parse_line(line);
    kfree(data);
    if (!count)
        set_root();
    current = 0;
}

int users_save(void)
{
    size_t cap = (size_t)count * 320 + 64, n = 0;
    char *out = kmalloc(cap);
    if (!out)
        return E_NOMEM;
    n += (size_t)ksnprintf(out, cap, "# SynthOS users: name:uid:role:full name:home:salt:pbkdf2-sha256\n");
    for (int i = 0; i < count; i++) {
        char salt[33] = "", hash[65] = "";
        if (users[i].has_password) {
            hex_encode(users[i].salt, 16, salt);
            hex_encode(users[i].hash, 32, hash);
        }
        n += (size_t)ksnprintf(out + n, cap - n, "%s:%u:%s:%s:%s:%s:%s\n", users[i].name, users[i].uid,
                               users[i].admin ? "admin" : "user", users[i].full, users[i].home, salt, hash);
    }
    int err = vfs_write_all(USERS_FILE, out, n);
    kfree(out);
    return err;
}

int users_count(void)
{
    return count;
}

const struct user *users_get(int index)
{
    return index >= 0 && index < count ? &users[index] : NULL;
}

static int index_of(const char *name)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(users[i].name, name))
            return i;
    return -1;
}

const struct user *user_find(const char *name)
{
    int i = index_of(name);
    return i >= 0 ? &users[i] : NULL;
}

const struct user *user_current(void)
{
    return &users[current];
}

const char *user_home(void)
{
    return users[current].home;
}

bool user_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (!n || n >= USER_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9' && i) || c == '_' || (c == '-' && i)))
            return false;
    }
    return true;
}

static void set_password(struct user *u, const char *password)
{
    if (!password || !*password) {
        u->has_password = false;
        return;
    }
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = hal_entropy() ^ (uint32_t)hal_ticks_ms() * 2654435761u;
        memcpy(u->salt + i, &r, 4);
    }
    pbkdf2_sha256(password, strlen(password), u->salt, 16, ITERATIONS, u->hash);
    u->has_password = true;
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if (*s == ':' || *s == '\n')
            *s = ' ';
}

static void make_home(const char *home)
{
    static const char *const dirs[] = {"Desktop", "Documents", "Downloads", "Pictures"};
    vfs_mkdirs(home);
    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
        char path[PATH_MAX_LEN];
        path_join(home, dirs[i], path, sizeof(path));
        if (!vfs_exists(path))
            vfs_mkdir(path);
    }
}

int user_add(const char *name, const char *full, const char *password, bool admin)
{
    if (!user_name_valid(name))
        return E_INVAL;
    if (index_of(name) >= 0)
        return E_EXIST;
    if (count >= USERS_MAX)
        return E_NOSPC;
    struct user *u = &users[count];
    memset(u, 0, sizeof(*u));
    str_copy(u->name, name, sizeof(u->name));
    str_copy(u->full, full && *full ? full : name, sizeof(u->full));
    sanitize(u->full);
    path_join("/home", name, u->home, sizeof(u->home));
    uint32_t uid = 1000;
    for (int i = 0; i < count; i++)
        uid = MAX(uid, users[i].uid + 1);
    u->uid = uid;
    u->admin = admin;
    set_password(u, password);
    count++;
    make_home(u->home);
    return users_save();
}

int user_delete(const char *name)
{
    int i = index_of(name);
    if (i < 0)
        return E_NOENT;
    if (i == 0 || i == current)
        return E_BUSY;
    memmove(&users[i], &users[i + 1], sizeof(users[0]) * (size_t)(count - i - 1));
    count--;
    if (current > i)
        current--;
    if (!strcmp(autologin, name))
        autologin[0] = 0;
    return users_save();
}

int user_set_password(const char *name, const char *password)
{
    int i = index_of(name);
    if (i < 0)
        return E_NOENT;
    set_password(&users[i], password);
    return users_save();
}

int user_set_full_name(const char *name, const char *full)
{
    int i = index_of(name);
    if (i < 0)
        return E_NOENT;
    str_copy(users[i].full, full, sizeof(users[i].full));
    sanitize(users[i].full);
    return users_save();
}

bool user_check(const char *name, const char *password)
{
    const struct user *u = user_find(name);
    if (!u)
        return false;
    if (!u->has_password)
        return !password || !*password;
    if (!password)
        return false;
    uint8_t hash[32];
    pbkdf2_sha256(password, strlen(password), u->salt, 16, ITERATIONS, hash);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= hash[i] ^ u->hash[i];
    return diff == 0;
}

bool user_logged_in(void)
{
    return logged_in;
}

void user_logout(void)
{
    logged_in = false;
}

const char *user_autologin(void)
{
    return autologin[0] && user_find(autologin) ? autologin : NULL;
}

bool users_need_login(void)
{
    if (user_autologin())
        return false;
    if (count > 1)
        return true;
    return users[0].has_password;
}

int user_switch(struct shell *sh, const char *name)
{
    int i = index_of(name);
    if (i < 0)
        return E_NOENT;
    current = i;
    logged_in = true;
    if (strcmp(users[i].home, "/home"))
        make_home(users[i].home);
    if (sh) {
        sh_set(sh, "USER", users[i].name);
        sh_set(sh, "HOME", users[i].home);
        sh_chdir(sh, users[i].home);
    }
    return 0;
}

int user_read_password(const char *prompt, char *buf, size_t cap)
{
    kprintf("%s", prompt);
    con_flush();
    size_t n = 0;
    for (;;) {
        int key = input_getkey(-1);
        if (key == KEY_ENTER || key == '\r')
            break;
        if (key == KEY_CTRL('c') || key == KEY_EOF) {
            kprintf("\n");
            return E_INTR;
        }
        if (key == KEY_BACKSPACE) {
            if (n)
                n--;
            continue;
        }
        if (key >= 0x20 && key < 0x100 && key != 0x7f && n + 1 < cap)
            buf[n++] = (char)key;
    }
    buf[n] = 0;
    kprintf("\n");
    return 0;
}

bool users_text_login(struct shell *sh)
{
    for (int attempt = 0;; attempt++) {
        char name[USER_NAME_MAX], password[128];
        con_role(ROLE_ACCENT);
        kprintf("synth login: ");
        con_reset();
        con_flush();
        int r = lineedit_simple(name, sizeof(name));
        if (r == LE_EOF)
            return false;
        if (!name[0])
            continue;
        const struct user *u = user_find(name);
        password[0] = 0;
        if ((!u || u->has_password) && user_read_password("Password: ", password, sizeof(password)) != 0)
            continue;
        if (u && user_check(name, password)) {
            user_switch(sh, name);
            memset(password, 0, sizeof(password));
            return true;
        }
        memset(password, 0, sizeof(password));
        con_role(ROLE_ERROR);
        kprintf("Login incorrect\n\n");
        con_reset();
    }
}

static void get_autologin(char *buf, size_t cap)
{
    str_copy(buf, autologin[0] ? autologin : "off", cap);
}

static int set_autologin(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!strcmp(value, "off") || !*value) {
        autologin[0] = 0;
        return 0;
    }
    if (!user_name_valid(value))
        return E_INVAL;
    str_copy(autologin, value, sizeof(autologin));
    return 0;
}

static const struct setting s_autologin = {"autologin", "off|<user>", "log this user in without a password at boot",
                                           get_autologin, set_autologin};
SETTING(s_autologin);
