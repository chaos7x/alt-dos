; SPDX-License-Identifier: GPL-2.0-or-later
; ATBOX.SYS - Geraetetreiber im Startabbild der DOS-Box (AT-DOS 6.0)
;
; EMMXXXX0: tut nichts als da zu sein. FreeDOS uebernimmt die UMBs
; (DOS=UMB) erst nach einer DEVICE=-Zeile (config.c: "might have been the
; UMB driver"). Die UMBs selbst liefert der Kernel ueber XMS (kernel/v86.c,
; wie VXMS bei OS/2) - ein EMM386 in der Box braucht es nicht. Der Name
; EMMXXXX0 ist der des EMS-Treibers: Programme oeffnen ihn, um EMS zu
; finden; INT 67h bedient ebenfalls der Kernel (wie VEMM bei OS/2).
;
; ATIN$, ATOUT$, ATERR$: Standard-Handles 0/1/2 des Box-Tasks im Kernel.
; Startet CMD ein DOS-Programm mit "prog > datei" (oder | , <), haengt
; BOXSHELL diese Geraete vor dem Start auf die Handles 0-2 des Programms
; (INT E0h 1Dh sagt, welche umgeleitet sind); gelesen und geschrieben wird
; ueber INT E0h 1Eh (AL = Handle, ES:DI Puffer, CX Bytes -> AX).
cpu 8086
org 0
        dw      d_in, -1                ; naechster Treiber der Datei
        dw      8000h                   ; Zeichengeraet
        dw      strat, intr
        db      "EMMXXXX0"              ; so suchen Programme den EMS-Treiber
d_in:   dw      d_out, -1
        dw      8000h
        dw      strat, intr0
        db      "ATIN$   "
d_out:  dw      d_err, -1
        dw      8000h
        dw      strat, intr1
        db      "ATOUT$  "
d_err:  dd      -1                      ; letzter Treiber der Datei
        dw      8000h
        dw      strat, intr2
        db      "ATERR$  "
rh      dd      0
strat:  mov     [cs:rh], bx
        mov     [cs:rh+2], es
        retf
intr0:  push    ax
        mov     al, 0
        jmp     short io
intr1:  push    ax
        mov     al, 1
        jmp     short io
intr2:  push    ax
        mov     al, 2
io:     push    ds
        push    bx
        push    cx
        push    di
        push    es
        lds     bx, [cs:rh]
        mov     ah, [bx+2]
        cmp     ah, 0                   ; INIT
        je      .init
        cmp     ah, 5                   ; zerstoerungsfrei lesen: nichts da
        je      .busy
        cmp     ah, 4                   ; lesen
        je      .xfer
        cmp     ah, 8                   ; schreiben
        je      .xfer
        cmp     ah, 9                   ; schreiben mit Pruefung
        jne     .done
.xfer:  mov     cx, [bx+12h]
        les     di, [bx+0Eh]
        mov     ah, 1Eh
        int     0E0h
        mov     [bx+12h], ax            ; uebertragen
        jmp     short .done
.busy:  mov     word [bx+3], 0300h      ; fertig + belegt (kein Zeichen)
        jmp     short .ret
.init:  mov     word [bx+0Eh], ende
        mov     [bx+10h], cs
.done:  mov     word [bx+3], 0100h      ; fertig, kein Fehler
.ret:   pop     es
        pop     di
        pop     cx
        pop     bx
        pop     ds
        pop     ax
        retf
intr:   push    ax
        push    ds
        push    bx
        push    cx
        push    di
        push    es
        lds     bx, [cs:rh]
        cmp     byte [bx+2], 0          ; INIT?
        je      io.init
        jmp     short io.done
ende:
