; SPDX-License-Identifier: GPL-2.0-or-later
; RBHMA.SYS - XMS-Treiber nur fuer die HMA, im Startabbild der Box von
; AT-DOS 5.1 (RBOX)
;
; Die Box laeuft im echten Real Mode. 100000h-10FFEFh (HMA) haelt der
; Kernel fuer sie frei (kernel.c, KMOVE_BASE = 110000h), A20 ist in der
; Box immer offen (der Stummel in start.asm schaltet sie bei jedem
; Ausflug ein, INT 15h/87h lehnt er ab). Mit DOS=HIGH zieht FreeDOS in
; die HMA (fragt nach jeder DEVICE=-Zeile).
;
; XMS-Bloecke (08h-0Fh) haelt der Kernel von AT-DOS im erweiterten
; Speicher (RBXMS= in der CONFIG.SYS, Vorgabe 1 MB): jeder Aufruf geht
; ueber INT E0h AH = 20h-25h an den Kernel (ein Ausflug). Der 286 kann
; die Bloecke nicht einblenden, Programme kopieren nur (0Bh) - genau so
; lagert sich FreeCOM aus (XMS_Swap: ~70 KB mehr in der Box).
;
; A20 an/aus (03h-07h) wie HIMEM ueber den 8042 (global + Zaehler fuer
; lokal). FreeDOS schaltet A20 bei jedem EXEC per 06h ab (gepackte
; Programme: EXEPACK meldet sonst unter 64 KB "Packed file is corrupt"
; oder haengt) und beim naechsten DOS-Aufruf per 05h wieder an. Der
; Stummel (start.asm) merkt beim Ausflug, ob A20 aus war, und stellt das
; beim Eintritt wieder her.
cpu 8086
org 0
        dd      -1                      ; letzter Treiber der Datei
        dw      8000h                   ; Zeichengeraet
        dw      strat, intr
        db      "XMSXXXX0"              ; Name wie HIMEM.SYS
rh      dd      0
old2f   dd      0
hma     db      0                       ; HMA vergeben?
glob    db      0                       ; 03h: global an
loc     dw      0                       ; 05h/06h: Zaehler

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
        mov     bx, 0200h               ; Treiberversion
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
.f3:    cmp     ah, 7                   ; 03h-07h: A20
        ja      .f8
        cmp     ah, 3
        jb      .no
        call    a20fn
        retf
.ok:    mov     ax, 1
        xor     bl, bl
        retf
.f8:    cmp     ah, 0Dh                 ; entsperren: nichts zu tun
        je      .ok
        cmp     ah, 0Fh                 ; Groesse aendern: nicht
        je      .no
        cmp     ah, 8                   ; 08h-0Eh: an den Kernel
        jb      .no
        cmp     ah, 0Eh
        ja      .no
        mov     al, ah                  ; 08h->20h, 09h->21h, 0Ah->22h,
                                        ; 0Bh->23h, 0Ch->25h, 0Eh->24h
        sub     al, 8
        cmp     al, 4
        jne     .t2
        mov     al, 5
        jmp     short .t3
.t2:    cmp     al, 6
        jne     .t3
        mov     al, 4
.t3:    add     al, 20h
        mov     ah, al
        int     0E0h                    ; AX/BX/DX vom Kernel, Rest bleibt
        retf
.no:    mov     bl, 80h                 ; nicht vorhanden
.fail:  xor     ax, ax
        retf

; 03h global an, 04h global aus, 05h lokal an, 06h lokal aus, 07h abfragen
a20fn:  cmp     ah, 7
        jne     .s
        call    a20q
        xor     ah, ah
        xor     bl, bl
        ret
.s:     cmp     ah, 3
        jne     .s4
        mov     byte [cs:glob], 1
        jmp     short .on
.s4:    cmp     ah, 4
        jne     .s5
        mov     byte [cs:glob], 0
        jmp     short .chk
.s5:    cmp     ah, 5
        jne     .s6
        inc     word [cs:loc]
.on:    mov     al, 0xDF
        call    kbc
        jmp     short .ok
.s6:    cmp     word [cs:loc], 0
        je      .chk
        dec     word [cs:loc]
.chk:   cmp     word [cs:loc], 0        ; aus nur ohne Zaehler und global
        jne     .ok
        cmp     byte [cs:glob], 0
        jne     .ok
        mov     al, 0xDD
        call    kbc
.ok:    mov     ax, 1
        xor     bl, bl
        ret

; AL = Ausgangsport des 8042 (DFh A20 an, DDh aus), nur wenn noetig
kbc:    push    ax
        call    a20q
        mov     ah, al                  ; AH = 1: an
        pop     bx                      ; BL = gewuenscht
        push    bx
        cmp     bl, 0xDF
        jne     .off
        test    ah, ah
        jnz     .r
        jmp     short .w
.off:   test    ah, ah
        jz      .r
.w:     pushf
        cli
        call    .ibf
        mov     al, 0xD1
        out     0x64, al
        call    .ibf
        mov     al, bl
        out     0x60, al
        call    .ibf
        popf
.r:     pop     ax
        ret
.ibf:   push    cx
        xor     cx, cx
.ib1:   in      al, 0x64
        test    al, 2
        loopnz  .ib1
        pop     cx
        ret

; AX = 1, wenn A20 an ist (0000:0500 und FFFF:0510 verschiedene Zellen)
a20q:   push    ds
        push    es
        push    bx
        push    cx
        pushf
        cli
        xor     bx, bx
        mov     ds, bx
        dec     bx
        mov     es, bx
        mov     bl, [0x500]
        mov     bh, [es:0x510]
        mov     byte [0x500], 0
        mov     byte [es:0x510], 0xFF
        mov     cl, [0x500]
        mov     [es:0x510], bh
        mov     [0x500], bl
        popf
        xor     ax, ax
        cmp     cl, 0xFF
        je      .x
        inc     ax
.x:     pop     cx
        pop     bx
        pop     es
        pop     ds
        ret

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
msg     db      "RBHMA: HMA und XMS fuer die Box (XMS 3.00)", 13, 10, "$"
