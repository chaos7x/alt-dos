; SPDX-License-Identifier: GPL-2.0-or-later
; ATBOX.SYS - Geraetetreiber im Startabbild der DOS-Box (AT-DOS 6.0)
;
; Tut nichts als da zu sein: FreeDOS uebernimmt die UMBs (DOS=UMB) erst
; nach einer DEVICE=-Zeile (config.c: "might have been the UMB driver").
; Die UMBs selbst liefert der Kernel ueber XMS (kernel/v86.c, wie VXMS
; bei OS/2) - ein EMM386 in der Box braucht es nicht. Der Geraetename
; EMMXXXX0 ist der des EMS-Treibers: Programme oeffnen ihn, um EMS zu
; finden; INT 67h bedient ebenfalls der Kernel (wie VEMM bei OS/2).
cpu 8086
org 0
        dd      -1                      ; letzter Treiber der Datei
        dw      8000h                   ; Zeichengeraet
        dw      strat, intr
        db      "EMMXXXX0"              ; so suchen Programme den EMS-Treiber
rh      dd      0
strat:  mov     [cs:rh], bx
        mov     [cs:rh+2], es
        retf
intr:   push    ds
        push    bx
        lds     bx, [cs:rh]
        cmp     byte [bx+2], 0          ; INIT?
        jne     .done
        mov     word [bx+0Eh], ende     ; Ende des residenten Teils
        mov     [bx+10h], cs
.done:  mov     word [bx+3], 0100h      ; fertig, kein Fehler
        pop     bx
        pop     ds
        retf
ende:
