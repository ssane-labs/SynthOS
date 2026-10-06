; Interrupt entry stubs: every vector funnels into x86_interrupt(struct regs *).

[bits 32]
section .text

extern x86_interrupt
global isr_table
global gdt_flush

%macro ISR_NOERR 1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 48
%if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
%else
    ISR_NOERR i
%endif
%assign i i + 1
%endrep

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp
    call x86_interrupt
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload
.reload:
    ret

section .rodata
align 4
isr_table:
%assign i 0
%rep 48
    dd isr %+ i
%assign i i + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
