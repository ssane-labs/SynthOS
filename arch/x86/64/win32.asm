; 32-bit Windows programs on the 64-bit kernel. They run in compatibility mode (code selector 0x08, FS at their
; TEB); a call to a built-in function comes in through w32_gate32 and leaves through w32_gate_dispatch, and the
; kernel calls their code (entry points, callbacks, window procedures) through arch_w32_enter.

CODE32 equ 0x08
TEB32  equ 0x18
CODE64 equ 0x30

extern w32_gate_dispatch
extern crt_cxx_frame_gate

section .text32 progbits alloc exec nowrite align=16

[bits 32]

; every built-in function's stub: push its index, jmp w32_gate32
global w32_gate32
w32_gate32:
    jmp far [gate64_far]

; where the kernel's calls into a program return to
global w32_ret32
w32_ret32:
    jmp far [back64_far]

align 4
gate64_far:
    dd gate64
    dw CODE64
back64_far:
    dd back64
    dw CODE64

; Microsoft x86 jmp_buf: Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel, Cookie.
global crt_setjmp3
global crt_longjmp
crt_setjmp3:
    mov edx, [esp + 4]
    mov [edx], ebp
    mov [edx + 4], ebx
    mov [edx + 8], edi
    mov [edx + 12], esi
    lea ecx, [esp + 4]
    mov [edx + 16], ecx
    mov ecx, [esp]
    mov [edx + 20], ecx
    mov ecx, [fs:0]
    mov [edx + 24], ecx
    mov dword [edx + 28], -1
    mov dword [edx + 32], 0x56433230
    xor eax, eax
    ret

crt_longjmp:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    test eax, eax
    jnz .value
    inc eax
.value:
    mov ecx, [edx + 24]
    mov [fs:0], ecx
    mov ebp, [edx]
    mov ebx, [edx + 4]
    mov edi, [edx + 8]
    mov esi, [edx + 12]
    mov esp, [edx + 16]
    jmp [edx + 20]

; MSVC C++ exception support: frame handlers get their FuncInfo in eax
global crt_cxx_frame_handler
crt_cxx_frame_handler:
    push dword [esp + 16]
    push dword [esp + 16]
    push dword [esp + 16]
    push dword [esp + 16]
    push eax
    call [crt_cxx_frame_gate]
    add esp, 20
    ret

; _EH_prolog: the frame setup of MSVC functions with C++ unwinding, handler in eax
global crt_eh_prolog
crt_eh_prolog:
    push -1
    push eax
    push dword [fs:0]
    mov [fs:0], esp
    mov eax, [esp + 12]
    mov [esp + 12], ebp
    lea ebp, [esp + 12]
    push eax
    ret

; The most frequent tiny functions run here in the program's own mode, without the two mode switches of the gate.
; Each falls back to the gate (its index in w32_fast_index, set by w32_gate_init) when API tracing is on or the case
; needs the kernel.
extern w32_trace
extern w32_precise_waits
extern w32_word_waiters

%macro SLOW 1
    push dword [w32_fast_index + %1 * 4]
    jmp w32_gate32
%endmacro

fast_get_last_error:
    cmp byte [w32_trace], 0
    jne .slow
    mov eax, [fs:0x34]
    ret
.slow:
    SLOW 0

fast_set_last_error:
    cmp byte [w32_trace], 0
    jne .slow
    mov eax, [esp + 4]
    mov [fs:0x34], eax
    ret 4
.slow:
    SLOW 1

fast_thread_id:
    cmp byte [w32_trace], 0
    jne .slow
    mov eax, [fs:0x24]
    ret
.slow:
    SLOW 2

fast_tls_get:
    cmp byte [w32_trace], 0
    jne .slow
    mov ecx, [esp + 4]
    cmp ecx, 64
    jae .slow
    mov eax, [fs:0xe10 + ecx * 4]
    mov dword [fs:0x34], 0
    ret 4
.slow:
    SLOW 3

fast_tls_set:
    cmp byte [w32_trace], 0
    jne .slow
    mov ecx, [esp + 4]
    cmp ecx, 64
    jae .slow
    mov eax, [esp + 8]
    mov [fs:0xe10 + ecx * 4], eax
    mov eax, 1
    ret 8
.slow:
    SLOW 4

; CRITICAL_SECTION: +4 lock count, +8 recursion, +12 owner thread
fast_enter_cs:
    cmp byte [w32_trace], 0
    jne .slow
    mov ecx, [esp + 4]
    mov edx, [fs:0x24]
    cmp [ecx + 12], edx
    je .mine
    xor eax, eax
    lock cmpxchg [ecx + 12], edx
    jne .slow
.mine:
    inc dword [ecx + 8]
    lock inc dword [ecx + 4]
    ret 4
.slow:
    SLOW 5

