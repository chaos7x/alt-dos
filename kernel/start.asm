; SPDX-License-Identifier: GPL-2.0-or-later
; AT-DOS 5.0 - Kernel-Einstieg und Low-Level-Routinen (80286)
;
; Der Kernel besteht aus zwei Segmenten wie ein Small-Modell-Programm:
; Code (CGROUP) ab linear 10000h, dahinter auf den naechsten Paragraphen
; ausgerichtet die Daten (DGROUP: CONST, _DATA, BSS mit Kernel-Stack).
; Beide duerfen je 64 KB gross werden. Im Real Mode ist CS=1000h und
; DS=SS=_kdata_seg, im Protected Mode zeigen KCODE und KDATA dorthin.
;
; Aufrufkonvention zu C: Watcom-Registerkonvention (AX, DX, BX, CX),
; aufgerufene Routinen erhalten alle Register ausser AX (bzw. DX:AX).

cpu 386

KCODE   equ 0x08
KDATA   equ 0x10

segment _TEXT  public class=CODE align=1 use16
segment _TEXTEND public class=CODE align=1 use16
; zweites Codesegment (KCODE2): selten gebrauchte Kernelteile, mit
; wcc -nt=KTEXT2 uebersetzt; Einspruenge __far, build.sh traegt die
; Selektoren der Fernaufrufe fest ein (Kernel liegt immer bei 10000h)
segment KTEXT2 public class=CODE align=16 use16
segment KTEXT2END public class=CODE align=1 use16
segment _DSTART public class=DATA align=16 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
segment _BSSEND public class=BSS align=2 use16
group CGROUP _TEXT _TEXTEND
group CGROUP2 KTEXT2 KTEXT2END
group DGROUP _DSTART CONST CONST2 _DATA _BSS _BSSEND

extern preload_, reboot_
extern _reboot_count
extern gdt_build_, kmain_, fapi_dispatch_, fault_, kbd_irq_, devhlp_dispatch_
extern fdc_isr_, preempt_
extern fpu_nm_
extern _gdtr, _idtr

; ---------------------------------------------------------------------------
segment _TEXT

..start:
global _start
_start:
        ; Real Mode, vom Bootsektor bei 1000:0000 angesprungen. AT-DOS 6.0
        ; braucht einen 386: pruefen mit 8086-Befehlen, bevor der erste
        ; 386-Befehl kommt (Flags-Bits 12-15: 8086 immer 1, 286 im Real
        ; Mode immer 0, 386 setzbar)
        pushf
        pop     ax
        and     ax, 0x0FFF
        push    ax
        popf
        pushf
        pop     ax
        and     ax, 0xF000
        cmp     ax, 0xF000
        je      .no386
        mov     ax, 0x7000
        push    ax
        popf
        pushf
        pop     ax
        test    ax, 0x7000
        jnz     .is386
.no386: push    cs                      ; nur hier im Real Mode: Text im
        pop     ds                      ; Codesegment lesen
        mov     si, .msg
.nput:  lodsb
        test    al, al
        jz      .nhalt
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .nput
.nhalt: hlt
        jmp     .nhalt
.msg    db 13, 10, "AT-DOS 6.0 braucht einen 80386 oder neuer.", 13, 10
          db "Auf dem 80286 l", 0x84, "uft AT-DOS 5.x.", 13, 10, 0
.is386:
        cli
        ; Datensegment: erster Paragraph hinter beiden Codesegmenten
        mov     ax, code_end
        add     ax, 15
        shr     ax, 4
        mov     bx, code2_end
        add     bx, 15
        shr     bx, 4
        add     ax, bx
        mov     bx, cs
        add     ax, bx
        mov     ds, ax
        mov     es, ax
        ; BSS selbst nullen - der Kernel-Stack liegt darin, also vor dem
        ; ersten PUSH. Der Bootsektor hat hier nur Reste geladen.
        mov     di, bss_start
        mov     cx, bss_end
        sub     cx, di
        xor     al, al
        cld
        rep     stosb
        mov     ax, ds
        mov     [_kdata_seg], ax
        mov     ss, ax
        mov     sp, kstack_top
        mov     [_boot_drive], dl

        ; Speicher abfragen, solange das BIOS noch erreichbar ist
        int     0x12                    ; konventionell in KB
        mov     [_mem_conv_kb], ax
        mov     ah, 0x88                ; erweitert ab 1 MB in KB
        int     0x15
        jnc     .ext_ok
        xor     ax, ax
.ext_ok:
        mov     [_mem_ext_kb], ax

        ; Speicherkarte (INT 15h E820h, bis 32 Eintraege zu 20 Byte):
        ; RAM mit Luecken, auch ueber 64 MB. Fehlt sie: E801h.
        push    es
        push    ds
        pop     es
        xor     ebx, ebx
        mov     di, _e820_map
.e820_next:
        mov     eax, 0xE820
        mov     edx, 0x534D4150         ; 'SMAP'
        mov     ecx, 20
        int     0x15
        jc      .e820_end
        cmp     eax, 0x534D4150
        jne     .e820_end
        cmp     cx, 20
        jb      .e820_skip
        add     di, 20
        inc     word [_e820_n]
        cmp     word [_e820_n], 32
        jae     .e820_end
.e820_skip:
        test    ebx, ebx
        jnz     .e820_next
.e820_end:
        pop     es
        cmp     word [_e820_n], 0
        jne     .e801_end
        mov     ax, 0xE801              ; AX/CX: KB 1-16 MB, BX/DX: 64-KB-
        xor     bx, bx                  ; Bloecke ueber 16 MB
        xor     cx, cx
        xor     dx, dx
        int     0x15
        jc      .e801_end
        test    ax, ax
        jnz     .e801_ax
        mov     ax, cx
        mov     bx, dx
.e801_ax:
        mov     [_e801_lo], ax
        mov     [_e801_hi], bx
.e801_end:

        ; Plattenparameter des BIOS fuer IDE.SYS sichern: Anzahl Platten
        ; (0040:0075) und die Tabellen hinter INT 41h/46h (je 16 Byte).
        ; MFM-Controller (WD1003) kennen kein IDENTIFY, die Geometrie gibt
        ; es nur hier. Die Tabellen koennen im RAM liegen, das der Kernel
        ; spaeter vergibt - deshalb jetzt kopieren.
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     al, [0x475]
        mov     [es:_bios_hdcount], al
        mov     di, _bios_fdpt
        lds     si, [0x41*4]
        mov     cx, 8
        rep     movsw
        xor     ax, ax
        mov     ds, ax
        lds     si, [0x46*4]
        mov     cx, 8
        rep     movsw
        pop     ds

        ; Geometrie der BIOS-Platten 80h-83h (INT 13h/08h) fuer FDISK: die
        ; CHS-Felder der Partitionstabelle muessen zur Uebersetzung des
        ; Controllers passen (Adaptec 64/32, IDE z. B. 16/63), sonst
        ; verwirft MS-DOS logische Laufwerke
        xor     cx, cx
        mov     cl, [_bios_hdcount]
        cmp     cl, 4
        jbe     .gok
        mov     cl, 4
.gok:   xor     bx, bx
.gloop: cmp     bx, cx
        jae     .gdone
        push    bx
        push    cx
        push    es
        mov     dl, bl
        or      dl, 0x80
        mov     ah, 0x08
        xor     di, di
        int     0x13
        pop     es
        jc      .gskip
        mov     si, cx                  ; CX der Antwort
        mov     al, dh                  ; Koepfe - 1
        pop     cx
        pop     bx
        push    bx
        push    cx
        mov     di, bx
        mov     [_bios_geo_h + di], al
        mov     ax, si
        and     al, 0x3F
        mov     [_bios_geo_s + di], al
        mov     ax, si
        xchg    al, ah                  ; AL = Zylinder unten, AH = CL
        shr     ah, 6                   ; Zylinder Bit 8-9
        inc     ax                      ; Anzahl = groesster + 1
        shl     di, 1
        mov     [_bios_geo_c + di], ax
.gskip: pop     cx
        pop     bx
        inc     bx
        jmp     .gloop
.gdone:

        ; VGA und Zeichensaetze fuer 43/50 Zeilen (MODE CON LINES=): Das
        ; BIOS sagt, ob es eine VGA ist (AX=1A00h -> AL=1Ah), und wo die
        ; ROM-Zeichensaetze liegen (AX=1130h: BH=3 8x8 0-127, BH=4 8x8
        ; 128-255, BH=6 8x16). Im Protected Mode gibt es kein BIOS mehr.
        push    bp
        push    es
        mov     ax, 0x1A00
        int     0x10
        cmp     al, 0x1A
        jne     .novga
        cmp     bl, 8                   ; aktive Anzeige: VGA Farbe
        jne     .novga
        mov     byte [_vga_ok], 1
        mov     bh, 3
        mov     di, _vga_font8a
        call    .font
        mov     bh, 4
        mov     di, _vga_font8b
        call    .font
        mov     bh, 6
        mov     di, _vga_font16
        call    .font
        jmp     .novga
.font:
        mov     ax, 0x1130
        push    di
        int     0x10                    ; -> ES:BP
        pop     di
        mov     [di], bp
        mov     [di+2], es
        ret
.novga:
        pop     es
        pop     bp

        ; Fuer MSD (DosSysInfo): Prozessor, Koprozessor, Ausstattungswort,
        ; Anschluesse aus dem BIOS-Datenbereich, BIOS-Datum und Modellbyte.
        ; 286 oder 386: im Real Mode haelt der 286 FLAGS Bit 12-15 auf 0
        ; (nur 286-Befehle; 386/486 unterscheiden ginge nur mit 32 Bit).
        pushf
        pushf
        pop     ax
        or      ax, 0x7000
        push    ax
        popf
        pushf
        pop     ax
        popf
        test    ax, 0x7000
        jz      .c286
        mov     byte [_cpu_386], 1
.c286:
        fninit                          ; 287/387 da? Statuswort 0
        mov     word [_fpu_ok], 0x5A5A
        fnstsw  [_fpu_ok]
        cmp     byte [_fpu_ok], 0
        mov     byte [_fpu_ok], 0
        mov     byte [_fpu_ok + 1], 0
        jne     .nofpu
        mov     byte [_fpu_ok], 1
        ; 287 oder 387-Kern (287XL, 387)? Der 287 rechnet nach FNINIT mit
        ; projektiver Unendlichkeit (+inf = -inf), der 387-Kern immer affin
        fld1
        fldz
        fdivp   st1, st0                ; 1/0 = +inf (Ausnahme maskiert)
        fld     st0
        fchs                            ; -inf
        fcompp
        fstsw   [_fpu_sw]
        fwait
        mov     ah, [_fpu_sw + 1]
        sahf
        je      .fpu287                 ; gleich: projektiv -> 287
        mov     byte [_fpu_ok], 2
.fpu287:
        fninit
.nofpu:
        int     0x11
        mov     [_bios_equip], ax
        push    ds
        mov     ax, 0x0040              ; COM1-4, LPT1-3 (0040:0000-000D)
        mov     ds, ax
        xor     si, si
        mov     di, _bios_ports
        mov     cx, 7
        rep     movsw
        mov     ax, 0xF000              ; Datum F000:FFF5, Modell FFFE
        mov     ds, ax
        mov     si, 0xFFF5
        mov     di, _bios_date
        mov     cx, 8
        rep     movsb
        mov     al, [0xFFFE]
        mov     [es:_bios_model], al
        pop     ds

        ; Start von Festplatte: CONFIG.SYS und Treiber per BIOS vorladen,
        ; solange es noch geht (BIOS-Plattenroutinen brauchen Interrupts)
        test    byte [_boot_drive], 0x80
        jz      .nopre
        sti
        call    preload_
.nopre:
        cli

        call    a20_enable
        call    a20_check
        mov     [_a20_ok], al
        call    gdt_build_              ; C: GDT und IDT aufbauen

        lgdt    [_gdtr]
        lidt    [_idtr]
        mov     ax, 1                   ; MSW.PE - ab hier kein Weg zurueck
        lmsw    ax
        jmp     KCODE:pm_entry          ; Prefetch-Queue leeren, CS laden

pm_entry:
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, kstack_top
        call    kmain_
        jmp     halt_forever_

; A20 ueber den Tastaturcontroller freischalten (AT-Methode)
a20_enable:
        call    kbc_wait
        mov     al, 0xD1
        out     0x64, al
        call    kbc_wait
        mov     al, 0xDF
        out     0x60, al
        call    kbc_wait
        ret
; AL = 1, wenn A20 offen ist: 0000:0500 und FFFF:0510 liegen dann nicht
; auf derselben Speicherzelle. Der Tastaturcontroller braucht manchmal
; einen Moment, deshalb mehrere Versuche.
a20_check:
        push    ds
        push    es
        push    si
        push    di
        push    cx
        xor     ax, ax
        mov     ds, ax
        dec     ax
        mov     es, ax
        mov     si, 0x0500
        mov     di, 0x0510
        mov     cx, 0x4000
.again:
        mov     al, [si]
        mov     ah, [es:di]
        push    ax
        mov     byte [si], 0x00
        mov     byte [es:di], 0xFF
        mov     al, [si]                ; 0xFF hier = Umlauf, A20 zu
        pop     bx
        mov     [es:di], bh
        mov     [si], bl
        cmp     al, 0xFF
        jne     .open
        loop    .again
        xor     al, al
        jmp     .done
.open:  mov     al, 1
.done:  pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds
        ret

kbc_wait:
        in      al, 0x64
        test    al, 2
        jnz     kbc_wait
        ret

; ---------------------------------------------------------------------------
; FAPI-Call-Gate: Ring 3 -> Ring 0
; Die CPU hat bereits auf den Ring-0-Stack aus dem TSS gewechselt.
; Eingang: AX = Funktion, BX/CX = Argumente, DS = Daten des Aufrufers
global fapi_entry_
fapi_entry_:
        push    ds
        push    es
        pusha
        mov     si, ds                  ; Selektor des Aufrufers merken
        mov     di, KDATA
        mov     ds, di
        mov     es, di
        mov     [_fapi_dx], dx          ; viertes Argument
        mov     dx, si
        call    fapi_dispatch_          ; (fn, usel, a1, a2) -> AX
        mov     bp, sp
        mov     [bp+14], ax             ; Rueckgabewert in gesichertes AX
        ; ES/DS des Aufrufers koennen auf ein gerade freigegebenes Segment
        ; zeigen - dann beim Zurueckladen nicht abstuerzen, sondern nullen
        mov     ax, [bp+16]
        call    sanitize_sel
        mov     [bp+16], ax
        mov     ax, [bp+18]
        call    sanitize_sel
        mov     [bp+18], ax
        popa
        pop     es
        pop     ds
        retf                            ; zurueck nach Ring 3

sanitize_sel:
        verr    ax
        jz      .ok
        xor     ax, ax
.ok:    ret

; ---------------------------------------------------------------------------
; Ausnahmen 0-16 (286) und Hardware-Interrupts
%macro EXC 1
exc%1:  push    word 0
        push    word %1
        jmp     exc_common
%endmacro
%macro EXC_ERR 1
exc%1:  push    word %1
        jmp     exc_common
%endmacro

EXC 0
EXC 1
EXC 2
EXC 3
EXC 4
EXC 5
EXC 6
EXC_ERR 8
EXC 9
EXC_ERR 10
EXC_ERR 11
EXC_ERR 12
EXC_ERR 13
EXC_ERR 14                              ; 386: #PF mit Fehlercode
EXC 15
EXC 16

; #NM (Ausnahme 7): 287-Befehl nach einem Taskwechsel (der 286 setzt bei
; jedem Hardware-Taskwechsel MSW.TS). fpu_nm tauscht den 287-Zustand aus
; und gibt 0 zurueck - dann wird der Befehl wiederholt; sonst (kein 287)
; wie jede andere Ausnahme.
exc7:   pusha
        push    ds
        push    es
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        clts                            ; der Kernel darf jetzt an den 287
        call    fpu_nm_
        pop     es
        pop     ds
        test    ax, ax
        popa
        jnz     .fault
        iret
.fault: push    word 0
        push    word 7
        jmp     exc_common

; Stack: [vec] [err] [ip] [cs] [flags] ...
exc_common:
        mov     bp, sp
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        mov     ax, [bp+0]              ; Vektor
        mov     dx, [bp+2]              ; Fehlercode
        mov     bx, [bp+6]              ; CS
        mov     cx, [bp+4]              ; IP
        call    fault_                  ; kehrt nicht zurueck
        jmp     halt_forever_

