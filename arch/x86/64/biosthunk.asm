; Real-mode BIOS call thunk for the 64-bit kernel. Assembled as a flat blob, copied to THUNK_ADDR at boot and called
; from long mode. Drops to compatibility mode, turns paging (and with it long mode) off, issues the interrupt described
; by the register block at THUNK_REGS in real mode, then comes back the same way.

THUNK_ADDR  equ 0x1000
THUNK_REGS  equ 0x1f00
THUNK_STACK equ 0x4000
CODE16      equ 0x20
DATA16      equ 0x28
CODE32      equ 0x08
DATA32      equ 0x10
CODE64      equ 0x30
EFER        equ 0xc0000080

[org THUNK_ADDR]
[bits 64]

thunk:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq
    cli
    mov [saved_rsp], rsp
    sgdt [saved_gdt64]
    sidt [saved_idt64]
    mov rax, cr3
    mov [saved_cr3], eax
    mov rax, cr4
    mov [saved_cr4], eax
    mov rax, cr0
    mov [saved_cr0], eax
    ; paging goes off on the way, so the trip uses this blob's own GDT (the kernel's is mapped high)
    lgdt [gdt64ptr]
    mov rsp, THUNK_STACK
    push qword CODE32
    lea rax, [rel compat]
    push rax
    retfq

[bits 32]
compat:
    mov ax, DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    mov ecx, EFER
    rdmsr
    and eax, ~(1 << 8)
    wrmsr
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

    o32 lgdt [gdt32]
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
    mov eax, [saved_cr4]
    mov cr4, eax
    mov eax, [saved_cr3]
    mov cr3, eax
    mov ecx, EFER
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, [saved_cr0]
    mov cr0, eax
    jmp CODE64:back64

[bits 64]
back64:
    mov ax, DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    lgdt [saved_gdt64]
    lidt [saved_idt64]
    mov rsp, [saved_rsp]
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff   ; 0x08 32-bit code
    dq 0x00cf92000000ffff   ; 0x10 data
    dq 0
    dq 0x00009a000000ffff   ; 0x20 16-bit code
    dq 0x000092000000ffff   ; 0x28 16-bit data
    dq 0x00209a0000000000   ; 0x30 64-bit code
gdt_end:
real_idt:
    dw 0x3ff
    dd 0
gdt32:
    dw gdt_end - gdt - 1
    dd gdt
gdt64ptr:
    dw gdt_end - gdt - 1
    dq gdt
saved_gdt64:
    dw 0
    dq 0
saved_idt64:
    dw 0
    dq 0
saved_rsp:  dq 0
saved_cr0:  dd 0
saved_cr3:  dd 0
saved_cr4:  dd 0

%if $ - $$ > THUNK_REGS - THUNK_ADDR
%error "BIOS thunk is too large"
%endif
