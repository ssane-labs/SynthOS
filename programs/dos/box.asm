; box.asm - draws directly into text video memory at B800:0000
        org 0x100

COLS    equ 80

start:  mov ax, 0x0003          ; 80x25 text mode, clears the screen
        int 0x10
        mov ax, 0xb800
        mov es, ax

        mov bx, 0               ; row
.rows:  mov cx, 0               ; column
.cols:  mov ax, bx
        add ax, cx
        and ax, 0x0f
        shl ax, 4               ; background color from (row + col)
        or  al, 0x0f
        mov ah, al
        mov al, 0xb1            ; shade character
        push ax
        mov ax, bx
        mov dx, COLS
        mul dx
        add ax, cx
        shl ax, 1
        mov di, ax
        pop ax
        stosw
        inc cx
        cmp cx, COLS
        jb .cols
        inc bx
        cmp bx, 25
        jb .rows

        mov si, title           ; centered title, white on blue
        mov di, (12 * COLS + 25) * 2
        mov ah, 0x1f
.title: lodsb
        test al, al
        jz .wait
        stosw
        jmp .title

.wait:  mov ah, 0x02            ; park the cursor on the last row
        xor bh, bh
        mov dx, 0x1800
        int 0x10
        mov ah, 0x00            ; wait for a key
        int 0x16
        mov ax, 0x0003
        int 0x10
        mov ax, 0x4c00
        int 0x21

title   db "  Direct video memory works!  ", 0