fast_leave_cs:
    cmp byte [w32_trace], 0
    jne .slow
    cmp byte [w32_precise_waits], 0
    je .slow
    cmp dword [w32_word_waiters], 0
    jne .slow
    mov ecx, [esp + 4]
    mov edx, [fs:0x24]
    cmp [ecx + 12], edx
    jne .done
    cmp dword [ecx + 8], 0
    jle .done
    lock dec dword [ecx + 4]
    dec dword [ecx + 8]
    jnz .done
    xor eax, eax
    xchg [ecx + 12], eax
.done:
    ret 4
.slow:
    SLOW 6

; msvcrt (cdecl): the caller pops the arguments
fast_memmove:
    cmp byte [w32_trace], 0
    jne .slow
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
    mov eax, edi
    cmp edi, esi
    jbe .forward
    lea edx, [esi + ecx]
    cmp edi, edx
    jae .forward
    lea esi, [esi + ecx - 1]
    lea edi, [edi + ecx - 1]
    std
    rep movsb
    cld
    jmp .out
.forward:
    mov edx, ecx
    shr ecx, 2
    rep movsd
    mov ecx, edx
    and ecx, 3
    rep movsb
.out:
    pop edi
    pop esi
    ret
.slow:
    SLOW 7

fast_memset:
    cmp byte [w32_trace], 0
    jne .slow
    push edi
    mov edi, [esp + 8]
    movzx eax, byte [esp + 12]
    mov ecx, [esp + 16]
    mov edx, edi
    imul eax, eax, 0x01010101
    push ecx
    shr ecx, 2
    rep stosd
    pop ecx
    and ecx, 3
    rep stosb
    mov eax, edx
    pop edi
    ret
.slow:
    SLOW 8

fast_strlen:
    cmp byte [w32_trace], 0
    jne .slow
    mov edx, [esp + 4]
    mov eax, edx
.next:
    cmp byte [eax], 0
    je .end
    inc eax
    jmp .next
.end:
    sub eax, edx
    ret
.slow:
    SLOW 9

fast_strcmp:
    cmp byte [w32_trace], 0
    jne .slow
    push esi
    mov ecx, [esp + 8]
    mov edx, [esp + 12]
.next:
    movzx eax, byte [ecx]
    movzx esi, byte [edx]
    cmp eax, esi
    jne .differ
    test eax, eax
    jz .differ
    inc ecx
    inc edx
    jmp .next
.differ:
    sub eax, esi
    pop esi
    ret
.slow:
    SLOW 10

section .rodata
global w32_fast_code
w32_fast_code:
    dd fast_get_last_error, fast_set_last_error, fast_thread_id, fast_tls_get, fast_tls_set, fast_enter_cs
    dd fast_leave_cs, fast_memmove, fast_memset, fast_strlen, fast_strcmp
    dd fast_memmove

section .data
global w32_fast_index
w32_fast_index:
    times 12 dd 0

section .text

[bits 64]

; [rsp] = index pushed by the stub, [rsp + 4] = the program's return address, then its arguments
gate64:
    mov esp, esp
    push rsi
    push rdi
    push rbx
    push rbp
    mov rbp, rsp
    mov edi, [rbp + 32]
    lea esi, [rbp + 40]
    and rsp, -16
    sub rsp, 48
    mov rdx, rsp
    cld
    call w32_gate_dispatch
    mov ecx, eax
    mov r8, [rsp]
    mov r9, [rsp + 8]
    movzx eax, byte [rsp + 32]
    test eax, eax
    jz .result
    cmp eax, 2
    je .f80
    cmp eax, 3
    je .f32
    fld qword [rsp + 16]
    jmp .result
.f32:
    fld dword [rsp + 16]
    jmp .result
.f80:
    fld tword [rsp + 16]
.result:
    mov rsp, rbp
    pop rbp
    pop rbx
    pop rdi
    pop rsi
    mov r10d, [rsp + 4]
    lea esp, [rsp + rcx + 8]
    mov eax, r8d
    mov edx, r9d
    push qword CODE32
    push r10
    retfq

; the program's code returned to w32_ret32; ebx still holds the frame arch_w32_enter left
back64:
    mov ebx, ebx
    mov rsp, rbx
    mov eax, eax
    shl rdx, 32
    or rax, rdx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; uint64_t arch_w32_enter(uint32_t fn, int argc, const uint32_t *args, uint32_t stack, uint32_t ebp, uint32_t ecx)
global arch_w32_enter
arch_w32_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov rbx, rsp
    mov r12d, edi
    mov r13d, r8d
    mov r14d, r9d
    test ecx, ecx
    jz .here
    mov esp, ecx
.here:
    and rsp, -16
    mov ecx, esi
