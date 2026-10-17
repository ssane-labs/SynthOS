#ifndef SYNTH_OMAHA_H
#define SYNTH_OMAHA_H

#include <synth/types.h>

/* How an install reports: a line of status, a stage with its progress, a way to stop, and the reason it failed. */
struct omaha_ui {
    void (*say)(void *ctx, const char *text);
    void (*progress)(void *ctx, const char *stage, uint64_t done, uint64_t total);
    bool (*cancelled)(void *ctx);
    void *ctx;
    char error[160];
};

/* Downloads the current package of an app from Google's update server and unpacks the program inside its
   offline installer into dest (only names under `strip`, without it). */
int omaha_install(const char *appid, const char *ap, const char *dest, const char *strip, struct omaha_ui *ui);

/* Google Chrome in /home/apps/chrome, with a shortcut on the desktop. */
int chrome_install(struct omaha_ui *ui);
bool chrome_path(char *out, size_t cap);

#endif
