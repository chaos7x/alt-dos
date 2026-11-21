; SPDX-License-Identifier: GPL-2.0-or-later
;
; DOSSTRUC.COM - gibt die inneren Strukturen des laufenden DOS als Text
; aus (Stufe 0 von docs/DOSKERN.md, Abschnitt „Undokumentiertes“):
; Liste der Listen, SDA, SFT-Kette, CDS, DPB-Kette, MCB-Kette, eigener
; PSP. Unter echtem MS-DOS 6.22 gemessen ist die Ausgabe der Massstab fuer
; den eigenen Box-Kern; dieselbe Ausgabe unter FreeDOS zeigt, wo die
; Vorlage abweicht. Nur Werte, kein fremder Code.
;
;   DOSSTRUC [> datei]
;
; Lagen nach dem Layout von MS-DOS 4.0 (INC/SYSVAR.INC, SF.INC,
; CURDIR.INC, DPB.INC, ARENA.INC, PDB.INC). Werte hexadezimal.

        cpu 8086
        org 100h

start:  mov dx, s_head
        call puts
        mov ah, 30h
        int 21h
        mov [ver], ax
        mov dx, s_ver
        call puts
        call phex16
        mov ax, 3306h
        int 21h
        mov dx, s_true
        call puts
        mov ax, bx
        call phex16
        mov dx, s_dx
        call puts
        mov ax, dx
        call phex16
        call crlf

; ---- Liste der Listen ----------------------------------------------------
        mov ah, 52h
        int 21h
        mov [lol], bx
        mov [lol+2], es
        mov dx, s_lol
        call puts
        mov ax, es
        mov dx, bx
        call pfar
        call crlf
        mov ax, [es:bx-2]
        mov [mcb1], ax
        mov si, lolf                    ; Tabelle: Offset, Breite, Name
.lf:    lodsw
        cmp ax, 0FFFFh
        je .le
        mov di, ax
        lodsb
        mov cl, al
        mov dx, si
        call puts                       ; Name
.ln:    lodsb
        or al, al
        jnz .ln
        les bx, [lol]
        cmp cl, 1
        jne .l2
        mov al, [es:bx+di]
        call phex8
        jmp .l9
.l2:    cmp cl, 2
        jne .l4
        mov ax, [es:bx+di]
        call phex16
        jmp .l9
.l4:    mov ax, [es:bx+di+2]
        mov dx, [es:bx+di]
        call pfar
.l9:    call crlf
        jmp .lf
.le:    push cs
        pop es
        mov dx, s_nul
        call puts
        les bx, [lol]
        lea si, [bx+22h]
        mov cx, 18
        call pdump
        call crlf

; ---- SDA -----------------------------------------------------------------
        push ds
        mov ax, 5D06h
        int 21h
        mov ax, ds
        pop ds
        jc .nos
        mov [sda], si
        mov [sda+2], ax
        mov [sdacx], cx
        mov [sdadx], dx
        mov dx, s_sda
        call puts
        mov dx, si
        call pfar
        mov dx, s_sdacx
        call puts
        mov ax, [sdacx]
        call phex16
        mov dx, s_sdadx
        call puts
        mov ax, [sdadx]
        call phex16
        call crlf
        les si, [sda]
        mov cx, 32
        call pdumpl                     ; erste 32 Byte
        mov dx, s_sdaf
        call puts
        les si, [sda]
        mov ax, [es:si+10h]
        call phex16                     ; aktueller PSP
        mov dx, s_sdadta
        call puts
        mov ax, [es:si+0Eh]
        mov dx, [es:si+0Ch]
        call pfar
        call crlf
        jmp .s5
.nos:   mov dx, s_nosda
        call puts
.s5:    push cs
        pop es
        mov ax, 5D0Bh                   ; Liste der SDA-Bereiche (DOS 4+)
        push ds
        int 21h
        mov ax, ds
        pop ds
        jc .no5b
        push ax
        mov dx, s_5db
        call puts
        pop ax
        mov es, ax
        mov cx, [es:si]
        mov ax, cx
        call phex16
        call crlf
        add si, 2
        cmp cx, 8
        jbe .b1
        mov cx, 8
.b1:    jcxz .b9
.b2:    push cx
        mov dx, s_sp4
        call puts
        mov ax, [es:si+2]
        mov dx, [es:si]
        call pfar
        mov dx, s_sp
        call puts
        mov ax, [es:si+4]
        call phex16
        call crlf
        add si, 6
        pop cx
        loop .b2
.b9:    push cs
        pop es
        jmp .sft
.no5b:  mov dx, s_no5b
        call puts

