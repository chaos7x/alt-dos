; SPDX-License-Identifier: GPL-2.0-or-later
; HALLO.COM - Testprogramm fuer die DOS-Boxen (AT-DOS 6.0, Phase 3)
;
; Ein echtes Real-Mode-Programm: laeuft unter MS-DOS genauso wie in einer
; DOS-Box von AT-DOS. Prueft, was der V86-Monitor nachfuehrt: INT 21h/10h/
; 16h, CLI/STI/PUSHF/POPF (virtuelles IF), einen eigenen Interruptvektor
; (INT 21h 25h, IRET), die Befehlszeile im PSP und dass 32-Bit-Register
; ueber Taktinterrupts erhalten bleiben. Rueckgabewert 5 (ERRORLEVEL).
;
; HALLO [Text] [/W]   /W: auf eine Taste warten

cpu 386
org 100h

        mov     dx, m_hallo
        call    print
        mov     ah, 30h                 ; DOS-Version
        int     21h
        add     al, '0'
        mov     ah, 0Eh
        int     10h
        mov     al, '.'
        int     10h
        mov     al, '0'
        int     10h
        int     10h
        mov     dx, m_crlf
        call    print

        ; virtuelles IF
        mov     dx, m_if
        call    print
        cli
        pushf
        pop     ax
        test    ax, 200h
        jnz     bad
        sti
        pushf
        pop     ax
        test    ax, 200h
        jz      bad
        mov     dx, m_ok
        call    print

        ; eigener Vektor: INT 60h -> h60 (IRET aus der Box)
        mov     ax, 2560h
        mov     dx, h60
        int     21h
        mov     ax, 3560h
        int     21h
        cmp     bx, h60
        jne     bad
        int     60h

        ; 32-Bit-Register ueber viele Taktinterrupts
        mov     dx, m_r32
        call    print
        mov     eax, 12345678h
        mov     ebx, 9ABCDEF0h
        mov     ecx, 4000000h
.l:     cmp     eax, 12345678h
        jne     bad
        cmp     ebx, 9ABCDEF0h
        jne     bad
        dec     ecx
        jnz     .l
        mov     dx, m_ok
        call    print

        ; Befehlszeile
        mov     dx, m_args
        call    print
        mov     si, 81h
        xor     bp, bp                  ; /W gesehen?
.a:     lodsb
        cmp     al, 13
        je      .ae
        cmp     al, '/'
        jne     .ap
        mov     ah, [si]
        and     ah, 0DFh
        cmp     ah, 'W'
        jne     .ap
        inc     bp
.ap:    mov     dl, al
        mov     ah, 2
        int     21h
        jmp     .a
.ae:    mov     dx, m_end
        call    print

        test    bp, bp
        jz      .done
        mov     dx, m_key
        call    print
        xor     ah, ah
        int     16h
        mov     dl, al
        mov     ah, 2
        int     21h
        mov     dx, m_crlf
        call    print
.done:  mov     ax, 4C05h
        int     21h

bad:    mov     dx, m_bad
        call    print
        mov     ax, 4C01h
        int     21h

h60:    push    dx
        push    ax
        mov     dx, m_int
        mov     ah, 9
        int     21h
        pop     ax
        pop     dx
        iret

print:  mov     ah, 9
        int     21h
        ret

m_hallo db "Hallo aus der DOS-Box! DOS-Version $"
m_crlf  db 13, 10, "$"
m_if    db "Virtuelles IF (CLI/STI/PUSHF): $"
m_ok    db "ok", 13, 10, "$"
m_int   db "Eigener Interrupt 60h: ok", 13, 10, "$"
m_r32   db "32-Bit-Register: $"
m_args  db "Befehlszeile: [$"
m_end   db "]", 13, 10, "$"
m_key   db "Taste? $"
m_bad   db "FEHLER", 13, 10, "$"
