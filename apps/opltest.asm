; SPDX-License-Identifier: GPL-2.0-or-later
; OPLTEST.COM - AdLib-Registerkopie der DOS-Box (AT-DOS 6.0)
;
; Stellt auf Kanal 0 ein Instrument und 440 Hz ein, spielt 1 s, wartet
; auf eine Taste und schaltet dann NUR die Taste wieder an (B0h) - die
; Frequenz und das Instrument muessen noch im Chip stehen. Test: PLAY
; im Hintergrund (DETACH PLAY \DEMO\ODE.IMF), START OPLTEST, nach dem
; ersten Ton wegschalten, PLAY spielt, zurueck, Taste: wieder 440 Hz mit
; demselben Klang (ohne Kopie stumm oder anderer Klang).
cpu 386
org 100h
    mov si,tab
.l: lodsw
    cmp ax,0FFFFh
    je .go
    call wr
    jmp .l
.go: mov cx,18
    call ticks
    mov ax,0B012h       ; Taste los
    call wr
    xor ah,ah           ; auf Taste warten (hinten schlafen)
    int 16h
    mov ax,0B032h       ; nur Taste an - A0h/Instrument von vorher
    call wr
    mov cx,36
    call ticks
    mov ax,0B012h
    call wr
    mov ax,4C00h
    int 21h
wr: push ax
    mov dx,388h
    mov al,ah
    out dx,al
    mov cx,6
.a: in al,dx
    loop .a
    pop ax
    inc dx
    out dx,al
    dec dx
    mov cx,35
.b: in al,dx
    loop .b
    ret
ticks: push es
    xor ax,ax
    mov es,ax
.t0: mov ax,[es:46Ch]
.t1: cmp ax,[es:46Ch]
    je .t1
    loop .t0
    pop es
    ret
tab dw 2001h,4010h,60F0h,8077h,2301h,4300h,63F0h,8377h,0C001h,0A044h,0B032h,0FFFFh
