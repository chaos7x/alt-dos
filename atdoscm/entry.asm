; SPDX-License-Identifier: GPL-2.0-or-later
; ATDOSCM.SYS - eigener DOS-Kern fuer die DOS-Boxen von AT-DOS 6.3
; (docs/DOSKERN.md, Stufe 2): Einsprung, Interrupt-Eingaenge, Stapel.
;
; Der AT-DOS-Kernel legt die Datei an 0060:0000 der Box und startet sie mit
; CS = DS = ES = SS = 0060h (kernel.c load_cm). Modell tiny: Code und Daten
; in einer Gruppe, Offsets ab Dateianfang.
;
; INT 21h: Register auf den Stapel des Aufrufers (Rahmen wie struct frame
; in cm.h), Wechsel auf den Kernstapel, int21_c(Rahmen) liefert den Rahmen,
; ueber den zurueckgekehrt wird - normal derselbe, bei EXEC ein neuer auf
; dem Stapel des Kindes, beim Programmende der des Elternprogramms.
; InDOS zaehlt die Tiefe; ein Aufruf aus dem Kern heraus (INT 24h, TSR in
; INT 28h) nimmt den zweiten Stapel.

cpu 8086

segment _TEXT  public class=CODE align=1 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
segment ZZEND  public class=BSS  align=16 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS ZZEND

extern cm_main_
extern int21_c_
extern _sda
extern _old2f
extern _setver_dx

segment _TEXT
..start:
global _start
_start:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0FFFEh              ; vorlaeufig ueber dem Kern
        cld
        mov     di, bss_start           ; BSS nullen (steht nicht in der Datei)
        mov     cx, kend
        sub     cx, di
        xor     al, al
        rep     stosb
        mov     sp, kstack_top
        sti
        call    cm_main_
.hang:  hlt
        jmp     .hang

; ---- INT 21h ---------------------------------------------------------------
global int21_entry, int21_entry_
int21_entry:
int21_entry_:
        sti
i21go:  push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     ax, sp                  ; DX:AX = Rahmen
        mov     dx, ss
        mov     bx, cs
        mov     ds, bx
        cli
        inc     byte [_sda + 1]         ; InDOS
        cmp     byte [_sda + 1], 1
        mov     ss, bx
        mov     sp, kstack_top
        je      .one
        mov     sp, kstack2_top
.one:   sti
        cld
        mov     es, bx
        call    int21_c_                ; -> DX:AX Rahmen fuer den Ruecksprung
        cli
        dec     byte [_sda + 1]
        mov     ss, dx
        mov     sp, ax
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        iret

; INT 20h = AH 00h, INT 27h = 31h mit DX Bytes -> Absaetze
global int20_entry, int20_entry_
int20_entry:
int20_entry_:
        mov     ah, 0
        jmp     int21_entry
global int27_entry, int27_entry_
int27_entry:
int27_entry_:
        add     dx, 15
        rcr     dx, 1                   ; Uebertrag mitnehmen
        shr     dx, 1
        shr     dx, 1
        shr     dx, 1
        mov     ax, 3100h
        jmp     int21_entry

; INT 22h (Vorgabe): Ende des ersten Prozesses - die Box ist fertig
global int22_entry, int22_entry_
int22_entry:
int22_entry_:
        mov     ah, 4Dh
        int     21h
        mov     ah, 01h
        int     0E0h
.h:     hlt
        jmp     .h

; INT 23h (Vorgabe): Programm abbrechen
global int23_entry, int23_entry_
int23_entry:
int23_entry_:
        mov     ax, 4C03h
        jmp     int21_entry

; INT 24h (Vorgabe): Fehlschlag melden
global int24_entry, int24_entry_
int24_entry:
int24_entry_:
        mov     al, 3
        iret

; INT 25h/26h: kommen in Stufe 4 (wie MS-DOS 7) - bis dahin Fehler
global int25_entry, int25_entry_
int25_entry:
int25_entry_:
        mov     ax, 8002h
        stc
        retf

; Fernaufruf der Laendertabelle (Zeichen umsetzen): nichts
global retf_entry, retf_entry_
retf_entry:
retf_entry_:
        retf

