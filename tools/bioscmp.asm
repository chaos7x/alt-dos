; SPDX-License-Identifier: GPL-2.0-or-later
;
; BIOSCMP.COM - Vergleichstest fuer DOS-Boxen
;
; Ruft BIOS- und DOS-Dienste so auf, wie Programme der Zeit es tun, und
; schreibt die Ergebnisse (Register, Flags, Puffer) zeilenweise in eine
; Datei.  Dasselbe Programm laeuft einmal unter echtem DOS (FreeDOS oder
; MS-DOS direkt auf der Maschine) und einmal in der Box; tools/bioscmp.sh
; vergleicht beide Dateien.  Jede Abweichung ist ein Kandidat fuer ein
; Programm, das in der Box anders laeuft als unter DOS.
;
; Aufruf:  BIOSCMP Ausgabedatei [Laufwerk]
;          Laufwerk = wo die Dateitests laufen (Vorgabe: aktuelles),
;          dort wird \BCT angelegt und am Ende wieder entfernt.
;
; Werte, die sich auch zwischen zwei echten Rechnern unterscheiden
; (Uhrzeit, Segmentadressen, Speichergroessen), gibt das Programm nicht
; aus oder nur als "da/nicht da".  Nur 8086-Befehle.

        cpu     8086
        org     100h

OSIZE   equ     0F000h                  ; Ausgabepuffer (eigenes Segment)

start:  mov     sp, stk_end
        mov     bx, (prog_end - start + 100h + 15) / 16 + 1000h + 10h
        mov     ah, 4Ah                 ; Speicher: Programm + Puffer
        int     21h
        mov     ax, cs
        add     ax, (prog_end - start + 100h + 15) / 16 + 1
        mov     [oseg], ax
        call    args
        cmp     byte [oname], 0
        jne     .ok
        mov     dx, m_use
        mov     ah, 9
        int     21h
        mov     ax, 4C01h
        int     21h
.ok:    mov     ah, 19h                 ; Laufwerk und Verzeichnis merken
        int     21h                     ; (die DOS-Tests wechseln es)
        mov     [cdrv], al
        add     al, 'A'
        mov     [cdir], al
        mov     si, cdir + 3
        xor     dl, dl
        mov     ah, 47h
        int     21h
        call    t_bios
        call    flush
        call    t_bda
        call    flush
        call    t_video
        call    flush
        call    t_kbd
        call    flush
        call    t_clock
        call    flush
        call    t_misc15
        call    flush
        call    t_disk
        call    flush
        call    t_ports
        call    flush
        call    t_dos
        call    flush
        call    t_files
        call    flush
        call    t_find
        call    flush
        call    t_fcb
        call    flush
        call    t_ioctl
        call    flush
        call    t_mux
        call    flush
        call    t_dosint
        call    flush
        call    cleanup
        call    flush
        mov     dl, [cdrv]              ; Laufwerk/Verzeichnis zurueck
        mov     ah, 0Eh
        int     21h
        mov     dx, cdir
        mov     ah, 3Bh
        int     21h
        mov     ax, 3                   ; Bildschirm sauber (die INT-10h-
        int     10h                     ; Tests lassen Cursor/Reste stehen)
        mov     dx, m_done
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

m_done  db      'BIOSCMP fertig.', 13, 10, '$'
cdrv    db      0
cdir    db      'C:\'
        times   65 db 0

m_use   db      'BIOSCMP Ausgabedatei [Laufwerk:]', 13, 10, '$'

; ---------------------------------------------------------------- Ausgabe

putc:   push    es                      ; AL -> Puffer
        push    di
        mov     es, [oseg]
        mov     di, [optr]
        cmp     di, OSIZE
        jae     .full
        stosb
        mov     [optr], di
.full:  pop     di
        pop     es
        ret

puts:   push    si                      ; SI -> ASCIIZ
        push    ax
.l:     lodsb
        or      al, al
        jz      .e
        call    putc
        jmp     .l
.e:     pop     ax
        pop     si
        ret

nib:    and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 7
.d:     jmp     putc

hex8:   push    ax                      ; AL hexadezimal
        push    cx
        mov     ah, al
        mov     cl, 4
        shr     al, cl
        call    nib
        mov     al, ah
        call    nib
        pop     cx
        pop     ax
        ret

hex16:  xchg    al, ah                  ; AX hexadezimal
        call    hex8
        xchg    al, ah
        jmp     hex8

space:  push    ax
        mov     al, ' '
        call    putc
        pop     ax
        ret

nl:     push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

; Hexbytes ES:SI, CX Stueck
hexbuf: jcxz    .e
        push    ax
        push    si
        push    cx
.l:     mov     al, [es:si]
        call    hex8
        inc     si
        loop    .l
        pop     cx
        pop     si
        pop     ax
.e:     ret

; Text ES:SI bis 0, hoechstens CX Zeichen, Steuerzeichen als '.'
txtbuf: push    ax
        push    si
        push    cx
        jcxz    .e
.l:     mov     al, [es:si]
        or      al, al
        jz      .e
        cmp     al, ' '
        jae     .p
        mov     al, '.'
.p:     call    putc
        inc     si
        loop    .l
.e:     pop     cx
        pop     si
        pop     ax
        ret

flush:  mov     dx, oname               ; Puffer in die Datei (nach jeder
                                        ; Gruppe: bleibt ein Test haengen,
                                        ; steht das Bisherige schon drin)
        xor     cx, cx
        mov     ah, 3Ch
        int     21h
        jc      .e
        mov     bx, ax
        mov     cx, [optr]
        push    ds
        mov     ds, [cs:oseg]
        xor     dx, dx
        mov     ah, 40h
        int     21h
        pop     ds
        mov     ah, 3Eh
        int     21h
.e:     ret

; ------------------------------------------------- Register festhalten

; snap: nach einem INT alle Register und Flags in r_* merken
snap:   pushf
        pop     word [cs:r_fl]
        mov     [cs:r_ax], ax
        mov     [cs:r_bx], bx
        mov     [cs:r_cx], cx
        mov     [cs:r_dx], dx
        mov     [cs:r_si], si
        mov     [cs:r_di], di
        mov     [cs:r_bp], bp
        mov     [cs:r_es], es
        push    cs
        pop     ds
        push    cs
        pop     es
        cld
        ret

%macro  T 1                             ; neue Zeile mit Namen
        call    nl
        mov     si, %%s
        call    puts
        jmp     %%e
%%s     db      %1, ':', 0
%%e:
%endmacro

%macro  R 1                             ; " ax=1234"
%defstr %%n %1
        mov     si, %%s
        call    puts
        mov     ax, [r_%1]
        call    hex16
        jmp     %%e
%%s     db      ' ', %%n, '=', 0
%%e:
%endmacro

%macro  RL 1                            ; nur das untere Byte
%defstr %%n %1
        mov     si, %%s
        call    puts
        mov     al, [r_%1]
        call    hex8
        jmp     %%e
%%s     db      ' ', %%n, 'L=', 0
%%e:
%endmacro

%macro  RH 1                            ; nur das obere Byte
%defstr %%n %1
        mov     si, %%s
        call    puts
        mov     al, [r_%1 + 1]
        call    hex8
        jmp     %%e
