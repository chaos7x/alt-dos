; SPDX-License-Identifier: GPL-2.0-or-later
; RBFPU.COM - Test fuer die Kompatibilitaetsbox (5.1): 287 ueber Alt+Esc.
; Laedt 1.0, zeigt "W" und wartet auf eine Taste (dazwischen Alt+Esc hin
; und zurueck), addiert 1.0 und zeigt das Ergebnis: "2" = Zustand heil,
; "1" = verloren. Braucht einen Koprozessor.
cpu 8086
org 0x100
        fninit
        fld1
        mov     ah, 0x0E
        mov     al, 'W'
        int     0x10
        xor     ax, ax
        int     0x16            ; hier Alt+Esc hin und zurueck
        fld1
        faddp   st1, st0
        fistp   word [x]
        fwait
        mov     ax, [x]
        add     al, '0'
        mov     ah, 0x0E
        int     0x10
        xor     ax, ax
        int     0x16
        ret
x:      dw      0
