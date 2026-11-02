; SPDX-License-Identifier: GPL-2.0-or-later
; BOXSHELL.COM - erster Prozess der DOS-Box von AT-DOS 6.0 (SHELL= in der
; CONFIG.SYS des Startabbilds). Holt sich ueber die Box-Schnittstelle
; (INT E0h) die Befehlszeile, mit der die Box gestartet wurde, startet
; FreeCOM damit (leer: interaktiv mit AUTOEXEC.BAT, sonst /C Befehl) und
; beendet die Box mit dessen Rueckgabewert, sobald FreeCOM endet (EXIT).
;
; Zuvor haengt BOXSHELL die AT-DOS-Laufwerke ab C: als Netzlaufwerke ein:
; CDS-Eintraege mit Flags C000h, INT 2Fh 11xxh (Redirector) geht ueber
; INT E0h 10h an den Kernel (kernel/redir.c). BOXSHELL bleibt als Eltern-
; prozess von FreeCOM im Speicher, der Haken also gueltig.
;
; INT E0h (nur in AT-DOS-Boxen): AH=00h Befehlszeile nach ES:DI (bis 120
; Zeichen, ohne Null) -> CX = Laenge; AH=01h Box beenden, AL = Rueckgabe-
; wert; AH=10h Redirector-Aufruf AL (ZF=1 erledigt, CF/AX Ergebnis);
; AH=11h SDA ist DS:SI; AH=12h -> DX:AX Bitmaske der AT-DOS-Laufwerke;
; AH=13h -> AL = Startlaufwerk der AT-DOS (0 = A:).

cpu 8086
org 100h

        mov     sp, stack_top
        mov     bx, ((stack_top - $$) + 100h + 15) / 16
        mov     ah, 4Ah                 ; Speicher auf das Noetige kuerzen
        int     21h

        call    redir_install
        mov     ah, 13h                 ; Startlaufwerk der AT-DOS
        int     0E0h
        add     al, 'A'
        mov     [t_dosbox+7], al
        mov     [t_setver], al
        call    setver_load             ; liest in envbuf, daher zuerst
        call    build_env

        push    cs
        pop     es
        mov     di, cmd
        xor     ah, ah                  ; Befehlszeile der Box
        int     0E0h
        mov     si, t_inter             ; leer: interaktiv
        jcxz    .build
        mov     si, t_cmd
.build: mov     di, tail + 1            ; "A:\ /E:1024 " + /K bzw. /C ...
.c1:    lodsb
        test    al, al
        jz      .c2
        stosb
        jmp     .c1
.c2:    mov     si, cmd                 ; Befehl anhaengen (cx Zeichen)
        rep     movsb
        mov     al, 13
        stosb
        mov     ax, di
        sub     ax, tail + 2
        mov     [tail], al

        mov     ax, cs                  ; Umgebung mit DOSBOX=
        mov     bx, envbuf
        mov     cl, 4
        shr     bx, cl
        add     ax, bx
        mov     [pblock], ax
        mov     [pb_tail+2], cs
        mov     [pb_fcb1+2], cs
        mov     [pb_fcb2+2], cs
        mov     dx, t_comspec
        mov     bx, pblock
        mov     ax, 4B00h               ; FreeCOM starten und warten
        int     21h
        mov     al, 0FFh
        jc      .exit
        mov     ah, 4Dh                 ; dessen Rueckgabewert
        int     21h
.exit:  mov     ah, 1                   ; Box beenden
        int     0E0h
        int     19h                     ; (ausserhalb einer Box: Neustart)

; ---- Umgebung: eigene + DOSBOX=X:\DOS\BOX\FREEDOS.IMG ------------------
; (wie unter AT-DOS; SETVER und spaeter andere Werkzeuge finden darueber
; das Startlaufwerk)
build_env:
        push    ds
        mov     ds, [2Ch]               ; eigene Umgebung
        xor     si, si
        mov     di, envbuf
        push    cs
        pop     es
.e1:    cmp     byte [si], 0            ; Ende der Liste?
        je      .e3
.e2:    lodsb
        stosb
        cmp     di, envbuf + ENVSZ - 48
        jae     .e3
        test    al, al
        jnz     .e2
        jmp     .e1
.e3:    pop     ds
        mov     si, t_dosbox
.e4:    lodsb
        stosb
        test    al, al
        jnz     .e4
        stosb                           ; zweite Null: Ende
        ret

; ---- SETVER: X:\DOS\BOX\SETVER.LST -> Versionstabelle (LoL+37h) --------
; Zeilen "NAME.EXT n.nn", ';' Kommentar. Tabelle wie beim SETVER-Treiber
; von MS-DOS: Laenge, Name, Haupt-, Nebenversion; 0 = Ende.
setver_load:
        push    cs
        pop     es
        mov     ax, 3D00h
        mov     dx, t_setver
        int     21h
        jc      .ret
        mov     bx, ax
        mov     ah, 3Fh
        mov     cx, RDSZ - 1
        mov     dx, rdbuf
        int     21h
        jc      .close
        mov     si, rdbuf
        add     si, ax
        mov     byte [si], 0
        mov     si, rdbuf
        mov     di, svtab
.line:  lodsb                           ; Leerraum uebergehen
        test    al, al
        jz      .end
        cmp     al, ' '
        jbe     .line
        cmp     al, ';'
        je      .skip
        dec     si
        cmp     di, svtab + SVSZ - 16   ; Platz fuer einen Eintrag?
        jae     .end
        push    di
        inc     di                      ; Laengenbyte
        xor     cx, cx
.name:  lodsb
        cmp     al, ' '
        jbe     .nend
        cmp     al, 'a'
        jb      .n1
        cmp     al, 'z'
        ja      .n1
        sub     al, 20h
.n1:    cmp     cx, 12
        jae     .name
        stosb
        inc     cx
        jmp     .name
.nend:  dec     si
.sp:    lodsb                           ; Leerzeichen bis zur Version
        cmp     al, ' '
        je      .sp
        cmp     al, 9
        je      .sp
        sub     al, '0'
        cmp     al, 9
        ja      .bad
        stosb                           ; Hauptversion (eine Ziffer)
        xor     dl, dl                  ; Nebenversion
        lodsb
        cmp     al, '.'
        jne     .minor
        lodsb
        sub     al, '0'
        cmp     al, 9
        ja      .minor
        mov     ah, 10
        mul     ah
        mov     dl, al
        lodsb
        sub     al, '0'
        cmp     al, 9
        ja      .minor
        add     dl, al
.minor: mov     al, dl
        stosb
        pop     bx                      ; Laengenbyte nachtragen
        jcxz    .drop
        mov     [bx], cl
        dec     si
        jmp     .skip
.drop:  mov     di, bx
        dec     si
        jmp     .skip
.bad:   pop     di                      ; Zeile verwerfen
        dec     si
.skip:  lodsb                           ; bis zum Zeilenende
        test    al, al
        jz      .end
        cmp     al, 10
        jne     .skip
        jmp     .line
.end:   mov     byte [di], 0
        cmp     di, svtab
        je      .close                  ; leer: nichts eintragen
        push    bx
        mov     ah, 52h
        int     21h
        mov     word [es:bx+37h], svtab
        mov     [es:bx+39h], cs
        pop     bx
.close: mov     ah, 3Eh
        int     21h
.ret:   ret

; ---- Redirector ----------------------------------------------------------
redir_install:
        push    ds
        mov     ax, 5D06h               ; DS:SI = SDA
        int     21h
        mov     ah, 11h                 ; dem Kernel melden
        int     0E0h
        pop     ds
        mov     ah, 12h                 ; welche Laufwerke hat AT-DOS?
        int     0E0h
        mov     [dmask], ax
        mov     [dmask+2], dx
        mov     ah, 52h                 ; Liste der Listen
        int     21h
        mov     cl, [es:bx+21h]         ; LASTDRIVE
        les     di, [es:bx+16h]         ; CDS
        xor     ch, ch
        xor     si, si                  ; Laufwerk
.d:     cmp     si, cx
        jae     .hook
        mov     bx, si
        mov     ax, [dmask]
        mov     dx, [dmask+2]
.sh:    test    bx, bx                  ; Bit si nach unten schieben
        jz      .tst
        shr     dx, 1
        rcr     ax, 1
        dec     bx
        jmp     .sh
.tst:   test    al, 1
        jz      .next
        mov     ax, si
        add     al, 'A'
        mov     [es:di], al             ; "X:"
        mov     word [es:di+1], 5C3Ah
        mov     byte [es:di+3], 0
        mov     word [es:di+43h], 0C000h    ; Netz + physisch
        mov     word [es:di+4Fh], 2     ; Stamm-Backslash
.next:  add     di, 58h
        inc     si
        jmp     .d
.hook:  mov     ax, 352Fh
        int     21h
        mov     [old2f], bx
        mov     [old2f+2], es
        mov     dx, int2f
        mov     ax, 252Fh
        int     21h
        ret

int2f:  cmp     ah, 11h
        jne     .chain
        test    al, al                  ; Installationsabfrage
        jnz     .op
        mov     al, 0FFh
        iret
.op:    push    ax
        mov     ah, 10h
        int     0E0h                    ; Kernel: ZF=1 erledigt
        jnz     .not
        inc     sp                      ; AX bleibt das Ergebnis; INC
        inc     sp                      ; laesst CF stehen (ADD nicht!)
        sti
        retf    2                       ; CF des Kernels zurueckgeben
.not:   pop     ax
.chain: jmp     far [cs:old2f]

ENVSZ     equ 1024
RDSZ      equ ENVSZ
SVSZ      equ 768
t_dosbox  db "DOSBOX=X:\DOS\BOX\FREEDOS.IMG", 0
t_setver  db "X:\DOS\BOX\SETVER.LST", 0
dmask     dd 0
old2f     dd 0

t_comspec db "A:\COMMAND.COM", 0
t_inter   db " A:\ /E:1024 /K A:\AUTOEXEC.BAT", 0
t_cmd     db " A:\ /E:1024 /C ", 0
pblock    dw 0                          ; Umgebung erben
pb_tail   dw tail, 0
pb_fcb1   dw fcb, 0
pb_fcb2   dw fcb, 0
fcb       times 16 db 0
tail      times 130 db 0
cmd       times 122 db 0
svtab     times SVSZ db 0               ; bleibt resident (LoL+37h)
          align 16
envbuf    times ENVSZ db 0
          times 256 db 0
stack_top:
rdbuf     equ envbuf                    ; Lesepuffer, vor dem Aufbau der Umgebung