%%s     db      ' ', %%n, 'H=', 0
%%e:
%endmacro

%macro  F 2                             ; Flag: F 'CF', 1
        mov     si, %%s
        call    puts
        mov     al, '0'
        test    word [r_fl], %2
        jz      %%z
        inc     al
%%z:    call    putc
        jmp     %%e
%%s     db      ' ', %1, '=', 0
%%e:
%endmacro

%define CF      F 'CF', 1
%define ZF      F 'ZF', 40h

%macro  S 1                             ; freier Text
        mov     si, %%s
        call    puts
        jmp     %%e
%%s     db      %1, 0
%%e:
%endmacro

; Fehlerfall einer DOS-Funktion: CF und bei CF=1 den Code in AX
dosres: CF
        test    word [r_fl], 1
        jz      .e
        R       ax
.e:     ret

; ----------------------------------------------------- BIOS allgemein

t_bios: T       'INT11 Ausstattung'     ; Bits, die Programme auswerten
        int     11h
        call    snap
        mov     ax, [r_ax]
        and     ax, 0C031h              ; Disketten, FPU... ohne Video/RAM
        mov     [r_ax], ax
        R       ax
        T       'INT11 Video'
        int     11h
        call    snap
        mov     ax, [r_ax]
        and     ax, 30h
        mov     [r_ax], ax
        R       ax
        T       'INT12 konventionell>=512K'
        int     12h
        call    snap
        mov     al, '0'
        cmp     word [r_ax], 512
        jb      .lo
        inc     al
.lo:    call    space
        call    putc
        ret

; ---------------------------------------- BIOS-Datenbereich 0040:xxxx

bda_tab: ; Stelle, Laenge (1/2)
        dw      10h, 2                  ; Ausstattung (wie INT 11h)
        dw      17h, 1, 18h, 1, 96h, 1, 97h, 1   ; Tastenzustand
        dw      1Ah, 2, 1Ch, 2, 80h, 2, 82h, 2   ; Tastaturpuffer
        dw      49h, 1, 4Ah, 2, 4Ch, 2, 4Eh, 2   ; Video
        dw      50h, 2, 60h, 2, 62h, 1, 63h, 2
        dw      65h, 1, 66h, 1, 84h, 1, 85h, 2
        dw      87h, 1, 88h, 1, 89h, 1, 8Ah, 1
        dw      3Eh, 1, 3Fh, 1, 41h, 1, 8Bh, 1   ; Diskette
        dw      8Fh, 1, 90h, 1, 91h, 1
        dw      75h, 1                  ; Zahl der Festplatten
        dw      71h, 1, 72h, 2          ; Untbr-Merker, Neustart
        dw      0, 0

t_bda:  T       'BDA'
        mov     si, bda_tab
.l:     lodsw
        mov     bx, ax
        lodsw
        or      ax, ax
        jz      .e
        push    si
        push    ax
        call    space
        mov     ax, bx
        call    hex8
        mov     al, '='
        call    putc
        mov     ax, 40h
        mov     es, ax
        pop     ax
        cmp     ax, 1
        mov     ax, [es:bx]
        push    cs
        pop     es
        jne     .w
        call    hex8
        jmp     .n
.w:     call    hex16
.n:     pop     si
        jmp     .l
.e:     ret

; ---------------------------------------------------------------- Video

t_video:
        T       'INT10 0F Modus'
        mov     ah, 0Fh
        int     10h
        call    snap
        R       ax
        RH      bx
        T       'INT10 03 Cursor'
        mov     ah, 3
        mov     bh, 0
        int     10h
        call    snap
        R       cx
        T       'INT10 02/03 setzen'
        mov     ah, 2
        mov     bh, 0
        mov     dx, 0510h
        int     10h
        mov     ah, 3
        mov     bh, 0
        int     10h
        call    snap
        R       dx
        T       'INT10 09/08 Zeichen'
        mov     ax, 0941h
        mov     bx, 001Eh
        mov     cx, 2
        int     10h
        mov     ah, 8
        mov     bh, 0
        int     10h
        call    snap
        R       ax
        T       'INT10 0E TTY/Cursor'
        mov     ax, 0E42h
        mov     bx, 7
        int     10h
        mov     ah, 3
        mov     bh, 0
        int     10h
        call    snap
        R       dx
        T       'INT10 13 Zeichenkette'
        mov     ax, 1301h
        mov     bx, 0007h
        mov     cx, 3
        mov     dx, 0600h
        mov     bp, s_abc
        int     10h
        mov     ah, 3
        mov     bh, 0
        int     10h
        call    snap
        R       dx
        mov     ax, 0600h               ; wieder leeren
        mov     bh, 7
        mov     cx, 0500h
        mov     dx, 064Fh
        int     10h
        T       'INT10 06 Rollen'
        mov     ax, 0601h
        mov     bh, 1Fh
        mov     cx, 0A00h
        mov     dx, 0A4Fh
        int     10h
        mov     ah, 2
        mov     bh, 0
        mov     dx, 0A05h
        int     10h
        mov     ah, 8
        mov     bh, 0
        int     10h
        call    snap
        R       ax
        mov     ax, 0600h
        mov     bh, 7
        mov     cx, 0A00h
        mov     dx, 0A4Fh
        int     10h
        T       'INT10 1A00 Kombination'
        mov     ax, 1A00h
        int     10h
        call    snap
        RL      ax
        R       bx
        T       'INT10 1200/10 EGA-Info'
        mov     ah, 12h
        mov     bl, 10h
        int     10h
        call    snap
        R       bx
        RL      cx
        T       'INT10 1130 Zeichensatz'
        mov     ax, 1130h
        mov     bh, 0
        push    bp
        int     10h
        pop     bp
        call    snap
        R       cx
        RL      dx
        T       'INT10 1B Zustand'
        mov     ah, 1Bh
        xor     bx, bx
        mov     di, buf
        mov     cx, 64
        mov     al, 0EEh
        rep     stosb
        mov     di, buf
        mov     ah, 1Bh
        int     10h
        call    snap
        RL      ax
        S       ' '
        mov     si, buf + 4             ; Zeiger auf Tabelle auslassen
        mov     cx, 60
        call    hexbuf
        T       'INT10 1B Tabelle'
        les     si, [buf]
        mov     cx, 16
        call    hexbuf
        push    cs
        pop     es
        T       'INT10 1C00 Zustandsgroesse'
        mov     ax, 1C00h
        mov     cx, 7
        int     10h
        call    snap
        RL      ax
        R       bx
        T       'INT10 1008 Rand'
        mov     ax, 1008h
        mov     bh, 0EEh
        int     10h
        call    snap
        RH      bx
        T       'INT10 1007 Attribut 1'
        mov     ax, 1007h
        mov     bx, 0EE01h
        int     10h
        call    snap
        RH      bx
        T       'INT10 1015 DAC 7'
        mov     ax, 1015h
        mov     bx, 7
        mov     cx, 0EEEEh
        mov     dh, 0EEh
        int     10h
        call    snap
        RH      dx
        R       cx
        T       'INT10 101A DAC-Seite'
        mov     ax, 101Ah
        int     10h
        call    snap
        R       bx
        T       'INT10 4F00 VBE'
        mov     ax, 4F00h
        mov     di, buf
        int     10h
        call    snap
        R       ax
        T       'INT10 FE00 TopView'
        mov     ah, 0FEh
        mov     di, 1234h
        int     10h
        call    snap
        R       di
        ret

s_abc   db      'XYZ'

; ------------------------------------------------------------- Tastatur

t_kbd:  T       'INT16 02 Umschalt'
        mov     ah, 2
        int     16h
        call    snap
        RL      ax
        T       'INT16 12 erweitert'
        mov     ah, 12h
        int     16h
        call    snap
        R       ax
        T       'INT16 01 leer'
        mov     ah, 1
        int     16h
        call    snap
        ZF
        T       'INT16 11 leer'
        mov     ah, 11h
        int     16h
        call    snap
        ZF
        T       'INT16 05 ablegen'
        mov     ah, 5
        mov     cx, 1E61h
        int     16h
        call    snap
        RL      ax
        T       'INT16 01 nach 05'
        mov     ah, 1
        int     16h
        call    snap
        ZF
        R       ax
        T       'INT16 00 nach 05'
        mov     ah, 0
        int     16h
        call    snap
        R       ax
        T       'INT16 05 FFFF'
        mov     ah, 5
        mov     cx, 0FFFFh
        int     16h
        mov     ah, 10h
        int     16h
        call    snap
        R       ax
        T       'INT16 05 voll'
        mov     cx, 20                  ; Puffer fasst 15
.f:     push    cx
        mov     ah, 5
        mov     cx, 3920h
        int     16h
        pop     cx
        mov     [r_ax], al
        loop    .f
        mov     al, [r_ax]
        call    space
        call    hex8
.d:     mov     ah, 1                   ; wieder leeren
        int     16h
        jz      .x
        mov     ah, 0
        int     16h
        jmp     .d
.x:     T       'INT16 BDA-Puffer leer'
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:1Ah]
        sub     ax, [es:1Ch]
        push    cs
        pop     es
        call    space
        call    hex16
        T       'INT16 09 Faehigkeiten'
        mov     ax, 0900h
        int     16h
        call    snap
        RL      ax
        T       'INT16 03 Rate'
        mov     ax, 0305h
        mov     bx, 010Ch
        int     16h
        call    snap
        CF
        ret

; ------------------------------------------------------------------ Uhr

t_clock:
        T       'INT1A 00 Takt'
        mov     ah, 0
        int     1Ah
        call    snap
        S       ' laeuft='              ; Takt muss zwischen zwei Abfragen
        mov     ax, [r_dx]              ; wachsen (Warten ueber 46Ch)
        mov     [tmp], ax
        mov     cx, 0
.w:     mov     ah, 0
        int     1Ah
        cmp     dx, [tmp]
        jne     .ok
        push    cx
        mov     cx, 2000
.ww:    loop    .ww
        pop     cx
        loop    .w
        mov     al, '0'
        jmp     .p
.ok:    mov     al, '1'
.p:     call    putc
        T       'INT1A 02 RTC-Zeit'
        mov     ah, 2
        int     1Ah
        call    snap
        CF
        T       'INT1A 04 RTC-Datum'
        mov     ah, 4
        int     1Ah
        call    snap
        CF
        S       ' Jh='
        mov     al, [r_cx + 1]
        call    hex8
        T       'INT1A B101 PCI'
        mov     ax, 0B101h
        int     1Ah
        call    snap
        RH      ax
        CF
        T       'BIOS F000:FFFE Modell'
        mov     ax, 0F000h
        mov     es, ax
        mov     al, [es:0FFFEh]
        push    cs
        pop     es
        call    space
        call    hex8
        ret

; ----------------------------------------------------------- INT 15h

t_misc15:
        T       'INT15 C0 Konfiguration'
        mov     ah, 0C0h
        int     15h
        call    snap
        CF
        test    word [r_fl], 1
        jnz     .n1
        RH      ax
        S       ' '
        mov     es, [r_es]              ; Tabelle: Laenge, Modell,
        mov     si, [r_bx]              ; Untermodell, Stand, Merkmale
        mov     cx, 8
        call    hexbuf
        push    cs
        pop     es
.n1:    T       'INT15 C1 EBDA'
        mov     ah, 0C1h
        int     15h
        call    snap
        CF
        T       'INT15 88 erweitert>0'
        mov     ah, 88h
        clc
        int     15h
        call    snap
        CF
        S       ' '
        mov     al, '0'
        cmp     word [r_ax], 0
        je      .z
        inc     al
.z:     call    putc
        T       'INT15 2402 A20'
        mov     ax, 2402h
        int     15h
        call    snap
        CF
        T       'INT15 2403 A20-Art'
        mov     ax, 2403h
        int     15h
        call    snap
        CF
        T       'INT15 86 warten'
        mov     ah, 86h
        xor     cx, cx
        mov     dx, 1000
        int     15h
        call    snap
        CF
        T       'INT15 C200 PS2-Maus'
        mov     ax, 0C200h
        mov     bh, 0
        int     15h
        call    snap
        CF
        T       'INT15 4F Tastenhaken'
        mov     ah, 4Fh                 ; Loslassen (sonst legt ein
        mov     al, 9Eh                 ; Tastaturhaken wie KEYB das
        stc                             ; Zeichen in den Puffer)
        int     15h
        call    snap
        CF
        RL      ax
        T       'INT15 90/91'
        mov     ax, 9001h
        int     15h
        call    snap
        CF
        T       'INT15 E801'
        mov     ax, 0E801h
        int     15h
        call    snap
        CF
        T       'INT15 53 APM'
        mov     ax, 5300h
        xor     bx, bx
        int     15h
        call    snap
        CF
        ret

; ---------------------------------------------------------- Disketten

t_disk: T       'INT13 08 A:'
        mov     ah, 8
        mov     dl, 0
        push    es
        int     13h
        call    snap
        pop     es
        CF
        RL      bx
        R       cx
        R       dx
        T       'INT13 15 A:'
        mov     ah, 15h
        mov     dl, 0
        int     13h
        call    snap
        CF
        RH      ax
        T       'INT13 15 B:'
        mov     ah, 15h
        mov     dl, 1
        int     13h
        call    snap
        CF
        RH      ax
        T       'INT13 00 A:'
        mov     ah, 0
        mov     dl, 0
        int     13h
        call    snap
        CF
        RH      ax
        T       'INT13 01 Status'
        mov     ah, 1
        mov     dl, 0
        int     13h
        call    snap
        RH      ax
        T       'INT13 02 A: Sektor 1'
        mov     ax, 0201h
        mov     cx, 1
        mov     dx, 0
        mov     bx, buf
        int     13h
        call    snap
        CF
        RH      ax
        S       ' sig='
        mov     ax, [buf + 510]
        call    hex16
        T       'INT13 16 A:'
        mov     ah, 16h
        mov     dl, 0
        int     13h
        call    snap
        CF
        RH      ax
        T       'INT13 08 80h'
        mov     ah, 8
        mov     dl, 80h
        push    es
        int     13h
        call    snap
        pop     es
        CF
        S       ' Platten='
        mov     al, [r_dx]
        call    hex8
        T       'INT13 41 80h'
        mov     ah, 41h
        mov     bx, 55AAh
        mov     dl, 80h
        int     13h
        call    snap
        CF
        T       'INT13 15 80h'
        mov     ah, 15h
        mov     dl, 80h
        int     13h
        call    snap
        CF
        RH      ax
        T       'INT1E Diskettenparameter'
        xor     ax, ax
        mov     es, ax
        les     si, [es:1Eh * 4]
        mov     cx, 11
        call    hexbuf
        push    cs
        pop     es
        ret

; ------------------------------------------------ serielle/parallele

t_ports:
        T       'INT14 03 COM1'
        mov     ah, 3
        mov     dx, 0
        int     14h
        call    snap
        RH      ax
        T       'INT17 02 LPT1'
        mov     ah, 2
        mov     dx, 0
        int     17h
        call    snap
        RH      ax
        T       'INT33 0000 Maus'
        xor     ax, ax                  ; Vektor 0:0 = kein Treiber
        mov     es, ax
        mov     ax, [es:33h * 4]
        or      ax, [es:33h * 4 + 2]
        push    cs
        pop     es
        jnz     .m
        S       ' kein Vektor'
        ret
.m:     mov     ax, 0
        mov     bx, 0
        int     33h
        call    snap
        R       ax
        ret

; ------------------------------------------------------------ DOS-Kern

t_dos:  T       'DOS 30 Version'
        mov     ax, 3000h
        int     21h
        call    snap
        R       ax
        T       'DOS 3306 echte Version'
        mov     ax, 3306h
        mov     bx, 0
        int     21h
        call    snap
        R       bx
        T       'DOS 3305 Startlaufwerk'
        mov     ax, 3305h
        int     21h
        call    snap
        RL      dx
        T       'DOS 3300 Untbr'
        mov     ax, 3300h
        int     21h
        call    snap
        RL      dx
        T       'DOS 54 Verify'
        mov     ah, 54h
        int     21h
        call    snap
        RL      ax
        T       'DOS 37 Schalterzeichen'
        mov     ax, 3700h
        int     21h
        call    snap
        RL      dx
        T       'DOS 0B Taste da'
        mov     ah, 0Bh
        int     21h
        call    snap
        RL      ax
        T       'DOS 48 FFFF'
        mov     ah, 48h
        mov     bx, 0FFFFh
        int     21h
        call    snap
        CF
        R       ax
        T       'DOS 48/49 1000h'
        mov     ah, 48h
        mov     bx, 1000h
        int     21h
        call    snap
        CF
        test    word [r_fl], 1
        jnz     .n
        mov     es, [r_ax]
        mov     ah, 49h
        int     21h
        call    snap
        CF
.n:     T       'DOS 58 Strategie'
        mov     ax, 5800h
        int     21h
        call    snap
        CF
        RL      ax
        T       'DOS 5802 UMB-Kette'
        mov     ax, 5802h
        int     21h
        call    snap
        CF
        RL      ax
        T       'DOS 59 erweiterter Fehler'
        mov     ah, 3Dh                 ; Fehler 2 erzeugen
        mov     al, 0
        mov     dx, n_none
        int     21h
        mov     ah, 59h
        xor     bx, bx
        push    ds
        int     21h
        pop     ds
        call    snap
        R       ax
        R       bx
        RH      cx
        T       'DOS 29 Parse'
        mov     si, n_parse
        mov     di, fcb2
        mov     ax, 2901h
        int     21h
        call    snap
        RL      ax
        S       ' '
        mov     si, fcb2
        mov     cx, 12
        call    txtbuf
        T       'DOS 19 Laufwerk'
        mov     ah, 19h
        int     21h
        call    snap
        mov     [cur_drv], al
        mov     al, [r_ax]
        add     al, 'A'
        call    space
        call    putc
        T       'DOS 0E Laufwerke'
        mov     ah, 0Eh
        mov     dl, [cur_drv]
        int     21h
        call    snap
        S       ' >=3:'                 ; LASTDRIVE ist Einstellung
        mov     al, '0'
        cmp     byte [r_ax], 3
        jb      .l
        inc     al
.l:     call    putc
        ret

n_none  db      'X:\NIXDA\NIX.TXT', 0
n_parse db      'B:LW  .LLD', 0

; ------------------------------------------------------- Dateien

; Laufwerk aus dem zweiten Argument einstellen, \BCT anlegen
t_files:
        mov     al, [tdrv]
        or      al, al
        jz      .cur
        sub     al, 'A'
        mov     dl, al
        mov     ah, 0Eh
        int     21h
.cur:   mov     ah, 19h
        int     21h
        add     al, 'A'
        mov     [p_root], al
        T       'DOS 36 frei'
        mov     ah, 36h
        mov     dl, 0
        int     21h
        call    snap
        R       ax                    ; Sektoren je Cluster
        R       cx                    ; Bytes je Sektor
        S       ' gesamt>frei:'
        mov     ax, [r_dx]
        cmp     ax, [r_bx]
        mov     al, '0'
        jb      .g
        inc     al
.g:     call    putc
        T       'DOS 1C FAT-Info'
        mov     ah, 1Ch
        mov     dl, 0
        push    ds
        int     21h
        pop     ds
        call    snap
        RL      ax
        R       cx
        T       'DOS 47 Verzeichnis'
        mov     ah, 47h
        mov     dl, 0
        mov     si, buf
        int     21h
        call    snap
        CF
        S       ' \'
        mov     si, buf
        mov     cx, 64
        call    txtbuf
        T       'DOS 3B nicht da'
        mov     ah, 3Bh
        mov     dx, n_nodir
        int     21h
        call    snap
        call    dosres
        T       'DOS 39 BCT'
        mov     ah, 39h
        mov     dx, n_bct
        int     21h
        call    snap
        call    dosres
        T       'DOS 39 BCT noch mal'
        mov     ah, 39h
        mov     dx, n_bct
        int     21h
        call    snap
        call    dosres
        T       'DOS 39 SUB'
        mov     ah, 39h
        mov     dx, n_sub
        int     21h
        call    snap
        call    dosres
        T       'DOS 3B BCT'
        mov     ah, 3Bh
        mov     dx, n_bct
        int     21h
        call    snap
        call    dosres
        T       'DOS 47 in BCT'
        mov     ah, 47h
        mov     dl, 0
        mov     si, buf
        int     21h
        call    snap
        S       ' \'
        mov     si, buf
        mov     cx, 64
        call    txtbuf
        T       'DOS 3C A.TXT'
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        mov     bx, [r_ax]
        mov     [h1], bx
        T       'DOS 40 schreiben 300'
        mov     ah, 40h
        mov     bx, [h1]
        mov     cx, 300
        mov     dx, pattern
        int     21h
        call    snap
        call    dosres
        R       ax
        T       'DOS 42 Ende'
        mov     ax, 4202h
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    snap
        call    dosres
        R       dx
        R       ax
        T       'DOS 42 -10 von hier'
        mov     ax, 4201h
        mov     bx, [h1]
        mov     cx, 0FFFFh
        mov     dx, -10
        int     21h
        call    snap
        call    dosres
        R       ax
        T       'DOS 42 vor den Anfang'
        mov     ax, 4200h
        mov     bx, [h1]
        mov     cx, 0FFFFh
        mov     dx, 0FFF0h
        int     21h
        call    snap
        call    dosres
        R       dx
        R       ax
        T       'DOS 42 hinter das Ende+lesen'
        mov     ax, 4200h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 1000
        int     21h
        mov     ah, 3Fh
        mov     bx, [h1]
        mov     cx, 10
        mov     dx, buf
        int     21h
        call    snap
        call    dosres
        R       ax
        T       'DOS 40 Laenge 0 kuerzt'
        mov     ax, 4200h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 200
        int     21h
        mov     ah, 40h
        mov     bx, [h1]
        xor     cx, cx
        int     21h
        mov     ax, 4202h
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    snap
        R       ax
        T       'DOS 45 Dup'
        mov     ah, 45h
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        mov     bx, [r_ax]
        mov     [h2], bx
        mov     ax, 4201h               ; Stelle wird geteilt
        mov     bx, [h2]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    snap
        R       ax
        mov     ah, 3Eh
        mov     bx, [h2]
        int     21h
        T       'DOS 5700 Zeit lesen'
        mov     ax, 5701h
        mov     bx, [h1]
        mov     cx, 6000h               ; 12:00:00
        mov     dx, 2B6Eh               ; 14.11.2001 (Jahr 1980+21)
        int     21h
        mov     ax, 5700h
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        R       cx
        R       dx
        T       'DOS 3E A.TXT'
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        T       'DOS 3E noch mal'
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        T       'DOS 5700 nach Schliessen'
        mov     ax, 3D00h
        mov     dx, n_a
        int     21h
        mov     bx, ax
        mov     [h1], ax
        mov     ax, 5700h
        int     21h
        call    snap
        R       cx
        R       dx
        mov     ah, 3Fh                 ; Inhalt pruefen
        mov     bx, [h1]
        mov     cx, 300
        mov     dx, buf
        int     21h
        call    snap
        R       ax
        S       ' '
        mov     si, buf + 190
        mov     cx, 10
        call    hexbuf
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
        T       'DOS 3D nicht da'
        mov     ax, 3D00h
        mov     dx, n_none2
        int     21h
        call    snap
        call    dosres
        T       'DOS 3D Pfad nicht da'
        mov     ax, 3D00h
        mov     dx, n_nopath
        int     21h
        call    snap
        call    dosres
        T       'DOS 3D Verzeichnis'
        mov     ax, 3D00h
        mov     dx, n_sub2
        int     21h
        call    snap
        call    dosres
        T       'DOS 3D Name mit Leerzeichen'
        mov     ax, 3D00h
        mov     dx, n_asp
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .nsp
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.nsp:   T       'DOS 3D a.txt klein'
        mov     ax, 3D02h
        mov     dx, n_alow
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .nlo
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.nlo:   T       'DOS 3D zweimal lesen'
        mov     ax, 3D00h
        mov     dx, n_a
        int     21h
        mov     [h1], ax
        mov     ax, 3D00h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .n2
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.n2:    mov     bx, [h1]
        mov     ah, 3Eh
        int     21h
        T       'DOS 3D zweimal schreibend'
        mov     ax, 3D02h
        mov     dx, n_a
        int     21h
        mov     [h1], ax
        mov     ax, 3D02h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .n3
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.n3:    mov     bx, [h1]
        mov     ah, 3Eh
        int     21h
        T       'SHARE schreibschutz, dann W/R'
        mov     ax, 3D22h               ; RW, andere duerfen nicht schreiben
        mov     dx, n_a
        int     21h
        mov     [h1], ax
        mov     ax, 3D01h               ; schreiben: 20h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .s1
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.s1:    mov     ax, 3D40h               ; lesen, nichts verbieten: geht
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .s2
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.s2:    mov     bx, [h1]
        mov     ah, 3Eh
        int     21h
        T       'SHARE alles verboten'
        mov     ax, 3D10h
        mov     dx, n_a
        int     21h
        mov     [h1], ax
        mov     ax, 3D40h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .s3
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.s3:    mov     bx, [h1]
        mov     ah, 3Eh
        int     21h
        T       'SHARE Groesse ueber zwei Handles'
        mov     ax, 3D42h
        mov     dx, n_a
        int     21h
        mov     [h1], ax
        mov     ax, 3D42h
        mov     dx, n_a
        int     21h
        mov     [h2], ax
        mov     ax, 4202h
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        int     21h
        mov     ah, 40h
        mov     bx, [h1]
        mov     cx, 50
        mov     dx, pattern
        int     21h
        mov     ax, 4202h
        mov     bx, [h2]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    snap
        call    dosres
        R       ax
        T       'SHARE Sperren'
        mov     ax, 5C00h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 10h
        xor     si, si
        mov     di, 10h
        int     21h
        call    snap
        call    dosres
        mov     ax, 5C00h               ; ueberschneidet sich: 21h
        mov     bx, [h2]
        xor     cx, cx
        mov     dx, 18h
        xor     si, si
        mov     di, 10h
        int     21h
        call    snap
        call    dosres
        mov     ax, 5C01h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 10h
        xor     si, si
        mov     di, 10h
        int     21h
        call    snap
        call    dosres
        mov     ax, 5C01h               ; nicht (mehr) gesperrt: 21h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 10h
        xor     si, si
        mov     di, 10h
        int     21h
        call    snap
        call    dosres
        mov     bx, [h2]
        mov     ah, 3Eh
        int     21h
        mov     bx, [h1]
        mov     ah, 3Eh
        int     21h
        T       'DOS 4300 Attribut'
        mov     ax, 4300h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        R       cx
        T       'DOS 4300 Verzeichnis'
        mov     ax, 4300h
        mov     dx, n_sub2
        int     21h
        call    snap
        call    dosres
        R       cx
        T       'DOS 4301 R'
        mov     ax, 4301h
        mov     cx, 1
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        T       'DOS 3D R schreibend'
        mov     ax, 3D01h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .nr
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.nr:    T       'DOS 3C R ueberschreiben'
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .nr2
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.nr2:   T       'DOS 41 R loeschen'
        mov     ah, 41h
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        mov     ax, 4301h
        mov     cx, 0
        mov     dx, n_a
        int     21h
        T       'DOS 5B neu, gibt es'
        mov     ah, 5Bh
        xor     cx, cx
        mov     dx, n_a
        int     21h
        call    snap
        call    dosres
        T       'DOS 5B neu B.TXT'
        mov     ah, 5Bh
        xor     cx, cx
        mov     dx, n_b
        int     21h
        call    snap
        call    dosres
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
        T       'DOS 56 auf vorhandenen'
        mov     ah, 56h
        mov     dx, n_b
        mov     di, n_a
        int     21h
        call    snap
        call    dosres
        T       'DOS 56 in SUB'
        mov     ah, 56h
        mov     dx, n_b
        mov     di, n_subb
        int     21h
        call    snap
        call    dosres
        T       'DOS 56 nicht da'
        mov     ah, 56h
        mov     dx, n_none2
        mov     di, n_c
        int     21h
        call    snap
        call    dosres
        T       'DOS 5A Zwischendatei'
        mov     di, buf                 ; Pfad "" = aktuelles Verzeichnis
        mov     byte [di], 0
        mov     ah, 5Ah
        xor     cx, cx
        mov     dx, buf
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .nt
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
        S       ' Laenge='
        mov     si, buf
        xor     cx, cx
