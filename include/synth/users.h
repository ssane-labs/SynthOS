#ifndef SYNTH_USERS_H
#define SYNTH_USERS_H

#include <synth/types.h>
#include <synth/vfs.h>

#define USER_NAME_MAX 32
#define USERS_FILE "/home/.users"
#define USERS_MAX 16

struct user {
    char name[USER_NAME_MAX];
    char full[64];
    char home[PATH_MAX_LEN];
    uint32_t uid;
    bool admin;
    bool has_password;
    uint8_t salt[16];
    uint8_t hash[32];
};

struct shell;

void users_init(void);
int users_save(void);
int users_count(void);
const struct user *users_get(int index);
const struct user *user_find(const char *name);
const struct user *user_current(void);
const char *user_home(void);
bool user_name_valid(const char *name);
int user_add(const char *name, const char *full, const char *password, bool admin);
int user_delete(const char *name);
int user_set_password(const char *name, const char *password);
int user_set_full_name(const char *name, const char *full);
bool user_check(const char *name, const char *password);
bool users_need_login(void);
int user_switch(struct shell *sh, const char *name);
int user_read_password(const char *prompt, char *buf, size_t cap);
bool users_text_login(struct shell *sh);
const char *user_autologin(void);
bool user_logged_in(void);
void user_logout(void);

#endif
