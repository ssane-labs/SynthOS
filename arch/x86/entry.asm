; SynthOS kernel entry point, loaded at 1 MiB by stage2.

[bits 32]

section .entry
global _start
extern x86_main
extern kernel_sectors
extern __bss_start
extern __bss_end

_start:
    jmp short begin
    align 8, db 0
    dd 0x4b4e5953
    dd kernel_sectors

begin:
    cld
    mov esi, eax
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    mov esp, stack_top
    push ebx
    push esi
    call x86_main
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
