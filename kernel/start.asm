; AT-DOS 5.0 - Kernel-Einstieg und Low-Level-Routinen (80286)
;
; Der Kernel ist ein einziges 64-KB-Segment (Code + Daten in DGROUP),
; geladen bei linear 10000h. Im Real Mode ist CS=DS=1000h, im Protected
; Mode zeigen KCODE/KDATA auf dieselbe Basis - alle Offsets bleiben gleich.
;
; Aufrufkonvention zu C: Watcom-Registerkonvention (AX, DX, BX, CX),
; aufgerufene Routinen erhalten alle Register ausser AX (bzw. DX:AX).

cpu 286

KCODE   equ 0x08
KDATA   equ 0x10

segment _TEXT  public class=CODE align=1 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
segment _BSSEND public class=BSS align=2 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS _BSSEND

extern preload_
extern gdt_build_, kmain_, fapi_dispatch_, fault_, kbd_irq_, devhlp_dispatch_
extern _gdtr, _idtr

; ---------------------------------------------------------------------------
segment _TEXT

..start:
global _start
_start:
        ; Real Mode, vom Bootsektor bei 1000:0000 angesprungen
        cli
        mov     ax, cs
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
        mov     ax, cs
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
EXC 7
EXC_ERR 8
EXC 9
EXC_ERR 10
EXC_ERR 11
EXC_ERR 12
EXC_ERR 13
EXC 14
EXC 15
EXC 16

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

; IRQ 6: Diskettencontroller fertig
irq6:
        push    ax
        push    ds
        mov     ax, KDATA
        mov     ds, ax
        mov     byte [_fdc_irq], 1
        mov     al, 0x20
        out     0x20, al
        pop     ds
        pop     ax
        iret

; IRQ 1: Tastatur
irq1:
        pusha
        push    ds
        push    es
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        in      al, 0x60
        xor     ah, ah
        call    kbd_irq_
        mov     al, 0x20
        out     0x20, al
        pop     es
        pop     ds
        popa
        iret

; alle anderen IRQs: maskiert, nur fuer den Fall der Faelle
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
        mov     [_devhlp_dx], dx
        mov     dx, si
        call    devhlp_dispatch_
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
segment _DATA

global _isr_table
_isr_table:
        dw exc0, exc1, exc2, exc3, exc4, exc5, exc6, exc7
        dw exc8, exc9, exc10, exc11, exc12, exc13, exc14, exc15
        dw exc16, 0, 0, 0, 0, 0, 0, 0
        dw 0, 0, 0, 0, 0, 0, 0, 0
        dw irq0, irq1, irq_other, irq_other
        dw irq_other, irq_other, irq6, irq_other
        dw irq_other, irq_other, irq_other, irq_other
        dw irq_other, irq_other, irq_other, irq_other

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
global _fdc_irq
_fdc_irq:    resb 1
global _mem_conv_kb, _mem_ext_kb
_mem_conv_kb: resw 1
_mem_ext_kb:  resw 1
kstack:      resb 1024
kstack_top:

segment _BSSEND
bss_end:
