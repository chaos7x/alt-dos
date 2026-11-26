; SPDX-License-Identifier: GPL-2.0-or-later
; GUI.EXE - Startcode und Hilfen in Assembler (NE, Watcom `format os2`,
; Speichermodell medium: Code fern, Daten nah in DGROUP).
;
; - Gate-Zeiger bei DS:0002 wie apps/crt0.asm (fapi.h ruft `call far [2]`)
; - tsw: kooperativer Wechsel zwischen den Win16-Tasks (je eigener Stapel)
; - tstart: erster Einsprung eines Win16-Programms (Register wie Windows)
; - int_entry: INT n aus einem Win16-Programm (FAPI_SEGCTL SEG_INTHND)
; - call16: Rueckruf in ein Win16-Programm (Fensterprozedur) mit dessen DS
; - fapi_sel: FAPI-Aufruf mit DS = beliebigem Selektor (Lesen/Schreiben
;   direkt in Segmente der Programme)

cpu 286

segment BEGDATA public class=BEGDATA align=16 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
segment STACK  stack  class=STACK align=16 use16
group DGROUP BEGDATA _DATA _BSS STACK
segment GCRT_TEXT public class=CODE align=1 use16

extern gui_main_, dos_int_

segment BEGDATA
        dw      0
global _fapi_gate
_fapi_gate:
        dw      0x0000, 0x0023

segment _DATA
global _ts_ax, _ts_bx, _ts_cx, _ts_dx, _ts_si, _ts_di, _ts_es, _ts_ds, _ts_ip, _ts_cs
_ts_ax: dw 0
_ts_bx: dw 0
_ts_cx: dw 0
_ts_dx: dw 0
_ts_si: dw 0
_ts_di: dw 0
_ts_es: dw 0
_ts_ds: dw 0
_ts_ip: dw 0
_ts_cs: dw 0

segment GCRT_TEXT
..start:
global _start
_start:
        call    far gui_main_
        xor     bx, bx
        mov     ax, 3                   ; FAPI_EXIT
        call    far [0x0002]
.hang:  jmp     .hang

; void tsw(unsigned __far *save, unsigned ss, unsigned sp)  (cdecl, fern)
; sichert SS:SP des Laufenden nach save[0..1], laedt den neuen Stapel
global tsw_
global _tsw
_tsw:
tsw_:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    ds
        les     bx, [bp+6]              ; save
        mov     ax, [bp+10]             ; neues SS
        mov     dx, [bp+12]             ; neues SP
        mov     [es:bx], ss
        mov     [es:bx+2], sp
        mov     ss, ax                  ; sperrt Interrupts fuer den
        mov     sp, dx                  ; naechsten Befehl (kein CLI im Ring 3)
        pop     ds
        pop     di
        pop     si
        pop     bp
        retf

; Stapelrahmen fuer den ersten Wechsel in einen neuen Task:
; tsw nimmt ds, di, si, bp und kehrt per RETF nach tstart zurueck.
global _tstart
_tstart:
        push    word [_ts_cs]
        push    word [_ts_ip]
        mov     bx, [_ts_bx]
        mov     cx, [_ts_cx]
        mov     dx, [_ts_dx]
        mov     si, [_ts_si]
        mov     di, [_ts_di]
        mov     es, [_ts_es]
        push    word [_ts_ds]
        mov     ax, [_ts_ax]
        xor     bp, bp
        pop     ds
        retf

; INT n aus einem Win16-Programm. Stapel: n, IP, CS, FLAGS (Kernel).
; Rahmen fuer C (struct iregs, von unten): di si dx cx bx ax es ds bp n ip cs fl
global _int_entry
_int_entry:
        push    bp
        push    ds
        push    es
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     bp, sp
        mov     ax, DGROUP
        mov     ds, ax
        mov     ax, bp
        mov     dx, ss
        call    far dos_int_            ; (struct iregs __far *) in DX:AX
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        pop     es
        pop     ds
        pop     bp
        add     sp, 2                   ; n
        iret

; long call16(void __far *fn, unsigned ds, unsigned nw, unsigned *w)  cdecl
; legt nw Worte aus w (nah, in DGROUP) der Reihe nach auf den Stapel
; (Pascal: erstes Argument zuerst), DS = ds, ruft fn; -> DX:AX
global _call16
_call16:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    ds
        push    es
        mov     cx, [bp+12]             ; nw
        mov     si, [bp+14]             ; w
.push:  jcxz    .go
        push    word [si]
        add     si, 2
        dec     cx
        jmp     .push
