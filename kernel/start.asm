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

cpu 286

KCODE   equ 0x08
KDATA   equ 0x10
SCRATCH equ 0x30
SCRATCH2 equ 0x40

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
extern fdc_isr_
extern fpu_nm_
extern _gdtr, _idtr, _kmoved, _kcode_lin, _kmove_fail, _gdt

; ---------------------------------------------------------------------------
segment _TEXT

..start:
global _start
_start:
        ; Real Mode, vom Bootsektor bei 1000:0000 angesprungen
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
        in      al, 0x21                ; Masken des BIOS (fuer die Box)
        mov     [_bios_pic1], al
        in      al, 0xA1
        mov     [_bios_pic2], al

        ; Speicher abfragen, solange das BIOS noch erreichbar ist
        int     0x12                    ; konventionell in KB
        mov     [_mem_conv_kb], ax
        mov     ah, 0x88                ; erweitert ab 1 MB in KB
        int     0x15
        jnc     .ext_ok
        xor     ax, ax
.ext_ok:
        mov     [_mem_ext_kb], ax

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
        cmp     byte [_kmoved], 0       ; Kernel ueber 1 MB legen?
        je      .nomove
        call    kmove
        call    a20_enable              ; das BIOS laesst A20 evtl. zu
        cmp     byte [_kmoved], 0
        jne     .nomove
        call    gdt_build_              ; Kopie ging nicht: GDT fuer unten
.nomove:

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

; Kernel (10000h bis Ende von DGROUP) nach kcode_lin kopieren - im Real
; Mode ueber INT 15h/87h (Blockkopie des AT-BIOS, je 32 KB). Danach ist
; die Kopie der gueltige Kernel: lgdt/lidt zeigen schon dorthin, der Sprung
; nach KCODE:pm_entry landet in ihr. Die alte Kopie wird nicht mehr benutzt.
kmove:
        push    es
        mov     ax, ds
        mov     es, ax
        ; Laenge = (kdata_seg - 1000h) * 16 + bss_end
        mov     ax, [_kdata_seg]
        sub     ax, 0x1000
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12
        add     ax, bss_end
        adc     dx, 0
        mov     [km_len], ax
        mov     [km_len + 2], dx
        xor     ax, ax
        mov     [km_off], ax
        mov     [km_off + 2], ax
.next:
        mov     ax, [km_len]            ; Rest = len - off
        mov     dx, [km_len + 2]
        sub     ax, [km_off]
        sbb     dx, [km_off + 2]
        mov     cx, ax
        or      ax, dx
        jz      .done
        or      dx, dx
        jnz     .big
        cmp     cx, 0x8000
        jbe     .sz
.big:   mov     cx, 0x8000
.sz:    inc     cx                      ; auf Worte aufrunden
        shr     cx, 1
        push    cx
        ; Tabelle: 6 Deskriptoren, Quelle bei +16, Ziel bei +24
        mov     di, km_gdt
        push    cx
        mov     cx, 24
        xor     ax, ax
        rep     stosw
        pop     cx
        mov     ax, [km_off]            ; Quelle = 10000h + off
        mov     dx, [km_off + 2]
        add     dx, 1
        mov     word [km_gdt + 16], 0xFFFF
        mov     [km_gdt + 18], ax
        mov     [km_gdt + 20], dl
        mov     byte [km_gdt + 21], 0x93
        mov     ax, [km_off]            ; Ziel = kcode_lin + off
        mov     dx, [km_off + 2]
        add     ax, [_kcode_lin]
        adc     dx, [_kcode_lin + 2]
        mov     word [km_gdt + 24], 0xFFFF
        mov     [km_gdt + 26], ax
        mov     [km_gdt + 28], dl
        mov     byte [km_gdt + 29], 0x93
        mov     si, km_gdt
        mov     ah, 0x87
        int     0x15
        cli
        pop     cx
        jc      .fail
        shl     cx, 1                   ; Bytes weiter
        add     [km_off], cx
        adc     word [km_off + 2], 0
        jmp     .next
.fail:  mov     byte [_kmoved], 0       ; geht nicht: Kernel bleibt unten
        mov     byte [_kmove_fail], 1
