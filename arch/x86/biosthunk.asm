; Real-mode BIOS call thunk. Assembled as a flat blob, copied to THUNK_ADDR at boot and called from
; paged protected mode. Leaves paging and protected mode, issues the interrupt described by the
; register block at THUNK_REGS, then restores the kernel's CPU state.

THUNK_ADDR  equ 0x1000
THUNK_REGS  equ 0x1f00
THUNK_STACK equ 0x4000
CODE16      equ 0x20
DATA16      equ 0x28
CODE32      equ 0x08
DATA32      equ 0x10

[org THUNK_ADDR]
[bits 32]

thunk:
    pushad
    pushfd
    cli
    mov [saved_esp], esp
    sgdt [saved_gdt]
    sidt [saved_idt]
    mov eax, cr0
    mov [saved_cr0], eax
    mov eax, cr3
    mov [saved_cr3], eax
    mov [saved_fs], fs
    mov [saved_gs], gs
    mov esp, THUNK_STACK
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    xor eax, eax
    mov cr3, eax
    jmp CODE16:pm16

[bits 16]
pm16:
    mov ax, DATA16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and al, 0xfe
    mov cr0, eax
    jmp 0:real

real:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, THUNK_STACK
    lidt [real_idt]
    mov eax, [0x46c]
    mov [THUNK_REGS + 36], eax
    mov al, [THUNK_REGS + 32]
    mov [intno], al
    jmp short .flush
.flush:
    mov ax, [THUNK_REGS + 26]
    mov es, ax
    mov eax, [THUNK_REGS + 0]
    mov ebx, [THUNK_REGS + 4]
    mov ecx, [THUNK_REGS + 8]
    mov edx, [THUNK_REGS + 12]
    mov esi, [THUNK_REGS + 16]
    mov edi, [THUNK_REGS + 20]
    push word [THUNK_REGS + 24]
    pop ds
    sti
    db 0xcd
intno:
    db 0x13
    pushfd
    cli
    push ds
    push ax
    xor ax, ax
    mov ds, ax
    pop ax
    mov [THUNK_REGS + 0], eax
    mov [THUNK_REGS + 4], ebx
    mov [THUNK_REGS + 8], ecx
    mov [THUNK_REGS + 12], edx
    mov [THUNK_REGS + 16], esi
    mov [THUNK_REGS + 20], edi
    pop word [THUNK_REGS + 24]
    mov [THUNK_REGS + 26], es
    pop dword [THUNK_REGS + 28]
    mov eax, [0x46c]
    mov [THUNK_REGS + 40], eax

    o32 lgdt [saved_gdt]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword CODE32:pm32

[bits 32]
pm32:
    mov ax, DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [saved_cr3]
    mov cr3, eax
    mov eax, [saved_cr0]
    mov cr0, eax
    jmp short paged
paged:
    lidt [saved_idt]
    mov fs, [saved_fs]
    mov gs, [saved_gs]
    mov esp, [saved_esp]
    popfd
    popad
    ret

align 4
real_idt:
    dw 0x3ff
    dd 0
saved_gdt:
    dw 0
    dd 0
saved_idt:
    dw 0
    dd 0
saved_esp:  dd 0
saved_cr0:  dd 0
saved_cr3:  dd 0
saved_fs:   dw 0
saved_gs:   dw 0

%if $ - $$ > THUNK_REGS - THUNK_ADDR
%error "BIOS thunk is too large"
%endif
