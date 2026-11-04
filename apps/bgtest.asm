; SPDX-License-Identifier: GPL-2.0-or-later
; BGTEST.COM - Grafik-Box im Hintergrund (AT-DOS 6.0, virtuelle VGA Stufe 2)
;
; Zeichnet je BIOS-Takt (18,2 je Sekunde) eine Zeile von oben nach unten
; und stellt bei Zeile 100 die Farbe 1 per Port 3C8h/3C9h auf Rot (CGA:
; den DAC-Eintrag aus Attributregister 1, per 3C0h/3C1h gelesen). Mit
; START in eigener Sitzung starten, mit Alt+Esc wegschalten, spaeter
; zurueck: laeuft die Box hinten weiter, ist das Bild inzwischen weiter
; gewachsen (und rot, falls Zeile 100 hinten erreicht wurde).
;   BGTEST      Modus 13h (320x200x256)
;   BGTEST 4    CGA-Modus 4 (320x200x4)
; Eine Taste beendet.
        org     100h
        mov     si, 81h
.arg:   lodsb
        cmp     al, ' '
        je      .arg
        cmp     al, '4'
        jne     .m13
        mov     byte [cga], 1
        mov     ax, 4
        int     10h
        push    word 0B800h
        jmp     .seg
.m13:   mov     ax, 13h
        int     10h
        push    word 0A000h
.seg:   pop     es
        xor     bx, bx                  ; Zeile
.next:  xor     ah, ah                  ; auf neuen Takt warten
        int     1Ah
        mov     si, dx
.w:     mov     dx, 3DAh                ; Ruecklauf (hinten nachgebildet)
        in      al, dx
        mov     ah, 1
        int     16h
        jnz     .end
        xor     ah, ah
        int     1Ah
        cmp     dx, si
        je      .w
        cmp     byte [cga], 0
        jne     .c4
        mov     ax, 320                 ; 13h: Zeile bx mit Farbe 1
        mul     bx
        mov     di, ax
        mov     al, 1
        mov     cx, 320
        rep     stosb
        jmp     .pal
.c4:    mov     ax, bx                  ; CGA: gerade/ungerade Zeilen
        shr     ax, 1
        mov     cx, 80
        mul     cx
        mov     di, ax
        test    bl, 1
        jz      .ev
        add     di, 2000h
.ev:    mov     al, 55h
        rep     stosb
.pal:   cmp     bx, 100
        jne     .n
        mov     ah, 1                   ; 13h: DAC-Eintrag 1
        cmp     byte [cga], 0
        je      .dac
        mov     dx, 3DAh                ; CGA: Farbe 1 zeigt ueber das
        in      al, dx                  ; Attributregister 1 auf einen
        mov     dx, 3C0h                ; anderen DAC-Eintrag (BIOS:
        mov     al, 21h                 ; 13h) - den nehmen (Index mit
        out     dx, al                  ; Bit 5, Bild bleibt an)
        inc     dx
        in      al, dx
        mov     ah, al
        mov     dx, 3DAh
        in      al, dx
.dac:   mov     dx, 3C8h                ; Farbe 1 -> rot
        mov     al, ah
        out     dx, al
        inc     dx
        mov     al, 63
        out     dx, al
        xor     al, al
        out     dx, al
        out     dx, al
.n:     inc     bx
        cmp     bx, 200
        jb      .next
.hold:  mov     ah, 1
        int     16h
        jz      .hold
.end:   xor     ah, ah
        int     16h
        mov     ax, 3
        int     10h
        ret
cga     db      0
