; SynthOS master boot record: loads stage2 from the sectors that follow it.
; Uses BIOS LBA extensions when present and falls back to CHS on old BIOSes.

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

%ifndef FORCE_CHS
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc .chs
    cmp bx, 0xaa55
    jne .chs

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    jmp .loaded
%endif

.chs:
    mov ah, 0x08
    mov dl, [boot_drive]
    xor di, di
    int 0x13
    jc disk_error
    and cx, 0x3f
    mov [spt], cx
    movzx dx, dh
    inc dx
    mov [heads], dx

    mov bx, STAGE2_ADDR
    mov word [lba], 1
.next:
    call read_chs
    add bx, 512
    inc word [lba]
    cmp word [lba], STAGE2_SECTORS + 1
    jbe .next

.loaded:
    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_ADDR

; reads sector [lba] to es:bx with CHS addressing
read_chs:
    mov di, 3
.retry:
    mov ax, [lba]
    xor dx, dx
    div word [spt]
    mov cl, dl
    inc cl
    xor dx, dx
    div word [heads]
    mov ch, al
    shl ah, 6
    or cl, ah
    mov dh, dl
    mov dl, [boot_drive]
    mov ax, 0x0201
    int 0x13
    jnc .ok
    xor ah, ah
    int 0x13
    dec di
    jnz .retry
    jmp disk_error
.ok:
    ret

disk_error:
    mov si, msg_disk
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp .print
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
spt:        dw 0
heads:      dw 0
lba:        dw 0
msg_disk:   db "SynthOS: cannot read boot disk", 0

times 446 - ($ - $$) db 0

partition_table:
    db 0x80, 0x00, 0x02, 0x00, 0x7f, 0xfe, 0xff, 0xff
    dd 1, ESP_LBA - 1
    db 0x00, 0xfe, 0xff, 0xff, 0xef, 0xfe, 0xff, 0xff
    dd ESP_LBA, ESP_SECTORS
    db 0x00, 0xfe, 0xff, 0xff, 0x7f, 0xfe, 0xff, 0xff
    dd DATA_LBA, IMAGE_SECTORS - DATA_LBA
    times 16 db 0

dw 0xaa55
