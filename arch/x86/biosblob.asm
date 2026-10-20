; Embeds the assembled real-mode BIOS thunk and processor start-up code into the kernel image.

section .rodata
global bios_thunk_start
global bios_thunk_end

bios_thunk_start:
    incbin THUNK_FILE
bios_thunk_end:

global smp_boot_start
global smp_boot_end

smp_boot_start:
    incbin SMP_FILE
smp_boot_end:

section .note.GNU-stack noalloc noexec nowrite progbits
