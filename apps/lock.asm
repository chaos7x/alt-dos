; SPDX-License-Identifier: GPL-2.0-or-later
;
; LOCK.COM / UNLOCK.COM (nasm -DUNLOCK) fuer die DOS-Box mit eigenem Kern:
; wie die internen Befehle LOCK/UNLOCK von MS-DOS 7 - LOCK [X:] erlaubt
; direkte Plattenzugriffe (INT 26h, 7305h) auf dem Laufwerk, bis UNLOCK
; oder die aufrufende Shell endet. Laufwerkssperre 440Dh 4Ah/6Ah mit
; DX = 'LK' (4C4Bh): der Box-Kern (atdoscm/dosint.c dlock_fn) gibt die
; Sperre dann der Shell (Elternprozess) statt diesem Programm, UNLOCK
; hebt alle Stufen auf.
        cpu     8086
        org     100h

start:  mov     si, 81h
.sp:    lodsb
        cmp     al, ' '
        je      .sp
        cmp     al, 9
        je      .sp
        xor     bl, bl                  ; 0 = aktuelles Laufwerk
        cmp     al, 13
        je      .go
        cmp     byte [si], ':'
        jne     usage
        and     al, 0DFh
        sub     al, 'A' - 1
        jbe     usage
        cmp     al, 26
        ja      usage
        mov     bl, al
.go:    mov     [drv], bl
        mov     ah, 19h                 ; Buchstabe fuer die Meldungen
        int     21h
        inc     al
        test    bl, bl
        jz      .cur
        mov     al, bl
.cur:   add     al, 'A' - 1
        mov     [m_ok_d], al
        mov     [m_q_d], al
        mov     [m_bad_d], al
        mov     ah, 36h                 ; Laufwerk da? (AX FFFFh: nein)
        mov     dl, [drv]
        int     21h
        cmp     ax, 0FFFFh
        jne     .ok
        mov     dx, m_bad
        call    puts
        mov     ax, 4C01h
        int     21h
.ok:
%ifndef UNLOCK
        mov     dx, m_warn
        call    puts
.ask:   mov     ah, 08h                 ; J/N wie MS-DOS 7
        int     21h
        and     al, 0DFh
        cmp     al, 'N'
        je      .no
        cmp     al, 'J'
        je      .yes
        cmp     al, 'Y'
        jne     .ask
.yes:   mov     dx, m_crlf
        call    puts
        mov     cx, 084Ah
%else
        mov     cx, 086Ah
%endif
        mov     ax, 440Dh
        mov     bl, [drv]
        xor     bh, bh                  ; Stufe 0
        mov     dx, 4C4Bh               ; 'LK': gehoert der Shell
        int     21h
        jc      .err
        mov     dx, m_ok
        call    puts
        mov     ax, 4C00h
        int     21h
%ifndef UNLOCK
.no:    mov     dx, m_crlf
        call    puts
        mov     ax, 4C01h
        int     21h
%endif
.err:   mov     dx, m_used
        cmp     ax, 5
        je      .pe
        mov     dx, m_nosup
.pe:    call    puts
        mov     ax, 4C01h
        int     21h

usage:  mov     dx, m_use
        call    puts
        mov     ax, 4C01h
        int     21h

puts:   mov     ah, 09h
        int     21h
        ret

drv     db      0
%ifndef UNLOCK
m_warn  db      'ACHTUNG: Mit gesperrtem Laufwerk d', 81h, 'rfen Programme direkt auf die', 13, 10
        db      'Platte schreiben (INT 26h). Das kann Daten zerst', 94h, 'ren.', 13, 10
        db      'Laufwerk '
m_q_d   db      'C: sperren (J/N)? $'
m_ok    db      'Laufwerk '
m_ok_d  db      'C: ist gesperrt (UNLOCK hebt es auf).', 13, 10, '$'
m_used  db      'Laufwerk kann nicht gesperrt werden: dort sind Dateien offen', 13, 10
        db      '(auch die Auslagerungsdatei oder ein LOOP-Abbild).', 13, 10, '$'
m_use   db      'LOCK [Laufwerk:] - direkte Plattenzugriffe erlauben (wie MS-DOS 7)', 13, 10, '$'
%else
m_q_d   db      0
m_ok    db      'Laufwerk '
m_ok_d  db      'C: ist entsperrt.', 13, 10, '$'
m_used  db      'Laufwerk war nicht gesperrt.', 13, 10, '$'
m_use   db      'UNLOCK [Laufwerk:] - Sperre von LOCK aufheben', 13, 10, '$'
%endif
m_bad   db      'Ung', 81h, 'ltiges Laufwerk '
m_bad_d db      'C:', 13, 10, '$'
m_nosup db      'Geht nur in der DOS-Box mit dem eigenen Kern (ATDOSCM).', 13, 10, '$'
m_crlf  db      13, 10, '$'