.tl:    lodsb
        or      al, al
        jz      .te
        inc     cx
        jmp     .tl
.te:    mov     al, cl
        call    hex8
        mov     ah, 41h
        mov     dx, buf
        int     21h
.nt:    T       'DOS 6C oeffnen/anlegen'
        mov     ax, 6C00h
        mov     bx, 2
        xor     cx, cx
        mov     dx, 11h                 ; da: oeffnen, sonst anlegen
        mov     si, n_c
        int     21h
        call    snap
        call    dosres
        R       cx
        test    word [r_fl], 1
        jnz     .n6
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.n6:    T       'DOS 6C noch mal'
        mov     ax, 6C00h
        mov     bx, 2
        xor     cx, cx
        mov     dx, 11h
        mov     si, n_c
        int     21h
        call    snap
        call    dosres
        R       cx
        test    word [r_fl], 1
        jnz     .n7
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.n7:    T       'DOS 6C ersetzen'
        mov     ax, 6C00h
        mov     bx, 2
        xor     cx, cx
        mov     dx, 12h
        mov     si, n_c
        int     21h
        call    snap
        call    dosres
        R       cx
        test    word [r_fl], 1
        jnz     .n8
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.n8:    T       'DOS 6C nicht da, nicht anlegen'
        mov     ax, 6C00h
        mov     bx, 0
        xor     cx, cx
        mov     dx, 01h
        mov     si, n_none2
        int     21h
        call    snap
        call    dosres
        T       'DOS 60 Truename'
        mov     ah, 60h
        mov     si, n_tru
        mov     di, buf
        int     21h
        call    snap
        call    dosres
        S       ' '
        mov     si, buf + 1             ; Laufwerk auslassen
        mov     cx, 64
        call    txtbuf
        T       'DOS 3A nicht leer'
        mov     ah, 3Ah
        mov     dx, n_sub2
        int     21h
        call    snap
        call    dosres
        T       'DOS 3A aktuelles'
        mov     ah, 3Ah
        mov     dx, n_dot
        int     21h
        call    snap
        call    dosres
        T       'DOS 41 Platzhalter'
        mov     ah, 41h
        mov     dx, n_wild
        int     21h
        call    snap
        call    dosres
        T       'DOS 5C sperren'
        mov     ax, 3D02h
        mov     dx, n_a
        int     21h
        mov     [h1], ax
        mov     ax, 5C00h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 10h
        xor     si, si
        mov     di, 10h
        int     21h
        call    snap
        call    dosres
        mov     ax, 5C01h
        mov     bx, [h1]
        xor     cx, cx
        mov     dx, 10h
        xor     si, si
        mov     di, 10h
        int     21h
        call    snap
        call    dosres
        T       'DOS 68 Commit'
        mov     ah, 68h
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        T       'DOS 4400 Datei'
        mov     ax, 4400h
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        mov     ax, [r_dx]              ; Laufwerk Bits 0-5 auslassen
        and     ax, 0FFC0h
        mov     [r_dx], ax
        R       dx
        T       'DOS 440A Datei fern'
        mov     ax, 440Ah
        mov     bx, [h1]
        int     21h
        call    snap
        call    dosres
        R       dx
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
        ret

n_nodir db      'NIXDA', 0
n_bct   db      '\BCT', 0
n_sub   db      '\BCT\SUB', 0
n_sub2  db      'SUB', 0
n_subb  db      'SUB\B.TXT', 0
n_a     db      'A.TXT', 0
n_alow  db      'a.txt', 0
n_asp   db      'A   .TXT', 0
n_b     db      'B.TXT', 0
n_c     db      'C.TXT', 0
n_none2 db      'NIX.TXT', 0
n_nopath db     'NIXDA\NIX.TXT', 0
n_tru   db      'sub\..\a.txt', 0
n_dot   db      '.', 0
n_wild  db      'ZZ*.*', 0

; ---------------------------------------------------- Suche (4E/4F)

; eine Suche ausgeben: SI -> Muster, CX = Attribut, DTA = dta1
%macro  FIND 3                          ; Name, Muster, Attribut
        T       %1
        mov     dx, dta1
        mov     ah, 1Ah
        int     21h
        mov     dx, %2
        mov     cx, %3
        call    findall
%endmacro

findall:
        mov     ah, 4Eh
        int     21h
        call    snap
        call    dosres
.l:     test    word [r_fl], 1
        jnz     .e
        call    dta_out
        mov     ah, 4Fh
        int     21h
        call    snap
        jmp     .l
.e:     mov     al, ' '                 ; Fehler, mit dem die Suche endet
        call    putc
        mov     ax, [r_ax]
        call    hex16
        ret

dta_out:
        call    space                   ; Name, Attribut, Groesse
        mov     si, dta1 + 1Eh
        mov     cx, 13
        call    txtbuf
        mov     al, '/'
        call    putc
        mov     al, [dta1 + 15h]
        call    hex8
        mov     al, '/'
        call    putc
        mov     ax, [dta1 + 1Ch]
        call    hex16
        mov     ax, [dta1 + 1Ah]
        call    hex16
        ret

t_find: mov     ah, 3Ch                 ; Bestand: A, C, D1..D3, SUB\B
        xor     cx, cx
        mov     dx, n_d1
        int     21h
        mov     bx, ax
        mov     ah, 3Eh
        int     21h
        mov     ah, 3Ch
        mov     cx, 2                   ; versteckt
        mov     dx, n_d2
        int     21h
        mov     bx, ax
        mov     ah, 3Eh
        int     21h
        mov     ah, 3Ch
        mov     cx, 0
        mov     dx, n_d3
        int     21h
        mov     bx, ax
        mov     ah, 3Eh
        int     21h
        FIND    'FIND *.*', p_all, 0
        FIND    'FIND *.* versteckt', p_all, 6
        FIND    'FIND *.* Verzeichnisse', p_all, 10h
        FIND    'FIND D?.TXT', p_dq, 0
        FIND    'FIND *', p_star, 0
        FIND    'FIND D*', p_dstar, 0
        FIND    'FIND .', n_dot, 10h
        FIND    'FIND kein Treffer', p_none, 0
        FIND    'FIND Pfad nicht da', p_nopath, 0
        FIND    'FIND SUB\*.*', p_sub, 10h
        FIND    'FIND Bezeichnung', p_lab, 8
        FIND    'FIND ohne Platzhalter', n_a, 0
        FIND    'FIND Leerzeichen', p_sp, 0
        T       'FIND verschachtelt'    ; Word-SETUP: zwei Suchen im Wechsel
        mov     dx, dta1
        mov     ah, 1Ah
        int     21h
        mov     dx, p_dq
        xor     cx, cx
        mov     ah, 4Eh
        int     21h
        call    dta_out
        mov     dx, dta2
        mov     ah, 1Ah
        int     21h
        mov     dx, p_sub
        mov     cx, 10h
        mov     ah, 4Eh
        int     21h
        mov     dx, dta3
        mov     ah, 1Ah
        int     21h
        mov     dx, p_all
        mov     cx, 0
        mov     ah, 4Eh
        int     21h
        mov     dx, dta1                ; erste Suche weiter
        mov     ah, 1Ah
        int     21h