.go:    mov     ax, [bp+10]             ; DS des Programms
        push    cs                      ; Ruecksprung, darueber die
        push    word .ret               ; Argumente (Pascal)
        push    word [bp+8]
        push    word [bp+6]
        mov     es, ax
        mov     ds, ax
        retf                            ; -> fn
.ret:   pop     es
        pop     ds
        pop     di
        pop     si
        pop     bp
        retf

; unsigned call_lib(void __far *fn, unsigned ds, unsigned heap)  cdecl
; LibEntry einer Win16-DLL wie Windows: DS = DI = hInstance, CX = Halde,
; ES:SI = Befehlszeile (hier 0) -> AX (0 = gescheitert)
global _call_lib
_call_lib:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    ds
        push    es
        mov     ax, [bp+10]
        mov     cx, [bp+12]
        mov     di, ax
        xor     si, si
        mov     es, si
        push    cs
        push    word .ret
        push    word [bp+8]
        push    word [bp+6]
        mov     ds, ax
        retf
.ret:   pop     es
        pop     ds
        pop     di
        pop     si
        pop     bp
        retf

; unsigned fapi_sel(fn, sel, a1, a2, a3)  cdecl: FAPI mit DS = sel
global _fapi_sel
_fapi_sel:
        push    bp
        mov     bp, sp
        push    ds
        push    bx
        push    cx
        push    dx
        mov     ax, [bp+6]
        mov     bx, [bp+10]
        mov     cx, [bp+12]
        mov     dx, [bp+14]
        mov     ds, [bp+8]
        call    0x0023:0x0000
        pop     dx
        pop     cx
        pop     bx
        pop     ds
        pop     bp
        retf

; Fehlende Win16-Funktion: Programm anhalten (gui/w16ldr.c merkt sich
; Modul/Ordinalzahl vor dem Sprung hierher nicht - nur Abbruch)
extern w16_missing_
global _w16_stub
_w16_stub:
        mov     ax, DGROUP
        mov     ds, ax
        call    far w16_missing_
.h:     jmp     .h

; InitTask (KERNEL.91): Register-Schnittstelle wie Windows
extern init_task_c_
global _InitTask
_InitTask:
        push    ds
        mov     ax, DGROUP
        mov     ds, ax
        call    far init_task_c_
        mov     bx, [_ts_bx]
        mov     cx, [_ts_cx]
        mov     dx, [_ts_dx]
        mov     si, [_ts_si]
        mov     di, [_ts_di]
        mov     es, [_ts_es]
        mov     ax, [_ts_ax]
        pop     ds
        retf

; DOS3Call (KERNEL.102): wie INT 21h
global _Dos3Call
_Dos3Call:
        int     0x21
        retf

; 32-Bit-Multiplikation und -Division fuer den Compiler (Watcom ruft sie
; im Modell medium fern): DX:AX * CX:BX -> DX:AX; DX:AX / CX:BX ->
; DX:AX Quotient, CX:BX Rest. Eigene Fassung statt der Watcom-Bibliothek.
global __U4M, __I4M, __U4D, __I4D
__U4M:
__I4M:
        push    si
        push    di
        mov     si, ax                  ; a_lo
        mov     di, dx                  ; a_hi
        mul     cx                      ; a_lo * b_hi
        xchg    ax, di                  ; di = Teil, ax = a_hi
        mul     bx                      ; a_hi * b_lo
        add     di, ax
        mov     ax, si
        mul     bx                      ; a_lo * b_lo
        add     dx, di
        pop     di
        pop     si
        retf

; vorzeichenlos: Schieben und Abziehen, 32 Runden
udiv32:                                 ; DX:AX / CX:BX (nah)
        push    si
        push    di
        push    bp
        xor     si, si                  ; Rest SI:DI
        xor     di, di
        mov     bp, 32
.l:     shl     ax, 1
        rcl     dx, 1
        rcl     di, 1
        rcl     si, 1
        cmp     si, cx
        jb      .n
        ja      .s
        cmp     di, bx
        jb      .n
.s:     sub     di, bx
        sbb     si, cx
        inc     ax
.n:     dec     bp
        jnz     .l
        mov     bx, di
        mov     cx, si
        pop     bp
        pop     di
        pop     si
        ret

__U4D:
        call    udiv32
        retf

__I4D:
        push    si
        xor     si, si                  ; Bit 0: Quotient negativ,
        or      dx, dx                  ; Bit 1: Rest negativ
        jns     .a
        neg     dx
        neg     ax
        sbb     dx, 0
        xor     si, 3
.a:     or      cx, cx
        jns     .b
        neg     cx
        neg     bx
        sbb     cx, 0
        xor     si, 1