; IRQ 0: Systemtakt, 100 Hz
irq0:
        push    ax
        push    ds
        mov     ax, KDATA
        mov     ds, ax
        add     word [_ticks], 1
        adc     word [_ticks+2], 0
        cmp     word [_reboot_count], 0     ; Strg+Alt+Entf angefordert und
        je      .norb                       ; niemand gibt ab: nach 5 s
        dec     word [_reboot_count]        ; trotzdem neu starten
        jnz     .norb
        call    reboot_
.norb:
        inc     byte [subtick]
        cmp     byte [subtick], 100
        jb      .eoi
        mov     byte [subtick], 0
        inc     word [_uptime_s]
.eoi:
        mov     al, 0x20
        out     0x20, al
        ; Praeemptiv (6.0): war ein Programm in Ring 3 unterbrochen, darf der
        ; Scheduler wechseln - es haelt nie die Kernelsperre, und alle
        ; anderen Tasks stehen an ihren normalen Wartepunkten. Im Kernel
        ; (Ring 0) wird nie unterbrochen, erst beim Ruecksprung (FAPI).
        ; Stack: [ds] [ax] [ip] [cs] [flags] ...
        push    bp
        mov     bp, sp
        test    byte [bp+8], 3
        pop     bp
        jz      .ret
        pusha
        push    es
        mov     ax, ds
        mov     es, ax
        cld                             ; das Programm kann DF gesetzt haben
        call    preempt_
        pop     es
        popa
.ret:
        pop     ds
        pop     ax
        iret

; IRQ 6: Diskettencontroller fertig - Flag setzen, wartenden Task wecken
irq6:
        pusha
        push    ds
        push    es
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        mov     byte [_fdc_irq], 1
        call    fdc_isr_
        mov     al, 0x20
        out     0x20, al
        pop     es
        pop     ds
        popa
        iret

; IRQ 1: Tastatur
irq1:
        pusha
        push    ds
        push    es
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        cmp     byte [_kbd_aux], 0      ; PS/2-Maus aktiv (DevHlp KbdLock)?
        je      .key
        in      al, 0x64                ; Byte von der Maus (AUX)?
        test    al, 0x20                ; dann gehoert es IRQ 12
        jnz     .aux
.key:   in      al, 0x60
        xor     ah, ah
        call    kbd_irq_
.aux:   mov     al, 0x20
        out     0x20, al
        pop     es
        pop     ds
        popa
        iret

; IRQs fuer Treiber (DevHlp SetIRQ): Tabelle irq_hoff/irq_hcs/irq_hds,
; Routine fern mit DS = Treiberdaten, AX = IRQ, danach EOI. Interrupt-Gate:
; IF bleibt aus, Verschachtelung gibt es nicht (ic_ptr darf global sein).
%macro IRQ 1
irq%1:  push    ax
        mov     ax, %1
        jmp     irq_common
%endmacro
IRQ 3
IRQ 4
IRQ 5
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 14
IRQ 15

; IRQ 13: Fehler des 287 (unmaskierte Ausnahme). Beim AT haelt ein Latch
; BUSY fest, bis man Port F0h beschreibt - sonst wartet der naechste
; 287-Befehl ewig. Kam der Fehler aus einem Programm, wird es wie bei
; einer Ausnahme beendet (wie OS/2 ohne eigenen Handler); im Kernel nur
; quittieren.
irq13:  push    ax
        xor     al, al
        out     0xF0, al                ; BUSY-Latch loeschen
        mov     al, 0x20
        out     0xA0, al
        out     0x20, al
        pop     ax
        push    bp
        mov     bp, sp
        test    byte [bp+4], 3          ; CS des Unterbrochenen
        pop     bp
        jz      .kern
        push    word 0
        push    word 16
        jmp     exc_common
.kern:  iret

irq_common:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     bx, KDATA
        mov     ds, bx
        mov     es, bx
        mov     bx, ax
        add     bx, bx
        cmp     word [_irq_hcs+bx], 0
        je      .eoi                    ; niemand zustaendig
        cmp     al, 7                   ; IRQ 7/15: Geister-Interrupt?
        je      .spur
        cmp     al, 15
        jne     .call
.spur:  push    ax
        mov     dx, 0x20
        cmp     al, 8
        jb      .isr
        mov     dx, 0xA0
.isr:   mov     al, 0x0B                ; OCW3: ISR lesen
        out     dx, al
        in      al, dx
        test    al, 0x80
        pop     ax
        jnz     .call
        cmp     al, 8                   ; Geist: kein EOI am eigenen PIC
        jb      .out
        mov     al, 0x20
        out     0x20, al                ; Slave-Geist: Master hat IRQ 2
        jmp     .out
