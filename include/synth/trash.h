#ifndef SYNTH_TRASH_H
#define SYNTH_TRASH_H

#include <synth/types.h>

#define TRASH_DIR "/home/.Trash"

/* moves path into the Trash and remembers where it came from; out gets the name it has there */
int trash_put(const char *path, char *out, size_t cap);
/* puts an item of the Trash back where it was; where gets that path */
int trash_restore(const char *name, char *where, size_t cap);
bool trash_origin(const char *name, char *where, size_t cap);
/* removes what has been in the Trash longer than the trashdays setting */
int trash_clean(void);

#endif
