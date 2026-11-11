; SPDX-License-Identifier: GPL-2.0-or-later
; RBHMA.SYS - XMS-Treiber nur fuer die HMA, im Startabbild der Box von
; AT-DOS 5.1 (RBOX)
;
; Die Box laeuft im echten Real Mode. 100000h-10FFEFh (HMA) haelt der
; Kernel fuer sie frei (kernel.c, KMOVE_BASE = 110000h), A20 ist in der
; Box immer offen (der Stummel in start.asm schaltet sie bei jedem
; Ausflug ein, INT 15h/87h lehnt er ab). Erweiterten Speicher hat die Box
; nicht - dieser Treiber kennt daher nur die HMA, wie HIMEM mit 0 KB XMS:
; mit DOS=HIGH zieht FreeDOS dorthin (fragt nach jeder DEVICE=-Zeile).
;
; A20 an/aus (03h-06h) melden Erfolg, ohne den 8042 anzufassen: die
; Leitung bleibt offen (wie bisher ohne Treiber). Alte Programme, die den
; Umlauf bei 1 MB brauchen (EXEPACK, PKLITE 1990), gehen damit so gut oder
; schlecht wie vorher; FreeDOS prueft vor Zugriffen auf die HMA selbst.
cpu 8086
org 0
        dd      -1                      ; letzter Treiber der Datei
        dw      8000h                   ; Zeichengeraet
        dw      strat, intr
        db      "XMSXXXX0"              ; Name wie HIMEM.SYS
rh      dd      0
old2f   dd      0
hma     db      0                       ; HMA vergeben?

; ---- INT 2Fh: 4300h da?, 4310h Einsprung --------------------------------
int2f:  cmp     ax, 4300h
        jne     .e
        mov     al, 80h
        iret
.e:     cmp     ax, 4310h
        jne     .chain
        push    cs
        pop     es
        mov     bx, xms
        iret
.chain: jmp     far [cs:old2f]

; ---- XMS-Einsprung (Fernaufruf) -----------------------------------------
xms:    jmp     short .go               ; Kopf wie in der Spezifikation:
        nop                             ; Platz fuer einen Haken
        nop
        nop
.go:    cmp     ah, 0
        jne     .f1
        mov     ax, 0300h               ; XMS 3.00
        mov     bx, 0100h               ; Treiberversion
        mov     dx, 1                   ; HMA vorhanden
        retf
.f1:    cmp     ah, 1                   ; HMA anfordern
        jne     .f2
        cmp     byte [cs:hma], 0
        jne     .used
        mov     byte [cs:hma], 1
        jmp     short .ok
.used:  mov     bl, 91h                 ; schon vergeben
        jmp     short .fail
.f2:    cmp     ah, 2                   ; HMA freigeben
        jne     .f3
        cmp     byte [cs:hma], 0
        je      .notal
        mov     byte [cs:hma], 0
        jmp     short .ok
.notal: mov     bl, 93h                 ; nicht vergeben
        jmp     short .fail
.f3:    cmp     ah, 7                   ; 03h-07h: A20 bleibt offen
        ja      .f8
        cmp     ah, 3
        jb      .no
.ok:    mov     ax, 1
        xor     bl, bl
        retf
.f8:    cmp     ah, 8                   ; freier XMS: keiner
        jne     .no
        xor     ax, ax
        xor     dx, dx
        mov     bl, 0A0h                ; alles vergeben
        retf
.no:    mov     bl, 80h                 ; nicht vorhanden
.fail:  xor     ax, ax
        retf

ende:                                   ; Ende des residenten Teils

strat:  mov     [cs:rh], bx
        mov     [cs:rh+2], es
        retf
intr:   push    ds
        push    es
        push    ax
        push    bx
        push    dx
        lds     bx, [cs:rh]
        cmp     byte [bx+2], 0          ; INIT?
        jne     .done
        mov     word [bx+0Eh], ende
        mov     [bx+10h], cs
        mov     ax, 4300h               ; schon ein XMS-Treiber da?
        int     2Fh
        cmp     al, 80h
        je      .done
        push    ds
        push    bx
        mov     ax, 352Fh
        int     21h
        mov     [cs:old2f], bx
        mov     [cs:old2f+2], es
        push    cs
        pop     ds
        mov     dx, int2f
        mov     ax, 252Fh
        int     21h
        mov     dx, msg
        mov     ah, 9
        int     21h
        pop     bx
        pop     ds
.done:  mov     word [bx+3], 0100h      ; fertig, kein Fehler
        pop     dx
        pop     bx
        pop     ax
        pop     es
        pop     ds
        retf
msg     db      "RBHMA: HMA fuer die Box (XMS 3.00, 64 KB)", 13, 10, "$"