; INT 28h (Leerlauf) und INT 2Ah (Netz): nichts
global iret_entry, iret_entry_
iret_entry:
iret_entry_:
        iret

; INT 29h: Zeichen AL schnell ausgeben
global int29_entry, int29_entry_
int29_entry:
int29_entry_:
        push    ax
        push    bx
        mov     ah, 0Eh
        mov     bx, 7
        int     10h
        pop     bx
        pop     ax
        iret

; INT 2Fh: SHARE da (1000h), Version fuer das naechste Programm (122Fh),
; sonst weiter an den alten Vektor (Stummel des Monitors: XMS, DPMI ...)
global int2f_entry, int2f_entry_
int2f_entry:
int2f_entry_:
        cmp     ax, 1000h
        je      .share
        cmp     ax, 122Fh
        je      .ver
        jmp     far [cs:_old2f]
.share: mov     al, 0FFh
        iret
.ver:   mov     [cs:_setver_dx], dx
        iret

;
; Strg+C (exec.c cc_frame): Register wie beim INT-21h-Aufruf, darunter der
; IRET-Rahmen des Aufrufers. INT 23h; IRET oder RETF mit CF = 0 -> Aufruf
; wiederholen (Ergebnis-Flags behalten: RETF 2), CF = 1 -> Programm beenden.
global ctrlc_stub, ctrlc_stub_
ctrlc_stub:
ctrlc_stub_:
        mov     [cs:cc_sp], sp
        clc
        int     23h
        jc      .abort
        cmp     sp, [cs:cc_sp]
        je      .again
        add     sp, 2
.again: int     21h
        retf    2
.abort: cmp     sp, [cs:cc_sp]
        je      .a2
        add     sp, 2
.a2:    mov     ax, 4C00h
        int     21h
cc_sp:  dw      0

; ---- Hilfen fuer C ---------------------------------------------------------
; void bint(unsigned n, struct rregs *r) - AX = n, DX = r
; struct rregs: ax bx cx dx si di bp ds es fl
global bint_
bint_:
        push    bx
        push    cx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     [cs:.n + 1], al         ; Nummer eintragen (Code ist schreibbar)
        jmp     short .go               ; Vorabholung leeren
.go:    mov     [cs:rptr], dx
        mov     bx, dx
        mov     ax, [bx + 0]
        mov     cx, [bx + 4]
        mov     dx, [bx + 6]
        mov     si, [bx + 8]
        mov     di, [bx + 10]
        mov     bp, [bx + 12]
        push    word [bx + 14]          ; DS
        mov     es, [bx + 16]
        mov     bx, [bx + 2]
        pop     ds
.n:     int     0                       ; (Nummer wird eingetragen)
        pushf
        push    ds
        push    bx
        mov     bx, cs
        mov     ds, bx
        mov     bx, [rptr]
        mov     [bx + 0], ax
        pop     word [bx + 2]
        mov     [bx + 4], cx
        mov     [bx + 6], dx
        mov     [bx + 8], si
        mov     [bx + 10], di
        mov     [bx + 12], bp
        pop     word [bx + 14]
        mov     [bx + 16], es
        pop     word [bx + 18]
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret
rptr:   dw      0

; void start_frame(u16 ss, u16 sp) - AX = SS, DX = SP: in einen Rahmen
; springen (erstes Programm), kehrt nicht zurueck
global start_frame_
start_frame_:
        cli
        mov     ss, ax
        mov     sp, dx
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        iret

; u16 xmscall(u16 ax, u16 dx, u16 *bx, u16 *dxo) - XMS-Treiber (_xms_ent)
global xmscall_
extern _xms_ent
xmscall_:
        push    bx
        push    cx
        push    si
        push    di
        push    bp
        push    es
        mov     si, bx
        mov     di, cx
        call    far [cs:_xms_ent]
        mov     [si], bx
        mov     [di], dx
        pop     es
        pop     bp
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret

; unsigned getcs(void)
global getcs_
getcs_: mov     ax, cs
        ret

segment _BSS
bss_start:
        resb    3072
kstack_top:
        resb    1024
kstack2_top:

segment ZZEND
kend:
global _cm_end
_cm_end:
