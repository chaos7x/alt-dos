; SPDX-License-Identifier: GPL-2.0-or-later
; SVBG.COM - SVGA-Box im Hintergrund (AT-DOS 6.0)
;
; 640x480x256 (VESA 101h) ueber das Bank-Fenster; je BIOS-Takt eine Zeile
; (Farbe = Zeile / 2) ueber alle fuenf 64-KB-Baenke, Fenster per 4F05h.
; Mit START starten, wegschalten, nach ein paar Sekunden zurueck: die
; Box hat hinten weitergezeichnet. Am Ende Taste -> Textmodus.
        org 100h
        mov ax,4F01h
        mov cx,0101h
        mov di,info
        int 10h
        mov ax,[info+4]
        mov bx,ax
        mov ax,64
        xor dx,dx
        div bx
        mov [gmul],ax
        mov ax,4F02h
        mov bx,0101h
        int 10h
        mov word [y],0
.line:  mov ax,[y]
        mov bx,640
        mul bx                  ; DX:AX = Offset
        mov di,ax
        mov [bk],dx
        mov dx,[bk]
        call setbank
        mov ax,0A000h
        mov es,ax
        mov al,[y]
        shr al,1
        or al,1
        mov cx,640
.px:    stosb
        or di,di
        jnz .n
        push ax
        push cx
        inc word [bk]
        mov dx,[bk]
        call setbank
        pop cx
        pop ax
.n:     loop .px
        mov ah,0                ; warten auf naechsten Takt
        int 1Ah
        mov bx,dx
.w:     mov ah,0
        int 1Ah
        cmp dx,bx
        je .w
        inc word [y]
        cmp word [y],480
        jb .line
        xor ax,ax
        int 16h
        mov ax,3
        int 10h
        ret
setbank:
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
gmul dw 1
y dw 0
bk dw 0
info times 256 db 0
