; files.asm - exercises DOS file handles: create, write, seek, read, delete
        org 0x100

start:  mov ah, 0x3c            ; create TEST.TXT
        xor cx, cx
        mov dx, fname
        int 0x21
        jc fail
        mov bx, ax
        mov ah, 0x40            ; write text
        mov cx, text_len
        mov dx, text
        int 0x21
        jc fail
        mov ax, 0x4200          ; rewind
        xor cx, cx
        xor dx, dx
        int 0x21
        mov ah, 0x3f            ; read back
        mov cx, 64
        mov dx, buffer
        int 0x21
        jc fail
        mov si, ax              ; bytes read
        mov ah, 0x3e
        int 0x21
        mov byte [buffer + si], '$'
        mov ah, 0x09
        mov dx, readback
        int 0x21
        mov dx, buffer
        int 0x21
        mov ah, 0x41            ; delete it again
        mov dx, fname
        int 0x21
        jc fail
        mov ah, 0x30            ; DOS version
        int 0x21
        add al, '0'
        mov [version], al
        mov ah, 0x09
        mov dx, done
        int 0x21
        mov ax, 0x4c00
        int 0x21
fail:   mov ah, 0x09
        mov dx, failed
        int 0x21
        mov ax, 0x4c01
        int 0x21

fname   db "TEST.TXT", 0
text    db "file I/O through INT 21h works", 13, 10
text_len equ $ - text
readback db "read back: $"
done    db "DOS version reported: "
version db "?", ".0", 13, 10, "$"
failed  db "file test FAILED", 13, 10, "$"
buffer  times 80 db 0
