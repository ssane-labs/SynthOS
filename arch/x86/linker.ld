OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        KEEP(*(.entry))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
        . = ALIGN(8);
        __start_synth_commands = .;
        KEEP(*(synth_commands))
        __stop_synth_commands = .;
        __start_synth_settings = .;
        KEEP(*(synth_settings))
        __stop_synth_settings = .;
        __start_synth_binfmts = .;
        KEEP(*(synth_binfmts))
        __stop_synth_binfmts = .;
        __start_synth_fs = .;
        KEEP(*(synth_fs))
        __stop_synth_fs = .;
    }

    image_end = .;

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    kernel_end = .;
    kernel_sectors = (image_end - 0x100000 + 511) / 512;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
