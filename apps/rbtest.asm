; SPDX-License-Identifier: GPL-2.0-or-later
; RBTEST.COM - Test fuer die Kompatibilitaetsbox (5.1, Stufe 2): laeuft
; ohne DOS im echten Real Mode, nur mit BIOS. Zeigt einen Zaehler (BIOS-
; Takt), gibt getippte Zeichen aus, q beendet (RET -> PSP:0 -> INT 20h).
cpu 8086
org 0x100
        mov     si, msg
        call    puts
.loop:  mov     ah, 1                   ; Taste da?
        int     0x16
        jnz     .key
        push    ds                      ; Takt in Zeile 0 rechts anzeigen
        xor     ax, ax
        mov     ds, ax
        mov     ax, [0x46C]
        pop     ds
        mov     bx, 0xB800
        mov     es, bx
        mov     di, 140
        mov     cx, 4
.hex:   rol     ax, 1
        rol     ax, 1
        rol     ax, 1
        rol     ax, 1
        push    ax
        and     al, 15
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 7
.d:     mov     ah, 0x1E
        stosw
        pop     ax
        loop    .hex
        jmp     .loop
.key:   xor     ah, ah
        int     0x16
        cmp     al, 'q'
        je      .end
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     .loop
.end:   mov     si, bye
        call    puts
        ret
puts:   lodsb
        or      al, al
        jz      .r
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     puts
.r:     ret
msg     db "RBTEST im Real Mode - tippen, Alt+Esc wechselt, q beendet", 13, 10, 0
bye     db 13, 10, "Ende.", 13, 10, 0
