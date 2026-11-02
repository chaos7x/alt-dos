; SPDX-License-Identifier: GPL-2.0-or-later
; DOS-Stub fuer FAPI-Anwendungen
; Laeuft nur, wenn jemand das Programm unter Real-Mode-DOS startet.

bits 16
org 0

        push    cs
        pop     ds
        mov     dx, msg
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C01
        int     0x21

msg     db "Diese FAPI-Anwendung kann nicht im Real Mode ausgef", 0x81, "hrt werden.", 13, 10, "$"
