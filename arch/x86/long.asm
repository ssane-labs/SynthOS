; Long mode for 64-bit Windows programs. The kernel keeps running as 32-bit code in compatibility
; mode; 64-bit interrupt stubs and call gates switch between the two code segments.

CODE32 equ 0x08
DATA32 equ 0x10
CODE64 equ 0x30
TEB64  equ 0x38
EFER   equ 0xc0000080

extern x86_interrupt

global long_switch_on
global long_switch_off
global isr64_table
global w64_gate
global arch_w64_call
global w64_call32
global w64_setjmp
global w64_longjmp
global w64_memcpy
global w64_memmove
global w64_memset
global w64_strlen
global w64_dispatch_fn
global w64_capture_context
global w64_restore_context
global w64_raise_exception
global w64_unwind_ex
global w64_raise_hook
global w64_unwind_hook

section .text

[bits 32]

; void long_switch_on(uint32_t pml4, const void *idt64)
long_switch_on:
    push ebx
    pushfd
    cli
    mov ecx, [esp + 12]
    mov ebx, [esp + 16]
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    mov cr3, ecx
    mov ecx, EFER
    rdmsr
    or eax, 0x100
    wrmsr
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp .paged
.paged:
    lidt [ebx]
    popfd
    pop ebx
    ret

; void long_switch_off(uint32_t root, const void *idt32)
long_switch_off:
    push ebx
    pushfd
    cli
    mov ecx, [esp + 12]
    mov ebx, [esp + 16]
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    mov cr3, ecx
    mov ecx, EFER
    rdmsr
    and eax, ~0x100
    wrmsr
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp .paged
.paged:
    lidt [ebx]
    popfd
    pop ebx
    ret

; uint64_t w64_call32(void *fn, const uint32_t *args, int count)
w64_call32:
    push ebp
    mov ebp, esp
    mov ecx, [ebp + 16]
    mov edx, [ebp + 12]
.push:
    test ecx, ecx
    jz .call
    dec ecx
    push dword [edx + ecx * 4]
    jmp .push
.call:
    call [ebp + 8]
    mov esp, ebp
    pop ebp
    ret

; uint64_t arch_w64_call(uint32_t fn, int argc, const uint64_t *args)
arch_w64_call:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov eax, gs
    push eax
    mov ax, TEB64
    mov gs, ax
    jmp CODE64:.in64
[bits 64]
.in64:
    mov esp, esp
    mov ebp, ebp
    mov rbx, rsp
    mov ecx, [rbp + 12]
    mov esi, [rbp + 16]
    mov eax, ecx
    cmp eax, 4
    jae .room
    mov eax, 4
.room:
    shl eax, 3
    sub rsp, rax
    and rsp, -16
    xor edx, edx
.copy:
    cmp edx, ecx
    jae .go
    mov rax, [rsi + rdx * 8]
    mov [rsp + rdx * 8], rax
    inc edx
    jmp .copy
.go:
    mov rcx, [rsp]
    mov rdx, [rsp + 8]
    mov r8, [rsp + 16]
    mov r9, [rsp + 24]
    movq xmm0, rcx
    movq xmm1, rdx
    movq xmm2, r8
    movq xmm3, r9
    mov eax, [rbp + 8]
    cld
    call rax
    mov rsp, rbx
    mov rdx, rax
    shr rdx, 32
    mov ecx, .back
    push CODE32
    push rcx
    o64 retf
[bits 32]
.back:
    pop ecx
    mov gs, cx
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

[bits 64]

; Import stubs put their descriptor in r11 and jump here with the Microsoft x64 arguments intact.
w64_gate:
    mov [rsp + 8], rcx
    mov [rsp + 16], rdx
    mov [rsp + 24], r8
    mov [rsp + 32], r9
    push rbp
    mov rbp, rsp
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    movq [rsp], xmm0
    movq [rsp + 8], xmm1
    movq [rsp + 16], xmm2
    movq [rsp + 24], xmm3
    movdqa [rsp + 32], xmm6
    movdqa [rsp + 48], xmm7
    movdqa [rsp + 64], xmm8
    movdqa [rsp + 80], xmm9
    movdqa [rsp + 96], xmm10
    movdqa [rsp + 112], xmm11
    movdqa [rsp + 128], xmm12
    movdqa [rsp + 144], xmm13
    movdqa [rsp + 160], xmm14
    movdqa [rsp + 176], xmm15
    mov rbx, rsp
    mov esi, r11d
    mov eax, .compat
    push CODE32
    push rax
    o64 retf
