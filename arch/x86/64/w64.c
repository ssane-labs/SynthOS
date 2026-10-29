#include <synth/arch_win32.h>
#include <synth/log.h>

/* 64-bit Windows programs on the 64-bit kernel come next: until then the long-mode machinery of the 32-bit kernel
   (long.asm) is absent, arch_long_supported says so and the loader refuses such programs before any of these runs. */

bool arch_long_supported(void)
{
    return false;
}

bool arch_long_enter(void)
{
    return false;
}

void arch_long_leave(void)
{
}

bool arch_long_active(void)
{
    return false;
}

uint64_t arch_w64_call(uint32_t fn, int argc, const uint64_t *args)
{
    UNUSED(argc);
    UNUSED(args);
    panic("win64: call to %x on the 64-bit kernel", fn);
}

uint64_t w64_call32(void *fn, const uint32_t *args, int count)
{
    UNUSED(args);
    UNUSED(count);
    panic("win64: call32 to %p on the 64-bit kernel", fn);
}

void *w64_dispatch_fn;
uint64_t w64_raise_hook, w64_unwind_hook;
char w64_gate[1], w64_setjmp[1], w64_longjmp[1], w64_memcpy[1], w64_memmove[1], w64_memset[1], w64_strlen[1];
char w64_capture_context[1], w64_restore_context[1], w64_raise_exception[1], w64_unwind_ex[1];
