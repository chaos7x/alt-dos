; SPDX-License-Identifier: GPL-2.0-or-later
; RBSHELL.COM - SHELL= der Kompatibilitaetsbox von AT-DOS 5.1
;
; Startet FreeCOM (A:\COMMAND.COM) und beendet nach dessen EXIT die Box
; mit seinem Rueckgabewert (INT E0h AH = 01h). Ohne diesen Umweg meldete
; FreeDOS nach EXIT "Bad or missing Command Interpreter".
cpu 8086
org 0x100

start:
        mov     sp, stack_top
        mov     bx, (stack_top - start + 0x100 + 15) / 16  ; Rest frei
        mov     ah, 0x4A
        int     0x21
        mov     ax, cs
        mov     [pb_tail + 2], ax
        mov     [pb_fcb1 + 2], ax
        mov     [pb_fcb2 + 2], ax
        mov     dx, shell
        mov     bx, pblock
        mov     ax, 0x4B00
        int     0x21
        mov     al, 0xFF
        jc      .done
        mov     ah, 0x4D
        int     0x21
.done:  mov     ah, 1                           ; Box beenden, AL = Code
        int     0xE0
        jmp     $

shell:  db      "A:\COMMAND.COM", 0
tail:   db      12, " A:\ /E:1024", 13
fcb:    db      0, "           ", 0, 0, 0, 0
pblock: dw      0                               ; Umgebung des Aufrufers
pb_tail: dw     tail, 0
pb_fcb1: dw     fcb, 0
pb_fcb2: dw     fcb, 0

        align   2
        times   256 db 0
stack_top:
