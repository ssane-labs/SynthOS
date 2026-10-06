; Leaves 64-bit long mode for 32-bit protected mode and enters the kernel at 1 MiB.
; Copied by the UEFI loader into a low executable page; position independent.
; in: esi = boot info address

BOOT_MAGIC equ 0x5359424f
KERNEL_ADDR equ 0x100000

[bits 64]
start:
    cli
    lea rax, [rel gdt]
    mov [rel gdtr + 2], eax
    lgdt [rel gdtr]
    lea rax, [rel compat]
    push 0x08
    push rax
    retfq

[bits 32]
compat:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    mov ecx, 0xc0000080
    rdmsr
    and eax, ~(1 << 8)
    wrmsr
    mov eax, cr4
    and eax, ~((1 << 5) | (1 << 7))
    mov cr4, eax
    xor eax, eax
    mov cr3, eax
    mov ebx, esi
    mov eax, BOOT_MAGIC
    jmp dword 0x08:KERNEL_ADDR

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdtr:
    dw gdtr - gdt - 1
    dd 0
