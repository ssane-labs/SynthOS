#ifndef SYNTH_TIMELINE_H
#define SYNTH_TIMELINE_H

#include <synth/types.h>

#define TIMELINE_FILE "/home/.timeline"

/* kinds: 'a' an app opened, 'f' a file opened, 's' a file saved, 'w' a web page visited */
void timeline_add(char kind, const char *title, const char *target);

#endif