.done:  pop     es
        ret

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
EXC 14
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
; 5.1: Kompatibilitaetsbox (eine DOS-Sitzung im echten Real Mode)
;
; unsigned rb_enter(void) - in den Real Mode zur Box; kehrt zurueck, wenn
; die Box per Alt+Esc, INT 20h oder INT E0h in den Kernel zurueckkommt
; (Grund im Zustandsblock, rb.c). Vorher hat C den Real-Mode-Teil (ab
; rbs_code) an rb_lin + 80h gelegt, den Zustandsblock gefuellt, 40:67 und
; CMOS 0Fh = 0Ah gesetzt und bei einem 386 SCRATCH (Daten) und SCRATCH2
; (Code, 9Ah) auf rb_lin gelegt. Hin: 286 per Reset ueber den Tastatur-
; controller (BIOS springt nach 40:67), 386 per PE loeschen. Zurueck: der
; Real-Mode-Teil laedt die GDT, LMSW und springt nach rb_reentry.
global rb_enter_, rb_reentry_, a20_on_
rb_enter_:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     [rb_ksp], sp
        str     word [rb_tr]
        sldt    word [rb_ldt]
        cli
        mov     al, 0x11                ; PICs zurueck auf 08h/70h (BIOS)
        out     0x20, al
        out     0xA0, al
        mov     al, 0x08
        out     0x21, al
        mov     al, 0x70
        out     0xA1, al
        mov     al, 4
        out     0x21, al
        mov     al, 2
        out     0xA1, al
        mov     al, 1
        out     0x21, al
        out     0xA1, al
        mov     al, 0xFF                ; Masken setzt der Real-Mode-Teil
        out     0x21, al
        out     0xA1, al
        mov     al, 0x36                ; Takt wie das BIOS: 18,2 Hz
        out     0x43, al
        xor     al, al
        out     0x40, al
        out     0x40, al
        cmp     byte [_cpu_386], 0
        jne     .r386
.w:     in      al, 0x64                ; 286: Reset, das BIOS springt
        test    al, 2                   ; ueber 40:67 in die Box
        jnz     .w
        mov     al, 0xFE
        out     0x64, al
.h:     hlt
        jmp     .h
.r386:  lidt    [rm_idtr]
        mov     ax, SCRATCH
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x2000
        jmp     SCRATCH2:(rbs_386 - rbs_code + 0x80)

; Rueckweg: CS = KCODE, sonst noch Real-Mode-Reste
rb_reentry_:
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, [rb_ksp]
        lidt    [_idtr]
        mov     bx, [rb_tr]             ; TSS ist noch "busy": LTR braeuchte
        and     bx, 0xFFF8              ; ein freies
        and     byte [_gdt + bx + 5], 0xFD
        ltr     [rb_tr]
        lldt    [rb_ldt]
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

a20_on_:
        push    ax
        call    a20_enable
        pop     ax
        ret




; ---------------------------------------------------------------------------
segment _TEXTEND
code_end:

