; Embeds the assembled real-mode BIOS thunk into the kernel image.

section .rodata
global bios_thunk_start
global bios_thunk_end

bios_thunk_start:
    incbin THUNK_FILE
bios_thunk_end:

section .note.GNU-stack noalloc noexec nowrite progbits
