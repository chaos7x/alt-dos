; SPDX-License-Identifier: GPL-2.0-or-later
; ATDOSCM.SYS - eigener DOS-Kern fuer die DOS-Boxen von AT-DOS 6.3
; (docs/DOSKERN.md): Einsprung, Interrupt-Eingaenge, Stapel.
;
; Aufbau wie der AT-DOS-Kernel: Code (CGROUP) und Daten (DGROUP) getrennt,
; als DOS-EXE gelinkt, build.sh nimmt den Kopf ab. Der AT-DOS-Kernel legt
; die Datei an 0070:0000 der Box und startet sie dort (kernel.c load_cm);
; die ersten 16 Byte des Codes springen nach boot.
;
; DOS=HIGH wie MS-DOS 5: gibt der XMS-Treiber der Box die HMA her, kommt
; der Code nach FFFF:0010 (Code-Offsets beginnen deshalb bei 10h) und die
; Daten ruecken nach 0070:0000 - unten bleiben nur Daten und die kleinen
; Eingaenge (Segment LTEXT in DGROUP: alle Interruptvektoren, Geraete-
; koepfe, Laendertabelle zeigen dorthin). Sonst bleibt alles, wie es
; geladen wurde (Code ab 0070:0000, Daten dahinter).
;
; A20: beim Start eines Programms schaltet der Kern A20 aus (EXEPACK-
; Entpacker rechnen mit dem Umlauf an 1 MB, wie FreeDOS), der Eingang von
; INT 21h schaltet es wieder ein, wenn FFFF:0090 auf 0000:0080 umlaeuft.
;
; INT 21h: Register auf den Stapel des Aufrufers (Rahmen wie struct frame
; in cm.h), Wechsel auf den Kernstapel, int21_c(Rahmen) liefert den Rahmen,
; ueber den zurueckgekehrt wird - normal derselbe, bei EXEC ein neuer auf
; dem Stapel des Kindes, beim Programmende der des Elternprogramms.
; InDOS zaehlt die Tiefe; ein Aufruf aus dem Kern heraus (INT 24h, TSR in
; INT 28h) nimmt den zweiten Stapel.

cpu 8086

segment _TEXT    public class=CODE align=16 use16
segment _TEXTEND public class=CODE align=16 use16
segment DOSHDR   public class=DATA align=16 use16
segment LTEXT    public class=DATA align=16 use16
segment CONST    public class=DATA align=2 use16
segment CONST2   public class=DATA align=2 use16
segment _DATA    public class=DATA align=2 use16
segment _BSS     public class=BSS  align=2 use16
segment ZZEND    public class=BSS  align=16 use16
group CGROUP _TEXT _TEXTEND
group DGROUP DOSHDR LTEXT CONST CONST2 _DATA _BSS ZZEND

extern cm_main_
extern int21_c_
extern int2f12_c_
extern int25_c_
extern int26_c_
extern _sda
extern _old2f
extern _setver_dx
extern _xms_ent

segment _TEXT
..start:
global _start
_start:                                 ; Offset 0: wird nur unten ausgefuehrt
        jmp     near boot
        times   16 - ($ - $$) nop       ; ab 10h: in der HMA FFFF:0010

boot:   cli
        cld
        mov     bp, cs                  ; Ladesegment (0070h)
        mov     ax, text_end            ; Daten: Absatz hinter dem Code
        mov     cl, 4
        shr     ax, cl
        add     ax, bp
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, kstack_top
        mov     di, bss_start           ; BSS nullen (steht nicht in der Datei)
        mov     cx, kend
        sub     cx, di
        xor     al, al
        rep     stosb
        mov     [hent + 2], cs
        mov     [hent12 + 2], cs
        mov     [hent25 + 2], cs
        mov     [hent26 + 2], cs
        sti
        mov     ax, 4300h               ; XMS da?
        int     2Fh
        cmp     al, 80h
        jne     .low
        mov     ax, 4310h
        int     2Fh
        mov     [_xms_ent], bx
        mov     [_xms_ent + 2], es
        mov     ah, 01h                 ; HMA anfordern
        mov     dx, 0FFFFh
        call    far [_xms_ent]
        cmp     ax, 1
        jne     .low
        mov     ah, 05h                 ; A20 ein (lokal)
        call    far [_xms_ent]
        cmp     ax, 1
        jne     .nohma
        push    ds                      ; Code nach FFFF:0010
        mov     ds, bp
        mov     ax, 0FFFFh
        mov     es, ax
        mov     si, 16
        mov     di, si
        mov     cx, text_end
        sub     cx, si
        rep     movsb
        pop     ds
        mov     byte [_in_hma], 1
        mov     word [hent + 2], 0FFFFh
        mov     word [hent12 + 2], 0FFFFh
        mov     word [hent25 + 2], 0FFFFh
        mov     word [hent26 + 2], 0FFFFh
        mov     ax, 0FFFFh
        push    ax
        mov     ax, .hi
        push    ax
        retf