; ---- Real-Mode-Teil der Box: wird nach rb_lin + 80h kopiert -------------
; Liegt in KTEXT2 (erstes Codesegment ist voll), kernel/rbox.c kopiert ihn.
; Zustandsblock ab rb_lin: 0 AX BX CX DX SI DI BP DS ES, 18 SP, 20 SS,
; 22 IP, 24 CS, 26 FLAGS, 28 PIC-Masken, 30 Grund, 32 GDTR (6), 38 Rueckweg
; (Offset, Selektor), 42 alter INT 9, 46 Cursor, 50 rb_seg, 52 Sektoren je
; Spur, 54 Koepfe, 56 Abbild linear (32 Bit), 60 Sektoren im Abbild,
; 62 alter INT 13h, 66 alter INT 15h, 70 Laufwerkstyp (INT 13h/08h),
; 72 BIOS ruft INT 15h/4Fh (dann Tasten dort statt in INT 9), 74 Rueckkehr
; nach einem Dienst (Bildschirm unveraendert, nicht neu einstellen), 76 287
; vorhanden, 77 287-Zustand der Box bei 1C40h gesichert (94 Byte).
; Stummel ab 80h (bis 5FFh), Bildschirmtext (80x25) ab 600h, IVT+BDA-Kopie
; ab 1600h (rbox.c), IVT 300h-3FFh ab 1B00h, GDT fuer INT 15h/87h ab 1C00h,
; Stapel bis 2000h.
segment KTEXT2
%define RBO(x) ((x) - rbs_code + 0x80)
rbs_code:
rbs_entry:                              ; Real Mode, CS = rb_seg
        cli
        mov     ax, cs
        mov     ds, ax
        mov     ss, ax
        mov     sp, 0x2000                  ; Stapel hinter der GDT
        call    rbs_ivrest              ; IVT 300h-3FFh: BIOS-Stapel beim Reset
        cmp     byte [76], 0            ; 287: Reset ueber Port F1h holt ihn
        je      .nofpu                  ; aus dem Protected Mode (FSETPM von
        xor     al, al                  ; AT-DOS), dann Zustand der Box
        out     0xF1, al
        fninit
        cmp     byte [77], 0
        je      .nofpu
        frstor  [0x1C40]
.nofpu:
        mov     al, [28]
        out     0x21, al
        mov     al, [29]
        out     0xA1, al
        sti
        cmp     byte [74], 0            ; nur ein Dienst: Bild steht noch
        jne     .quiet
        mov     ax, 3                   ; BIOS stellt die Karte selbst ein
        int     0x10
        push    es
        mov     ax, 0xB800
        mov     es, ax
        mov     si, 0x600
        xor     di, di
        mov     cx, 2000
        cld
        rep     movsw
        pop     es
        mov     dx, [46]
        mov     bh, 0
        mov     ah, 2
        int     0x10
.quiet: push    ds
        xor     ax, ax
        mov     ds, ax
        and     byte [0x417], 0xF7      ; Alt ist beim Zurueckkommen los
        and     byte [0x418], 0xFD
        pop     ds
        in      al, 0x64                ; wartet ein Byte im 8042? Der PIC
        test    al, 1                   ; ist neu eingestellt und sieht die
        jz      .kok                    ; schon hohe IRQ-Leitung nie mehr
        test    al, 0x20                ; als Flanke - abholen, sonst steht
        jnz     .aux                    ; die Tastatur
        int     9
        jmp     short .kok
.aux:   in      al, 0x60                ; Maus: die Box hat keinen Treiber
.kok:   cli
        mov     ss, [20]
        mov     sp, [18]
        push    word [26]
        push    word [24]
        push    word [22]
        mov     ax, [0]
        mov     bx, [2]
        mov     cx, [4]
        mov     dx, [6]
        mov     si, [8]
        mov     di, [10]
        mov     bp, [12]
        mov     es, [16]
        mov     ds, [14]
        iret

rbs_int9:                               ; Alt+Esc: zurueck zu AT-DOS
        cmp     byte [cs:72], 0         ; BIOS ruft INT 15h/4Fh: dort (ein
        je      .own                    ; zweites Lesen von 60h verliert in
        jmp     far [cs:42]             ; manchen Nachbildungen Tasten)
.own:   push    ax
        in      al, 0x60
        cmp     al, 1
        je      .esc
        cmp     al, 0x53                ; Entf: mit Strg+Alt Box beenden
        jne     .chain                  ; (sonst startete das BIOS den
        push    ds                      ;  ganzen Rechner neu)
        xor     ax, ax
        mov     ds, ax
        mov     al, [0x417]
        pop     ds
        and     al, 0x0C
        cmp     al, 0x0C
        jne     .chain
        mov     al, 0x20
        out     0x20, al
        pop     ax
        mov     word [cs:30], 2
        jmp     rbs_topm
.esc:   push    ds
        xor     ax, ax
        mov     ds, ax
        test    byte [0x417], 8
        pop     ds
        jz      .chain
        mov     al, 0x20
        out     0x20, al
        pop     ax
        mov     word [cs:30], 1
        jmp     rbs_topm
.chain: pop     ax
        jmp     far [cs:42]

