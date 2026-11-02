; SPDX-License-Identifier: GPL-2.0-or-later
; BOXSHELL.COM - erster Prozess der DOS-Box von AT-DOS 6.0 (SHELL= in der
; CONFIG.SYS des Startabbilds). Holt sich ueber die Box-Schnittstelle
; (INT E0h) die Befehlszeile, mit der die Box gestartet wurde, startet
; FreeCOM damit (leer: interaktiv mit AUTOEXEC.BAT, sonst /C Befehl) und
; beendet die Box mit dessen Rueckgabewert, sobald FreeCOM endet (EXIT).
;
; INT E0h (nur in AT-DOS-Boxen): AH=00h Befehlszeile nach ES:DI (bis 120
; Zeichen, ohne Null) -> CX = Laenge; AH=01h Box beenden, AL = Rueckgabewert.

cpu 8086
org 100h

        mov     sp, stack_top
        mov     bx, ((stack_top - $$) + 100h + 15) / 16
        mov     ah, 4Ah                 ; Speicher auf das Noetige kuerzen
        int     21h

        push    cs
        pop     es
        mov     di, cmd
        xor     ah, ah                  ; Befehlszeile der Box
        int     0E0h
        mov     si, t_inter             ; leer: interaktiv
        jcxz    .build
        mov     si, t_cmd
.build: mov     di, tail + 1            ; "A:\ /E:1024 " + /K bzw. /C ...
.c1:    lodsb
        test    al, al
        jz      .c2
        stosb
        jmp     .c1
.c2:    mov     si, cmd                 ; Befehl anhaengen (cx Zeichen)
        rep     movsb
        mov     al, 13
        stosb
        mov     ax, di
        sub     ax, tail + 2
        mov     [tail], al

        mov     [pb_tail+2], cs
        mov     [pb_fcb1+2], cs
        mov     [pb_fcb2+2], cs
        mov     dx, t_comspec
        mov     bx, pblock
        mov     ax, 4B00h               ; FreeCOM starten und warten
        int     21h
        mov     al, 0FFh
        jc      .exit
        mov     ah, 4Dh                 ; dessen Rueckgabewert
        int     21h
.exit:  mov     ah, 1                   ; Box beenden
        int     0E0h
        int     19h                     ; (ausserhalb einer Box: Neustart)

t_comspec db "A:\COMMAND.COM", 0
t_inter   db " A:\ /E:1024 /K A:\AUTOEXEC.BAT", 0
t_cmd     db " A:\ /E:1024 /C ", 0
pblock    dw 0                          ; Umgebung erben
pb_tail   dw tail, 0
pb_fcb1   dw fcb, 0
pb_fcb2   dw fcb, 0
fcb       times 16 db 0
tail      times 130 db 0
cmd       times 122 db 0
          times 256 db 0
stack_top:
