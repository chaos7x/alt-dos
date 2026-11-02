; SPDX-License-Identifier: GPL-2.0-or-later
; AT-DOS 6.0 - Bootsektor fuer FAT16-Partitionen (Festplatte)
;
; Wird vom MBR nach 0000:7C00 geladen, DL = BIOS-Laufwerk. Der BPB (Bytes
; 3..61) gehoert dem Dateisystem: ATSYS.COM bzw. tools/sysboot.py
; uebernehmen ihn aus dem vorhandenen Bootsektor und ersetzen nur den Code.
;
; Sucht ATDOS.SYS im Stammverzeichnis, liest die Datei der FAT-Kette nach
; (sie muss nicht zusammenhaengend liegen) nach 1000:0000 und springt mit
; DL = Laufwerk hinein. Der BPB bleibt bei 0000:7C00 stehen; der Kernel
; liest dort Startsektor und Seriennummer der Partition.
;
; Gelesen wird per CHS (INT 13h/02h), Geometrie vom BIOS (INT 13h/08h) -
; das SCAT-BIOS von 1990 kennt keine LBA-Erweiterungen. Damit muss
; ATDOS.SYS unterhalb von Zylinder 1024 liegen.

cpu 286
bits 16
org 0x7C00

KSEG    equ 0x1000
BUF     equ 0x7E00              ; Sektorpuffer (Verzeichnis, FAT)

        jmp     short start
        nop
oem         db "ALTDOS60"
bps         dw 512
spc         db 4
reserved    dw 1
nfats       db 2
rootents    dw 512
totalsec    dw 0
media       db 0xF8
spf         dw 0
spt         dw 63
heads       dw 16
hidden      dd 0
totalsec32  dd 0
drive       db 0x80
            db 0
bootsig     db 0x29
serial      dd 0
label       db "AT-DOS     "
fstype      db "FAT16   "

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        cld
        mov     [drive], dl

        mov     si, msg_load
        call    puts

        mov     ah, 0x08                ; Geometrie vom BIOS
        int     0x13
        jc      .geo_bpb
        and     cx, 0x3F
        mov     [spt], cx
        mov     dl, dh
        xor     dh, dh
        inc     dx
        mov     [heads], dx
.geo_bpb:
        ; fat_lba = hidden + reserved
        mov     ax, [hidden]
        mov     dx, [hidden + 2]
        add     ax, [reserved]
        adc     dx, 0
        mov     [fat_lba], ax
        mov     [fat_lba + 2], dx
        ; root_lba = fat_lba + nfats * spf
        mov     cl, [nfats]
.fats:  add     ax, [spf]
        adc     dx, 0
        dec     cl
        jnz     .fats
        mov     [root_lba], ax
        mov     [root_lba + 2], dx
        ; data_lba = root_lba + rootents / 16
        mov     bx, [rootents]
        shr     bx, 4
        add     ax, bx
        adc     dx, 0
        mov     [data_lba], ax
        mov     [data_lba + 2], dx

        ; ATDOS.SYS im Stammverzeichnis suchen
        mov     ax, [root_lba]
        mov     dx, [root_lba + 2]
        mov     bp, [rootents]
        shr     bp, 4                   ; Sektoren des Stammverzeichnisses
.dirsec:
        xor     bx, bx
        mov     es, bx
        mov     bx, BUF
        call    read
        mov     di, BUF
.entry: cmp     byte [di], 0
        je      not_found
        push    di
        mov     si, kname
        mov     cx, 11
        repe    cmpsb
        pop     di
        je      found
        add     di, 32
        cmp     di, BUF + 512
        jb      .entry
        add     ax, 1
        adc     dx, 0
        dec     bp
        jnz     .dirsec
not_found:
        mov     si, msg_nokrn
        jmp     fail

found:
        mov     ax, [di + 26]           ; erster Cluster
        mov     bx, KSEG
        mov     es, bx
.clus:
        push    ax
        ; LBA = data_lba + (Cluster - 2) * spc
        sub     ax, 2
        xor     ch, ch
        mov     cl, [spc]
        mul     cx
        add     ax, [data_lba]
        adc     dx, [data_lba + 2]
.sec:   xor     bx, bx
        call    read
        mov     bx, es
        add     bx, 0x20
        mov     es, bx
        add     ax, 1
        adc     dx, 0
        loop    .sec
        pop     ax
        ; naechster Cluster: FAT-Sektor fat_lba + Cluster/256
        push    es
        xor     bx, bx
        mov     es, bx
        mov     si, ax
        and     si, 0xFF
        shl     si, 1
        mov     al, ah
        xor     ah, ah
        xor     dx, dx
        add     ax, [fat_lba]
        adc     dx, [fat_lba + 2]
        mov     bx, BUF
        call    read
        pop     es
        mov     ax, [BUF + si]
        cmp     ax, 0xFFF8
        jb      .clus

        mov     dl, [drive]
        jmp     KSEG:0

; einen Sektor lesen: DX:AX = LBA, ES:BX = Ziel. Erhaelt alle Register.
read:   pusha
        mov     di, 4                   ; Versuche
.again: pusha
        xchg    ax, cx                  ; LBA / spt in zwei Schritten, sonst
        mov     ax, dx                  ; laeuft DIV ab 2 GB ueber
        xor     dx, dx
        div     word [spt]
        xchg    ax, cx                  ; CX = oberer Teil der Spur
        div     word [spt]              ; AX = Spur (unten), DX = Sektor
        inc     dx
        push    dx
        mov     dx, cx
        div     word [heads]            ; AX = Zylinder, DX = Kopf
        pop     cx                      ; CL = Sektor ab 1
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     dh, dl
        mov     dl, [drive]
        mov     ax, 0x0201
        int     0x13
        popa
        jnc     .ok
        pusha
        xor     ax, ax
        mov     dl, [drive]
        int     0x13
        popa
        dec     di
        jnz     .again
        mov     si, msg_err
        jmp     fail
.ok:    popa
        ret

fail:   call    puts
.h:     hlt
        jmp     .h

puts:   lodsb
        test    al, al
        jz      .done
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     puts
.done:  ret

kname     db "ATDOS   SYS"
msg_load  db "AT-DOS 6.0 wird geladen", 13, 10, 0
msg_nokrn db "ATDOS.SYS fehlt", 0
msg_err   db "Lesefehler", 0

fat_lba  equ 0x7B00             ; Variablen unterhalb des Bootsektors
root_lba equ 0x7B04
data_lba equ 0x7B08

        times 510-($-$$) db 0
        dw 0xAA55