.hi:    cli                             ; jetzt in der HMA: Daten nach unten
        mov     es, bp
        xor     si, si
        xor     di, di
        mov     cx, kend
        rep     movsb
        mov     ds, bp
        mov     es, bp
        mov     ss, bp
        mov     sp, kstack_top
        sti
        jmp     short .low
.nohma: mov     ah, 02h                 ; HMA zurueck
        call    far [_xms_ent]
.low:   push    ds
        pop     es
        call    cm_main_
.hang:  hlt
        jmp     .hang

; Einspruenge aus den Eingaengen von INT 21h und INT 2Fh 12xxh (fern)
h21:    call    int21_c_
        retf
h12:    call    int2f12_c_
        retf
h25:    call    int25_c_
        retf
h26:    call    int26_c_
        retf

; ---- Kopf des Datensegments wie MS-DOS: Liste der Listen bei DS:0026h ----
; (Programme wie MEM holen DS ueber INT 2Fh 1203h und lesen dort direkt)
segment DOSHDR
        times   24h db 0
global _lol_area
_lol_area: times 82h db 0               ; LoL-2 (erster MCB) .. LoL+7Fh

; ---- Eingaenge im ersten MB (LTEXT, Teil von DGROUP: CS = DS) -----------
segment LTEXT
hent:   dw      h21, 0                  ; Code: CGROUP-Offset, Segment
hent12: dw      h12, 0
hent25: dw      h25, 0
hent26: dw      h26, 0
global _in_hma
_in_hma: db     0                       ; Code liegt in der HMA
global _a20_drop
_a20_drop: db   0                       ; nach dem Aufruf A20 aus (EXEC)
        align   2

; ---- INT 21h ---------------------------------------------------------------
; Rahmen wie SAVE_WORLD von MS-DOS (AX zuunterst, dann BX CX DX SI DI BP
; DS ES, darueber IP CS FLAGS) - INT 2Fh 1218h gibt ihn so heraus.
global int21_entry, int21_entry_, _int21_entry
_int21_entry:
int21_entry:
int21_entry_:                           ; ohne STI (Falle ohne VME); wer
                                        ; wartet, schaltet IF selbst ein
i21go:  push    es
        push    ds
        push    bp
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        mov     di, hent                ; Ziel im Code: int21_c
dosgo:  mov     ax, sp                  ; DX:AX = Rahmen
        mov     dx, ss
        mov     bx, cs
        mov     ds, bx
        mov     cx, kstack_top          ; ohne CLI/STI: in der Box ist jedes
        inc     byte [_sda + 1]         ; davon eine Falle (386 ohne VME);
        cmp     byte [_sda + 1], 1      ; nach MOV SS sperrt die CPU selbst
        je      .one                    ; Interrupts bis nach dem MOV SP
        mov     cx, kstack2_top
.one:   mov     ss, bx
        mov     sp, cx
        cld
        mov     es, bx
        cmp     byte [_in_hma], 0
        je      .call
        call    a20chk
.call:  call    far [di]                ; -> DX:AX Rahmen zurueck
        cmp     byte [_a20_drop], 0
        je      .ret
        mov     byte [_a20_drop], 0
        push    ax
        push    dx
        mov     ah, 06h                 ; A20 aus (lokal): Programmstart
        call    far [_xms_ent]
        pop     dx
        pop     ax
.ret:   mov     ss, dx                  ; erst zurueck auf den Stapel des
        mov     sp, ax                  ; Aufrufers (MOV SS sperrt bis nach
        dec     byte [_sda + 1]         ; MOV SP), dann InDOS (ein INT 21h
                                        ; dazwischen nimmt den 2. Stapel)
        pop     ax
        pop     bx
        pop     cx
        pop     dx
        pop     si
        pop     di
        pop     bp
        pop     ds
        pop     es
        iret

; INT 2Fh 12xxh (DOS-intern, fuer Redirectoren und TSRs): gleicher Weg
i2f12:  push    es
        push    ds
        push    bp
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        mov     di, hent12
        jmp     dosgo

