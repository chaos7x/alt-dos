; SPDX-License-Identifier: GPL-2.0-or-later
; ATSYS.COM - AT-DOS auf eine FAT16-Partition bringen (laeuft unter MS-DOS)
;
;   A:\>ATSYS D:
;
; Wie SYS: kopiert alle Dateien des aktuellen Laufwerks (der AT-DOS-
; Diskette) ins Stammverzeichnis des Ziels, auch die versteckte ATDOS.SYS,
; dazu \DOS, \DOS\SYSTEM und \DEMO, und ersetzt danach den Code im Bootsektor der Partition. Der BPB (Bytes
; 3..61) bleibt stehen. Verweigert Laufwerke mit IO.SYS, MSDOS.SYS oder
; IBMBIO.COM, damit eine MS-DOS-Startplatte nicht unbrauchbar wird.
;
; Danach muss die Partition aktiv sein (FDISK) und die Platte die erste
; Festplatte des BIOS sein, damit das BIOS von ihr startet.
;
; Braucht MS-DOS ab 4.0 (INT 25h/26h mit Paket fuer Partitionen > 32 MB).

cpu 286
bits 16
org 0x100

        cld
        mov     [pkt_seg], cs
        mov     dx, msg_title
        call    print
        ; Ziel-Laufwerk aus der Befehlszeile
        mov     si, 0x81
.skip:  lodsb
        cmp     al, ' '
        je      .skip
        cmp     al, 9
        je      .skip
        and     al, 0xDF
        cmp     al, 'C'
        jb      usage
        cmp     al, 'Z'
        ja      usage
        cmp     byte [si], ':'
        jne     usage
        mov     [dst_letter], al
        mov     [chk_path], al
        sub     al, 'A'
        mov     [dst_drive], al

        mov     ah, 0x30                ; DOS-Version >= 4
        int     0x21
        cmp     al, 4
        jb      err_dos

        mov     ah, 0x19                ; Quelle = aktuelles Laufwerk
        int     0x21
        cmp     al, [dst_drive]
        je      usage
        add     al, 'A'
        mov     [src_path], al
        mov     [pat_buf], al
        mov     [src_letter], al

        mov     ah, 0x1A                ; eigener DTA (80h ist die Befehlszeile)
        mov     dx, dta
        int     0x21

        ; MS-DOS-Startplatte? Dann nicht anfassen.
        mov     si, sysnames
.chk:   cmp     byte [si], 0
        je      .chk_ok
        mov     di, chk_path + 3
.cpy:   lodsb
        stosb
        test    al, al
        jnz     .cpy
        mov     ah, 0x4E
        mov     cx, 0x07
        mov     dx, chk_path
        int     0x21
        jnc     err_msdos
        jmp     .chk
.chk_ok:
        ; Bootsektor lesen und pruefen
        mov     al, [dst_drive]
        mov     cx, 0xFFFF
        mov     bx, packet
        int     0x25
        pop     dx                      ; INT 25h laesst die Flags liegen
        jc      err_read
        cmp     word [bootbuf + 11], 512
        jne     err_fs
        cmp     byte [bootbuf + 0x26], 0x29
        jne     err_fs
        mov     si, bootbuf + 0x36
        mov     di, fat16
        mov     cx, 8
        repe    cmpsb
        jne     err_fs

        ; Dateien kopieren
        mov     ah, 0x4E
        mov     cx, 0x07                ; auch versteckt, System, schreibgeschuetzt
        mov     dx, src_path
        int     0x21
        jc      err_nofiles
.file:  call    copy_file
        mov     ah, 0x4F
        int     0x21
        jnc     .file

        ; Unterverzeichnisse \DOS, \DOS\SYSTEM, \DEMO (Reihenfolge: Eltern zuerst)
        mov     si, subdirs
.sd:    cmp     byte [si], 0
        je      .nodos
        mov     [pfx_ptr], si
        mov     di, si                  ; Laenge des Praefixes "DOS\..."
        xor     cx, cx
.len:   cmp     byte [di], 0
        je      .lend
        inc     di
        inc     cx
        jmp     .len
.lend:  mov     [subdir], cl
        mov     di, pat_buf + 3         ; "A:\" + Praefix + "*.*"
        push    cx
        rep     movsb
        mov     si, star
        movsw
        movsw
        pop     cx
        mov     si, [pfx_ptr]           ; "C:\" + Praefix ohne letztes "\"
        mov     di, dst_dir + 3
        dec     cx
        rep     movsb
        mov     byte [di], 0
        mov     al, [dst_letter]
        mov     [dst_dir], al
        mov     ah, 0x4E
        mov     cx, 0x07
        mov     dx, pat_buf
        int     0x21
        jc      .sdnext                 ; gibt es auf der Quelle nicht
        mov     ah, 0x39                ; Zielverzeichnis anlegen; gibt es schon: egal
        mov     dx, dst_dir
        int     0x21
        mov     ah, 0x4E
        mov     cx, 0x07
        mov     dx, pat_buf
        int     0x21
.dfile: call    copy_file
        mov     ah, 0x4F
        int     0x21
        jnc     .dfile
.sdnext:
        mov     si, [pfx_ptr]
        xor     ah, ah
        mov     al, [subdir]
        add     si, ax
        inc     si                      ; hinter die 0
        jmp     .sd
.nodos: mov     ah, 0x0D                ; alle Puffer auf die Platte
        int     0x21

        ; neuen Code in den Bootsektor, BPB bleibt
        mov     si, hdboot
        mov     di, bootbuf
        movsw
        movsb
        mov     si, hdboot + 0x3E
        mov     di, bootbuf + 0x3E
        mov     cx, 512 - 0x3E
        rep     movsb
        mov     al, [dst_drive]
        mov     cx, 0xFFFF
        mov     bx, packet
        int     0x26
        pop     dx
        jc      err_write

        mov     dx, msg_done
        call    print
        mov     ax, 0x4C00
        int     0x21

; Datei aus dem DTA kopieren: Quelle X:\NAME -> Ziel Y:\NAME
copy_file:
        mov     di, src_name
        mov     bx, dst_name
        mov     cl, [subdir]            ; 0 oder Laenge des Praefixes
        mov     si, [pfx_ptr]
.p:     test    cl, cl
        jz      .np
        lodsb
        stosb
        mov     [bx], al
        inc     bx
        dec     cl
        jmp     .p
.np:    mov     si, dta + 0x1E
.n:     lodsb
        stosb
        mov     [bx], al
        inc     bx
        test    al, al
        jnz     .n
        mov     dx, src_file            ; Ausgabe "  NAME"
        mov     si, src_name
        call    show_name

        mov     ax, 0x3D00
        mov     dx, src_file
        int     0x21
        jc      err_copy
        mov     [hsrc], ax
        mov     ax, 0x4301              ; vorhandenes Ziel beschreibbar machen
        xor     cx, cx
        mov     dx, dst_file
        int     0x21
        mov     ah, 0x3C
        xor     cx, cx
        mov     dx, dst_file
        int     0x21
        jc      err_copy
        mov     [hdst], ax
.loop:  mov     ah, 0x3F
        mov     bx, [hsrc]
        mov     cx, COPYBUF
        mov     dx, copybuf
        int     0x21
        jc      err_copy
        test    ax, ax
        jz      .eof
        mov     cx, ax
        mov     ah, 0x40
        mov     bx, [hdst]
        mov     dx, copybuf
        int     0x21
        jc      err_copy
        cmp     ax, cx
        jne     err_copy
        jmp     .loop
.eof:   mov     ah, 0x3E
        mov     bx, [hdst]
        int     0x21
        mov     ah, 0x3E
        mov     bx, [hsrc]
        int     0x21
        mov     ax, 0x4301              ; Attribute uebernehmen
        xor     ch, ch
        mov     cl, [dta + 0x15]
        and     cl, 0x27
        mov     dx, dst_file
        int     0x21
        ret

show_name:
        mov     dx, msg_indent
        call    print
.c:     lodsb
        test    al, al
        jz      .e
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     .c
.e:     mov     dx, msg_crlf
        jmp     print

print:  mov     ah, 0x09
        int     0x21
        ret

usage:      mov     dx, msg_usage
            jmp     fail
err_dos:    mov     dx, msg_dos
            jmp     fail
err_msdos:  mov     dx, msg_msdos
            jmp     fail
err_read:   mov     dx, msg_read
            jmp     fail
err_fs:     mov     dx, msg_fs
            jmp     fail
err_nofiles: mov    dx, msg_nofiles
            jmp     fail
err_copy:   mov     dx, msg_copy
            jmp     fail
err_write:  mov     dx, msg_write
fail:       call    print
            mov     ax, 0x4C01
            int     0x21

; ---------------------------------------------------------------------------
COPYBUF     equ 4096

msg_title   db "ATSYS - AT-DOS 5.2 auf eine Festplatte einrichten", 13, 10, '$'
msg_usage   db "Aufruf von der AT-DOS-Diskette:  ATSYS Laufwerk:   (z. B. ATSYS D:)", 13, 10, '$'
msg_dos     db "Ben", 0x94, "tigt MS-DOS 4.0 oder neuer.", 13, 10, '$'
msg_msdos   db "Auf dem Ziel liegt ein MS-DOS - abgebrochen.", 13, 10, '$'
msg_read    db "Bootsektor nicht lesbar.", 13, 10, '$'
msg_fs      db "Ziel ist keine FAT16-Partition.", 13, 10, '$'
msg_nofiles db "Keine Dateien auf dem Quellaufwerk.", 13, 10, '$'
msg_copy    db 13, 10, "Fehler beim Kopieren - Bootsektor nicht ge", 0x84, "ndert.", 13, 10, '$'
msg_write   db "Bootsektor nicht schreibbar.", 13, 10, '$'
msg_done    db "Fertig. Die Partition muss aktiv sein (FDISK) und die Platte", 13, 10
            db "die erste Festplatte im BIOS, dann startet AT-DOS von ihr.", 13, 10, '$'
msg_indent  db "  ", '$'
msg_crlf    db 13, 10, '$'
fat16       db "FAT16   "
sysnames    db "IO.SYS", 0, "MSDOS.SYS", 0, "IBMBIO.COM", 0, 0

src_path    db "A:\*.*", 0
chk_path    db "C:\", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
src_file:
src_letter  db "A:\"
src_name    times 26 db 0
dst_file:
dst_letter  db "C:\"
dst_name    times 26 db 0
subdirs     db "DOS\", 0, "DOS\SYSTEM\", 0, "DEMO\", 0, 0
star        db "*.*", 0
pat_buf     db "A:\", 0
            times 20 db 0
dst_dir     db "C:\", 0
            times 20 db 0
pfx_ptr     dw 0
subdir      db 0

dst_drive   db 0
hsrc        dw 0
hdst        dw 0

packet      dd 0                    ; Sektor 0 der Partition
            dw 1
            dw bootbuf
pkt_seg     dw 0

hdboot:     incbin "build/hdboot.bin"

section .bss
bootbuf     resb 512
dta         resb 128
copybuf     resb COPYBUF
