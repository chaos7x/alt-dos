; SPDX-License-Identifier: GPL-2.0-or-later
; Startcode fuer Win16-Programme der GUI ohne Laufzeitbibliothek:
; InitTask, WaitEvent, InitApp, WinMain, Ende ueber INT 21h 4Ch.
cpu 286
segment _NULL  public class=BEGDATA align=16 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
segment STACK  stack  class=STACK align=16 use16
group DGROUP _NULL _DATA _BSS STACK
segment _TEXT  public class=CODE align=1 use16
extern INITTASK, WAITEVENT, INITAPP, WINMAIN

segment _NULL
        times 16 db 0                   ; Instanzdaten (pStackTop & Co.)

segment _TEXT
global _wstart_
_wstart_:
..start:
        call    far INITTASK
        or      ax, ax
        jz      .ende
        push    di
        push    si
        push    es
        push    bx
        push    dx
        xor     ax, ax
        push    ax
        call    far WAITEVENT
        mov     bp, sp
        push    word [bp+8]             ; hInstance
        call    far INITAPP
        call    WINMAIN                 ; nah (Small), Pascal: Argumente liegen schon
        mov     ah, 0x4C
        int     0x21
.ende:  mov     ax, 0x4CFF
        int     0x21