; A20 an? FFFF:0090 (Code) gegen 0000:0080 (Vektor 20h) - gleich heisst
; Umlauf, dann ueber XMS lokal einschalten. Veraendert nur Flags.
a20chk: push    ax
        push    bx
        push    si
        push    es
        xor     si, si
        mov     es, si
        mov     ax, [es:80h]
        mov     bx, [es:82h]
        dec     si
        mov     es, si
        cmp     ax, [es:90h]
        jne     .ok
        cmp     bx, [es:92h]
        jne     .ok
        mov     ah, 05h
        call    far [_xms_ent]
.ok:    pop     es
        pop     si
        pop     bx
        pop     ax
        ret

; INT 20h = AH 00h, INT 27h = 31h mit DX Bytes -> Absaetze
global int20_entry, int20_entry_, _int20_entry
_int20_entry:
int20_entry:
int20_entry_:
        mov     ah, 0
        jmp     int21_entry
global int27_entry, int27_entry_, _int27_entry
_int27_entry:
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
global int22_entry, int22_entry_, _int22_entry
_int22_entry:
int22_entry:
int22_entry_:
        mov     ah, 4Dh
        int     21h
        mov     ah, 01h
        int     0E0h
.h:     hlt
        jmp     .h

; INT 23h (Vorgabe): Programm abbrechen
global int23_entry, int23_entry_, _int23_entry
_int23_entry:
int23_entry:
int23_entry_:
        mov     ax, 4C03h
        jmp     int21_entry

; INT 24h (Vorgabe): Fehlschlag melden
global int24_entry, int24_entry_, _int24_entry
_int24_entry:
int24_entry:
int24_entry_:
        mov     al, 3
        iret

; INT 25h/26h (absolut lesen/schreiben): wie INT 21h, aber Ruecksprung mit
; RETF - die Flags des Aufrufers bleiben auf dem Stapel. Dafuer den
; IRET-Rahmen um ein Wort nach unten kopieren (IP CS FL FL).
global int25_entry, int25_entry_, _int25_entry
_int25_entry:
int25_entry:
int25_entry_:
        call    abs_frame
        push    es
        push    ds
        push    bp
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        mov     di, hent25
        jmp     dosgo
global int26_entry, int26_entry_, _int26_entry
_int26_entry:
int26_entry:
int26_entry_:
        call    abs_frame
        push    es
        push    ds
        push    bp
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        mov     di, hent26
        jmp     dosgo

; Stapel beim Aufruf: [Rueck] IP CS FL -> [Rueck] IP CS FL FL
abs_frame:
        sti
        push    ax                      ; Platz
        push    bp
        mov     bp, sp                  ; [bp+2] Platz [bp+4] Rueck
        push    ax                      ; [bp+6] IP [bp+8] CS [bp+10] FL
        mov     ax, [bp + 4]
        mov     [bp + 2], ax            ; Ruecksprung nach unten
        mov     ax, [bp + 6]
        mov     [bp + 4], ax            ; IP
        mov     ax, [bp + 8]
        mov     [bp + 6], ax            ; CS
        mov     ax, [bp + 10]
        mov     [bp + 8], ax            ; FL (fuer IRET), [bp+10] bleibt
        pop     ax
        pop     bp
        ret

; Fernaufruf der Laendertabelle (Zeichen umsetzen): nichts
global retf_entry, retf_entry_, _retf_entry
_retf_entry:
retf_entry:
retf_entry_:
        retf

; INT 28h (Leerlauf) und INT 2Ah (Netz): nichts
global iret_entry, iret_entry_, _iret_entry
_iret_entry:
iret_entry:
iret_entry_:
        iret

; INT 29h: Zeichen AL schnell ausgeben
global int29_entry, int29_entry_, _int29_entry
_int29_entry:
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
global int2f_entry, int2f_entry_, _int2f_entry
_int2f_entry:
int2f_entry:
int2f_entry_:
        cmp     ax, 1000h
        je      .share
        cmp     ah, 12h                 ; DOS-intern (122Fh: Version)
        jne     .n12
        cmp     al, 2Fh
        jne     i2f12
.n12:
        cmp     ax, 122Fh
        je      .ver
        cmp     ax, 4A01h               ; freie HMA (DOS=HIGH wie MS-DOS 5)
        je      .hma
        cmp     ah, 15h                 ; MSCDEX: Kernel (INT E0h 20h,
        je      .cd                     ; AL = Unterfunktion), wie BOXSHELL
        jmp     far [cs:_old2f]
.cd:    push    ax
        mov     ah, 20h
        int     0E0h                    ; ZF=1 erledigt, CF Fehler
        jnz     .cdn
        inc     sp                      ; AX ist das Ergebnis; INC laesst
        inc     sp                      ; CF stehen
        pushf
        cmp     ah, 20h                 ; AX nicht gesetzt: wie vorher
        jne     .cdr
        mov     ah, 15h
