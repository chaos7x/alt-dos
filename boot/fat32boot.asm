; SPDX-License-Identifier: GPL-2.0-or-later
; AT-DOS 6.0 - Bootsektor fuer FAT32-Partitionen (Festplatte)
;
; Wie hdboot.asm, aber fuer FAT32: der BPB reicht bis 59h (SYS behaelt ihn
; und ersetzt nur Sprung und Code ab 5Ah), der Stamm ist eine Clusterkette
; ab dem Stammcluster, FAT-Eintraege haben 28 Bit. Sucht ATDOS.SYS im Stamm,
; liest die Datei der FAT-Kette nach nach 1000:0000 und springt mit
; DL = Laufwerk hinein; der BPB bleibt bei 0000:7C00 (Startsektor und
; Seriennummer fuer den Kernel).
;
; FAT32 gibt es nur ab dem 386 (der Kernel braucht ihn ohnehin), daher
; 32-Bit-Register. Gelesen wird mit den LBA-Erweiterungen (INT 13h/42h),
; wenn das BIOS sie hat, sonst per CHS mit der Geometrie von INT 13h/08h.
;
; Zwei Sektoren wie bei Microsoft: Sektor 0 (Bootsektor) richtet ein und
; laedt Sektor 2 der Partition (reserviert; SYS schreibt ihn) nach
; 0000:7E00 - die Datei ist 1024 Byte lang, die zweite Haelfte ist
; Sektor 2, Adressen laufen durch.

cpu 386
bits 16
org 0x7C00

KSEG    equ 0x1000
BUF     equ 0x8000              ; Sektorpuffer (Verzeichnis, FAT)
fat_lba  equ 0x7B00             ; Variablen unterhalb des Bootsektors
data_lba equ 0x7B04
uselba   equ 0x7B08

        jmp     short start
        nop
oem         db "ALTDOS60"
bps         dw 512
spc         db 8
reserved    dw 32
nfats       db 2
rootents    dw 0
totalsec    dw 0
media       db 0xF8
spf16       dw 0
spt         dw 63
heads       dw 255
hidden      dd 0
totalsec32  dd 0
spf32       dd 0
extflags    dw 0
fsver       dw 0
rootclus    dd 2
fsinfo      dw 1
bkboot      dw 6
            times 12 db 0
drive       db 0x80
            db 0
bootsig     db 0x29
serial      dd 0
label       db "NO NAME    "
fstype      db "FAT32   "

start:                                  ; 5Ah
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7B00
        sti
        cld
        mov     [drive], dl
        mov     byte [uselba], 0

        mov     si, msg_load
        call    puts

        mov     ah, 0x08                ; Geometrie vom BIOS
        mov     dl, [drive]
        int     0x13
        jc      .geo_bpb
        and     cx, 0x3F
        mov     [spt], cx
        movzx   dx, dh
        inc     dx
        mov     [heads], dx
.geo_bpb:
        mov     ah, 0x41                ; LBA-Erweiterungen?
        mov     bx, 0x55AA
        mov     dl, [drive]
        int     0x13
        jc      .nolba
        cmp     bx, 0xAA55
        jne     .nolba
        test    cl, 1
        jz      .nolba
        mov     byte [uselba], 1
.nolba:
        mov     eax, [hidden]           ; Sektor 2 der Partition nach 7E00
        add     eax, 2
        mov     bx, 0x7E00
        call    read
        jmp     stage2

; einen Sektor lesen: EAX = LBA, ES:BX = Ziel. Erhaelt alle Register.
read:   pushad
        mov     bp, sp
        mov     di, 4                   ; Versuche
.again: mov     eax, [bp + 28]
        mov     bx, [bp + 16]
        cmp     byte [uselba], 0
        je      .chs
        push    dword 0                 ; Disk Address Packet
        push    eax
        push    es
        push    bx
        push    word 1
        push    word 16
        mov     si, sp
        mov     dl, [drive]
        mov     ah, 0x42
        int     0x13
        mov     sp, bp
        jnc     .ok
        jmp     .retry
.chs:   xor     edx, edx
        movzx   ecx, word [spt]
        div     ecx                     ; EAX = Spur, EDX = Sektor - 1
        mov     cx, dx
        inc     cx
        xor     edx, edx
        movzx   esi, word [heads]
        div     esi                     ; EAX = Zylinder, EDX = Kopf
        cmp     eax, 1023
        ja      .err
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     dh, dl
        mov     dl, [drive]
        mov     ax, 0x0201
        int     0x13
        jnc     .ok
.retry: xor     ax, ax
        mov     dl, [drive]
        int     0x13
        dec     di
        jnz     .again
.err:   mov     si, msg_err
        jmp     fail
.ok:    popad
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

msg_load  db "AT-DOS 6.0 wird geladen", 13, 10, 0
msg_err   db "Lesefehler", 0

        times 510-($-$$) db 0
        dw 0xAA55

; ---- Sektor 2 der Partition (0000:7E00) --------------------------------
stage2:
        movzx   eax, word [reserved]    ; fat_lba = hidden + reserved
        add     eax, [hidden]
        mov     [fat_lba], eax
        movzx   ecx, byte [nfats]       ; data_lba = fat_lba + nfats * spf
        mov     eax, [spf32]
        mul     ecx
        add     eax, [fat_lba]
        mov     [data_lba], eax
        test    byte [extflags], 0x80   ; Spiegeln aus: aktive FAT nehmen
        jz      .mirror
        mov     al, [extflags]
        and     eax, 0x0F
        mul     dword [spf32]
        add     [fat_lba], eax
.mirror:

        ; ATDOS.SYS im Stamm (Kette ab rootclus) suchen
        mov     eax, [rootclus]
.dclus: push    eax
        call    c2lba
        movzx   bp, byte [spc]
.dsec:  xor     bx, bx
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
        inc     eax
        dec     bp
        jnz     .dsec
        pop     eax
        call    next
        cmp     eax, 0x0FFFFFF8
        jb      .dclus
not_found:
        mov     si, msg_nokrn
        jmp     fail

found:  pop     eax                     ; Verzeichnis-Cluster weg
        mov     ax, [di + 20]           ; erster Cluster, oben
        shl     eax, 16
        mov     ax, [di + 26]           ; unten
        mov     bx, KSEG
        mov     es, bx
.clus:  push    eax
        call    c2lba
        movzx   cx, byte [spc]
.sec:   xor     bx, bx
        call    read
        mov     bx, es
        add     bx, 0x20
        mov     es, bx
        inc     eax
        loop    .sec
        pop     eax
        call    next
        cmp     eax, 0x0FFFFFF8
        jb      .clus

        mov     dl, [drive]
        jmp     KSEG:0

; EAX = Cluster -> EAX = erster Sektor (EDX, ECX geaendert)
c2lba:  sub     eax, 2
        movzx   ecx, byte [spc]
        mul     ecx
        add     eax, [data_lba]
        ret

; EAX = Cluster -> EAX = naechster Cluster (liest den FAT-Sektor nach BUF)
next:   push    es
        push    si
        push    bx
        xor     bx, bx
        mov     es, bx
        mov     si, ax
        and     si, 127
        shl     si, 2
        shr     eax, 7
        add     eax, [fat_lba]
        mov     bx, BUF
        call    read
        mov     eax, [BUF + si]
        and     eax, 0x0FFFFFFF
        pop     bx
        pop     si
        pop     es
        ret

kname     db "ATDOS   SYS"
msg_nokrn db "ATDOS.SYS fehlt", 0

        times 1024-($-$$) db 0