.call:  push    ax
        mov     cx, [_irq_hoff+bx]
        mov     [ic_ptr], cx
        mov     cx, [_irq_hcs+bx]
        mov     [ic_ptr+2], cx
        mov     ds, [_irq_hds+bx]
        call    far [es:ic_ptr]
        mov     bx, KDATA
        mov     ds, bx
        pop     ax
.eoi:   cmp     al, 8
        mov     al, 0x20
        jb      .m
        out     0xA0, al
.m:     out     0x20, al
.out:   pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        iret

; IRQ 2 (Kaskade): maskiert, nur fuer den Fall der Faelle
irq_other:
        push    ax
        mov     al, 0x20
        out     0xA0, al
        out     0x20, al
        pop     ax
        iret

; ---------------------------------------------------------------------------
; Hilfsroutinen fuer C

; void task_jump(unsigned tss_sel)  - Hardware-Taskwechsel per JMP auf TSS
global task_jump_
task_jump_:
        mov     [tj_sel], ax
        jmp     far [tj_ptr]
        ret                             ; hier geht es weiter, wenn dieser
                                        ; Task wieder an der Reihe ist

; 287: void fpu_save(unsigned sel, unsigned off) / fpu_restore(...) -
; 94 Byte Zustand; fpu_reset() leert den 287 und setzt TS wieder (damit
; der naechste Befehl eines Programms wieder ueber #NM laeuft);
; fpu_pm_init() einmal nach dem Umschalten: MSW.MP, FSETPM (der 287 muss
; erfahren, dass die CPU im Protected Mode ist; 287XL/387 uebergehen es)
global fpu_save_, fpu_restore_, fpu_reset_, fpu_pm_init_
fpu_save_:
        push    ds
        push    bx
        mov     ds, ax
        mov     bx, dx
        fnsave  [bx]
        fwait
        pop     bx
        pop     ds
        ret
fpu_restore_:
        push    ds
        push    bx
        mov     ds, ax
        mov     bx, dx
        frstor  [bx]
        fwait
        pop     bx
        pop     ds
        ret
fpu_reset_:
        push    ax
        clts
        fninit
        smsw    ax
        or      al, 8                   ; TS
        lmsw    ax
        pop     ax
        ret
fpu_pm_init_:
        push    ax
        smsw    ax
        or      al, 2                   ; MP: auch WAIT prueft TS
        lmsw    ax
        clts
        fsetpm
        fninit
        pop     ax
        ret

; void load_tr(unsigned sel)
global load_tr_
load_tr_:
        ltr     ax
        ret

; unsigned sel_readable(unsigned sel)  - 1, wenn Ring 3 lesen darf
global sel_readable_
sel_readable_:
        verr    ax
        mov     ax, 0
        jnz     .no
        inc     ax
.no:    ret

; unsigned sel_writable(unsigned sel)  - 1, wenn Ring 3 schreiben darf
global sel_writable_
sel_writable_:
        verw    ax
        mov     ax, 0
        jnz     .no
        inc     ax
.no:    ret

; unsigned sel_limit(unsigned sel)
global sel_limit_
sel_limit_:
        lsl     ax, ax
        jz      .ok
        xor     ax, ax
.ok:    ret

; void outb(unsigned port, unsigned val)
global outb_
outb_:
        xchg    ax, dx
        out     dx, al
        out     0x80, al                ; kurze I/O-Pause
        xchg    ax, dx
        ret

; unsigned inb(unsigned port)
global inb_
inb_:
        push    dx
        mov     dx, ax
        in      al, dx
        xor     ah, ah
        pop     dx
        ret

; void enable_ints(void)
global enable_ints_
enable_ints_:
        sti
        ret

; unsigned long get_ticks(void)
global get_ticks_
get_ticks_:
        pushf
        cli
        mov     ax, [_ticks]
        mov     dx, [_ticks+2]
        popf
        ret

; void idle_wait(void)  - bis zum naechsten Interrupt schlafen
global idle_wait_
idle_wait_:
        sti
        hlt
        ret

; void zero_seg(unsigned sel, unsigned words)
global zero_seg_
zero_seg_:
        push    es
        push    di
        push    cx
        mov     es, ax
        mov     cx, dx
        xor     di, di
        xor     ax, ax
        cld
        rep     stosw
        pop     cx
        pop     di
        pop     es
        ret

; ---------------------------------------------------------------------------
; Geraetetreiber

; void drv_call(unsigned code_sel, unsigned data_sel, unsigned entry,
;               unsigned req_off)
; Strategieroutine fern aufrufen: DS = Treiberdaten, ES:BX = Anforderung
global drv_call_
drv_call_:
        pusha
        push    ds
        push    es
        mov     [dc_ptr], bx
        mov     [dc_ptr+2], ax
        mov     bx, cx
        mov     ax, KDATA
        mov     es, ax
        mov     ds, dx
        call    far [es:dc_ptr]
        pop     es
        pop     ds
        popa
        ret

; DevHlp: Treiber ruft fern, DS = Treiberdaten.
; AX = Funktion, BX/CX/DX = Argumente -> devhlp_dispatch(fn, dsel, bx, cx)
global devhlp_entry_
devhlp_entry_:
        push    ds
        push    es
        push    si
        push    di
        push    bp
        push    dx
        push    bx
        push    cx
        mov     si, ds
        push    ax
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        pop     ax
        push    word [_devhlp_dx]       ; wiedereintrittsfest: DevHlp auch
        mov     [_devhlp_dx], dx        ; aus Interruptroutinen
        mov     dx, si
        call    devhlp_dispatch_
        pop     word [_devhlp_dx]
        pop     cx
        pop     bx
        pop     dx
        pop     bp
        pop     di
        pop     si
        pop     es
        pop     ds
        retf

; unsigned irq_off(void)  - Flags sichern, Interrupts aus
global irq_off_
irq_off_:
        pushf
        pop     ax
        cli
        ret

; void irq_restore(unsigned flags)
global irq_restore_
irq_restore_:
        push    ax
        popf
        ret

; void cpu_reset(void)  - Neustart ueber den Tastaturcontroller (Puls auf
; der Reset-Leitung), als Rueckfall ein Dreifachfehler
; int rm_geometry(void) - Geometrie des Startlaufwerks (INT 13h/08h), Real Mode
global rm_geometry_
rm_geometry_:
        push    bx
        push    cx
        push    dx
        push    di
        push    es
        xor     di, di
        mov     es, di
        mov     ah, 0x08
        mov     dl, [_boot_drive]
        int     0x13
        jc      .err
        mov     al, cl
        and     ax, 0x3F
        jz      .err
        mov     [geo_spt], ax
        mov     al, dh
        xor     ah, ah
        inc     ax
        mov     [geo_heads], ax
        xor     ax, ax
        jmp     .done
.err:   mov     ax, 1
.done:  pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

; int rm_read(unsigned long lba) - DX:AX = LBA, ein Sektor nach 0000:8000
; ueber INT 13h (CHS), Real Mode. 0 = ok
global rm_read_
rm_read_:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        mov     di, 3                   ; Versuche
.again:
        push    ax
        push    dx
        mov     bx, ax                  ; LBA / Sektoren je Spur
        mov     ax, dx
        xor     dx, dx
        div     word [geo_spt]
        mov     cx, ax
        mov     ax, bx
        div     word [geo_spt]
        mov     si, dx                  ; Sektor-Index
        mov     bx, ax                  ; Spur CX:BX / Koepfe
        mov     ax, cx
        xor     dx, dx
        div     word [geo_heads]
        test    ax, ax
        jnz     .bad
        mov     ax, bx
        div     word [geo_heads]        ; AX = Zylinder, DX = Kopf
        cmp     ax, 1024
        jae     .bad
        mov     ch, al
        mov     cl, ah
        shl     cl, 6
        mov     bx, si
        inc     bx
        or      cl, bl
        mov     dh, dl
        mov     dl, [_boot_drive]
        mov     bx, 0x0800
        mov     es, bx
        xor     bx, bx
        mov     ax, 0x0201
        int     0x13
        pop     dx
        pop     ax
        jnc     .ok
        push    ax
        push    dx
        xor     ax, ax                  ; Laufwerk zuruecksetzen
        mov     dl, [_boot_drive]
        int     0x13
        pop     dx
        pop     ax
        dec     di
        jnz     .again
        mov     ax, 1
        jmp     .out
.bad:   pop     dx
        pop     ax
        mov     ax, 1
        jmp     .out
.ok:    xor     ax, ax
.out:   pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

global cpu_reset_
cpu_reset_:
        cli
.w:     in      al, 0x64
        test    al, 2
        jnz     .w
        mov     al, 0xFE
        out     0x64, al
        mov     cx, 0xFFFF
.wait:  loop    .wait
        lidt    [null_idtr]             ; keine Interrupts mehr erreichbar:
        int     3                       ; Ausnahme -> Doppel- -> Dreifachfehler
.h:     hlt
        jmp     .h

global halt_forever_
halt_forever_:
        cli
.h:     hlt
        jmp     .h

; ---------------------------------------------------------------------------
segment _TEXTEND
code_end:

segment KTEXT2END
code2_end:

segment _DATA

global _kcode_size
_kcode_size: dw code_end                ; Codegroesse (Offset in CGROUP)
global _kcode2_size
_kcode2_size: dw code2_end              ; zweites Codesegment (CGROUP2)
global _kdata_size
_kdata_size: dw bss_end                 ; Datengroesse samt BSS (DGROUP)

global _isr_table
_isr_table:
        dw exc0, exc1, exc2, exc3, exc4, exc5, exc6, exc7
        dw exc8, exc9, exc10, exc11, exc12, exc13, exc14, exc15
        dw exc16, 0, 0, 0, 0, 0, 0, 0
        dw 0, 0, 0, 0, 0, 0, 0, 0
        dw irq0, irq1, irq_other, irq3
        dw irq4, irq5, irq6, irq7
        dw irq8, irq9, irq10, irq11
        dw irq12, irq13, irq14, irq15

global _irq_hoff, _irq_hcs, _irq_hds
_irq_hoff: times 16 dw 0                ; Einsprung im Treibercode
_irq_hcs:  times 16 dw 0                ; Codeselektor, 0 = frei
_irq_hds:  times 16 dw 0                ; Datenselektor
ic_ptr:    dw 0, 0

tj_ptr: dw 0
tj_sel: dw 0
dc_ptr: dw 0, 0
null_idtr: dw 0, 0, 0
geo_spt:   dw 63
geo_heads: dw 16

segment _BSS

bss_start:
global _boot_drive, _ticks, _uptime_s
global _fapi_dx, _devhlp_dx
_fapi_dx:    resw 1
_devhlp_dx:  resw 1
_boot_drive: resb 1
subtick:     resb 1
_ticks:      resw 2
_uptime_s:   resw 1
global _a20_ok
_a20_ok:     resb 1
global _kdata_seg
_kdata_seg:  resw 1
global _fdc_irq
_fdc_irq:    resb 1
global _kbd_aux
_kbd_aux:    resb 1
global _mem_conv_kb, _mem_ext_kb, _e820_map, _e820_n, _e801_lo, _e801_hi
_mem_conv_kb: resw 1
_mem_ext_kb:  resw 1
_e820_n:      resw 1
_e801_lo:     resw 1
_e801_hi:     resw 1
_e820_map:    resb 32 * 20
global _bios_hdcount, _bios_fdpt, _bios_geo_h, _bios_geo_s, _bios_geo_c
_bios_hdcount: resb 1
_bios_fdpt:   resb 32
_bios_geo_h:  resb 4                    ; Koepfe - 1 (INT 13h/08h)
_bios_geo_s:  resb 4                    ; Sektoren je Spur, 0 = keine
_bios_geo_c:  resw 4                    ; Zylinder
global _cpu_386, _fpu_ok, _bios_equip, _bios_ports, _bios_date, _bios_model
_cpu_386:     resb 1
_fpu_ok:      resw 1
_fpu_sw:      resw 1
_bios_equip:  resw 1
_bios_ports:  resw 7                    ; COM1-4, LPT1-3
_bios_date:   resb 9
_bios_model:  resb 1
global _vga_ok, _vga_font8a, _vga_font8b, _vga_font16
_vga_ok:      resb 1
_vga_font8a:  resw 2                    ; Offset, Segment (ROM)
_vga_font8b:  resw 2
_vga_font16:  resw 2
kstack:      resb 1024
kstack_top:

segment _BSSEND
bss_end:
