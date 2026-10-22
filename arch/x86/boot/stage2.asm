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
    call detect_disk

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
    mov dword [BOOTINFO_ADDR + 24], 0
    mov dword [BOOTINFO_ADDR + 52], 0
    mov dword [BOOTINFO_ADDR + 56], 0
    cmp dword [0x7c00 + INSTALL_RECORD], 'SYID'
    jne .no_record
    mov eax, [0x7c00 + INSTALL_RECORD + 4]
    mov [BOOTINFO_ADDR + 60], eax
    mov eax, [0x7c00 + INSTALL_RECORD + 8]
    mov [BOOTINFO_ADDR + 64], eax
    or dword [BOOTINFO_ADDR + 24], BOOT_SYSTEM
.no_record:

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:protected

detect_disk:
%ifndef FORCE_CHS
    mov ah, 0x41
    mov bx, 0x55aa
    mov dl, [drive]
    int 0x13
    jc .chs
    cmp bx, 0xaa55
    jne .chs
    mov byte [use_lba], 1
    ret
.chs:
%endif
    mov byte [use_lba], 0
    mov ah, 0x08
    mov dl, [drive]
    push es
    xor di, di
    mov es, di
    int 0x13
    pop es
    jc disk_error
    and cx, 0x3f
    movzx ecx, cx
    mov [spt], ecx
    movzx edx, dh
    inc edx
    mov [heads], edx
    ret

read_chunk:
    cmp byte [use_lba], 0
    je read_chunk_chs
    mov byte [tries], 8
.retry:
    mov ax, [count]
    mov [dap_count], ax
    mov eax, [lba]
    mov [dap_lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jnc .ok
    xor ah, ah
    mov dl, [drive]
    int 0x13
    dec byte [tries]
    jnz .retry
    jmp disk_error
.ok:
    ret

read_chunk_chs:
    push es
    mov ax, BUFFER_SEG
    mov es, ax
    xor bx, bx
    mov eax, [lba]
    mov [chs_lba], eax
    mov cx, [count]
.sector:
    push cx
    mov si, 3
.retry:
    mov eax, [chs_lba]
    xor edx, edx
    div dword [spt]
    mov cl, dl
    inc cl
    xor edx, edx
    div dword [heads]
    mov ch, al
    shl ah, 6
    or cl, ah
    mov dh, dl
    mov dl, [drive]
    mov ax, 0x0201
    int 0x13
    jnc .ok
    xor ah, ah
    int 0x13
    dec si
    jnz .retry
    jmp disk_error
.ok:
    pop cx
    add bx, 512
    inc dword [chs_lba]
    loop .sector
    pop es
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
%ifdef FORCE_E801
    xor bp, bp
    mov di, MMAP_ADDR
%endif
    test bp, bp
    jnz .store
    int 0x12
    movzx eax, ax
    shl eax, 10
    xor ebx, ebx
    call add_region
    mov ax, 0xe801
    xor cx, cx
    xor dx, dx
    int 0x15
    jc .e88
    jcxz .use_ax
    mov ax, cx
    mov bx, dx
.use_ax:
    movzx ecx, bx
    shl ecx, 16
    movzx eax, ax
    shl eax, 10
    cmp eax, 0x3c00 << 10
    jne .split
    add eax, ecx
    xor ecx, ecx
.split:
    push ecx
    mov ebx, 0x100000
    call add_region
    pop eax
    mov ebx, 0x1000000
    call add_region
    jmp .store
.e88:
    mov ah, 0x88
    int 0x15
    jc .store
    movzx eax, ax
    shl eax, 10
    mov ebx, 0x100000
    call add_region
.store:
    mov [mmap_count], bp
    ret

; appends a usable region (ebx = base, eax = length) to the memory map at di
add_region:
    test eax, eax
    jz .skip
    mov [di], ebx
    mov dword [di + 4], 0
    mov [di + 8], eax
    mov dword [di + 12], 0
    mov dword [di + 16], 1
    mov dword [di + 20], 1
    add di, 24
    inc bp
.skip:
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
use_lba:        db 0
spt:            dd 0
heads:          dd 0
chs_lba:        dd 0
mmap_count:     dw 0
count:          dw 0
lba:            dd 0
remaining:      dd 0
dest:           dd 0
kernel_sectors: dd 0

tries:      db 0
msg_banner: db "SynthOS loader ", 0
msg_kernel: db 13, 10, "SynthOS: kernel image is missing or damaged", 0
msg_disk:   db 13, 10, "SynthOS: disk read error", 0

times STAGE2_SECTORS * 512 - ($ - $$) db 0