.l:     mov     ah, 4Fh
        int     21h
        call    snap
        jc      .e
        call    dta_out
        jmp     .l
.e:     mov     ax, [r_ax]
        call    space
        call    hex16
        T       'FIND Loeschen dabei'
        mov     dx, dta1
        mov     ah, 1Ah
        int     21h
        mov     dx, p_dq
        xor     cx, cx
        mov     ah, 4Eh
        int     21h
        call    dta_out
        mov     dx, dta1 + 1Eh          ; gefundene Datei loeschen
        mov     ah, 41h
        int     21h
.l2:    mov     ah, 4Fh
        int     21h
        call    snap
        jc      .e2
        call    dta_out
        jmp     .l2
.e2:    mov     ax, [r_ax]
        call    space
        call    hex16
        T       'FIND 4F ohne 4E'
        mov     di, dta2
        mov     cx, 43
        xor     al, al
        rep     stosb
        mov     dx, dta2
        mov     ah, 1Ah
        int     21h
        mov     ah, 4Fh
        int     21h
        call    snap
        call    dosres
        mov     dx, 80h
        mov     ah, 1Ah
        int     21h
        ret

n_d1    db      'D1.TXT', 0
n_d2    db      'D2.TXT', 0
n_d3    db      'D3.TXT', 0
p_all   db      '*.*', 0
p_dq    db      'D?.TXT', 0
p_star  db      '*', 0
p_dstar db      'D*', 0
p_none  db      'QQ*.*', 0
p_nopath db     'NIXDA\*.*', 0
p_sub   db      'SUB\*.*', 0
p_lab   db      '\*.*', 0
p_sp    db      'D1  .TXT', 0

; ------------------------------------------------------------- FCB

t_fcb:  mov     dx, dta1
        mov     ah, 1Ah
        int     21h
        T       'FCB 11 Suche'
        mov     si, n_fcbq
        call    fcbset
        mov     ah, 11h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        S       ' '
        mov     si, dta1 + 1
        mov     cx, 11
        call    txtbuf
        mov     ah, 12h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        S       ' '
        mov     si, dta1 + 1
        mov     cx, 11
        call    txtbuf
        T       'FCB 16 anlegen'
        mov     si, n_fcbf
        call    fcbset
        mov     ah, 16h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        S       ' Satz='
        mov     ax, [fcb + 0Eh]
        call    hex16
        T       'FCB 15 schreiben'
        mov     ah, 15h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        T       'FCB 10 schliessen'
        mov     ah, 10h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        T       'FCB 0F oeffnen'
        mov     si, n_fcbf
        call    fcbset
        mov     ah, 0Fh
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        S       ' Groesse='
        mov     ax, [fcb + 12h]
        call    hex16
        mov     ax, [fcb + 10h]
        call    hex16
        T       'FCB 14 lesen'
        mov     ah, 14h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        mov     ah, 14h                 ; zweiter Satz: Dateiende
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        mov     ah, 10h
        mov     dx, fcb
        int     21h
        T       'FCB 23 Groesse'
        mov     si, n_fcbf
        call    fcbset
        mov     word [fcb + 0Eh], 32
        mov     ah, 23h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        S       ' Saetze='
        mov     ax, [fcb + 21h]
        call    hex16
        T       'FCB 17 umbenennen'
        mov     si, n_fcbr
        call    fcbset
        mov     ah, 17h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        T       'FCB 13 loeschen'
        mov     si, n_fcbd
        call    fcbset
        mov     ah, 13h
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        T       'FCB 0F nicht da'
        mov     si, n_fcbd
        call    fcbset
        mov     ah, 0Fh
        mov     dx, fcb
        int     21h
        call    snap
        RL      ax
        mov     dx, 80h
        mov     ah, 1Ah
        int     21h
        ret

; FCB aus 12 (+16 fuer Umbenennen) Byte bei SI fuellen
fcbset: mov     di, fcb
        push    si
        mov     cx, 40
        xor     al, al
        rep     stosb
        pop     si
        mov     di, fcb
        mov     cx, 12
        rep     movsb
        mov     di, fcb + 10h           ; neuer Name beim Umbenennen
        mov     cx, 12
        rep     movsb
        ret

n_fcbq  db      0, 'D???????TXT', 0, '           '
n_fcbf  db      0, 'FCBTEST DAT', 0, '           '
n_fcbr  db      0, 'FCBTEST DAT', 0, 'FCBNEU  DAT'
n_fcbd  db      0, 'FCBNEU  DAT', 0, '           '

; --------------------------------------------------------- IOCTL

t_ioctl:
        T       'IOCTL 4400 Handle 0'
        mov     ax, 4400h
        xor     bx, bx
        int     21h
        call    snap
        call    dosres
        R       dx
        T       'IOCTL 4408 Wechselmedium'
        mov     ax, 4408h
        mov     bl, 0
        int     21h
        call    snap
        call    dosres
        R       ax
        T       'IOCTL 4409 fern'
        mov     ax, 4409h
        mov     bl, 0
        int     21h
        call    snap
        call    dosres
        R       dx
        T       'IOCTL 440E logisch'
        mov     ax, 440Eh
        mov     bl, 0
        int     21h
        call    snap
        call    dosres
        RL      ax
        T       'IOCTL 440D 0860 Parameter'
        mov     ax, 440Dh
        mov     bl, 0
        mov     cx, 0860h
        mov     dx, buf
        mov     byte [buf], 0
        int     21h
        call    snap
        call    dosres
        S       ' '
        mov     si, buf + 7             ; BPB: Bytes/Sektor, Sekt./Cluster
        mov     cx, 3
        call    hexbuf
        T       'IOCTL 440D 0866 Seriennummer'
        mov     ax, 440Dh
        mov     bl, 0
        mov     cx, 0866h
        mov     dx, buf
        int     21h
        call    snap
        call    dosres
        S       ' '
        mov     si, buf + 17            ; FS-Typ
        mov     cx, 8
        call    txtbuf
        T       'DOS 6900 Seriennummer'
        mov     ax, 6900h
        mov     bl, 0
        mov     dx, buf
        int     21h
        call    snap
        call    dosres
        S       ' '
        mov     si, buf + 17
        mov     cx, 8
        call    txtbuf
        T       'DOS 3D EMMXXXX0'
        mov     ax, 3D00h
        mov     dx, n_emm
        int     21h
        call    snap
        CF
        test    word [r_fl], 1
        jnz     .ne
        mov     bx, [r_ax]
        mov     ah, 3Eh
        int     21h
.ne:    T       'DOS 3D NUL'
        mov     ax, 3D02h
        mov     dx, n_nul
        int     21h
        call    snap
        call    dosres
        test    word [r_fl], 1
        jnz     .nn
        mov     bx, [r_ax]
        mov     [h1], bx
        mov     ax, 4400h
        int     21h
        call    snap
        R       dx
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
.nn:    ret

n_emm   db      'EMMXXXX0', 0
n_nul   db      'NUL', 0

; ------------------------------------------------- INT 2Fh-Abfragen

