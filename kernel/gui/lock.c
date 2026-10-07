#include "internal.h"

static bool locked;

void gui_lock(void)
{
    locked = false;
}

bool gui_locked(void)
{
    return locked;
}
