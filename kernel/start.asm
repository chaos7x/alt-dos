; DOS 5.0 - Kernel-Einstieg und Low-Level-Routinen (80286)
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

extern gdt_build_, kmain_, fapi_dispatch_, fault_
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
        cli

        call    a20_enable
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
        mov     dx, si
        call    fapi_dispatch_          ; (fn, usel, a1, a2) -> AX
        mov     bp, sp
        mov     [bp+14], ax             ; Rueckgabewert in gesichertes AX
        popa
        pop     es
        pop     ds
        retf                            ; zurueck nach Ring 3

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
        dw irq0, irq_other, irq_other, irq_other
        dw irq_other, irq_other, irq_other, irq_other
        dw irq_other, irq_other, irq_other, irq_other
        dw irq_other, irq_other, irq_other, irq_other

tj_ptr: dw 0
tj_sel: dw 0

segment _BSS

global _boot_drive, _ticks, _uptime_s
_boot_drive: resb 1
subtick:     resb 1
_ticks:      resw 2
_uptime_s:   resw 1
global _mem_conv_kb, _mem_ext_kb
_mem_conv_kb: resw 1
_mem_ext_kb:  resw 1
kstack:      resb 1024
kstack_top:

segment _BSSEND
global _kernel_end
_kernel_end:
