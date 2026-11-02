; SPDX-License-Identifier: GPL-2.0-or-later
; SNAP.COM - Testhilfe fuer den FAPI-Stub unter DOSBox-X (nasm -f bin): TSR, F12 schreibt den Textbildschirm (B800:0, 8000
; Byte) nach SNAPn.BIN. Haengt an INT 16h (nur sicher, wenn das Programm
; die Tastatur ueber das BIOS abfragt - wie der FAPI-Stub).
org 0x100
        jmp     install
old16   dd 0
num     db '0'
fname   db 'SNAP0.BIN', 0
new16:  cmp     ah, 0x00
        je      .chk
        cmp     ah, 0x01
        je      .chk
        cmp     ah, 0x10
        je      .chk
        cmp     ah, 0x11
        je      .chk
        jmp     far [cs:old16]
.chk:   push    ax
        mov     ah, 0x11
        pushf
        call    far [cs:old16]
        jz      .none
        cmp     ah, 0x86                ; F12 (erweitert)
        je      .snap
        cmp     ah, 0x58
        jne     .none
.snap:  mov     ah, 0x10
        pushf
        call    far [cs:old16]          ; Taste verbrauchen
        push    bx
        push    cx
        push    dx
        push    ds
        push    cs
        pop     ds
        mov     al, [num]
        mov     [fname + 4], al
        inc     byte [num]
        mov     ah, 0x3C
        xor     cx, cx
        mov     dx, fname
        int     0x21
        jc      .done
        mov     bx, ax
        push    ds
        mov     ax, 0xB800
        mov     ds, ax
        mov     ah, 0x40
        mov     cx, 8000
        xor     dx, dx
        int     0x21
        pop     ds
        mov     ah, 0x3E
        int     0x21
.done:  pop     ds
        pop     dx
        pop     cx
        pop     bx
.none:  pop     ax
        jmp     far [cs:old16]
install:
        mov     ax, 0x3516
        int     0x21
        mov     [old16], bx
        mov     [old16 + 2], es
        mov     ax, 0x2516
        mov     dx, new16
        int     0x21
        mov     dx, install
        add     dx, 15
        mov     cl, 4
        shr     dx, cl
        mov     ax, 0x3100
        int     0x21