.cdr:   popf
        sti
        retf    2
.cdn:   pop     ax
        jmp     far [cs:_old2f]
.hma:   xor     bx, bx
        cmp     byte [cs:_in_hma], 0
        je      .h0
        mov     bx, 0FFFFh
        mov     es, bx
        mov     di, text_end            ; hinter dem Code
        mov     bx, 0                   ; bis zum Ende: 10000h - text_end
        sub     bx, di
        iret
.h0:    mov     di, 0FFFFh              ; keine HMA: ES:DI = FFFF:FFFF
        mov     es, di
        iret
.share: mov     al, 0FFh
        iret
.ver:   mov     [cs:_setver_dx], dx
        iret

;
; Strg+C (exec.c cc_frame): Register wie beim INT-21h-Aufruf, darunter der
; IRET-Rahmen des Aufrufers. INT 23h; IRET oder RETF mit CF = 0 -> Aufruf
; wiederholen (Ergebnis-Flags behalten: RETF 2), CF = 1 -> Programm beenden.
global ctrlc_stub, ctrlc_stub_, _ctrlc_stub
_ctrlc_stub:
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

; ---- Hilfen fuer C (im Code; DS = SS = Daten) ------------------------------
segment _TEXT
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
.go:    mov     [rptr], dx
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
        push    ds                      ; Flags ueber LAHF statt PUSHF -
        push    bx                      ; PUSHF ist in der Box ohne VME
        push    ax                      ; eine Falle; gebraucht werden
        lahf                            ; nur CF und ZF (Bits 0-7)
        mov     bx, ss
        mov     ds, bx
        mov     bx, [rptr]
        mov     [bx + 18], ah
        mov     byte [bx + 19], 0
        pop     word [bx + 0]
        pop     word [bx + 2]
        mov     [bx + 4], cx
        mov     [bx + 6], dx
        mov     [bx + 8], si
        mov     [bx + 10], di
        mov     [bx + 12], bp
        pop     word [bx + 14]
        mov     [bx + 16], es
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret

; void start_frame(u16 ss, u16 sp) - AX = SS, DX = SP: in einen Rahmen
; springen (erstes Programm), kehrt nicht zurueck
global start_frame_
start_frame_:
        cli
        mov     ss, ax
        mov     sp, dx
        pop     ax
        pop     bx
        pop     cx
        pop     dx
        pop     si
        pop     di
        pop     bp
        pop     ds
        pop     es
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
        call    far [_xms_ent]
        mov     [si], bx
        mov     [di], dx
        pop     es
        pop     bp
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret

; void devcall(u16 seg, u16 strat, u16 intr, u16 rq) - AX, DX, BX, CX:
; Treiber aufrufen wie DOS (ES:BX = Anforderung im Datensegment)
global devcall_
devcall_:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     [cs:dv_seg], ax
        mov     [cs:dv_seg2], ax
        mov     [cs:dv_st], dx
        mov     [cs:dv_in], bx
        push    ds
        pop     es
        mov     bx, cx
        push    es
        push    bx
        call    far [cs:dv_st]
        pop     bx
        pop     es
        call    far [cs:dv_in]
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
dv_st:  dw      0
dv_seg: dw      0
dv_in:  dw      0
dv_seg2: dw     0

; unsigned crit24(ax, di, bp, si) - AX, DX, BX, CX: INT 24h auf dem
; Stapel des Programms (SS:SP = _user_frame, Rahmen wie SAVE_WORLD) -> AX
extern _user_frame
global crit24_
crit24_:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     [cs:c_ss], ss
        mov     [cs:c_sp], sp
        mov     di, dx
        mov     si, cx
        mov     bp, bx
        les     bx, [_user_frame]
        cli
        mov     dx, es
        mov     ss, dx
        mov     sp, bx
        sti
        int     24h
        cli
        mov     ss, [cs:c_ss]
        mov     sp, [cs:c_sp]
        sti
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        xor     ah, ah
        ret
c_ss:   dw      0
c_sp:   dw      0

; unsigned getds(void) - Segment der Daten (und der Eingaenge)
global getds_
getds_: mov     ax, ds
        ret

segment _TEXTEND                        ; Ende des Codes (auf 16 gerundet)
text_end:

segment _BSS
bss_start:
rptr:   resw    1
        resb    3072
kstack_top:
        resb    1024
kstack2_top:

segment ZZEND
kend:
global _cm_end
_cm_end:
