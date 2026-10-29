; SynthOS 64-bit kernel entry. The loaders start every kernel the same way (32-bit protected mode at 1 MiB,
; eax = boot magic, ebx = boot info). The 64-bit kernel runs from the window at 0xb0000000 (above the 2.75 GiB
; 32-bit programs get), so this stub works with physical addresses until paging is on: it moves the ramdisk pack
; behind the kernel, maps low memory, the kernel window and all of the first 4 GiB at DIRECT, enables long mode
; and calls x86_main.

CODE64 equ 0x30
DATA   equ 0x10
EFER   equ 0xc0000080
KOFF   equ 0xb0000000           ; kernel virtual = physical + KOFF

[bits 32]

section .entry
global _start
global boot_pml4
global boot_pack
extern x86_main
extern kernel_sectors
extern __bss_start
extern __bss_end
extern pack_start

%define PHYS(x) ((x) - KOFF)

_start:
    jmp short begin
    align 8, db 0
    dd 0x4b4e5953
    dd kernel_sectors

begin:
    cli
    mov ebp, eax
    ; the pack goes right behind the kernel's bss, 2 MiB aligned
    mov edi, PHYS(__bss_end) + 0x1fffff
    and edi, ~0x1fffff
    mov [boot_pack_addr], edi
    mov esi, PHYS(pack_start)
    cmp dword [esi], 0x4b505953
    jne .no_pack
    mov ecx, [esi + 4]
    add ecx, 8
    lea esi, [esi + ecx - 1]
    lea edi, [edi + ecx - 1]
    std
    rep movsb
    jmp .moved
.no_pack:
    mov dword [edi], 0
.moved:
    cld
    mov edi, PHYS(__bss_start)
    mov ecx, PHYS(__bss_end)
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; identity 0-4 GiB for the boot, then the window at 0xb0000000 over the first 256 MiB
    mov edi, PHYS(boot_pd_low)
    mov eax, 0x83
    xor edx, edx
    mov ecx, 2048
.ident:
    mov [edi], eax
    mov [edi + 4], edx
    add eax, 0x200000
    adc edx, 0
    add edi, 8
    loop .ident
    mov edi, PHYS(boot_pd_low) + (KOFF >> 21) * 8
    mov eax, 0x83
    mov ecx, 128
.window:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .window
    ; DIRECT (0xffff800000000000): all of the first 4 GiB, for devices and page tables
    mov edi, PHYS(boot_pd_direct)
    mov eax, 0x83
    xor edx, edx
    mov ecx, 2048
.direct:
    mov [edi], eax
    mov [edi + 4], edx
    add eax, 0x200000
    adc edx, 0
    add edi, 8
    loop .direct
    xor ecx, ecx
.pdpt:
    mov eax, ecx
    shl eax, 12
    lea edx, [eax + PHYS(boot_pd_low) + 3]
    mov [PHYS(boot_pdpt_low) + ecx * 8], edx
    lea edx, [eax + PHYS(boot_pd_direct) + 3]
    mov [PHYS(boot_pdpt_direct) + ecx * 8], edx
    inc ecx
    cmp ecx, 4
    jb .pdpt
    mov eax, PHYS(boot_pdpt_low) + 3
    mov [PHYS(boot_pml4)], eax
    mov eax, PHYS(boot_pdpt_direct) + 3
    mov [PHYS(boot_pml4) + 256 * 8], eax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)
    mov cr4, eax
    mov eax, PHYS(boot_pml4)
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
    mov eax, [boot_pack_addr]
    mov rdx, boot_pack
    mov [rdx], eax
    mov edi, ebp
    mov esi, ebx
    mov rax, x86_main
    call rax
.hang:
    cli
    hlt
    jmp .hang

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

; set before the bss is cleared, so it lives here (at its physical address, mapped one to one)
align 4
boot_pack_addr:
    dd 0

section .bss
alignb 4096
boot_pml4:
    resb 4096
boot_pdpt_low:
    resb 4096
boot_pdpt_direct:
    resb 4096
global boot_pd_low
boot_pd_low:
    resb 4096 * 4
boot_pd_direct:
    resb 4096 * 4
boot_pack:
    resd 1
alignb 16
stack_bottom:
    resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
