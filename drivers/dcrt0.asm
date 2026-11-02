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
group DGROUP _TEXT CONST CONST2 _DATA _BSS

extern strategy_

segment _TEXT
..start:
header:
        db      "DRV1"
        dw      drv_entry

drv_entry:
        mov     [_req_sel], es
        mov     [_req_off], bx
        call    strategy_
        retf

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

segment _BSS
global _req_sel, _req_off, _devhlp, _irq_fn
_req_sel:   resw 1
_req_off:   resw 1
_devhlp:    resd 1
_irq_fn:    resw 1
