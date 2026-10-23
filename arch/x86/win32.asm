; Entering and leaving Win32 program code: own stack, FS pointing at the TEB.

[bits 32]
section .text

global arch_w32_call
global arch_w32_abort

TEB_SELECTOR equ 0x18
DATA_SELECTOR equ 0x10

; uint32_t arch_w32_call(void *fn, int argc, const uint32_t *args, void *stack_top, struct w32_jump *jump)
arch_w32_call:
    mov eax, [esp + 20]             ; jump
    mov [eax + 0], ebx
    mov [eax + 4], esi
    mov [eax + 8], edi
    mov [eax + 12], ebp
    lea ecx, [esp + 4]
    mov [eax + 16], ecx             ; esp after our return
    mov ecx, [esp]
    mov [eax + 20], ecx             ; return address
    mov dword [eax + 24], 1

    mov edx, [esp + 4]              ; fn
    mov ecx, [esp + 8]              ; argc
    mov esi, [esp + 12]             ; args
    mov edi, [esp + 16]             ; stack top
    mov ebx, eax                    ; jump survives the call in ebx (callee-saved)

    mov esp, edi
    and esp, -16
.push:
    test ecx, ecx
    jz .call
    dec ecx
    push dword [esi + ecx * 4]
    jmp .push
.call:
    mov ax, TEB_SELECTOR
    mov fs, ax
    cld
    call edx

    mov ecx, ebx
    jmp leave_to_kernel

; void arch_w32_abort(struct w32_jump *jump, uint32_t code)
arch_w32_abort:
    mov ecx, [esp + 4]
    mov eax, [esp + 8]
    sti

leave_to_kernel:
    mov dx, DATA_SELECTOR
    mov fs, dx
    cld
    mov dword [ecx + 24], 0
    mov ebx, [ecx + 0]
    mov esi, [ecx + 4]
    mov edi, [ecx + 8]
    mov ebp, [ecx + 12]
    mov esp, [ecx + 16]
    jmp [ecx + 20]

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

; uint32_t arch_call_on_stack(uint32_t (*fn)(void *), void *arg, void *stack_top)
global arch_call_on_stack
arch_call_on_stack:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    mov ecx, [ebp + 12]
    mov esp, [ebp + 16]
    and esp, ~15
    sub esp, 12
    push ecx
    call eax
    mov esp, ebp
    pop ebp
    ret

section .text

; void arch_switch(uint32_t *save_esp, uint32_t new_esp)
global arch_switch
arch_switch:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    push ebp
    push ebx
    push esi
    push edi
    pushfd
    mov [eax], esp
    mov esp, edx
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

section .text

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

section .text

; MSVC C++ exception support: frame handlers get their FuncInfo in eax, funclets run on the frame's ebp,
; constructors and destructors are thiscall.
extern cxx_frame_handler
global crt_cxx_frame_handler
crt_cxx_frame_handler:
    push dword [esp + 16]
    push dword [esp + 16]
    push dword [esp + 16]
    push dword [esp + 16]
    push eax
    call cxx_frame_handler
    add esp, 20
    ret

; uint32_t arch_call_funclet(void *fn, uint32_t ebp)
global arch_call_funclet
arch_call_funclet:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, [esp + 20]
    mov ebp, [esp + 24]
    call eax
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; void arch_thiscall(void *fn, void *self, int argc, uint32_t a, uint32_t b)
global arch_thiscall
arch_thiscall:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov ecx, [ebp + 16]
    cmp ecx, 2
    jb .one
    push dword [ebp + 24]
.one:
    test ecx, ecx
    jz .call
    push dword [ebp + 20]
.call:
    mov ecx, [ebp + 12]
    call [ebp + 8]
    lea esp, [ebp - 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; void arch_continue_at(void *ip, uint32_t ebp, uint32_t esp)
global arch_continue_at
arch_continue_at:
    mov eax, [esp + 4]
    mov ebp, [esp + 8]
    mov esp, [esp + 12]
    jmp eax

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
