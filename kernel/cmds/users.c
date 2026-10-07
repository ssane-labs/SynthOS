#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/gui.h>
#include <synth/string.h>
#include <synth/users.h>

static int user_fail(struct shell *sh, const char *who, int err)
{
    const char *why = err == E_EXIST   ? "that user already exists"
                      : err == E_INVAL ? "invalid name (use lowercase letters, digits, - and _)"
                      : err == E_BUSY  ? "cannot remove root or the user who is logged in"
                      : err == E_NOENT ? "no such user"
                                       : err_str(err);
    return sh_error(sh, who, "%s", why);
}

static bool read_new_password(struct shell *sh, const char *who, char *out, size_t cap)
{
    char again[128];
    if (user_read_password("New password (empty for none): ", out, cap) != 0)
        return false;
    if (user_read_password("Retype new password: ", again, sizeof(again)) != 0)
        return false;
    bool same = !strcmp(out, again);
    memset(again, 0, sizeof(again));
    if (!same) {
        sh_error(sh, who, "passwords do not match");
        return false;
    }
    return true;
}

COMMAND(users, NULL, "system", "users", "list user accounts")
{
    UNUSED(argc);
    UNUSED(argv);
    const struct user *me = user_current();
    for (int i = 0; i < users_count(); i++) {
        const struct user *u = users_get(i);
        sh_role(sh, u == me ? ROLE_ACCENT : ROLE_TEXT);
        sh_printf(sh, "%c %-12s", u == me ? '*' : ' ', u->name);
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, " %-24s %-6s %-10s %s\n", u->full, u->admin ? "admin" : "user",
                  u->has_password ? "password" : "no password", u->home);
    }
    sh_role(sh, ROLE_TEXT);
    return 0;
}

COMMAND(useradd, "adduser", "system", "useradd [-a] [-p password] <name> [full name...]",
        "create a user account (-a: administrator)")
{
    bool admin = false;
    const char *password = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-a"))
            admin = true;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            password = argv[++i];
        else
            return command_usage(sh, command_find("useradd"));
    }
    if (i >= argc)
        return command_usage(sh, command_find("useradd"));
    if (!user_current()->admin)
        return sh_error(sh, "useradd", "only an administrator can add users");
    const char *name = argv[i++];
    char full[64] = "";
    for (; i < argc; i++) {
        if (full[0])
            str_append(full, " ", sizeof(full));
        str_append(full, argv[i], sizeof(full));
    }
    char secret[128] = "";
    if (!password) {
        if (!read_new_password(sh, "useradd", secret, sizeof(secret)))
            return 1;
        password = secret;
    }
    int err = user_add(name, full, password, admin);
    memset(secret, 0, sizeof(secret));
    if (err)
        return user_fail(sh, "useradd", err);
    sh_printf(sh, "user %s created, home %s\n", name, user_find(name)->home);
    return 0;
}

COMMAND(userdel, "deluser", "system", "userdel <name>", "delete a user account (the home folder is kept)")
{
    if (argc != 2)
        return command_usage(sh, command_find("userdel"));
    if (!user_current()->admin)
        return sh_error(sh, "userdel", "only an administrator can delete users");
    int err = user_delete(argv[1]);
    if (err)
        return user_fail(sh, "userdel", err);
    sh_printf(sh, "user %s deleted\n", argv[1]);
    return 0;
}

COMMAND(passwd, NULL, "system", "passwd [-p password] [user]", "change a password")
{
    const char *password = NULL;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-p")) {
        password = argv[i + 1];
        i += 2;
    }
    const struct user *me = user_current();
    const char *name = i < argc ? argv[i] : me->name;
    const struct user *target = user_find(name);
    if (!target)
        return user_fail(sh, "passwd", E_NOENT);
    if (target != me && !me->admin)
        return sh_error(sh, "passwd", "only an administrator can change other passwords");
    char secret[128] = "";
    if (!password) {
        if (target == me && me->has_password) {
            if (user_read_password("Current password: ", secret, sizeof(secret)) != 0)
                return 1;
            bool ok = user_check(me->name, secret);
            memset(secret, 0, sizeof(secret));
            if (!ok)
                return sh_error(sh, "passwd", "wrong password");
        }
        if (!read_new_password(sh, "passwd", secret, sizeof(secret)))
            return 1;
        password = secret;
    }
    int err = user_set_password(name, password);
    memset(secret, 0, sizeof(secret));
    if (err)
        return user_fail(sh, "passwd", err);
    sh_printf(sh, "password for %s %s\n", name, *password ? "updated" : "removed");
    return 0;
}

COMMAND(su, "login", "system", "su [user]", "switch to another user")
{
    const char *name = argc > 1 ? argv[1] : "root";
    const struct user *u = user_find(name);
    if (!u)
        return user_fail(sh, "su", E_NOENT);
    if (u->has_password && !user_current()->admin) {
        char secret[128];
        if (user_read_password("Password: ", secret, sizeof(secret)) != 0)
            return 1;
        bool ok = user_check(name, secret);
        memset(secret, 0, sizeof(secret));
        if (!ok)
            return sh_error(sh, "su", "authentication failure");
    }
    user_switch(sh, name);
    return 0;
}

COMMAND(logout, NULL, "system", "logout", "end the session and show the login screen")
{
    UNUSED(argc);
    UNUSED(argv);
#if CONFIG_GUI
    if (gui_active()) {
        gui_quit(QUIT_LOGOUT);
        return 0;
    }
#endif
    kprintf("\n");
    return users_text_login(sh) ? 0 : 1;
}

COMMAND(lock, NULL, "system", "lock", "lock the screen until the password is entered")
{
    UNUSED(argc);
    UNUSED(argv);
#if CONFIG_GUI
    if (gui_active()) {
        gui_lock();
        return 0;
    }
#endif
    const struct user *u = user_current();
    if (!u->has_password)
        return sh_error(sh, "lock", "%s has no password; set one with passwd", u->name);
    con_clear();
    for (;;) {
        char secret[128];
        con_role(ROLE_ACCENT);
        kprintf("SynthOS is locked by %s.\n", u->name);
        con_reset();
        if (user_read_password("Password: ", secret, sizeof(secret)) == 0 && user_check(u->name, secret)) {
            memset(secret, 0, sizeof(secret));
            return 0;
        }
        memset(secret, 0, sizeof(secret));
        kprintf("Sorry, try again.\n\n");
    }
}
