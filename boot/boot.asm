; SPDX-License-Identifier: GPL-2.0-or-later
; AT-DOS 5.0 - Bootsektor (1,44-MB-Diskette, FAT12)
;
; Die Diskette ist ein normales FAT12-Dateisystem. Der Kernel ist die Datei
; ATDOS.SYS (versteckt, System) und liegt - wie einst IO.SYS - als erste
; Datei zusammenhaengend am Anfang des Datenbereichs. Dieser Lader holt sie
; nach 1000:0000 (linear 10000h) und springt im Real Mode dorthin.
; Programme liegen als Dateien im Dateisystem und laedt der Kernel selbst.

bits 16
org 0x7C00

KSEG    equ 0x1000
; Sektorzahl aus der Dateigroesse im Verzeichniseintrag (frueher fest 160
; = 80 KB, 5.1 wuchs darueber); je Sektor ES:0 - keine DMA-Grenze innerhalb
; eines Sektors

        jmp     short start
        nop

; BIOS-Parameterblock (Standard-1,44-MB-Diskette)
oem         db "ALTDOS50"
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
serial      dd 0x19890501
label       db "ALT-DOS    "
fstype      db "FAT12   "

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        cld
        mov     [drive], dl

        ; Stammverzeichnis beginnt nach reserviert + FATs
        mov     al, [nfats]
        xor     ah, ah
        mul     word [spf]
        add     ax, [reserved]
        mov     [lba], ax
        push    ax                      ; Anfang des Stammverzeichnisses
        ; ersten Sektor des Stammverzeichnisses nach 0050:0000 lesen und
        ; ATDOS.SYS unter den ersten 16 Eintraegen suchen - sonst ist es
        ; keine Systemdiskette (FORMAT ohne /S)
        mov     ax, 0x0050
        mov     es, ax
        xor     bx, bx
        call    read1
        mov     cx, 16
        xor     bx, bx
.find:  push    cx
        mov     si, kname
        mov     di, bx                  ; ES = 0050h
        mov     cx, 11
        repe    cmpsb
        pop     cx
        je      .found
        add     bx, 32
        loop    .find
        mov     si, msg_nosys
        call    puts
        xor     ax, ax                  ; Taste, dann neu starten
        int     0x16
        int     0x19
.found:
        mov     ax, [es:bx + 28]        ; Dateigroesse -> Sektoren
        mov     dx, [es:bx + 30]
        add     ax, 511
        adc     dx, 0
        mov     cx, 9
.div:   shr     dx, 1
        rcr     ax, 1
        loop    .div
        mov     [nsect], ax
        mov     si, msg_load
        call    puts
        ; erster Datensektor = Stammverzeichnis + seine Groesse
        pop     ax
        mov     bx, [rootents]
        shr     bx, 4                   ; 16 Eintraege je Sektor
        add     ax, bx
        mov     [lba], ax

        mov     ax, KSEG
        mov     es, ax
        xor     bx, bx
        mov     cx, [nsect]
.next:  push    cx
        call    read1
        mov     ax, es
        add     ax, 0x20                ; 512 Bytes weiter
        mov     es, ax
        test    byte [lba], 7
        jnz     .noprog
        mov     al, '.'
        call    putc
.noprog:
        pop     cx
        loop    .next

        mov     dl, [drive]
        jmp     KSEG:0

; Sektor [lba] nach ES:0 lesen, danach lba + 1
read1:  mov     ax, [lba]
        xor     dx, dx
        div     word [spt]              ; AX = Spur, DX = Sektor-Index
        mov     cl, dl
        inc     cl                      ; Sektor ab 1
        xor     dx, dx
        div     word [heads]            ; AX = Zylinder, DX = Kopf
        mov     ch, al
        mov     dh, dl
        mov     dl, [drive]
        mov     si, 4                   ; Wiederholungen
.retry: mov     ax, 0x0201
        xor     bx, bx
        int     0x13
        jnc     .ok
        xor     ax, ax                  ; Laufwerk zuruecksetzen
        int     0x13
        dec     si
        jnz     .retry
        jmp     disk_error
.ok:    inc     word [lba]
        ret

disk_error:
        mov     si, msg_err
        call    puts
.halt:  hlt
        jmp     .halt

puts:   lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     puts
.done:  ret

putc:   push    bx
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        pop     bx
        ret

lba     dw 1
nsect   dw 0
msg_load db "AT-DOS 5.1 wird geladen", 0
msg_err  db 13, 10, "Diskettenfehler!", 0
msg_nosys db "Keine Systemdiskette - wechseln und Taste dr", 0x81, "cken", 13, 10, 0
kname    db "ATDOS   SYS"

        times 510-($-$$) db 0
        dw 0xAA55
