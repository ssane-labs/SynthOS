; hello.asm - classic DOS .COM program (INT 21h, AH=09h)
        org 0x100

start:  mov ah, 0x09
        mov dx, message
        int 0x21
        mov ax, 0x4c00
        int 0x21

message db "Hello from MS-DOS land, running inside SynthOS!", 13, 10, "$"