; INT 13h: Laufwerk 0 ist das Abbild im erweiterten Speicher (nur lesen,
; Kopie per INT 15h/87h des BIOS - kein Ausflug zu AT-DOS); die Platten
; und Laufwerke von AT-DOS sieht die Box nicht.
rbs_int13:
        sti
        cmp     dl, 0
        jne     .other
        cmp     ah, 2
        je      .read
        cmp     ah, 8
        je      .parm
        cmp     ah, 3
        je      .wp
        cmp     ah, 5
        je      .wp
        cmp     ah, 0x15
        je      .type
        mov     ah, 0                   ; 00, 04, 16h (kein Wechsel), 17h ...
        jmp     .ok
.type:  mov     ah, 1                   ; Diskette ohne Wechselleitung
        jmp     .ok
.wp:    mov     ah, 3                   ; schreibgeschuetzt
        jmp     .err
.other: test    dl, 0x80
        jz      .nodrv
        cmp     ah, 8                   ; Platten: keine
        jne     .nodrv
        xor     dx, dx
.nodrv: mov     ah, 0x80                ; Zeitueberschreitung: kein Laufwerk
        jmp     .err
.parm:  mov     bl, [cs:70]
        mov     ax, [cs:52]
        mul     byte [cs:54]            ; Sektoren je Zylinder
        mov     cx, ax
        mov     ax, [cs:60]
        xor     dx, dx
        div     cx
        dec     ax                      ; letzter Zylinder
        mov     ch, al
        mov     cl, ah
        ror     cl, 1
        ror     cl, 1
        and     cl, 0xC0
        or      cl, [cs:52]
        mov     dh, [cs:54]
        dec     dh
        mov     dl, 1
        xor     ax, ax
        mov     di, 0x1E * 4            ; Parametertabelle des BIOS
        push    ds
        mov     es, ax
        les     di, [es:di]
        pop     ds
        jmp     .ok
.read:  push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        push    ax
        mov     bp, cs
        mov     ds, bp
        ; LBA = (Zylinder * Koepfe + Kopf) * Sektoren + Sektor - 1
        mov     di, dx                  ; DH = Kopf (MUL zerstoert DX)
        mov     al, cl
        rol     al, 1
        rol     al, 1
        and     al, 3
        mov     ah, al
        mov     al, ch                  ; AX = Zylinder
        mul     word [54]
        mov     dx, di
        add     al, dh
        adc     ah, 0
        mul     word [52]
        and     cx, 0x3F
        dec     cx
        add     ax, cx
        mov     si, ax                  ; SI = LBA
        pop     ax
        push    ax
        xor     ah, ah
        mov     bp, ax                  ; BP = Anzahl
        add     ax, si
        cmp     ax, [60]
        ja      .range
        ; Quelle: Abbild + LBA * 512
        mov     ax, si
        mov     dx, si
        mov     cl, 9
        shl     ax, cl
        mov     cl, 7
        shr     dx, cl
        add     ax, [56]
        adc     dx, [58]
        mov     di, 0x1C00
        mov     [di + 18], ax
        mov     [di + 20], dl
        ; Ziel: ES:BX linear
        pop     ax
        pop     cx                      ; ES des Aufrufers
        push    cx
        push    ax
        mov     ax, cx
        mov     dx, cx
        mov     cl, 4
        shl     ax, cl
        mov     cl, 12
        shr     dx, cl
        add     ax, bx
        adc     dx, 0
        mov     [di + 26], ax
        mov     [di + 28], dl
        mov     cx, bp
        xchg    ch, cl                  ; Worte = Sektoren * 256
        mov     ax, cs
        mov     es, ax
        mov     si, di
        call    rbs_ivsave              ; der 286 kehrt per Reset zurueck,
        mov     ah, 0x87                ; das BIOS legt seinen Stapel dabei
        pushf                           ; nach 0030:0100 - mitten in die IVT
        call    far [66]                ; altes INT 15h
        pushf
        call    rbs_ivrest
        popf
        pop     ax
        jc      .rderr
        mov     bx, bp
        mov     al, bl
        mov     ah, 0
        jmp     .rdok