; ---- SFT -----------------------------------------------------------------
.sft:   mov dx, s_sft
        call puts
        les bx, [lol]
        les di, [es:bx+4]
        xor bp, bp                      ; Eintragsnummer
.t1:    cmp di, 0FFFFh
        je .t9
        mov dx, s_blk
        call puts
        mov ax, es
        mov dx, di
        call pfar
        mov dx, s_cnt
        call puts
        mov cx, [es:di+4]
        mov ax, cx
        call phex16
        call crlf
        lea si, [di+6]
.t2:    jcxz .t8
        push cx
        cmp word [es:si], 0             ; Verweise
        je .t7
        mov dx, s_sp4
        call puts
        mov ax, bp
        call phex8
        mov dx, s_col
        call puts
        mov cx, 0035h                   ; Eintrag bis hinter den Namen
        push si
        call pdump
        pop si
        call crlf
.t7:    pop cx
        add si, 3Bh
        inc bp
        loop .t2
.t8:    les di, [es:di]
        jmp .t1
.t9:    push cs
        pop es

; ---- CDS -----------------------------------------------------------------
        mov dx, s_cds
        call puts
        les bx, [lol]
        mov cl, [es:bx+21h]             ; LASTDRIVE
        xor ch, ch
        les si, [es:bx+16h]
        mov al, 'A'
.c1:    jcxz .c9
        test word [es:si+43h], 0C000h   ; Flags: Netz/physisch
        jz .c8
        push cx
        push ax
        mov dx, s_sp4
        call puts
        mov dl, al
        call putc
        mov dx, s_col
        call puts
        mov ax, [es:si+43h]
        call phex16                     ; Flags
        mov dx, s_sp
        call puts
        mov ax, [es:si+47h]
        mov dx, [es:si+45h]
        call pfar                       ; DPB
        mov dx, s_sp
        call puts
        mov ax, [es:si+49h]
        call phex16                     ; Cluster
        mov dx, s_sp
        call puts
        mov ax, [es:si+4Fh]
        call phex16                     ; Offset Backslash
        mov dx, s_sp
        call puts
        push si
        mov cx, 67
.c2:    mov dl, [es:si]
        or dl, dl
        jz .c3
        call putc
        inc si
        loop .c2
.c3:    pop si
        call crlf
        pop ax
        pop cx
.c8:    add si, 58h
        inc al
        dec cx
        jmp .c1
.c9:    push cs
        pop es

; ---- DPB -----------------------------------------------------------------
        mov dx, s_dpb
        call puts
        les bx, [lol]
        les si, [es:bx]
        mov cx, 26
.d1:    cmp si, 0FFFFh
        je .d9
        push cx
        mov dx, s_sp4
        call puts
        mov cx, 21h                     ; DPB von 4.0 (33 Byte)
        push si
        call pdump
        pop si
        call crlf
        les si, [es:si+19h]
        pop cx
        loop .d1
.d9:    push cs
        pop es

; ---- MCB -----------------------------------------------------------------
        mov dx, s_mcb
        call puts
        mov ax, [mcb1]
        mov cx, 60
.m1:    mov es, ax
        push cx
        push ax
        mov dx, s_sp4
        call puts
        call phex16                     ; Segment des MCB
        mov dx, s_sp
        call puts
        mov dl, [es:0]
        call putc
        mov dx, s_sp
        call puts
        mov ax, [es:1]
        call phex16                     ; Besitzer
        mov dx, s_sp
        call puts
        mov ax, [es:3]
        call phex16                     ; Groesse
        mov dx, s_sp
        call puts
        mov si, 8
        mov cx, 8
.m2:    mov dl, [es:si]
        cmp dl, ' '
        jb .m3
        cmp dl, 7Eh
        ja .m3
        call putc
        inc si
        loop .m2
.m3:    call crlf
        pop ax
        pop cx
        cmp byte [es:0], 'M'
        jne .m9
        add ax, [es:3]
        inc ax
        loop .m1
.m9:    push cs
        pop es

; ---- eigener PSP ---------------------------------------------------------
        mov dx, s_psp
        call puts
        mov ax, cs
        call phex16
        call crlf
        xor si, si
        mov cx, 60h
        call pdumpl
        mov ax, 4C00h
        int 21h

; ---- Ausgabe -------------------------------------------------------------
; CX Byte ab ES:SI als Hex mit Leerzeichen
pdump:  jcxz .e
.l:     mov al, [es:si]
        call phex8
        mov dl, ' '
        call putc
        inc si
        loop .l
