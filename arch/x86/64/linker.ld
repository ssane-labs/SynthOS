OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* loaded at 1 MiB, run at 0xb0100000: the 64-bit kernel stays out of the 4 MiB - 2.75 GiB 32-bit programs use */
KERNEL_PHYS = 0x100000;
KOFF = 0xb0000000;

SECTIONS
{
    . = KERNEL_PHYS;

    .entry : {
        KEEP(*(.entry))
    }

    . += KOFF;

    .text : AT(ADDR(.text) - KOFF) {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(16);
        __start_text32 = .;
        KEEP(*(.text32))
        __stop_text32 = .;
        __text_end = .;
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
        *(.got .got.plt)
        . = ALIGN(8);
        __start_synth_commands = .;
        KEEP(*(synth_commands))
        __stop_synth_commands = .;
        . = ALIGN(8);
        __start_synth_settings = .;
        KEEP(*(synth_settings))
        __stop_synth_settings = .;
        . = ALIGN(8);
        __start_synth_binfmts = .;
        KEEP(*(synth_binfmts))
        __stop_synth_binfmts = .;
        . = ALIGN(8);
        __start_synth_win32 = .;
        KEEP(*(synth_win32))
        __stop_synth_win32 = .;
        . = ALIGN(8);
        __start_synth_win64 = .;
        KEEP(*(synth_win64))
        __stop_synth_win64 = .;
        . = ALIGN(8);
        __start_synth_gui_apps = .;
        KEEP(*(synth_gui_apps))
        __stop_synth_gui_apps = .;
        . = ALIGN(8);
        __start_w32sigs = .;
        KEEP(*(w32sigs))
        __stop_w32sigs = .;
        . = ALIGN(8);
        __start_synth_fs = .;
        KEEP(*(synth_fs))
        __stop_synth_fs = .;
        . = ALIGN(8);
        __start_w32vtbl = .;
        KEEP(*(w32vtbl))
        __stop_w32vtbl = .;
        . = ALIGN(16);
        __start_w32state = .;
        KEEP(*(w32state))
        __stop_w32state = .;
    }

    image_end = .;
    pack_start = ALIGN(image_end, 16);

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    kernel_end = .;
    kernel_sectors = (image_end - KOFF - KERNEL_PHYS + 511) / 512;
    ASSERT(kernel_end < KOFF + 0x8000000, "the 64-bit kernel must fit its 128 MiB window")

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