[bits 32]
.compat:
    mov ax, DATA32
    mov ds, ax
    mov es, ax
    cld
    lea eax, [ebp + 16]
    push ebx
    push eax
    push esi
    call [w64_dispatch_fn]
    add esp, 12
    mov cx, TEB64
    mov gs, cx
    jmp CODE64:.back
[bits 64]
.back:
    mov ebx, ebx
    shl rdx, 32
    mov eax, eax
    or rax, rdx
    movq xmm0, rax
    movdqa xmm6, [rbx + 32]
    movdqa xmm7, [rbx + 48]
    movdqa xmm8, [rbx + 64]
    movdqa xmm9, [rbx + 80]
    movdqa xmm10, [rbx + 96]
    movdqa xmm11, [rbx + 112]
    movdqa xmm12, [rbx + 128]
    movdqa xmm13, [rbx + 144]
    movdqa xmm14, [rbx + 160]
    movdqa xmm15, [rbx + 176]
    lea rsp, [rbx + 200]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret

; Interrupts while long mode is active: save the 64-bit state, drop to compatibility mode and hand
; x86_interrupt the same struct regs frame the 32-bit stubs build.
%macro ISR64 1
isr64_%1:
%if !(%1 == 8 || (%1 >= 10 && %1 <= 14) || %1 == 17 || %1 == 21 || %1 == 29 || %1 == 30)
    push 0
%endif
    push %1
    jmp isr64_common
%endmacro

%assign i 0
%rep 48
    ISR64 i
%assign i i + 1
%endrep

isr64_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rbx, rsp
    cmp qword [rbx + 144], CODE64
    jne .no_save
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]
.no_save:
    mov eax, .compat
    push CODE32
    push rax
    o64 retf
[bits 32]
.compat:
    push dword [ebx + 152]
    push dword [ebx + 144]
    push dword [ebx + 136]
    push dword [ebx + 128]
    push dword [ebx + 120]
    push dword [ebx + 112]
    push dword [ebx + 104]
    push dword [ebx + 96]
    push dword [ebx + 88]
    push dword [ebx + 160]
    push dword [ebx + 80]
    push dword [ebx + 72]
    push dword [ebx + 64]
    push ds
    push es
    push fs
    push gs
    mov ax, DATA32
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call x86_interrupt
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    jmp CODE64:.back
[bits 64]
.back:
    mov ebx, ebx
    cmp qword [rbx + 144], CODE64
    jne .no_restore
    lea rax, [rbx - 512]
    and rax, -16
    fxrstor64 [rax]
.no_restore:
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

; Microsoft x64 _JUMP_BUFFER: Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip, MxCsr, FpCsr, Xmm6-15.
w64_setjmp:
    mov [rcx], rdx
    mov [rcx + 8], rbx
    lea rax, [rsp + 8]
    mov [rcx + 16], rax
    mov [rcx + 24], rbp
    mov [rcx + 32], rsi
    mov [rcx + 40], rdi
    mov [rcx + 48], r12
    mov [rcx + 56], r13
    mov [rcx + 64], r14
    mov [rcx + 72], r15
    mov rax, [rsp]
    mov [rcx + 80], rax
    stmxcsr [rcx + 88]
    fnstcw [rcx + 92]
    movdqu [rcx + 96], xmm6
    movdqu [rcx + 112], xmm7
    movdqu [rcx + 128], xmm8
    movdqu [rcx + 144], xmm9
    movdqu [rcx + 160], xmm10
    movdqu [rcx + 176], xmm11
    movdqu [rcx + 192], xmm12
    movdqu [rcx + 208], xmm13
    movdqu [rcx + 224], xmm14
    movdqu [rcx + 240], xmm15
    xor eax, eax
    ret