mux_tab: ; AX, was ausgeben (1 = AL, 2 = AX, 3 = AX+BX)
        dw      0100h, 1                ; PRINT
        dw      0600h, 1                ; ASSIGN
        dw      1000h, 1                ; SHARE
        dw      1100h, 1                ; Redirector
        dw      1400h, 1                ; NLSFUNC
        dw      1500h, 3                ; MSCDEX (BX = Laufwerke)
        dw      1600h, 1                ; Windows erweitert
        dw      1A00h, 1                ; ANSI.SYS
        dw      4300h, 1                ; XMS
        dw      4800h, 1                ; DOSKEY
        dw      4A10h, 2                ; SMARTDRV
        dw      0B700h, 1               ; APPEND
        dw      0AD00h, 1               ; DISPLAY/KEYB
        dw      0, 0

t_mux:  mov     si, mux_tab
.l:     lodsw
        mov     dx, ax
        lodsw
        or      dx, dx
        jz      .e
        push    si
        push    ax
        T       'INT2F'
        mov     ax, dx
        call    space
        call    hex16
        mov     ax, dx
        xor     bx, bx
        mov     cx, 0EBABh              ; SMARTDRV-Kennung
        int     2Fh
        call    snap
        pop     ax
        cmp     al, 1
        jne     .w
        RL      ax
        jmp     .n
.w:     R       ax
        cmp     al, 3
        jne     .n
        R       bx
.n:     pop     si
        jmp     .l
.e:     ret

; ------------------------------- DOS-intern: INT 2Fh 12xxh, 21h 5Eh/5Fh

t_dosint:
        T       'DOS 5E00 Rechnername'
        mov     ax, 5E00h
        mov     dx, buf
        xor     cx, cx
        int     21h
        call    snap
        CF
        RH      cx
        T       'DOS 5F02 Umleitung 0'
        mov     ax, 5F02h
        xor     bx, bx
        mov     si, buf
        mov     di, buf + 40
        int     21h
        call    snap
        call    dosres
        T       'DOS 5F07 C: an'
        mov     ax, 5F07h
        mov     dl, 2
        int     21h
        call    snap
        call    dosres
        T       'DOS 5F08 Z: aus'
        mov     ax, 5F08h
        mov     dl, 25
        int     21h
        call    snap
        call    dosres
        T       'INT2F 1200 da'
        mov     ax, 1200h
        int     2Fh
        call    snap
        RL      ax
        T       'INT2F 1204 /'
        mov     ax, '/'
        push    ax
        mov     ax, 1204h
        int     2Fh
        call    snap
        pop     ax
        RL      ax
        ZF
        T       'INT2F 1213 q'
        mov     ax, 'q'
        push    ax
        mov     ax, 1213h
        int     2Fh
        call    snap
        pop     ax
        RL      ax
        T       'INT2F 1211 normieren'
        mov     si, n_norm
        mov     di, buf
        mov     ax, 1211h
        int     2Fh
        call    snap
        S       ' '
        mov     si, buf
        call    puts
        T       'INT2F 1212/1225 Laenge'
        mov     di, n_norm
        mov     ax, 1212h
        int     2Fh
        call    snap
        R       cx
        mov     si, n_norm
        mov     ax, 1225h
        int     2Fh
        call    snap
        R       cx
        T       'INT2F 1214 gleich'
        mov     si, buf
        mov     di, buf
        mov     ax, 1214h
        int     2Fh
        call    snap
        ZF
        T       'INT2F 121A Laufwerk'
        mov     si, n_drvn
        mov     ax, 121Ah
        int     2Fh
        call    snap
        RL      ax
        mov     ax, [r_si]
        sub     ax, n_drvn
        mov     [r_si], ax
        R       si
        T       'INT2F 121E vergleichen'
        mov     si, n_cmp1
        mov     di, n_cmp2
        mov     ax, 121Eh
        int     2Fh
        call    snap
        ZF
        ret
        ; 1216h/1218h/1220h/1226h ff. nur von innen (SS = DOS) - nicht hier

n_norm  db      'c:/dos/abc.Txt', 0
n_drvn  db      'd:\x', 0
n_cmp1  db      'A/B.C', 0
n_cmp2  db      'a\b.c', 0

; ---------------------------------------------------------- Aufraeumen

cleanup:
        mov     dx, dta1
        mov     ah, 1Ah
        int     21h
        mov     ah, 3Bh                 ; \BCT
        mov     dx, n_bct
        int     21h
        mov     ah, 41h
        mov     dx, n_subb
        int     21h
        mov     ah, 3Bh
        mov     dx, n_sub
        int     21h
.l:     mov     ah, 4Eh                 ; alles in SUB
        mov     dx, p_all
        mov     cx, 6
        int     21h
        jc      .s
        mov     ax, 4301h
        xor     cx, cx
        mov     dx, dta1 + 1Eh
        int     21h
        mov     ah, 41h
        mov     dx, dta1 + 1Eh
        int     21h
        jnc     .l
.s:     mov     ah, 3Bh
        mov     dx, n_bct
        int     21h
.l2:    mov     ah, 4Eh
        mov     dx, p_all
        mov     cx, 6
        int     21h
        jc      .s2
        mov     ax, 4301h
        xor     cx, cx
        mov     dx, dta1 + 1Eh
        int     21h
        mov     ah, 41h
        mov     dx, dta1 + 1Eh
        int     21h
        jnc     .l2
.s2:    mov     ah, 3Bh
        mov     dx, n_root
        int     21h
        mov     ah, 3Ah
        mov     dx, n_sub
        int     21h
        mov     ah, 3Ah
        mov     dx, n_bct
        int     21h
        T       'Aufraeumen'
        mov     ax, 4300h
        mov     dx, n_bct
        int     21h
        call    snap
        call    dosres
        call    nl
        mov     dx, 80h
        mov     ah, 1Ah
        int     21h
        ret

n_root  db      '\', 0

; ------------------------------------------------------ Befehlszeile

args:   mov     si, 81h                 ; erstes Wort = Ausgabedatei
        mov     di, oname
.s:     lodsb
        cmp     al, ' '
        je      .s
        cmp     al, 9
        je      .s
.c:     cmp     al, ' '
        jbe     .e1
        stosb
        lodsb
        jmp     .c
.e1:    mov     byte [di], 0
        dec     si
.s2:    lodsb                           ; zweites Wort = Laufwerk
        cmp     al, ' '
        je      .s2
        cmp     al, 13
        je      .e
        cmp     al, 'a'
        jb      .u
        sub     al, 20h
.u:     cmp     al, 'A'
        jb      .e
        cmp     al, 'Z'
        ja      .e
        mov     [tdrv], al
.e:     ret

; ------------------------------------------------------------- Daten

pattern:
%assign i 0
%rep 300
        db      (i * 7 + 3) & 0FFh
%assign i i + 1
%endrep

oseg    dw      0
optr    dw      0
tdrv    db      0
cur_drv db      0
p_root  db      0
h1      dw      0
h2      dw      0
tmp     dw      0
r_ax    dw      0
r_bx    dw      0
r_cx    dw      0
r_dx    dw      0
r_si    dw      0
r_di    dw      0
r_bp    dw      0
r_es    dw      0
r_fl    dw      0
oname   times 80 db 0
fcb     times 40 db 0
fcb2    times 40 db 0
dta1    times 64 db 0
dta2    times 64 db 0
dta3    times 64 db 0
buf     times 600 db 0
        times 512 db 0
stk_end:
prog_end:
