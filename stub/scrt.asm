; DOS-Stub fuer gebundene FAPI-Programme: Einstieg und Gate (Real Mode)
;
; Steht als MZ-Programm vor dem FX-Kopf (siehe tools/mkfx.py --bind). DOS
; laedt den ganzen Rumpf - Stub, FX-Kopf und Programmabbild - in den
; Speicher; der Stub legt das Abbild als eigenes 64-KB-Segment zurecht,
; traegt sein Gate in den Gate-Zeiger des Programms (Offset 2, siehe
; apps/crt0.asm) ein und springt hinein. Jeder FAPI-Aufruf landet dann in
; `gate` und wird in stub/fapidos.c in DOS- und BIOS-Aufrufe uebersetzt
; (wie die Family API von OS/2 mit BIND).
;
; Watcom-Registerkonvention: Argumente AX, DX, BX, CX.

cpu 8086

segment _TEXT  public class=CODE align=1 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS

extern stub_main_
extern fapi_dos_

FAPI_SIGRETURN  equ 0x21

segment _TEXT
..start:
global _start
_start:
        jmp     short init
global _fx_rel
_fx_rel dw      0                       ; Offset 2: FX-Kopf ab Stubanfang (mkfx)

init:   mov     ax, cs                  ; SS:SP setzt DOS schon auf das Ende
        mov     ds, ax                  ; des Stubs (MZ-Kopf, mkfx)
        mov     [_psp_seg], es
        mov     [stk_top], sp
        cld
        call    stub_main_
.hang:  jmp     .hang

; void run_app(unsigned cs, unsigned ds, unsigned sp, unsigned ip)
; - kehrt nicht zurueck (AX = CS, DX = DS = SS, BX = SP, CX = IP)
global run_app_
run_app_:
        mov     [_app_cs], ax
        cli
        mov     ss, dx
        mov     sp, bx
        sti
        mov     ds, dx
        mov     es, dx
        push    ax                      ; CS
        push    cx                      ; IP
        retf

; ---- Gate: call far aus dem Programm, AX = Funktion, BX/CX/DX ----------
global gate_entry_
gate_entry_:
        push    bp
        push    si
        push    di
        push    es
        push    ds
        push    bx
        push    cx
        push    dx
        mov     si, ds
        mov     di, cs
        mov     ds, di
        mov     [_app_ds], si
        mov     [app_ss], ss
        mov     [app_sp], sp
        cmp     ax, FAPI_SIGRETURN
        jne     .call
        cmp     byte [_in_sig], 0
        je      .call
        mov     byte [_in_sig], 0       ; zurueck in den unterbrochenen
        cli                             ; Aufruf: Rahmen und Ergebnis
        mov     ss, [sig_ss]
        mov     sp, [sig_sp]
        sti
        mov     ax, [sig_res]
        jmp     .out
.call:  cli
        mov     ss, di
        mov     sp, [stk_top]
        sti
        push    dx                      ; a3
        mov     dx, bx                  ; a1
        mov     bx, cx                  ; a2
        pop     cx                      ; a3
        call    fapi_dos_
        cli
        mov     ss, [app_ss]
        mov     sp, [app_sp]
        sti
        cmp     byte [_sig_go], 0       ; Signal zustellen?
        jne     .sig
.out:   pop     dx
        pop     cx
        pop     bx
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     bp
        retf
; wie der Kernel (sig_deliver): Rahmen und Ergebnis merken, in die Weiche
; des Programms springen (AX = Signal); DosSigReturn kehrt nach .out zurueck
.sig:   mov     byte [_sig_go], 0
        mov     byte [_in_sig], 1
        mov     [sig_res], ax
        mov     [sig_ss], ss
        mov     [sig_sp], sp
        mov     ax, [_sig_num]
        push    word [_app_cs]          ; Weiche im Codesegment
        push    word [_sig_tramp]
        mov     cx, [_app_ds]
        mov     ds, cx
        mov     es, cx
        retf

