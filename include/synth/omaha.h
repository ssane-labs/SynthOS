#ifndef SYNTH_OMAHA_H
#define SYNTH_OMAHA_H

struct shell;

/* Downloads the current package of an app from Google's update server into dir and runs its installer. */
int omaha_install(struct shell *sh, const char *appid, const char *ap, const char *dir);

#endif
