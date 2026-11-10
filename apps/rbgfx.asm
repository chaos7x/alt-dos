; SPDX-License-Identifier: GPL-2.0-or-later
; RBGFX.COM - Test fuer die Kompatibilitaetsbox (5.1): Grafik ueber Alt+Esc.
; Modus 13h mit Muster, Farbe 1 rot, Taste abwarten (dazwischen Alt+Esc hin
; und zurueck), dann Bild und Palette pruefen: "OK" = beides heil, "B." =
; Bild kaputt, ".X" = Palette kaputt. Taste -> Textmodus, Ende.
cpu 8086
org 0x100
        mov     ax, 0x13
        int     0x10
        mov     ax, 0xA000
        mov     es, ax
        xor     di, di
        mov     cx, 64000
.f:     mov     ax, di
        xor     al, ah
        stosb
        loop    .f
        mov     dx, 0x3C8
        mov     al, 1
        out     dx, al
        inc     dx
        mov     al, 63
        out     dx, al
        xor     al, al
        out     dx, al
        out     dx, al
        xor     ax, ax
        int     0x16            ; hier Alt+Esc hin und zurueck
        xor     di, di          ; Bild pruefen
        mov     cx, 64000
        mov     bl, 'O'
.c:     mov     ax, di
        xor     al, ah
        cmp     [es:di], al
        je      .n
        mov     bl, 'B'
.n:     inc     di
        loop    .c
        mov     dx, 0x3C7       ; Farbe 1 pruefen
        mov     al, 1
        out     dx, al
        mov     dx, 0x3C9
        in      al, dx
        mov     bh, 'K'
        cmp     al, 63
        je      .p
        mov     bh, 'X'
.p:     mov     ah, 0x0E
        mov     al, bl
        int     0x10
        mov     al, bh
        int     0x10
        xor     ax, ax
        int     0x16
        mov     ax, 3
        int     0x10
        ret