.range: pop     ax
        mov     ah, 4                   ; Sektor nicht gefunden
.rderr: mov     al, 0
        stc
        jmp     .rdend
.rdok:  clc
.rdend: pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        jc      .err
.ok:    push    bp
        mov     bp, sp
        and     byte [bp + 6], 0xFE
        pop     bp
        iret
.err:   push    bp
        mov     bp, sp
        or      byte [bp + 6], 1
        pop     bp
        iret

; INT 15h: der erweiterte Speicher gehoert AT-DOS - die Box meldet keinen
; und darf ihn nicht beschreiben (HIMEM, RAMDRIVE finden nichts)
rbs_int15:
        cmp     ah, 0x4F                ; Tastatur-Abfang (BIOS-INT 9)
        jne     .no4f
        mov     byte [cs:72], 1
        cmp     al, 1
        je      .esc
        cmp     al, 0x53
        jne     .chain
        push    ax
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     al, [0x417]
        pop     ds
        and     al, 0x0C
        cmp     al, 0x0C
        pop     ax
        jne     .chain
        mov     al, 0x20                ; Strg+Alt+Entf: Box beenden
        out     0x20, al
        mov     word [cs:30], 2
        jmp     rbs_topm
.esc:   push    ds
        push    ax
        xor     ax, ax
        mov     ds, ax
        test    byte [0x417], 8
        pop     ax
        pop     ds
        jz      .chain
        push    bp                      ; Taste verbraucht (CF = 0), das
        mov     bp, sp                  ; BIOS legt sie nicht ab
        and     byte [bp + 6], 0xFE
        pop     bp
        push    ax
        mov     al, 0x20
        out     0x20, al
        pop     ax
        mov     word [cs:30], 1
        jmp     rbs_topm
.no4f:  cmp     ah, 0x88
        je      .none
        cmp     ah, 0x87
        je      .deny
        cmp     ah, 0x89
        je      .deny
        cmp     ax, 0xE801
        je      .deny
        cmp     ax, 0xE820
        je      .deny
.chain: jmp     far [cs:66]
.none:  xor     ax, ax
        push    bp
        mov     bp, sp
        and     byte [bp + 6], 0xFE
        pop     bp
        iret
.deny:  mov     ah, 0x86                ; nicht unterstuetzt
        push    bp
        mov     bp, sp
        or      byte [bp + 6], 1
        pop     bp
        iret

rbs_int20:                              ; Programm zu Ende
        cli
        mov     word [cs:30], 2
        jmp     short rbs_topm

rbs_inte0:                              ; Dienst des Kernels
        cli
        mov     word [cs:30], 3

rbs_topm:                               ; Stapel: IP, CS, FLAGS der Box
        cli
        mov     [cs:0], ax
        mov     [cs:2], bx
        mov     [cs:4], cx
        mov     [cs:6], dx
        mov     [cs:8], si
        mov     [cs:10], di
        mov     [cs:12], bp
        mov     [cs:14], ds
        mov     [cs:16], es
        pop     word [cs:22]
        pop     word [cs:24]
        pop     word [cs:26]
        mov     [cs:18], sp
        mov     [cs:20], ss
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        in      al, 0x21
        mov     [28], al
        in      al, 0xA1
        mov     [29], al
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [0x450]             ; Cursor Seite 0
        pop     ds
        mov     [46], ax
        mov     ax, 0xB800              ; Bildschirmtext sichern
        mov     ds, ax
        xor     si, si
        mov     di, 0x600
        mov     cx, 2000
        cld
        rep     movsw
        mov     ax, cs
        mov     ds, ax
        cmp     byte [76], 0            ; 287 der Box sichern (AT-DOS macht
        je      .nofpu                  ; FNINIT/FSETPM)
        fnsave  [0x1C40]
        fwait
        mov     byte [77], 1
.nofpu: call    rbs_ivsave
        call    rbs_a20                 ; Kernel und GDT liegen ueber 1 MB
        lgdt    [32]
        mov     ax, 1
        lmsw    ax
        jmp     far [38]

