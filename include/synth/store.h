#ifndef SYNTH_STORE_H
#define SYNTH_STORE_H

#include <synth/types.h>

/* The App Store: a catalog of programs that run in SynthOS (/system/store/catalog.txt, refreshed from the
   SynthOS releases), what is installed, and installing, updating and removing them. */

enum store_kind {
    STORE_ZIP,       /* a zip unpacked into /home/apps/<id> */
    STORE_INSTALLER, /* the vendor's own Windows installer, run with its silent switch */
    STORE_CHROME,    /* Google Chrome from Google's update server */
    STORE_BUILTIN,   /* comes with the image */
};

enum store_state {
    STORE_GET,
    STORE_INSTALLED,
    STORE_UPDATE,
    STORE_UNAVAILABLE,
};

struct store_app {
    char id[24], name[48], developer[48], category[24], icon[16], version[24], size[16];
    char summary[120], about[600], note[120];
    char url[2][320], sha256[65];
    char file[64], args[64], check[160], open[160], bin[64], post[16], strip[64], uninstall[64];
    enum store_kind kind;
    int bits;
};

struct store_ui {
    void (*progress)(void *ctx, const char *stage, size_t done, size_t total);
    void (*say)(void *ctx, const char *text);
    bool (*cancelled)(void);
    /* starts a downloaded installer; without it store_install only downloads it and names the file */
    bool (*run)(void *ctx, const char *path, const char *args);
    void *ctx;
};

int store_load(void);
int store_count(void);
const struct store_app *store_app(int index);
const struct store_app *store_find(const char *id);
enum store_state store_state(const struct store_app *a);
bool store_program(const struct store_app *a, char *out, size_t cap);
int store_install(const struct store_app *a, const struct store_ui *ui, char *msg, size_t cap);
int store_remove(const struct store_app *a, char *uninstall, size_t ucap, char *msg, size_t cap);
int store_refresh(const struct store_ui *ui, char *msg, size_t cap);
int store_categories(const char *out[], int max);

struct shell;
void pkg_add_path(struct shell *sh, const char *dir);
void pkg_python_site(struct shell *sh, const char *root);

#endif