.push:
    test ecx, ecx
    jz .pushed
    dec ecx
    mov eax, [rdx + rcx * 4]
    sub rsp, 4
    mov [rsp], eax
    jmp .push
.pushed:
    sub rsp, 4
    mov dword [rsp], w32_ret32
    mov ax, TEB32
    mov fs, ax
    cld
    mov ebp, r13d
    mov ecx, r14d
    push qword CODE32
    push r12
    retfq

; uint32_t arch_w32_call(void *fn, int argc, const uint32_t *args, void *stack_top, struct w32_jump *jump)
global arch_w32_call
arch_w32_call:
    mov [r8], rbx
    mov [r8 + 8], rbp
    mov [r8 + 16], r12
    mov [r8 + 24], r13
    mov [r8 + 32], r14
    mov [r8 + 40], r15
    lea rax, [rsp + 8]
    mov [r8 + 48], rax
    mov rax, [rsp]
    mov [r8 + 56], rax
    mov dword [r8 + 64], 1
    push r8
    sub rsp, 8
    xor r8d, r8d
    xor r9d, r9d
    call arch_w32_enter
    add rsp, 8
    pop r8
    mov dword [r8 + 64], 0
    ret

; uint32_t arch_native_call(uint32_t (*fn)(uint32_t), uint32_t arg, void *stack_top, struct w32_jump *jump): a kernel
; function on a program's stack, with the jump arch_w32_abort returns through
global arch_native_call
arch_native_call:
    mov [rcx], rbx
    mov [rcx + 8], rbp
    mov [rcx + 16], r12
    mov [rcx + 24], r13
    mov [rcx + 32], r14
    mov [rcx + 40], r15
    lea rax, [rsp + 8]
    mov [rcx + 48], rax
    mov rax, [rsp]
    mov [rcx + 56], rax
    mov dword [rcx + 64], 1
    push rbx
    push r12
    push rbp
    mov rbx, rsp
    mov r12, rcx
    mov rax, rdi
    mov edi, esi
    mov rsp, rdx
    and rsp, -16
    call rax
    mov rsp, rbx
    mov dword [r12 + 64], 0
    pop rbp
    pop r12
    pop rbx
    ret

; void arch_w32_abort(struct w32_jump *jump, uint32_t code)
global arch_w32_abort
arch_w32_abort:
    mov eax, esi
    sti
    cld
    mov dword [rdi + 64], 0
    mov rbx, [rdi]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    jmp [rdi + 56]

; void arch_switch(uint32_t *save_esp, uint32_t new_esp)
global arch_switch
arch_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov [rdi], esp
    mov esp, esi
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; uint32_t arch_call_on_stack(uint32_t (*fn)(void *), void *arg, void *stack_top)
global arch_call_on_stack
arch_call_on_stack:
    push rbp
    mov rbp, rsp
    mov rax, rdi
    mov rdi, rsi
    mov rsp, rdx
    and rsp, -16
    call rax
    mov rsp, rbp
    pop rbp
    ret

; void arch_continue_at(void *ip, uint32_t ebp, uint32_t esp): resume a program at ip with that frame
global arch_continue_at
arch_continue_at:
    mov ax, TEB32
    mov fs, ax
    mov ebp, esi
    mov esp, edx
    push qword CODE32
    push rdi
    retfq

; uint64_t w32_native_call(const void *fn, const uint64_t ints[6], const uint64_t sse[8], const uint64_t *stack,
;                          size_t words, struct gate_ret *out): a System V call built by the gate
global w32_native_call
w32_native_call:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    mov rbx, r9
    mov r12, rdi
    lea rax, [r8 * 8 + 15]
    and rax, -16
    sub rsp, rax
    and rsp, -16
    xor eax, eax
.copy:
    cmp rax, r8
    jae .copied
    mov r10, [rcx + rax * 8]
    mov [rsp + rax * 8], r10
    inc rax
    jmp .copy
.copied:
    movq xmm0, [rdx]
    movq xmm1, [rdx + 8]
    movq xmm2, [rdx + 16]
    movq xmm3, [rdx + 24]
    movq xmm4, [rdx + 32]
    movq xmm5, [rdx + 40]
    movq xmm6, [rdx + 48]
    movq xmm7, [rdx + 56]
    mov r10, rsi
    mov rdi, [r10]
    mov rsi, [r10 + 8]
    mov rdx, [r10 + 16]
    mov rcx, [r10 + 24]
    mov r8, [r10 + 32]
    mov r9, [r10 + 40]
    mov eax, 8
    call r12
    mov [rbx], rax
    mov [rbx + 8], rdx
    movq [rbx + 16], xmm0
    cmp byte [rbx + 32], 2
    jne .done
    fstp tword [rbx + 16]
.done:
    lea rsp, [rbp - 16]
    pop r12
    pop rbx
    pop rbp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
