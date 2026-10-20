; Application processor start-up. Assembled as a flat blob, copied to a free page below 1 MB and started there by
; a startup IPI (real mode, CS = page >> 4). The kernel fills in the data block before the IPI; the code uses only
; offsets from its own start, so the page can be anywhere.

[bits 16]
[org 0]

start:
    cli
    cld
    mov ax, cs
    mov ds, ax
    movzx esp, ax
    shl esp, 4
    add esp, 4096 - 16      ; a stack at the top of this page until the kernel's
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    o32 jmp far [far_ptr]

[bits 32]
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    call .here
.here:
    pop ebx
    sub ebx, .here
    mov eax, [ebx + v_cr4]
    mov cr4, eax
    mov eax, [ebx + v_cr3]
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80010000
    mov cr0, eax
    mov eax, 1
    lock xadd [ebx + v_next], eax
    cmp eax, [ebx + v_max]
    jae .halt
    mov ecx, [ebx + v_stacks]
    mov esp, [ecx + eax * 4]
    test esp, esp
    jz .halt
    push eax
    call [ebx + v_entry]
.halt:
    cli
    hlt
    jmp .halt

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt_end:

gdtr:
    dw gdt_end - gdt - 1
    dd 0                    ; linear address of gdt, filled in by the kernel

far_ptr:
    dd 0                    ; linear address of pm32
    dw 0x08

align 4
v_cr3:    dd 0
v_cr4:    dd 0
v_entry:  dd 0
v_stacks: dd 0
v_next:   dd 1
v_max:    dd 0

; offsets the kernel patches
times 2038 - ($ - $$) db 0
off_gdtr:   dw gdtr + 2
off_far:    dw far_ptr
off_pm32:   dw pm32
off_gdt:    dw gdt
off_vars:   dw v_cr3
