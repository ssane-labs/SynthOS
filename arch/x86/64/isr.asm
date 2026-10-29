; 64-bit interrupt entry stubs: every vector funnels into x86_interrupt(struct regs *).

[bits 64]
section .text

extern x86_interrupt
global isr_table
global gdt_load

%macro ISR_NOERR 1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push qword %1
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
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    call x86_interrupt
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; void gdt_load(const struct table_ptr *ptr): new GDT, data segments 0x10, code 0x30
gdt_load:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    pop rax
    push qword 0x30
    push rax
    retfq

section .rodata
align 8
isr_table:
%assign i 0
%rep 48
    dq isr %+ i
%assign i i + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
