; SynthOS master boot record: loads stage2 from the sectors that follow it.

%include "layout.inc"

[bits 16]
[org 0x7c00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld
    mov [boot_drive], dl

    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc .no_lba
    cmp bx, 0xaa55
    jne .no_lba

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .disk_error

    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_ADDR

.no_lba:
    mov si, msg_lba
    jmp fail
.disk_error:
    mov si, msg_disk

fail:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp fail
.halt:
    cli
    hlt
    jmp .halt

dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw STAGE2_ADDR, 0
    dq 1

boot_drive: db 0
msg_lba:    db "SynthOS: BIOS without LBA disk support", 0
msg_disk:   db "SynthOS: cannot read boot disk", 0

times 446 - ($ - $$) db 0

partition_table:
    db 0x80, 0x00, 0x02, 0x00, 0x7f, 0xfe, 0xff, 0xff
    dd 1, IMAGE_SECTORS - 1
    times 48 db 0

dw 0xaa55
