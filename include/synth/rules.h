#ifndef SYNTH_RULES_H
#define SYNTH_RULES_H

/* kinds: "at" (subject "HH:MM"), "open" (an app's name), "save" (a file's path), "login", "start" */
void rules_event(const char *kind, const char *subject);
void rules_run_pending(void);

#endif
