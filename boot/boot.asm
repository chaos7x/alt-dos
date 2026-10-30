; DOS 5.0 - Bootsektor (1,44-MB-Diskette, FAT12)
;
; Die Diskette ist ein normales FAT12-Dateisystem. Der Kernel ist die Datei
; KERNEL.SYS (versteckt, System) und liegt - wie einst IO.SYS - als erste
; Datei zusammenhaengend am Anfang des Datenbereichs. Dieser Lader holt sie
; nach 1000:0000 (linear 10000h) und springt im Real Mode dorthin.
; Programme liegen als Dateien im Dateisystem und laedt der Kernel selbst.

bits 16
org 0x7C00

KSEG    equ 0x1000
NSECT   equ 64                  ; KERNEL.SYS ist auf 32 KB aufgefuellt

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

        mov     si, msg_load
        call    puts

        ; erster Datensektor = reserviert + FATs + Stammverzeichnis
        mov     al, [nfats]
        xor     ah, ah
        mul     word [spf]
        add     ax, [reserved]
        mov     bx, [rootents]
        shr     bx, 4                   ; 16 Eintraege je Sektor
        add     ax, bx
        mov     [lba], ax

        mov     ax, KSEG
        mov     es, ax
        xor     bx, bx
        mov     cx, NSECT

.next:
        push    cx
        ; LBA -> CHS
        mov     ax, [lba]
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
.retry:
        mov     ax, 0x0201
        int     0x13
        jnc     .ok
        xor     ax, ax                  ; Laufwerk zuruecksetzen
        int     0x13
        dec     si
        jnz     .retry
        jmp     disk_error
.ok:
        mov     ax, es
        add     ax, 0x20                ; 512 Bytes weiter
        mov     es, ax
        inc     word [lba]
        test    byte [lba], 7
        jnz     .noprog
        mov     al, '.'
        call    putc
.noprog:
        pop     cx
        loop    .next

        mov     dl, [drive]
        jmp     KSEG:0

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
msg_load db "DOS 5.0 wird geladen", 0
msg_err  db 13, 10, "Diskettenfehler!", 0

        times 510-($-$$) db 0
        dw 0xAA55
