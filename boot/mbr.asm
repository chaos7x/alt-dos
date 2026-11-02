; SPDX-License-Identifier: GPL-2.0-or-later
; AT-DOS 6.0 - Master Boot Record (nur fuer das Plattenabbild aus build.sh)
;
; Klassischer Ablauf: nach 0000:0600 verschieben, aktive Partition suchen,
; deren ersten Sektor nach 0000:7C00 laden und mit DL = Laufwerk, DS:SI =
; Tabelleneintrag hineinspringen. Damit funktioniert auch ein MS-DOS-
; Bootsektor in einer anderen Partition.
;
; Der Sektor wird aus der Sektornummer (LBA in der Tabelle) und der
; Geometrie des BIOS (INT 13h/08h) berechnet, nicht aus dem CHS-Feld der
; Tabelle: FDISK schreibt dort die Geometrie des Controllers, an dem
; partitioniert wurde (IDE z. B. 16/63, Adaptec 64/32). Am anderen
; Controller zeigt dasselbe CHS auf einen anderen Sektor - "Kein
; Betriebssystem". Nur wenn das BIOS keine Geometrie liefert oder der
; Zylinder ueber 1023 liegt, gilt das CHS-Feld.

cpu 286
bits 16
org 0x0600

        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        cld
        mov     si, 0x7C00
        mov     di, 0x0600
        mov     cx, 256
        rep     movsw
        jmp     0:main

main:   mov     [drive], dl
        mov     si, table
        mov     cx, 4
.find:  test    byte [si], 0x80
        jnz     .found
        add     si, 16
        loop    .find
        mov     si, msg_noact
        jmp     fail
.found: mov     [cur], si
        mov     al, [si + 1]            ; Rueckfall: CHS aus der Tabelle
        mov     [chs_h], al
        mov     ax, [si + 2]
        mov     [chs_cx], ax
        push    es
        mov     ah, 0x08                ; Geometrie des BIOS
        mov     dl, [drive]
        int     0x13
        pop     es
        jc      .read
        and     cx, 0x3F                ; Sektoren je Spur
        jz      .read
        mov     [spt], cx
        mov     dl, dh
        xor     dh, dh
        inc     dx
        mov     [heads], dx
        mov     si, [cur]
        mov     bx, [si + 8]            ; LBA, untere 16 Bit
        mov     ax, [si + 10]           ;      obere 16 Bit
        xor     dx, dx
        div     word [spt]
        xchg    ax, bx                  ; BX = Quotient oben
        div     word [spt]              ; AX = Spur unten, DX = Sektor - 1
        inc     dx
        mov     cx, dx                  ; CL = Sektor (1-63)
        mov     dx, bx                  ; DX:AX = Spur
        cmp     dx, [heads]
        jae     .read                   ; Zylinder zu gross
        div     word [heads]            ; AX = Zylinder, DX = Kopf
        cmp     ax, 1024
        jae     .read
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     [chs_cx], cx
        mov     [chs_h], dl
.read:  mov     si, [cur]
        mov     di, 4                   ; Versuche
.again: mov     bx, 0x7C00
        mov     dh, [chs_h]             ; Kopf
        mov     cx, [chs_cx]            ; Sektor/Zylinder
        mov     dl, [drive]
        mov     ax, 0x0201
        int     0x13
        jnc     .ok
        xor     ax, ax
        mov     dl, [drive]
        int     0x13
        dec     di
        jnz     .again
        mov     si, msg_err
        jmp     fail
.ok:    cmp     word [0x7DFE], 0xAA55
        mov     si, msg_nosys
        jne     fail
        mov     si, [cur]
        mov     dl, [drive]             ; Laufwerk vom BIOS
        jmp     0:0x7C00

fail:   lodsb
        test    al, al
        jz      .h
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     fail
.h:     hlt
        jmp     .h

cur       dw 0
spt       dw 0
heads     dw 0
chs_cx    dw 0
chs_h     db 0
drive     db 0x80
msg_noact db "Keine aktive Partition", 0
msg_err   db "Lesefehler", 0
msg_nosys db "Kein Betriebssystem", 0

        times 0x1BE-($-$$) db 0
table:  times 64 db 0
        dw 0xAA55
