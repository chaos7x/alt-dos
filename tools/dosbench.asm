; SPDX-License-Identifier: GPL-2.0-or-later
;
; DOSBENCH.COM - Messprogramm fuer Stufe 0 von docs/DOSKERN.md: Zeit je
; Dateizugriff ueber reine INT-21h-Aufrufe, dazu Speicher und Version.
; Laeuft unter jedem DOS (8086), direkt wie in der DOS-Box, damit alle
; Faelle mit demselben Programm gemessen werden.
;
;   DOSBENCH Pfad\ [n]    z. B. DOSBENCH D:\ 4  (Ausgabe auf STDOUT; n = 1-9:
;                         die Tests mit vielen kleinen Aufrufen n-mal)
;   DOSBENCH /T           nur den BIOS-Takt ausgeben (Zeit um ein Programm)
;
; Im Pfad legt es BENCH\ an und raeumt es danach weg. Erwartet dort
; MANY\ mit vielen Dateien (Suche) - fehlt es, entfaellt der Test.
; Zeiten in BIOS-Takten (46Ch, 18,2 je s) und ms.

        cpu 8086
        org 100h

BUFSZ   equ 8000h                       ; 32 KB
NSMALL  equ 200                         ; kleine Dateien
SMSZ    equ 4096

start:  mov ax, cs                      ; groesstes Programm = eigener
        dec ax                          ; Block beim Laden (MCB vor PSP)
        mov es, ax
        mov ax, [es:3]
        mov [mblk], ax
        mov ah, 4Ah                     ; Block auf 64 KB
        mov bx, 1000h
        push cs
        pop es
        int 21h
        mov sp, 0FFFEh
        ; Befehlszeile -> base
        mov si, 81h
.sk:    lodsb
        cmp al, ' '
        je .sk
        cmp al, 9
        je .sk
        dec si
        mov di, base
.cp:    lodsb
        cmp al, ' '
        jbe .ce
        stosb
        jmp .cp
.ce:    mov word [nrep], 1               ; Wiederholungen (Ziffer danach)
.cr:    cmp al, ' '
        jne .cn
        lodsb
        jmp .cr
.cn:    sub al, '1'
        cmp al, 8
        ja .cq
        cbw
        inc ax
        mov [nrep], ax
.cq:    cmp di, base
        jne .c0
        mov dx, s_use
        call puts
        mov ax, 4C01h
        int 21h
.c0:    cmp word [base], '/T'           ; /T: nur Takt ausgeben
        je .tk
        cmp word [base], '/t'
        jne .c1
.tk:    mov dx, s_tick
        call puts
        call ticks
        call pnum32
        call crlf
        mov ax, 4C00h
        int 21h
.c1:    cmp byte [di-1], '\'
        je .c2
        mov al, '\'
        stosb
.c2:    mov [baseend], di
        ; Puffer fuellen
        mov di, buf
        mov cx, BUFSZ
        xor al, al
.fb:    stosb
        inc al
        loop .fb

        mov dx, s_head
        call puts
        mov ax, [nrep]
        call pnum16
        call crlf
        ; Version
        mov dx, s_ver
        call puts
        mov ah, 30h
        int 21h
        push ax
        xor ah, ah
        call pnum16
        mov dl, '.'
        call putc
        pop ax
        mov al, ah
        xor ah, ah
        call pnum2
        mov dx, s_true
        call puts
        mov ax, 3306h
        int 21h
        push bx
        mov al, bl
        xor ah, ah
        call pnum16
        mov dl, '.'
        call putc
        pop bx
        mov al, bh
        xor ah, ah
        call pnum2
        test dh, 10h
        jz .nh
        mov dx, s_hma
        call puts
.nh:    call crlf
        mov dx, s_mem
        call puts
        mov ax, [mblk]
        mov cl, 6                       ; Paragraphen -> KB
        shr ax, cl
        call pnum16
        mov dx, s_kb
        call puts

        ; Arbeitsverzeichnis
        mov si, s_dbench0
        call mkpath0
        mov ah, 39h
        mov dx, path
        int 21h

        ; T1 4 MB schreiben
        mov dx, t_w
        call tbeg
        mov si, s_big
        call mkpath
        mov ah, 3Ch
        xor cx, cx
        mov dx, path
        int 21h
        jc .err
        mov bx, ax
        mov bp, 128
.w1:    mov ah, 40h
        mov cx, BUFSZ
        mov dx, buf
        int 21h
        jc .err
        dec bp
        jnz .w1
        mov ah, 3Eh
        int 21h
        call tend

        ; T2 4 MB lesen (32 KB)
        mov dx, t_r
        call tbeg
        mov cx, BUFSZ
        mov bp, 128
        call rdbig
        call tend
        ; T3 1 MB lesen in 512 Byte
        mov dx, t_r512
        call tbeg
        mov ax, [nrep]
        mov [rcnt], ax
.x3:
        mov cx, 512
        mov bp, 2048
        call rdbig
        dec word [rcnt]
        jnz .x3
        call tend
        ; T4 2000 x Seek + 64 Byte lesen, verstreut
        mov dx, t_seek
        call tbeg
        mov ax, [nrep]
        mov [rcnt], ax
.x4:
        call opbig
        mov bp, 2000
        mov word [rnd], 1234h
.s1:    mov ax, [rnd]                   ; LCG
        mov cx, 25173
        mul cx
        add ax, 13849
        mov [rnd], ax
        mov dx, ax                      ; Stelle = (ax & 3Fh) << 16 | ax
        mov cx, ax
        and cx, 3Fh
        mov ax, 4200h
        int 21h
        mov ah, 3Fh
        mov cx, 64
        mov dx, buf
        int 21h
        jc .err
        dec bp
        jnz .s1
        mov ah, 3Eh
        int 21h
        dec word [rcnt]
        jnz .x4
        call tend

        ; T5 200 Dateien zu 4 KB anlegen
        mov dx, t_mk
        call tbeg
        xor bp, bp
.m1:    call smname
        mov ah, 3Ch
        xor cx, cx
        mov dx, path
        int 21h
        jc .err
        mov bx, ax
        mov ah, 40h
        mov cx, SMSZ
        mov dx, buf
        int 21h
        mov ah, 3Eh
        int 21h
        inc bp
        cmp bp, NSMALL
        jb .m1
        call tend
        ; T6 200 Dateien oeffnen, lesen, schliessen
        mov dx, t_rd
        call tbeg
        mov ax, [nrep]
        mov [rcnt], ax
.x6:
        xor bp, bp
.r1:    call smname
        mov ax, 3D00h
        mov dx, path
        int 21h
        jc .err
        mov bx, ax
        mov ah, 3Fh
        mov cx, SMSZ
        mov dx, buf
        int 21h
        mov ah, 3Eh
        int 21h
        inc bp
        cmp bp, NSMALL
        jb .r1
        dec word [rcnt]
        jnz .x6
        call tend
        ; T7 1000 x Attribut abfragen
        mov dx, t_attr
        call tbeg
        mov ax, [nrep]
        mov [rcnt], ax
.x7:
        mov bp, 1000
        mov si, s_big
        call mkpath
.a1:    mov ax, 4300h
        mov dx, path
        int 21h
        jc .err
        dec bp
        jnz .a1
        dec word [rcnt]
        jnz .x7
        call tend
        ; T8 Suche ueber MANY\*.* (3 Durchgaenge)
        mov ah, 1Ah
        mov dx, dta
        int 21h
        mov si, s_many
        call mkpath0                    ; ohne Unterverzeichnis BENCH
        mov ax, 4E00h
        xor cx, cx
        mov dx, path
        int 21h
        jc .nomany
        mov dx, t_find
        call tbeg
        mov ax, [nrep]
        mov [rcnt], ax
.x8:
        mov bp, 3
        mov word [cnt], 0
.f0:    mov ax, 4E00h                   ; AL = 0 (sonst Rest von 4Fh)
        xor cx, cx
        mov dx, path
        int 21h
.f1:    jc .f2
        inc word [cnt]
        mov ah, 4Fh
        int 21h
        jmp .f1
.f2:    dec bp
        jnz .f0
        dec word [rcnt]
        jnz .x8
        call tend2
        mov dx, s_ent
        call puts
        mov ax, [cnt]
        call pnum16
        call crlf
        jmp .del
.nomany:
        mov dx, s_nomany
        call puts
        ; T9 200 Dateien loeschen
.del:   mov dx, t_del
        call tbeg
        xor bp, bp
.d1:    call smname
        mov ah, 41h
        mov dx, path
        int 21h
        inc bp
        cmp bp, NSMALL
        jb .d1
        call tend
        ; aufraeumen
        mov si, s_big
        call mkpath
        mov ah, 41h
        mov dx, path
        int 21h
        mov si, s_dbench0
        call mkpath0
        mov ah, 3Ah
        mov dx, path
        int 21h
        mov ax, 4C00h
        int 21h
.err:   push ax
        mov dx, s_err
        call puts
        pop ax
        xor ah, ah
        call pnum16
        call crlf
        mov ax, 4C02h
        int 21h

; BIG.TMP oeffnen -> BX
opbig:  mov si, s_big
        call mkpath
        mov ax, 3D00h
        mov dx, path
        int 21h
        jc .e
        mov bx, ax
        ret
.e:     pop ax
        jmp start.err
; BP Bloecke zu CX Byte aus BIG.TMP
rdbig:  push cx
        call opbig
        pop cx
.l:     mov ah, 3Fh
        mov dx, buf
        push cx
        int 21h
        pop cx
        jc .e
        dec bp
        jnz .l
        mov ah, 3Eh
        int 21h
        ret
.e:     pop ax
        jmp start.err

; Name BENCH\Fnnn.TMP fuer BP
smname: mov si, s_sm
        call mkpath
        mov di, [pend]
        sub di, 7                       ; Stelle der Ziffern in Fnnn.TMP
        mov ax, bp
        mov bl, 100
        div bl
        add al, '0'
        mov [di], al
        mov al, ah
        xor ah, ah
        mov bl, 10
        div bl
        add ax, '00'
        mov [di+1], al
        mov [di+2], ah
        ret

; path = base + "BENCH\" + SI
mkpath: push si
        mov si, s_dbench
        call mkpath0
        pop si
        mov di, [pend]
        jmp cat
; path = base + SI
mkpath0:
        mov di, path
        push si
        mov si, base
.l:     cmp si, [baseend]
        je .e
        movsb
        jmp .l
.e:     pop si
cat:    lodsb
        stosb
        or al, al
        jnz cat
        dec di
        mov [pend], di
        ret

; Zeitmessung
tbeg:   call puts
        call ticks
        mov [t0], ax
        mov [t0+2], dx
        ret
tend:   call tend2
        call crlf
        ret
tend2:  call ticks
        sub ax, [t0]
        sbb dx, [t0+2]
        push ax
        call pnum16
        mov dx, s_tk
        call puts
        pop ax
        mov cx, 55
        mul cx
        call pnum32
        mov dx, s_ms
        call puts
        ret
ticks:  push es
        xor ax, ax
        mov es, ax
        cli
        mov ax, [es:46Ch]
        mov dx, [es:46Eh]
        sti
        pop es
        ret

; Ausgabe
puts:   push ax
        push bx
        push cx
        push si
        mov si, dx
        xor cx, cx
.l:     cmp byte [si], 0
        je .w
        inc si
        inc cx
        jmp .l
.w:     mov ah, 40h
        mov bx, 1
        int 21h
        pop si
        pop cx
        pop bx
        pop ax
        ret
putc:   push dx
        mov [cbuf], dl
        mov byte [cbuf+1], 0
        mov dx, cbuf
        call puts
        pop dx
        ret
crlf:   mov dx, s_crlf
        jmp puts
pnum2:  cmp ax, 10                      ; zweistellig
        jae pnum16
        mov dl, '0'
        call putc
pnum16: xor dx, dx
pnum32: push bx
        push cx
        push di
        mov di, nbuf + 11
        mov byte [di], 0
        mov bx, 10
.l:     mov cx, ax                      ; DX:AX / 10
        mov ax, dx
        xor dx, dx
        div bx
        xchg ax, cx
        div bx
        dec di
        add dl, '0'
        mov [di], dl
        mov dx, cx
        or cx, ax
        jnz .l
        mov dx, di
        call puts
        pop di
        pop cx
        pop bx
        ret

s_use   db 'DOSBENCH Pfad\  (z. B. DOSBENCH D:\)', 13, 10, 0
s_tick  db 'Takt ', 0
s_head  db 'DOSBENCH 1, Wiederholungen ', 0
s_ver   db 'DOS-Version ', 0
s_true  db ', wahr ', 0
s_hma   db ', in der HMA', 0
s_mem   db 'Groesstes Programm ', 0
s_kb    db ' KB', 13, 10, 0
s_tk    db ' Takte, ', 0
s_ms    db ' ms', 0
s_crlf  db 13, 10, 0
s_err   db 13, 10, 'Fehler ', 0
s_ent   db '  Eintraege gefunden ', 0
s_nomany db 'Suche MANY\*.*: kein MANY\', 13, 10, 0
s_dbench db 'BENCH\', 0
s_dbench0 db 'BENCH', 0
s_big   db 'BIG.TMP', 0
s_sm    db 'F000.TMP', 0
s_many  db 'MANY\*.*', 0
t_w     db '4 MB schreiben (32 KB)      ', 0
t_r     db '4 MB lesen (32 KB)          ', 0
t_r512  db '1 MB lesen (512 Byte)       ', 0
t_seek  db '2000 x Seek + 64 Byte lesen ', 0
t_mk    db '200 Dateien anlegen (4 KB)  ', 0
t_rd    db '200 Dateien oeffnen/lesen   ', 0
t_attr  db '1000 x Attribut abfragen    ', 0
t_find  db 'Suche MANY\*.* x 3          ', 0
t_del   db '200 Dateien loeschen        ', 0

section .bss
mblk    resw 1
baseend resw 1
nrep    resw 1
rcnt    resw 1
pend    resw 1
t0      resd 1
rnd     resw 1
cnt     resw 1
cbuf    resb 2
nbuf    resb 12
base    resb 80
path    resb 128
dta     resb 64
        alignb 16
buf     resb BUFSZ
