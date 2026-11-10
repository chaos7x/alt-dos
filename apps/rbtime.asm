; SPDX-License-Identifier: GPL-2.0-or-later
; RBTIME.COM - Messung fuer die Kompatibilitaetsbox (5.1): wie viele
; Ausfluege Box -> AT-DOS -> Box schafft der Rechner je Sekunde?
; RBOX \DEMO\RBTIME.COM - ruft 10 s lang INT E0h mit einem unbekannten
; Dienst (RBOX antwortet AX = FFFFh) und zeigt "n Ausfluege/s". Die Zeit
; kommt aus der Echtzeituhr (CMOS): der BIOS-Takt der Box steht, solange
; AT-DOS laeuft. Auf dem 286 vergleicht man RBRESET=KBC (Vorgabe) mit
; RBRESET=FAULT in der CONFIG.SYS. Taste beendet.
cpu 8086
org 0x100

        mov     si, t_wait
        call    print
        call    sec
        mov     bl, al
.w0:    call    sec                     ; auf eine Sekundengrenze warten
        cmp     al, bl
        je      .w0
        mov     bl, al
        mov     cx, 10
        xor     si, si
        xor     di, di
.loop:  push    cx
        mov     ah, 0x7F                ; unbekannter Dienst: ein Ausflug
        int     0xE0
        pop     cx
        add     si, 1
        adc     di, 0
        call    sec
        cmp     al, bl
        je      .loop
        mov     bl, al
        loop    .loop
        mov     ax, si                  ; Anzahl / 10 s
        mov     dx, di
        mov     cx, 10
        div     cx
        call    pnum
        mov     si, t_res
        call    print
        xor     ax, ax
        int     0x16
        ret

sec:    cli                             ; Sekunde aus der Echtzeituhr
        mov     al, 0
        out     0x70, al
        in      al, 0x71
        sti
        ret

pnum:   xor     cx, cx                  ; AX dezimal ausgeben
        mov     bx, 10
.d:     xor     dx, dx
        div     bx
        push    dx
        inc     cx
        test    ax, ax
        jnz     .d
.o:     pop     ax
        add     al, '0'
        mov     ah, 0x0E
        int     0x10
        loop    .o
        ret

print:  lodsb
        test    al, al
        jz      .e
        mov     ah, 0x0E
        int     0x10
        jmp     print
.e:     ret

t_wait: db      "RBTIME: 10 s Ausfluege zu AT-DOS ...", 13, 10, 0
t_res:  db      " Ausfluege/s - Taste beendet", 13, 10, 0
