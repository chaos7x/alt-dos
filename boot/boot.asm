; DOS 5.0 - Bootsektor (1,44-MB-Diskette)
;
; Laedt das System-Image (Kernel + Anwendungen) ab Sektor 2 nach
; 1000:0000 (linear 10000h) und springt im Real Mode dorthin.
; Den Wechsel in den Protected Mode erledigt der Kernel selbst.

bits 16
org 0x7C00

KSEG    equ 0x1000
NSECT   equ 128                 ; 32 KB Kernel + 4 x 8 KB Anwendungen

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

        mov     ax, KSEG
        mov     es, ax
        xor     bx, bx
        mov     cx, NSECT

.next:
        push    cx
        ; LBA -> CHS (18 Sektoren/Spur, 2 Koepfe)
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

        ; Diskettenmotor abschalten - im Protected Mode laeuft kein BIOS mehr
        mov     dx, 0x3F2
        mov     al, 0x0C
        out     dx, al

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

drive   db 0
lba     dw 1
spt     dw 18
heads   dw 2
msg_load db "DOS 5.0 wird geladen", 0
msg_err  db 13, 10, "Diskettenfehler!", 0

        times 510-($-$$) db 0
        dw 0xAA55