w64_longjmp:
    mov eax, edx
    test eax, eax
    jnz .value
    inc eax
.value:
    mov rbx, [rcx + 8]
    mov rsp, [rcx + 16]
    mov rbp, [rcx + 24]
    mov rsi, [rcx + 32]
    mov rdi, [rcx + 40]
    mov r12, [rcx + 48]
    mov r13, [rcx + 56]
    mov r14, [rcx + 64]
    mov r15, [rcx + 72]
    ldmxcsr [rcx + 88]
    fldcw [rcx + 92]
    movdqu xmm6, [rcx + 96]
    movdqu xmm7, [rcx + 112]
    movdqu xmm8, [rcx + 128]
    movdqu xmm9, [rcx + 144]
    movdqu xmm10, [rcx + 160]
    movdqu xmm11, [rcx + 176]
    movdqu xmm12, [rcx + 192]
    movdqu xmm13, [rcx + 208]
    movdqu xmm14, [rcx + 224]
    movdqu xmm15, [rcx + 240]
    jmp [rcx + 80]

w64_memcpy:
    mov rax, rcx
    mov r9, rdi
    mov r10, rsi
    mov rdi, rcx
    mov rsi, rdx
    mov rcx, r8
    shr rcx, 3
    rep movsq
    mov rcx, r8
    and ecx, 7
    rep movsb
    mov rdi, r9
    mov rsi, r10
    ret

w64_memmove:
    mov rax, rcx
    cmp rcx, rdx
    jbe w64_memcpy
    lea r9, [rdx + r8]
    cmp rcx, r9
    jae w64_memcpy
    mov r9, rdi
    mov r10, rsi
    lea rdi, [rcx + r8 - 1]
    lea rsi, [rdx + r8 - 1]
    mov rcx, r8
    std
    rep movsb
    cld
    mov rdi, r9
    mov rsi, r10
    ret

w64_memset:
    mov r9, rdi
    mov r10, rcx
    mov rdi, rcx
    movzx eax, dl
    mov rdx, 0x0101010101010101
    imul rax, rdx
    mov rcx, r8
    shr rcx, 3
    rep stosq
    mov rcx, r8
    and ecx, 7
    rep stosb
    mov rdi, r9
    mov rax, r10
    ret

; CONTEXT (x64): ContextFlags 0x30, MxCsr 0x34, SegCs 0x38, SegSs 0x42, EFlags 0x44, Rax 0x78 .. R15 0xf0,
; Rip 0xf8, Xmm0 0x1a0. RtlCaptureContext describes its caller.
w64_capture_context:
    mov [rcx + 0x78], rax
    pushfq
    pop rax
    mov [rcx + 0x44], eax
    mov [rcx + 0x80], rcx
    mov [rcx + 0x88], rdx
    mov [rcx + 0x90], rbx
    lea rax, [rsp + 8]
    mov [rcx + 0x98], rax
    mov [rcx + 0xa0], rbp
    mov [rcx + 0xa8], rsi
    mov [rcx + 0xb0], rdi
    mov [rcx + 0xb8], r8
    mov [rcx + 0xc0], r9
    mov [rcx + 0xc8], r10
    mov [rcx + 0xd0], r11
    mov [rcx + 0xd8], r12
    mov [rcx + 0xe0], r13
    mov [rcx + 0xe8], r14
    mov [rcx + 0xf0], r15
    mov rax, [rsp]
    mov [rcx + 0xf8], rax
    stmxcsr [rcx + 0x34]
    mov dword [rcx + 0x30], 0x10000b
    mov word [rcx + 0x38], CODE64
    mov word [rcx + 0x42], DATA32
%assign i 0
%rep 16
    movdqu [rcx + 0x1a0 + i * 16], xmm %+ i
%assign i i + 1
%endrep
    mov rax, [rcx + 0x78]
    ret

w64_restore_context:
    mov r11, [rcx + 0x98]
    mov rax, [rcx + 0xf8]
    mov [r11 - 8], rax
    mov rax, [rcx + 0x80]
    mov [r11 - 16], rax
    mov rax, [rcx + 0xd0]
    mov [r11 - 24], rax
    mov eax, [rcx + 0x44]
    or eax, 0x200
    mov [r11 - 32], rax
    ldmxcsr [rcx + 0x34]
%assign i 0
%rep 16
    movdqu xmm %+ i, [rcx + 0x1a0 + i * 16]
%assign i i + 1
%endrep
    mov rax, [rcx + 0x78]
    mov rdx, [rcx + 0x88]
    mov rbx, [rcx + 0x90]
    mov rbp, [rcx + 0xa0]
    mov rsi, [rcx + 0xa8]
    mov rdi, [rcx + 0xb0]
    mov r8, [rcx + 0xb8]
    mov r9, [rcx + 0xc0]
    mov r10, [rcx + 0xc8]
    mov r12, [rcx + 0xd8]
    mov r13, [rcx + 0xe0]
    mov r14, [rcx + 0xe8]
    mov r15, [rcx + 0xf0]
    lea rsp, [r11 - 32]
    popfq
    pop r11
    pop rcx
    ret

; RaiseException(code, flags, count, args): build the record and the caller's context, then dispatch.
w64_raise_exception:
    sub rsp, 0x598
    mov [rsp + 0x20], ecx
    and edx, 1
    mov [rsp + 0x24], edx
    mov qword [rsp + 0x28], 0
    mov rax, [rsp + 0x598]
    mov [rsp + 0x30], rax
    cmp r8d, 15
    jbe .count
    mov r8d, 15
.count:
    test r9, r9
    jnz .keep
    xor r8d, r8d
.keep:
    mov [rsp + 0x38], r8d
    xor eax, eax
.copy:
    cmp eax, r8d
    jae .copied
    mov r10, [r9 + rax * 8]
    mov [rsp + 0x40 + rax * 8], r10
    inc eax
    jmp .copy
.copied:
    lea rcx, [rsp + 0xc0]
    call w64_capture_context
    mov rax, [rsp + 0x598]
    mov [rsp + 0xc0 + 0xf8], rax
    lea rax, [rsp + 0x5a0]
    mov [rsp + 0xc0 + 0x98], rax
    lea rcx, [rsp + 0x20]
    lea rdx, [rsp + 0xc0]
    mov eax, w64_raise_hook
    call [rax]
    add rsp, 0x598
    ret

; RtlUnwindEx(frame, ip, record, value, context, history): hands the caller's context to the unwinder.
w64_unwind_ex:
    sub rsp, 0x518
    mov [rsp + 0x520], rcx
    mov [rsp + 0x528], rdx
    mov [rsp + 0x530], r8
    mov [rsp + 0x538], r9
    lea rcx, [rsp + 0x40]
    call w64_capture_context
    mov rax, [rsp + 0x518]
    mov [rsp + 0x40 + 0xf8], rax
    lea rax, [rsp + 0x520]
    mov [rsp + 0x40 + 0x98], rax
    mov rcx, [rsp + 0x520]
    mov rdx, [rsp + 0x528]
    mov r8, [rsp + 0x530]
    mov r9, [rsp + 0x538]
    lea rax, [rsp + 0x40]
    mov [rsp + 0x20], rax
    mov rax, [rsp + 0x548]
    mov [rsp + 0x28], rax
    mov eax, w64_unwind_hook
    call [rax]
    add rsp, 0x518
    ret

w64_strlen:
    mov rax, rcx
.scan:
    cmp byte [rax], 0
    je .done
    inc rax
    jmp .scan
.done:
    sub rax, rcx
    ret

section .data
align 4
w64_dispatch_fn:
    dd 0
align 8
w64_raise_hook:
    dq 0
w64_unwind_hook:
    dq 0

section .rodata
align 4
isr64_table:
%assign i 0
%rep 48
    dd isr64_ %+ i
%assign i i + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