; ---- Interrupts --------------------------------------------------------
; INT 24h: kritischer Fehler (Diskette fehlt, schreibgeschuetzt): ohne
; Rueckfrage "Fehler" (AL = 3, ab DOS 3.1); DI = Fehlercode merken
global int24_
int24_:
        mov     [cs:_crit_err], di
        inc     byte [cs:_crit_hit]
        mov     al, 3
        iret

; INT 23h: Strg+C waehrend eines DOS-Aufrufs - nur merken, DOS macht weiter
global int23_
int23_:
        mov     byte [cs:_brk_flag], 1
        iret

; void do_int(unsigned char n, struct regs *r)  (AX = n, DX = r)
; Register aus r laden, INT n, Register und Flags zurueck nach r
global do_int_
do_int_:
        push    bx
        push    cx
        push    si
        push    di
        push    bp
        push    es
        push    ds
        mov     [cs:.n + 1], al         ; Befehl INT n umschreiben ...
        jmp     short .flush            ; ... und die Prefetch-Queue leeren
.flush: mov     [cs:r_ptr], dx
        mov     [cs:save_ss], ss
        mov     [cs:save_sp], sp
        mov     bp, dx
        mov     ax, [bp+0]
        mov     bx, [bp+2]
        mov     cx, [bp+4]
        mov     dx, [bp+6]
        mov     si, [bp+8]
        mov     di, [bp+10]
        mov     es, [bp+14]
        push    word [bp+12]
        pop     ds
.n:     int     0x21
        cli                             ; DOS 2.x (EXEC) vergisst SS:SP
        mov     ss, [cs:save_ss]
        mov     sp, [cs:save_sp]
        sti
        push    ds
        push    bp
        pushf
        mov     bp, cs
        mov     ds, bp
        mov     bp, [r_ptr]
        pop     word [bp+16]            ; Flags
        pop     word [bp+18]            ; BP (selten gebraucht)
        pop     word [bp+12]            ; DS
        mov     [bp+0], ax
        mov     [bp+2], bx
        mov     [bp+4], cx
        mov     [bp+6], dx
        mov     [bp+8], si
        mov     [bp+10], di
        mov     [bp+14], es
        pop     ds
        pop     es
        pop     bp
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret

; unsigned long ulmul(unsigned a, unsigned b) - 16 x 16 -> 32 Bit
global ulmul_
ulmul_:
        mul     dx
        ret

; unsigned get_cs(void)
global get_cs_
get_cs_:
        mov     ax, cs
        ret

; unsigned cpu_type(void) - 0 = 8086/8088, 2 = 286, 3 = 386 oder neuer
global cpu_type_
cpu_type_:
        pushf
        pushf
        pop     ax
        and     ax, 0x0FFF              ; Bits 12-15 loeschen
        push    ax
        popf
        pushf
        pop     ax
        and     ax, 0xF000
        cmp     ax, 0xF000              ; 8086: bleiben gesetzt
        je      .c86
        pushf
        pop     ax
        or      ax, 0x7000              ; 286 (Real Mode): bleiben 0
        push    ax
        popf
        pushf
        pop     ax
        popf
        test    ax, 0x7000
        mov     ax, 2
        jz      .r
        mov     ax, 3
.r:     ret
.c86:   popf
        xor     ax, ax
        ret

segment _DATA
global _psp_seg, _app_ds, _app_cs, _sig_go, _in_sig, _sig_num, _sig_tramp
global _brk_flag, _crit_err, _crit_hit
_psp_seg    dw 0
_app_ds     dw 0
_app_cs     dw 0
app_ss      dw 0
app_sp      dw 0
stk_top     dw 0
sig_ss      dw 0
sig_sp      dw 0
sig_res     dw 0
_sig_num    dw 0
_sig_tramp  dw 0
_sig_go     db 0
_in_sig     db 0
_brk_flag   db 0
_crit_hit   db 0
_crit_err   dw 0
r_ptr       dw 0
save_ss     dw 0
save_sp     dw 0
