; SynthOS stage2 loader: A20, memory map, kernel load above 1 MiB, protected mode.

%include "layout.inc"

[bits 16]
[org STAGE2_ADDR]

stage2:
    mov [drive], dl
    mov si, msg_banner
    call print

    call enable_a20
    call enter_unreal
    call detect_memory

    mov dword [lba], KERNEL_LBA
    mov word [count], 1
    call read_chunk
    mov ax, BUFFER_SEG
    mov fs, ax
    cmp dword [fs:8], KERNEL_MAGIC
    jne bad_kernel
    mov eax, [fs:12]
    mov [kernel_sectors], eax
    mov [remaining], eax
    mov dword [dest], KERNEL_ADDR

.next_chunk:
    mov eax, [remaining]
    test eax, eax
    jz .loaded
    cmp eax, CHUNK_SECTORS
    jbe .sized
    mov eax, CHUNK_SECTORS
.sized:
    mov [count], ax
    call read_chunk
    call enter_unreal
    movzx ecx, word [count]
    shl ecx, 7
    mov esi, BUFFER_ADDR
    mov edi, [dest]
    xor ax, ax
    mov ds, ax
    mov es, ax
    a32 rep movsd
    movzx eax, word [count]
    add [lba], eax
    sub [remaining], eax
    shl eax, 9
    add [dest], eax
    mov al, '.'
    call putc
    jmp .next_chunk

.loaded:
    mov ax, 0x0003
    int 0x10

    mov dword [BOOTINFO_ADDR + 0], BOOT_MAGIC
    movzx eax, byte [drive]
    mov [BOOTINFO_ADDR + 4], eax
    mov dword [BOOTINFO_ADDR + 8], MMAP_ADDR
    movzx eax, word [mmap_count]
    mov [BOOTINFO_ADDR + 12], eax
    mov eax, [kernel_sectors]
    mov [BOOTINFO_ADDR + 16], eax
    mov dword [BOOTINFO_ADDR + 20], KERNEL_LBA

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:protected

read_chunk:
    mov ax, [count]
    mov [dap_count], ax
    mov eax, [lba]
    mov [dap_lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc disk_error
    ret

enable_a20:
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xfe
    out 0x92, al
.done:
    ret

enter_unreal:
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $ + 2
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    and al, 0xfe
    mov cr0, eax
    pop es
    pop ds
    sti
    ret

detect_memory:
    xor ebx, ebx
    xor bp, bp
    mov di, MMAP_ADDR
.loop:
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 24
    mov dword [di + 20], 1
    int 0x15
    jc .done
    cmp eax, 0x534d4150
    jne .done
    jcxz .skip
    inc bp
    add di, 24
.skip:
    test ebx, ebx
    jz .done
    cmp bp, MMAP_MAX
    jb .loop
.done:
    mov [mmap_count], bp
    ret

bad_kernel:
    mov si, msg_kernel
    jmp halt
disk_error:
    mov si, msg_disk
halt:
    call print
.forever:
    cli
    hlt
    jmp .forever

print:
    lodsb
    test al, al
    jz .done
    call putc
    jmp print
.done:
    ret

putc:
    push bx
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    pop bx
    ret

[bits 32]
protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, BOOT_MAGIC
    mov ebx, BOOTINFO_ADDR
    jmp 0x08:KERNEL_ADDR

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

align 4
dap:
    db 0x10, 0
dap_count:
    dw 0
    dw 0, BUFFER_SEG
dap_lba:
    dq 0

drive:          db 0
mmap_count:     dw 0
count:          dw 0
lba:            dd 0
remaining:      dd 0
dest:           dd 0
kernel_sectors: dd 0

msg_banner: db "SynthOS loader ", 0
msg_kernel: db 13, 10, "SynthOS: kernel image is missing or damaged", 0
msg_disk:   db 13, 10, "SynthOS: disk read error", 0

times STAGE2_SECTORS * 512 - ($ - $$) db 0
