; SPDX-License-Identifier: GPL-2.0-or-later
; AT-DOS - Startsektor fuer DOS-Abbilder der Box (MKDOSIMG)
;
; Startet MS-DOS 4.0-6.22 bzw. PC DOS 4-7 aus einem Diskettenabbild, das
; MKDOSIMG anlegt: IO.SYS/IBMBIO.COM liegt als erste Datei zusammen-
; haengend am Anfang des Datenbereichs. Eigene Umsetzung des Lade-
; protokolls dieser DOS-Versionen (nachgelesen in MSBOOT.ASM/MSLOAD.ASM
; von MS-DOS 4.0, MIT): die ersten 3 Sektoren von IO.SYS nach 0070:0000,
; Sprung dorthin mit
;   CH = Media-Byte, DL = Laufwerk (BPB 24h), AX:BX = erster Datensektor,
;   DS:SI = Diskettenparameter (INT 1Eh)
; IO.SYS liest den Rest selbst (BPB bei 0:7C00). So braucht MKDOSIMG den
; Startsektor der Quelle nicht - Quelle kann auch eine Festplatte sein.

bits 16
cpu 8086
org 0x7C00

BIOSSEG equ 0x70
LOADN   equ 3

        jmp     short start
        nop
oem         db "MSDOS5.0"
bps         dw 512
spc         db 1
reserved    dw 1
nfats       db 2
rootents    dw 224
totalsec    dw 2880
media       db 0xF0
spf         dw 9
spt         dw 18
heads       dw 2
hidden      dd 0
totalsec32  dd 0
drive       db 0
            db 0
            db 0x29
serial      dd 0
vlabel      db "MDOS       "
fstype      db "FAT12   "

start:
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        sti
        cld
        ; Stammverzeichnis: reserviert + FATs * spf + versteckt
        mov     al, [nfats]
        cbw
        mul     word [spf]
        add     ax, [reserved]
        adc     dx, 0
        add     ax, [hidden]
        adc     dx, [hidden + 2]
        mov     [dirs], ax
        mov     [dirs + 2], dx
        ; + Groesse des Stamms = erster Datensektor
        push    ax
        push    dx
        mov     ax, [rootents]
        mov     cl, 4                   ; 16 Eintraege je Sektor
        add     ax, 15
        shr     ax, cl
        mov     cx, ax
        pop     dx
        pop     ax
        add     ax, cx
        adc     dx, 0
        mov     [data], ax
        mov     [data + 2], dx
        ; Stammsektor 1 nach 0:0500, erste Eintraege pruefen
        mov     ax, [dirs]
        mov     dx, [dirs + 2]
        mov     bx, 0x0500
        push    ds
        pop     es
        call    rd
        jc      err
        cmp     byte [0x0500], ' '      ; leer/geloescht -> kein System
        jbe     err
        cmp     byte [0x0500], 0xE5
        je      err
        mov     al, [0x0500 + 11]
        test    al, 0x04                ; Systemdatei?
        jz      err
        ; 3 Sektoren IO.SYS nach 0070:0000
        mov     ax, BIOSSEG
        mov     es, ax
        xor     bx, bx
        mov     cx, LOADN
        mov     ax, [data]
        mov     dx, [data + 2]
.ld:    push    ax
        push    dx
        push    cx
        call    rd
        pop     cx
        pop     dx
        pop     ax
        jc      err
        add     ax, 1
        adc     dx, 0
        add     bx, [bps]
        loop    .ld
        ; Uebergabe
        mov     ch, [media]
        mov     dl, [drive]
        mov     bx, [data]
        mov     ax, [data + 2]
        lds     si, [0x1E * 4]
        jmp     BIOSSEG:0

err:    mov     si, msg
.p:     lodsb
        or      al, al
        jz      .w
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     short .p
.w:     xor     ax, ax
        int     0x16
        int     0x19

; DX:AX = Sektor -> ES:BX, ein Sektor (CHS aus dem BPB)
rd:     mov     cx, 3
.try:   push    cx
        push    ax
        push    dx
        div     word [spt]              ; AX = Spur*Koepfe+Kopf, DX = Sektor-1
        mov     cl, dl
        inc     cl
        xor     dx, dx
        div     word [heads]            ; AX = Zylinder, DX = Kopf
        mov     dh, dl
        mov     ch, al
        ror     ah, 1
        ror     ah, 1
        or      cl, ah
        mov     dl, [drive]
        mov     ax, 0x0201
        int     0x13
        pop     dx
        pop     ax
        pop     cx
        jnc     .ok
        push    ax
        xor     ax, ax
        int     0x13
        pop     ax
        loop    .try
        stc
.ok:    ret

msg     db "Kein DOS-Systemabbild - Taste druecken", 13, 10, 0
dirs    dd 0
data    dd 0

        times 510 - ($ - $$) db 0
        dw 0xAA55
