; SynthOS 64-bit kernel entry. The loaders start every kernel the same way (32-bit protected mode at 1 MiB,
; eax = boot magic, ebx = boot info); this stub maps the first 4 GiB, turns on long mode and calls x86_main.

CODE64 equ 0x30
DATA   equ 0x10
EFER   equ 0xc0000080

[bits 32]

section .entry
global _start
global boot_pml4
global boot_pdpt
global boot_pd
extern x86_main
extern kernel_sectors
extern __bss_start
extern __bss_end
extern pack_start

PACK_ADDR equ 0x400000

_start:
    jmp short begin
    align 8, db 0
    dd 0x4b4e5953
    dd kernel_sectors

begin:
    cli
    mov ebp, eax
    cmp dword [pack_start], 0x4b505953
    jne .no_pack
    mov ecx, [pack_start + 4]
    add ecx, 8
    lea esi, [pack_start + ecx - 1]
    lea edi, [PACK_ADDR + ecx - 1]
    std
    rep movsb
    jmp .moved
.no_pack:
    mov dword [PACK_ADDR], 0
.moved:
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; identity map 0-4 GiB with 2 MiB pages
    mov edi, boot_pd
    mov eax, 0x83
    xor edx, edx
    mov ecx, 2048
.pd:
    mov [edi], eax
    mov [edi + 4], edx
    add eax, 0x200000
    adc edx, 0
    add edi, 8
    loop .pd
    mov edi, boot_pdpt
    mov eax, boot_pd
    or eax, 3
    mov ecx, 4
.pdpt:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 4096
    add edi, 8
    loop .pdpt
    mov eax, boot_pdpt
    or eax, 3
    mov [boot_pml4], eax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)
    mov cr4, eax
    mov eax, boot_pml4
    mov cr3, eax
    mov ecx, EFER
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax
    lgdt [gdt_ptr]
    jmp CODE64:long_entry

[bits 64]
long_entry:
    mov ax, DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top
    mov edi, ebp
    mov esi, ebx
    call x86_main
.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 16
gdt:
    dq 0
    dq 0x00cf9a000000ffff   ; 0x08 32-bit code
    dq 0x00cf92000000ffff   ; 0x10 data
    dq 0
    dq 0x00009a000000ffff   ; 0x20 16-bit code
    dq 0x000092000000ffff   ; 0x28 16-bit data
    dq 0x00209a0000000000   ; 0x30 64-bit code
    dq 0
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

section .bss
align 4096
boot_pml4:
    resb 4096
boot_pdpt:
    resb 4096
boot_pd:
    resb 4096 * 4
align 16
stack_bottom:
    resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
