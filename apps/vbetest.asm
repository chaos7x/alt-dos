; SPDX-License-Identifier: GPL-2.0-or-later
; VBETEST: VBE 101h (640x480x256) mit linearem Bildspeicher ueber DPMI 0800h
cpu 386
org 100h
        mov ax,4F01h            ; Modusinfo 101h nach info
        mov cx,101h
        mov di,info
        push cs
        pop es
        int 10h
        cmp ax,004Fh
        jne nolfb
        test byte [info],80h    ; Bit 7: LFB vorhanden
        jz nolfb
        mov eax,[info+28h]
        mov [phys],eax
        mov ax,1687h
        int 2Fh
        test ax,ax
        jnz nodpmi
        mov [entry],di
        mov [entry+2],es
        mov [hp],si
        mov ah,4Ah
        push cs
        pop es
        mov bx,1000h
        int 21h
        mov ah,48h
        mov bx,[hp]
        int 21h
        jc nodpmi
        mov es,ax
        mov ax,4F02h            ; Modus setzen (noch im Real Mode)
        mov bx,4101h
        int 10h
        cmp ax,004Fh
        jne nolfb
        xor ax,ax
        call far [entry]
        jc nodpmi
        ; PM 16 Bit
        mov bx,[phys+2]
        mov cx,[phys]
        mov si,0005h            ; 300 KB
        mov di,0
        mov ax,0800h
        int 31h
        jc pmerr
        mov [lin],cx
        mov [lin+2],bx
        mov ax,0000h            ; Deskriptor anlegen
        mov cx,1
        int 31h
        jc pmerr
        mov [sel],ax
        mov bx,ax
        mov cx,[lin+2]
        mov dx,[lin]
        mov ax,0007h
        int 31h
        mov bx,[sel]
        mov cx,0004h            ; Grenze 4FFFFh
        mov dx,0FFFFh
        mov ax,0008h
        int 31h
        mov es,[sel]
        xor edi,edi             ; Streifen: Farbe = Zeile / 16
        xor edx,edx
.row:   mov eax,edx
        shr eax,4
        mov ecx,640
.px:    mov [es:edi],al
        inc edi
        loop .px
        inc edx
        cmp edx,480
        jb .row
        mov ah,0                ; Taste (INT 16h an den Real Mode)
        int 16h
        mov ax,0003h
        int 10h
        mov ax,4C00h
        int 21h
pmerr:  mov ax,0003h
        int 10h
        mov ax,4C02h
        int 21h
nolfb:  mov dx,m1
        jmp short out
nodpmi: mov dx,m2
out:    mov ah,9
        int 21h
        mov ax,4C01h
        int 21h
m1 db 'kein VBE-LFB',13,10,'$'
m2 db 'kein DPMI',13,10,'$'
entry dd 0
hp dw 0
phys dd 0
lin dd 0
sel dw 0
info times 256 db 0
