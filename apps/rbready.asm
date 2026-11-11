; SPDX-License-Identifier: GPL-2.0-or-later
; RBREADY.COM - meldet der Kompatibilitaetsbox von AT-DOS 5.1 "bereit"
;
; Mit PROTECTONLY=NO startet AT-DOS die Box gleich beim Hochfahren (wie
; OS/2 1.x). RBSHELL ruft COMMAND.COM dann einmal mit /K RBREADY.COM auf:
; steht der Prompt, schaltet INT E0h AH = 17h zurueck zu AT-DOS, die Box
; wartet im Hintergrund. Ohne INT E0h (Vektor 0) tut es nichts.
cpu 8086
org 0x100
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0xE0 * 4]
        or      ax, [es:0xE0 * 4 + 2]
        jz      .ende
        mov     ah, 0x17
        int     0xE0
.ende:  mov     ax, 0x4C00
        int     0x21