.e:     ret
; dasselbe in Zeilen zu 16 Byte
pdumpl: jcxz .e
.l:     push cx
        mov dx, s_sp4
        call puts
        mov ax, si
        call phex16
        mov dx, s_col
        call puts
        pop cx
        push cx
        cmp cx, 16
        jbe .n
        mov cx, 16
.n:     mov bx, cx
        call pdump
        call crlf
        pop cx
        sub cx, bx
        jnz .l
.e:     ret
; AX:DX als ssss:oooo
pfar:   push dx
        call phex16
        mov dl, ':'
        call putc
        pop ax
        jmp phex16
phex16: push ax
        mov al, ah
        call phex8
        pop ax
phex8:  push ax
        push cx
        mov cl, 4
        mov ah, al
        shr al, cl
        call .n
        mov al, ah
        and al, 0Fh
        call .n
        pop cx
        pop ax
        ret
.n:     push dx
        add al, '0'
        cmp al, '9'
        jbe .o
        add al, 7
.o:     mov dl, al
        call putc
        pop dx
        ret
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
crlf:   push dx
        mov dx, s_crlf
        call puts
        pop dx
        ret

; Felder der Liste der Listen: Offset, Breite (1/2/4), Name
lolf:   dw -2
        db 2, 'LoL-2 erster MCB     ', 0
        dw 00h
        db 4, 'LoL+00 DPB-Kette     ', 0
        dw 04h
        db 4, 'LoL+04 SFT-Kette     ', 0
        dw 08h
        db 4, 'LoL+08 CLOCK$        ', 0
        dw 0Ch
        db 4, 'LoL+0C CON           ', 0
        dw 10h
        db 2, 'LoL+10 Sektorgroesse ', 0
        dw 12h
        db 4, 'LoL+12 Puffer        ', 0
        dw 16h
        db 4, 'LoL+16 CDS           ', 0
        dw 1Ah
        db 4, 'LoL+1A FCB-SFT       ', 0
        dw 1Eh
        db 2, 'LoL+1E FCBS geschuetzt', 0
        dw 20h
        db 1, 'LoL+20 Blockgeraete  ', 0
        dw 21h
        db 1, 'LoL+21 LASTDRIVE     ', 0
        dw 34h
        db 1, 'LoL+34 JOIN-Laufwerke', 0
        dw 35h
        db 2, 'LoL+35 Sonderprogr.  ', 0
        dw 37h
        db 4, 'LoL+37 SETVER        ', 0
        dw 3Bh
        db 2, 'LoL+3B (A20-Zaehler) ', 0
        dw 3Fh
        db 2, 'LoL+3F BUFFERS x     ', 0
        dw 41h
        db 2, 'LoL+41 BUFFERS y     ', 0
        dw 43h
        db 1, 'LoL+43 Startlaufwerk ', 0
        dw 44h
        db 1, 'LoL+44 386-Merker    ', 0
        dw 45h
        db 2, 'LoL+45 erw. Speicher ', 0
        dw 0FFFFh

s_head  db 'DOSSTRUC 1', 13, 10, 0
s_ver   db 'INT 21h 30h AX=', 0
s_true  db '  3306h BX=', 0
s_dx    db ' DX=', 0
s_lol   db 'Liste der Listen ', 0
s_nul   db 'LoL+22 NUL-Kopf      ', 0
s_sda   db 'SDA (5D06h) ', 0
s_sdacx db '  immer CX=', 0
s_sdadx db ' im DOS DX=', 0
s_sdaf  db '  SDA+10 PSP ', 0
s_sdadta db '  SDA+0C DTA ', 0
s_nosda db 'SDA (5D06h): Fehler', 13, 10, 0
s_5db   db 'SDA-Liste (5D0Bh) Bereiche ', 0
s_no5b  db 'SDA-Liste (5D0Bh): Fehler', 13, 10, 0
s_sft   db 'SFT (benutzte Eintraege, je 3Bh Byte, erste 35h)', 13, 10, 0
s_blk   db '  Block ', 0
s_cnt   db ' Eintraege ', 0
s_cds   db 'CDS (benutzte, je 58h Byte): Lw Flags DPB Cluster \-Stelle Pfad', 13, 10, 0
s_dpb   db 'DPB-Kette (je erste 21h Byte)', 13, 10, 0
s_mcb   db 'MCB-Kette: Segment Art Besitzer Groesse Name', 13, 10, 0
s_psp   db 'eigener PSP ', 0
s_sp4   db '    ', 0
s_sp    db ' ', 0
s_col   db ': ', 0
s_crlf  db 13, 10, 0

section .bss
ver     resw 1
lol     resd 1
mcb1    resw 1
sda     resd 1
sdacx   resw 1
sdadx   resw 1
cbuf    resb 2
