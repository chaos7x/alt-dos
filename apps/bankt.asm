; SPDX-License-Identifier: GPL-2.0-or-later
; BANKT.COM - SVGA-Box beim Sitzungswechsel (AT-DOS 6.0)
;
; Stellt 640x480x256 (VESA 101h) ueber das Bank-Fenster ein, fuellt fuenf
; 64-KB-Baenke mit eigenen Farben und laesst das Fenster auf Bank 2 stehen.
; Mit START in eigener Sitzung starten, mit Alt+Esc wegschalten und
; zurueck: das Bild muss gleich sein. Nach einer Taste zeichnet BANKT
; oben ins Fenster (ohne die Bank neu zu setzen) eine weisse Linie - sie
; muss bei Zeile 204/205 erscheinen (Anfang von Bank 2). Zweite Taste:
; Textmodus, Ende. Fenster A und B werden beide gestellt (Tseng-Karten
; teilen sie in Schreiben/Lesen), Granularitaet aus 4F01h.
        org 100h
        mov ax,4F01h
        mov cx,0101h
        mov di,info
        int 10h
        cmp ax,004Fh
        jne fehler
        mov ax,[info+4]         ; Granularitaet in KB
        or ax,ax
        jz fehler
        mov bx,ax
        mov ax,64
        xor dx,dx
        div bx
        mov [gmul],ax
        mov ax,4F02h
        mov bx,0101h            ; 640x480x256
        int 10h
        cmp ax,004Fh
        jne fehler
        xor dx,dx
.bank:  push dx
        call setbank
        mov ax,0A000h
        mov es,ax
        xor di,di
        pop dx
        push dx
        mov al,dl
        mov ah,40
        mul ah
        add al,20
        mov ah,al
        mov cx,8000h
        rep stosw
        pop dx
        inc dx
        cmp dx,5
        jb .bank
        mov dx,2                ; Box steht danach auf Bank 2
        call setbank
        xor ax,ax
        int 16h
        mov ax,0A000h
        mov es,ax
        xor di,di
        mov al,15
        mov cx,640
        rep stosb
        xor ax,ax
        int 16h
        mov ax,3
        int 10h
        ret
fehler: mov dx,msg
        mov ah,9
        int 21h
        ret

setbank:                        ; DX = 64-KB-Bank -> Fenster A und B
        mov ax,dx
        mul word [gmul]
        mov dx,ax
        push dx
        mov ax,4F05h
        xor bx,bx
        int 10h
        pop dx
        mov ax,4F05h
        mov bx,1
        int 10h
        ret

msg     db 'VESA-Modus 101h (640x480x256) nicht verfuegbar.',13,10,'$'
gmul    dw 1
info    times 256 db 0
