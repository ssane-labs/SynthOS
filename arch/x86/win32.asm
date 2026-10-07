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
