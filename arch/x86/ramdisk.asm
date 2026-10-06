; Embeds the packed root filesystem into the kernel image.

section .rodata
global synth_ramdisk_start
global synth_ramdisk_end

align 16
synth_ramdisk_start:
    incbin RAMDISK_FILE
synth_ramdisk_end:

section .note.GNU-stack noalloc noexec nowrite progbits
