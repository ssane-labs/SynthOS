; synth.inc - constants for SynthOS SX programs
; usage: .include "synth.inc"

; theme roles for 'sys role'
.equ ROLE_TEXT, 0
.equ ROLE_MUTED, 1
.equ ROLE_ACCENT, 2
.equ ROLE_ACCENT2, 3
.equ ROLE_HIGHLIGHT, 4
.equ ROLE_OK, 5
.equ ROLE_WARN, 6
.equ ROLE_ERROR, 7
.equ ROLE_DIR, 8
.equ ROLE_EXEC, 9

; palette colors for 'sys color' and cell attributes (attr = bg * 16 + fg)
.equ BLACK, 0
.equ BLUE, 1
.equ GREEN, 2
.equ CYAN, 3
.equ RED, 4
.equ MAGENTA, 5
.equ BROWN, 6
.equ LIGHTGRAY, 7
.equ DARKGRAY, 8
.equ LIGHTBLUE, 9
.equ LIGHTGREEN, 10
.equ LIGHTCYAN, 11
.equ LIGHTRED, 12
.equ PINK, 13
.equ YELLOW, 14
.equ WHITE, 15

; key codes returned by 'sys getkey' / 'sys pollkey'
.equ KEY_BACKSPACE, 8
.equ KEY_TAB, 9
.equ KEY_ENTER, 10
.equ KEY_ESC, 27
.equ KEY_UP, 0x100
.equ KEY_DOWN, 0x101
.equ KEY_LEFT, 0x102
.equ KEY_RIGHT, 0x103
.equ KEY_HOME, 0x104
.equ KEY_END, 0x105
.equ KEY_PGUP, 0x106
.equ KEY_PGDN, 0x107
.equ KEY_INSERT, 0x108
.equ KEY_DELETE, 0x109
.equ KEY_F1, 0x10a

; modes for 'sys open'
.equ O_READ, 0
.equ O_WRITE, 1
.equ O_APPEND, 2
.equ O_RDWR, 3

; standard handles
.equ STDIN, 0
.equ STDOUT, 1
.equ STDERR, 2
