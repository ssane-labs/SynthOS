#include <synth/arch_win32.h>

/* The 64-bit kernel is in long mode all the time: 64-bit Windows programs need no switch (w64.asm) */

bool arch_long_supported(void)
{
    return true;
}

bool arch_long_enter(void)
{
    return true;
}

void arch_long_leave(void)
{
}

bool arch_long_active(void)
{
    return true;
}
