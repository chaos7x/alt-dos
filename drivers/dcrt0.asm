; SPDX-License-Identifier: GPL-2.0-or-later
; Startcode fuer Geraetetreiber (Ring 0)
;
; Das Abbild beginnt mit struct drv_header. Der Kernel ruft die
; Strategieroutine fern auf: DS = Treiberdaten, ES:BX = Anforderung,
; SS = Kernelstack (deshalb baut Watcom die Treiber mit -zu: SS != DS).

cpu 386

segment _TEXT  public class=CODE align=1 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
segment ITEXT  public class=ICODE align=1 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS ITEXT

; ITEXT (Klasse ICODE, hinter dem BSS): Code, der nur beim INIT laeuft
; (#pragma code_seg("ITEXT", "ICODE") im Treiber). Nach dem INIT meldet
; drv_entry seinen Anfang in buf_off (DRV_KEEP), der Kernel kuerzt den
; Treiber dort und gibt die Seiten dahinter frei (docs/INIT-SEGMENT.md).
segment ITEXT
global _init_start
_init_start:

extern strategy_

segment _TEXT
..start:
header:
        db      "DRV1"
        dw      drv_entry

drv_entry:
        mov     [_req_sel], es
        mov     [_req_off], bx
        push    es
        push    bx
        call    strategy_
        pop     bx
        pop     es
        cmp     byte [es:bx], 0         ; DRV_INIT: Grenze melden
        jne     .r
        mov     word [es:bx + 12], _init_start  ; drv_req.buf_off = keep
.r:     retf

; unsigned devhlp(unsigned fn, unsigned dx, unsigned bx, unsigned cx)
global devhlp_
devhlp_:
        call    far [_devhlp]
        ret

; Interrupt: der Kernel ruft hier fern an (DS = Treiberdaten, AX = IRQ,
; Interrupts gesperrt) -> Routine aus irq_fn (set_irq in drvlib.h)
global irq_entry_
irq_entry_:
        call    [_irq_fn]
        retf

; Ferne Einspruenge fuer Netzwerk (include/net.h): der Kernel ruft mit
; DS = Treiberdaten, ES:BX = Struktur -> Routine aus fent_fn[n] bekommt
; ES:BX als fernen Zeiger (DX:AX), Rueckgabe in AX (netlib.h)
%macro FENT 1
global fent%1_
fent%1_:
        mov     ax, bx
        mov     dx, es
        call    [_fent_fn + 2 * %1]
        retf
%endmacro
FENT 0
FENT 1
FENT 2
FENT 3
FENT 4

segment _BSS
global _req_sel, _fent_fn, _req_off, _devhlp, _irq_fn
_req_sel:   resw 1
_req_off:   resw 1
_devhlp:    resd 1
_irq_fn:    resw 1
_fent_fn:   resw 5
