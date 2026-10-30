; AT-DOS 5.0 - Master Boot Record (nur fuer das Plattenabbild aus build.sh)
;
; Klassischer Ablauf: nach 0000:0600 verschieben, aktive Partition suchen,
; deren ersten Sektor (CHS aus der Partitionstabelle) nach 0000:7C00 laden
; und mit DL = Laufwerk, DS:SI = Tabelleneintrag hineinspringen. Damit
; funktioniert auch ein MS-DOS-Bootsektor in einer anderen Partition.

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

main:   mov     si, table
        mov     cx, 4
.find:  test    byte [si], 0x80
        jnz     .found
        add     si, 16
        loop    .find
        mov     si, msg_noact
        jmp     fail
.found: mov     [cur], si
        mov     di, 4                   ; Versuche
.again: mov     bx, 0x7C00
        mov     dh, [si + 1]            ; Kopf
        mov     cx, [si + 2]            ; Sektor/Zylinder
        mov     ax, 0x0201
        int     0x13
        jnc     .ok
        xor     ax, ax
        int     0x13
        dec     di
        jnz     .again
        mov     si, msg_err
        jmp     fail
.ok:    cmp     word [0x7DFE], 0xAA55
        mov     si, msg_nosys
        jne     fail
        mov     si, [cur]               ; DL = Laufwerk vom BIOS
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
msg_noact db "Keine aktive Partition", 0
msg_err   db "Lesefehler", 0
msg_nosys db "Kein Betriebssystem", 0

        times 0x1BE-($-$$) db 0
table:  times 64 db 0
        dw 0xAA55
