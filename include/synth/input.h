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

/* when the desktop last ran its event loop, and how long it had to wait at most since the last look */
extern volatile uint64_t input_pump_at;
void input_pump_mark(void);
uint32_t input_lag_take(uint32_t *slow);
/* long kernel work (unpacking, copying, downloading) calls this at points where it holds no locks: the desktop gets
   a turn whenever it waited 25 ms */
void input_yield(void);
extern bool input_yielding;

#endif