; IVT 300h-3FFh (Vektoren C0h-FFh, darunter INT E0h) nach rb_lin + 1B00h
; sichern bzw. zurueck: das AT-BIOS nimmt nach dem Reset 0030:0100 als
; Stapel. Alle Register bleiben.
rbs_ivsave:
        push    cx
        push    si
        push    di
        push    ds
        push    es
        xor     si, si
        mov     ds, si
        mov     si, 0x300
        push    cs
        pop     es
        mov     di, 0x1B00
        mov     cx, 128
        cld
        rep     movsw
        jmp     short rbs_ivend
rbs_ivrest:
        push    cx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds
        mov     si, 0x1B00
        xor     di, di
        mov     es, di
        mov     di, 0x300
        mov     cx, 128
        cld
        pushf
        cli
        rep     movsw
        popf
rbs_ivend:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        ret

; A20 ueber den Tastaturcontroller einschalten: das AT-BIOS schaltet sie
; nach INT 15h/87h wieder ab (SeaBIOS laesst sie an). Nur wenn sie aus ist
; (0000:0500 und FFFF:0510 dieselbe Zelle) - das Ausgangsport des 8042 bei
; jedem Ausflug zu schreiben, legte in QEMU mitten im Tippen IRQ 1 still.
rbs_a20:
        push    ds
        push    es
        push    bx
        xor     bx, bx
        mov     ds, bx
        dec     bx
        mov     es, bx
        mov     bl, [0x500]
        mov     bh, [es:0x510]
        mov     byte [0x500], 0
        mov     byte [es:0x510], 0xFF
        mov     al, [0x500]             ; FFh: Umlauf, A20 ist aus
        mov     [es:0x510], bh
        mov     [0x500], bl
        pop     bx
        pop     es
        pop     ds
        cmp     al, 0xFF
        jne     .ret
        call    .w
        mov     al, 0xD1
        out     0x64, al
        call    .w
        mov     al, 0xDF
        out     0x60, al
.w:     xor     cx, cx
.w1:    in      al, 0x64
        test    al, 2
        loopnz  .w1
.ret:   ret

cpu 386
rbs_386:                                ; PM, CS-Basis rb_lin: PE loeschen
        push    word [cs:50]
        push    word RBO(rbs_entry)
        mov     eax, cr0                ; PE, MP, EM, TS loeschen wie der
        and     al, 0xF0                ; Reset des 286 (sonst: 287-Befehl
        mov     cr0, eax                ; in der Box -> INT 7 -> Endlosschleife)
        retf                            ; laedt CS = rb_seg (Real Mode)
cpu 286
rbs_end:

segment KTEXT2END
code2_end:

segment _DATA
rm_idtr: dw 0x3FF, 0, 0
global _rbs_code, _rbs_len, _rbs_entry_off, _rbs_int9_off, _rbs_int20_off, _rbs_inte0_off
global _rbs_int13_off, _rbs_int15_off
_rbs_code:      dw rbs_code
_rbs_len:       dw rbs_end - rbs_code
_rbs_entry_off: dw RBO(rbs_entry)
_rbs_int9_off:  dw RBO(rbs_int9)
_rbs_int20_off: dw RBO(rbs_int20)
_rbs_inte0_off: dw RBO(rbs_inte0)
_rbs_int13_off: dw RBO(rbs_int13)
_rbs_int15_off: dw RBO(rbs_int15)

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
global _mem_conv_kb, _mem_ext_kb
_mem_conv_kb: resw 1
_mem_ext_kb:  resw 1
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
global _bios_pic1, _bios_pic2
_bios_pic1:   resb 1
_bios_pic2:   resb 1
rb_ksp:       resw 1
rb_tr:        resw 1
rb_ldt:       resw 1
global _vga_ok, _vga_font8a, _vga_font8b, _vga_font16
_vga_ok:      resb 1
_vga_font8a:  resw 2                    ; Offset, Segment (ROM)
_vga_font8b:  resw 2
_vga_font16:  resw 2
km_len:       resw 2
km_off:       resw 2
km_gdt:       resb 48
kstack:      resb 1024
kstack_top:

segment _BSSEND
bss_end:
