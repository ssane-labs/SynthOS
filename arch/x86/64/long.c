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

void w64_syscall(void);

static void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* the syscall instruction of ntdll's stubs: code 0x30 (and 0x38 for SS, which w64_syscall puts back), the entry in
   LSTAR, TF and DF cleared */
void arch_w64_syscall_init(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080u));
    wrmsr(0xc0000080u, ((uint64_t)hi << 32 | lo) | 1);
    wrmsr(0xc0000081u, 0x30ull << 32);
    wrmsr(0xc0000082u, (uint64_t)(uintptr_t)w64_syscall);
    wrmsr(0xc0000084u, 0x500);
}
