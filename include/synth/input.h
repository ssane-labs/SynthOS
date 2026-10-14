#ifndef SYNTH_INPUT_H
#define SYNTH_INPUT_H

#include <synth/keys.h>
#include <synth/types.h>

int input_getkey(int timeout_ms);
bool input_poll(void);
void input_unget(int key);
void input_set_break(bool enabled);
bool input_break_pending(void);
bool input_info_take(void);
void input_clear_break(void);
void input_inject(int key);
void input_set_pump(void (*pump)(int timeout_ms));

#endif
