#ifndef SYNTH_WINSHELL_H
#define SYNTH_WINSHELL_H

#include <synth/path.h>
#include <synth/types.h>

/* What the Windows shell does with a file or a link, read from the associations installers write to the registry:
   HKCR\.ext -> ProgID -> shell\<verb>\command, HKCR\<scheme>\shell\open\command, the user's UserChoice, and the
   browsers under SOFTWARE\Clients\StartMenuInternet. */

struct winshell_browser {
    char key[64];
    char name[64];
    char program[PATH_MAX_LEN];
};

/* the desktop opens what no registered program claims (set while it runs) */
extern void (*winshell_open_hook)(const char *target);

bool winshell_class(const char *assoc, char *cls, size_t cap);
bool winshell_assoc_command(const char *assoc, const char *verb, char *out, size_t cap);
bool winshell_command(const char *verb, const char *target, const char *params, char *out, size_t cap);
bool winshell_split(const char *cmdline, char *program, size_t cap, const char **args);
void winshell_browsers(void (*fn)(const struct winshell_browser *b, void *ctx), void *ctx);
bool winshell_browser(const char *key, struct winshell_browser *out);
bool winshell_browser_command(const char *key, const char *target, char *out, size_t cap);
bool winshell_default_browser(char *key, size_t cap);
bool winshell_set_default_browser(const char *key);
bool winshell_is_url(const char *target);

struct shell;
int msiexec_main(struct shell *sh, int argc, char **argv);

/* what Settings lists as installed: the Uninstall entries installers write */
struct winshell_program {
    char key[96];
    char name[96];
    char version[32];
    char publisher[64];
    char uninstall[384];
};

void winshell_programs(void (*fn)(const struct winshell_program *p, void *ctx), void *ctx);

/* what installers start at logon: the Run and RunOnce values of HKCU and HKLM */
struct winshell_run {
    char key[96];
    char name[64];
    char command[512];
    bool once;
};

void winshell_run_entries(void (*fn)(const struct winshell_run *r, void *ctx), void *ctx);
bool winshell_run_remove(const struct winshell_run *r);

#endif