.b:     call    udiv32
        test    si, 1
        jz      .c
        neg     dx
        neg     ax
        sbb     dx, 0
.c:     test    si, 2
        jz      .d
        neg     cx
        neg     bx
        sbb     cx, 0
.d:     pop     si
        retf

; Aufrufspur (SET GUITRACE=1): jeder Import geht ueber einen Vorschaltcode
; (w16ldr.c tr_thunk), der mit DS = GUI und AX = Kennung hierher ruft;
; die Kennung (Modul << 12 | Nummer) kommt in einen Ring von 128 Worten.
extern _tr_ring, _tr_pos, _tr_dseg, _cur_task, _w16_trace, tr_logall_, tr_logret_
segment GCRT_TEXT
; Stapel beim Eintritt: [sp] Ruecksprung in den Stummel, +4 ax, +6 ds
; des Stummels, +8/+10 Ruecksprung ins Programm. Ohne Bit 15 in der
; Kennung wird der Ruecksprung ins Programm auf tr_post umgebogen
; (Schattenstapel), das die Rueckgabe DX:AX vermerkt.
; Segment _tr_dseg: Rueckgaben AX bei 0, DX bei 256, gueltig bei 512
; (je 128 Worte), Tiefe je Task bei 768 (Platz s = cur_task + 1, 13
; Worte), Schattenstapel je Task bei 800 + s * 256 (32 x 8 Byte). Je Task,
; weil ein Aufruf in einen anderen Task wechseln kann (ShellExecute ->
; neues Programm, GetMessage) - ein gemeinsamer Stapel gab dann den
; Ruecksprung des falschen Tasks zurueck.
TR_RDX  equ 256
TR_ROK  equ 512
TR_SP   equ 768
TR_SHAD equ 800
global _tr_common
_tr_common:
        push    bx
        push    bp
        mov     bp, sp
        push    es
        push    si
        cmp     byte [_w16_trace], 2            ; GUITRACE=2: jeden Aufruf
        jne     .nolog                          ; gleich ins GUI.LOG
        push    ax
        push    cx
        push    dx
        push    di
        call    far tr_logall_
        pop     di
        pop     dx
        pop     cx
        pop     ax
.nolog:
        mov     bx, [_tr_pos]
        and     bx, 127
        shl     bx, 1
        mov     [_tr_ring+bx], ax
        mov     es, [_tr_dseg]
        mov     word [es:TR_ROK+bx], 0
        test    ax, 0x8000
        jnz     .nosh
        mov     si, [_cur_task]
        inc     si
        shl     si, 1                           ; 2s
        mov     bx, [es:TR_SP+si]
        cmp     bx, 32
        jae     .nosh
        inc     word [es:TR_SP+si]
        shl     bx, 3
        shl     si, 7                           ; 256 s
        add     bx, si
        mov     ax, [bp+12]
        mov     [es:TR_SHAD+bx], ax
        mov     ax, [bp+14]
        mov     [es:TR_SHAD+bx+2], ax
        mov     ax, [_tr_pos]
        mov     [es:TR_SHAD+bx+4], ax
        mov     word [bp+12], tr_post
        mov     [bp+14], cs
.nosh:  inc     word [_tr_pos]
        pop     si
        pop     es
        pop     bp
        pop     bx
        retf

tr_post:
        sub     sp, 4
        push    bp
        mov     bp, sp
        push    ds
        push    es
        push    bx
        push    cx
        push    si
        mov     bx, DGROUP
        mov     ds, bx
        mov     es, [_tr_dseg]
        mov     si, [_cur_task]
        inc     si
        shl     si, 1
        mov     bx, [es:TR_SP+si]
        dec     bx
        mov     [es:TR_SP+si], bx
        shl     bx, 3
        shl     si, 7
        add     bx, si
        mov     cx, [es:TR_SHAD+bx]
        mov     [bp+2], cx
        mov     cx, [es:TR_SHAD+bx+2]
        mov     [bp+4], cx
        mov     bx, [es:TR_SHAD+bx+4]
        and     bx, 127
        shl     bx, 1
        mov     [es:bx], ax
        mov     [es:TR_RDX+bx], dx
        mov     word [es:TR_ROK+bx], 1
        cmp     byte [_w16_trace], 2            ; GUITRACE=2: Rueckgabe
        jne     .norl                           ; auch ins GUI.LOG
        push    ax
        push    dx
        push    di
        call    far tr_logret_
        pop     di
        pop     dx
        pop     ax
.norl:  pop     si
        pop     cx
        pop     bx
        pop     es
        pop     ds
        pop     bp
        retf
