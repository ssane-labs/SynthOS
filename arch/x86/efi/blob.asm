; Embeds the long-mode exit trampoline into the UEFI loader.

section .rdata
global trampoline_start
global trampoline_end

trampoline_start:
    incbin TRAMPOLINE_FILE
trampoline_end:
