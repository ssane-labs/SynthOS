; mzhello.asm - a real MZ .EXE with separate code, data and stack segments
; built with 'nasm -f bin': the header and relocation table are written by hand
        bits 16
        org 0

header:
        dw "MZ"
        dw file_size % 512
        dw (file_size + 511) / 512
        dw 1                            ; relocation entries
        dw (module - header) / 16       ; header size in paragraphs
        dw 0x0040                       ; extra paragraphs needed (stack)
        dw 0xffff                       ; extra paragraphs wanted
        dw (stack - module) / 16        ; SS relative to load segment
        dw 0x0400                       ; SP
        dw 0                            ; checksum
        dw 0                            ; IP
        dw 0                            ; CS relative to load segment
        dw relocations - header
        dw 0
relocations:
        dw fixup - module + 1, 0        ; patch the immediate of "mov ax, data segment"
        align 16, db 0

module:
fixup:  mov ax, (data - module) / 16    ; becomes an absolute segment after relocation
        mov ds, ax
        mov ah, 0x09
        mov dx, message - data
        int 0x21
        mov dx, exit_note - data
        int 0x21
        mov ax, 0x4c07                  ; exit code 7, visible with 'echo $?'
        int 0x21

        align 16, db 0
data:
message   db "Hello from an MZ .EXE: segments relocated, DS set up by the loader.", 13, 10, "$"
exit_note db "Returning exit code 7 - the prompt will show it.", 13, 10, "$"

        align 16, db 0
stack:
file_size equ stack - header
