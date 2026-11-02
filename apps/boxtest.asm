; SPDX-License-Identifier: GPL-2.0-or-later
; BOXTEST.COM - Test der DOS-Boxen, Stufe 2 (AT-DOS 6.0)
;
; Ein Real-Mode-Programm wie ein typisches DOS-Spiel: schreibt direkt in
; den Bildspeicher B800, haengt sich an INT 1Ch (Takt) und INT 9
; (Tastatur, Scancodes von Port 60h, EOI an den PIC), wartet auf den
; Strahlruecklauf (3DAh) und rollt mit INT 10h. Laeuft unter MS-DOS
; genauso. Esc beendet.

cpu 386
org 100h

        mov     ax, 3                   ; Textmodus, Bild leer
        int     10h
        mov     ax, 0B800h
        mov     es, ax
        ; Titelzeile direkt in den Bildspeicher, weiss auf blau
        xor     di, di
        mov     si, t_title
        mov     ah, 1Fh
.t:     lodsb
        test    al, al
        jz      .te
        stosw
        jmp     .t
.te:    mov     cx, 80
        sub     cx, di
        shr     cx, 1                   ; (di/2 Zeichen geschrieben)
        add     cx, di
        sub     cx, di
        mov     al, ' '
.tf:    cmp     di, 160
        jae     .tg
        stosw
        jmp     .tf
.tg:
        ; Farbbalken in Zeile 2: 16 Hintergrundfarben
        mov     di, 2*160
        xor     bl, bl
.cb:    mov     ah, bl
        shl     ah, 4
        or      ah, 0Fh
        mov     al, ' '
        mov     cx, 5
        rep     stosw
        inc     bl
        cmp     bl, 16
        jb      .cb

        ; Vektoren umbiegen
        mov     ax, 351Ch
        int     21h
        mov     [old1c], bx
        mov     [old1c+2], es
        mov     ax, 251Ch
        mov     dx, new1c
        int     21h
        mov     ax, 3509h
        int     21h
        mov     [old9], bx
        mov     [old9+2], es
        mov     ax, 2509h
        mov     dx, new9
        int     21h

        mov     dh, 4                   ; Cursor Zeile 4
        xor     dl, dl
        xor     bh, bh
        mov     ah, 2
        int     10h
        mov     dx, t_help
        mov     ah, 9
        int     21h

; Hauptschleife: auf Ruecklauf warten, Takte und letzten Scancode zeigen
main:   mov     dx, 3DAh
.w1:    in      al, dx                  ; erst Ruecklauf vorbei ...
        test    al, 8
        jnz     .w1
.w2:    in      al, dx                  ; ... dann sein Anfang
        test    al, 8
        jz      .w2
        mov     ax, 0B800h
        mov     es, ax
        mov     di, 23*160 + 2*0
        mov     si, t_ticks
        call    putstr
        mov     ax, [ticks]
        call    puthex
        mov     si, t_scan
        call    putstr
        mov     al, [lastsc]
        xor     ah, ah
        call    puthex
        mov     si, t_keys
        call    putstr
        mov     ax, [nkeys]
        call    puthex
        ; neue Taste? dann eine Zeile ausgeben und Fenster rollen
        mov     al, [newkey]
        test    al, al
        jz      .nk
        mov     byte [newkey], 0
        cmp     al, 1                   ; Esc
        je      quit
        mov     ax, 0601h               ; Fenster Zeilen 6-20 eins hoch
        mov     bh, 1Eh                 ; gelb auf blau
        mov     cx, 0600h
        mov     dx, 144Fh
        int     10h
        mov     dx, 1400h               ; Zeile 20
        xor     bh, bh
        mov     ah, 2
        int     10h
        mov     dx, t_line
        mov     ah, 9
        int     21h
        mov     ax, [lastsc]
        xor     ah, ah
        mov     di, 20*160 + 2*22
        call    puthex
.nk:    jmp     main

quit:   push    ds
        lds     dx, [old9]
        mov     ax, 2509h
        int     21h
        pop     ds
        push    ds
        lds     dx, [old1c]
        mov     ax, 251Ch
        int     21h
        pop     ds
        mov     dh, 22
        xor     dl, dl
        xor     bh, bh
        mov     ah, 2
        int     10h
        mov     dx, t_bye
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

; si -> ES:DI, hellgrau
putstr: mov     ah, 07h
.l:     lodsb
        test    al, al
        jz      .e
        stosw
        jmp     .l
.e:     ret

; AX vierstellig hex -> ES:DI, hellgruen
puthex: mov     cx, 4
.l:     rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 7
.d:     mov     ah, 0Ah
        stosw
        pop     ax
        loop    .l
        ret

new1c:  inc     word [cs:ticks]
        jmp     far [cs:old1c]

new9:   push    ax
        in      al, 60h
        mov     [cs:lastsc], al
        test    al, 80h                 ; Loslassen: nicht zaehlen
        jnz     .r
        mov     [cs:newkey], al
        inc     word [cs:nkeys]
.r:     cmp     byte [cs:lastsc], 1     ; Esc selbst behandeln,
        je      .own                    ; sonst an das BIOS weiter
        pop     ax
        jmp     far [cs:old9]
.own:   mov     al, 20h                 ; EOI selbst
        out     20h, al
        pop     ax
        iret

t_title db " BOXTEST - DOS-Box Stufe 2: B800, INT 1Ch, INT 9, 3DAh, INT 10h", 0
t_help  db "Tasten druecken (Scancode per INT 9 / Port 60h), Esc beendet.$"
t_ticks db "Takte (INT 1Ch): ", 0
t_scan  db "   letzter Scancode: ", 0
t_keys  db "   Tasten: ", 0
t_line  db "Taste mit Scancode    $"
t_bye   db "BOXTEST beendet.", 13, 10, "$"
ticks   dw 0
lastsc  dw 0
nkeys   dw 0
newkey  db 0
old1c   dd 0
old9    dd 0
