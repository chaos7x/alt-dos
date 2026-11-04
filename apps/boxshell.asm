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
; AH=13h -> AL = Startlaufwerk der AT-DOS (0 = A:); AH=14h aktuelles
; Laufwerk:Verzeichnis des Aufrufers (8.3) nach ES:DI; AH=15h Programm
; fertig, AL = Rueckgabewert: der Kernel parkt die Box bis zum naechsten
; DOS-Programm des Aufrufers (kehrt dann zurueck) oder beendet sie.

cpu 8086
org 100h

        mov     sp, stack_top
        mov     bx, ((stack_top - $$) + 100h + 15) / 16
        mov     ah, 4Ah                 ; Speicher auf das Noetige kuerzen
        int     21h

        call    redir_install
        mov     ah, 13h                 ; Startlaufwerk der AT-DOS
        int     0E0h
        mov     dl, al                  ; Vorgabe-Laufwerk der Box: das
        push    ax                      ; Startlaufwerk (A: kann jetzt die
        mov     ah, 0Eh                 ; AT-DOS-Diskette sein, ggf. leer)
        int     21h
        pop     ax
        add     al, 'A'
        mov     si, drvpos              ; Laufwerksbuchstaben eintragen
.dl:    mov     bx, [si]
        test    bx, bx
        jz      .dd
        mov     [bx], al
        inc     si
        inc     si
        jmp     .dl
.dd:    call    setver_load             ; liest in envbuf, daher zuerst
        call    build_env

next:   push    cs                      ; (nach dem Parken: naechstes Programm)
        pop     es
        mov     di, cmd
        xor     ah, ah                  ; Befehlszeile der Box
        int     0E0h
        push    cx
        jcxz    .cwd                    ; /C: Bildschirm leeren - die Box
        mov     ax, 0003h               ; gibt beim Ende nur die Ausgabe
        int     10h                     ; des Programms an CMD zurueck
.cwd:   call    goto_cwd
        pop     cx
        call    get_pname               ; DOSBOX.INI: Einstellungen fuer
        call    ini_apply               ; dieses Programm
        jcxz    .viacom
        call    direct                  ; .COM/.EXE: ohne FreeCOM
        jnc     .run
.viacom:
        mov     si, t_cmd               ; "X:\DOS\BOX\ /E:1024 /C befehl"
        mov     di, tail + 1
        call    copystr
        jcxz    .inter
        jmp     .c2
.inter: mov     dx, t_hello             ; interaktiv: Meldung, dazu
        mov     ah, 9                   ; X:\AUTOEXEC.BAT, wenn es sie gibt
        int     21h                     ; (gehoert der DOS-Box)
        dec     di                      ; "/C " wieder weg
        dec     di
        dec     di
        mov     dx, t_autoexec
        mov     ax, 4300h
        int     21h
        jc      .c1
        mov     si, t_kauto
        call    copystr
        mov     si, t_autoexec
        call    copystr
.c1:    xor     cx, cx
.c2:    mov     si, cmd                 ; Befehl anhaengen (cx Zeichen)
        rep     movsb
        mov     al, 13
        stosb
        mov     ax, di
        sub     ax, tail + 2
        mov     [tail], al
        mov     dx, t_comspec

.run:   mov     ax, cs                  ; Umgebung mit DOSBOX=
        mov     bx, envbuf
        mov     cl, 4
        shr     bx, cl
        add     ax, bx
        mov     [pblock], ax
        mov     [pb_tail+2], cs
        mov     [pb_fcb1+2], cs
        mov     [pb_fcb2+2], cs
        mov     bx, pblock
        mov     ax, 4B00h               ; FreeCOM starten und warten
        int     21h
        mov     al, 0FFh
        jc      .exit
        mov     ah, 4Dh                 ; dessen Rueckgabewert
        int     21h
.exit:  cmp     word [dosver], 0        ; DOS_VERSION zuruecknehmen
        je      .park
        push    ax
        mov     ax, 122Fh
        xor     dx, dx
        int     2Fh
        pop     ax
.park:  mov     ah, 15h                 ; fertig: der Kernel parkt die Box
        int     0E0h                    ; (wie NTVDM offen halten) oder
        cli                             ; beendet sie; zurueck kommt sie
        mov     ax, cs                  ; erst mit dem naechsten Programm
        mov     ss, ax
        mov     sp, stack_top
        sti
        jmp     next

copystr:                                ; DS:SI -> ES:DI ohne die Null
        lodsb
        test    al, al
        jz      .r
        stosb
        jmp     copystr
.r:     ret

; ---- DOSBOX.INI (X:\DOS\BOX) -------------------------------------------
; Einstellungen je Programm, die beim Programmstart wirken koennen (DOS=
; und RMSIZE= gelten beim Start der Box und stehen in der CONFIG.SYS):
;   [*]           fuer alle Programme
;   [NAME.EXT]    fuer ein Programm (geht vor)
;   XMS_MEMORY_LIMIT=KB   (0-3008, Vorgabe 2048) -> INT E0h 16h
;   EMS_MEMORY_LIMIT=KB   (0-4096, Vorgabe 2048) -> INT E0h 17h
;   DOS_VERSION=n.nn      -> INT 2Fh 122Fh fuer das naechste Programm
;                            (ein Eintrag in SETVER.LST geht vor)
;   POPF_PATCH=ON|OFF     Hilfen der Nachbildung fuer DPMI-Programme,
;   SB_IRQ_IDLE=ON|OFF    Vorgabe ON (siehe v86.c, box_hk) -> INT E0h 18h
;   IF_WATCHDOG=ON|OFF    (BL Bit 0/1/2/3 = an)
;   PVI=ON|OFF            CLI/STI ohne Falle (ab Pentium, CR4.PVI)
;   IDLE_SENSITIVITY=n    0-100 wie OS/2 (Vorgabe 75, 100 = aus)
;                         -> INT E0h 19h, BX = n + 1 (0 = Vorgabe)
; Wird vor jedem Programm neu gelesen: Aenderungen gelten sofort.

get_pname:                              ; Name aus cmd (CX Zeichen) -> pname
        push    cx
        push    si
        push    di
        push    bx
        mov     di, pname
        mov     si, t_cmdcom
        jcxz    .cp
        xor     si, si
        xor     bx, bx                  ; Anfang des Namens
.w:     cmp     si, cx
        jae     .we
        mov     al, [cmd+si]
        inc     si
        cmp     al, ' '
        je      .we1
        cmp     al, '\'
        je      .bs
        cmp     al, ':'
        jne     .w
.bs:    mov     bx, si
        jmp     .w
.we1:   dec     si
.we:    mov     cx, si
        sub     cx, bx
        cmp     cx, 12
        jbe     .cn
        mov     cx, 12
.cn:    lea     si, [cmd+bx]
.c:     jcxz    .z
        lodsb
        call    upcase
        stosb
        dec     cx
        jmp     .c
.cp:    lodsb
        stosb
        test    al, al
        jnz     .cp
        jmp     .r
.z:     mov     byte [di], 0
.r:     pop     bx
        pop     di
        pop     si
        pop     cx
        ret

upcase: cmp     al, 'a'
        jb      .u
        cmp     al, 'z'
        ja      .u
        sub     al, 20h
.u:     ret

ini_apply:
        push    cx
        push    es
        mov     word [xmskb], 2048
        mov     word [emskb], 2048
        mov     word [dosver], 0
        mov     byte [hk], 15
        mov     word [idles], 0
        mov     ah, 48h                 ; 4 KB Lesepuffer
        mov     bx, 256
        int     21h
        jc      .set
        mov     [ibuf], ax
        mov     es, ax
        mov     byte [es:0], 0
        mov     dx, t_ini
        mov     ax, 3D00h
        int     21h
        jc      .free
        mov     bx, ax
        push    ds
        mov     ds, [cs:ibuf]
        xor     dx, dx
        mov     cx, 4095
        mov     ah, 3Fh
        int     21h
        pop     ds
        jc      .close
        mov     di, ax
        mov     byte [es:di], 0
.close: mov     ah, 3Eh
        int     21h
        mov     bx, t_star              ; erst [*], dann [NAME.EXT]
        call    ini_pass
        mov     bx, pname
        call    ini_pass
.free:  mov     es, [ibuf]
        mov     ah, 49h
        int     21h
.set:   mov     bl, [hk]
        mov     ah, 18h
        int     0E0h
        mov     bx, [idles]
        mov     ah, 19h
        int     0E0h
        mov     bx, [xmskb]
        mov     ah, 16h
        int     0E0h
        call    set_xmssize
        mov     bx, [emskb]
        mov     ah, 17h
        int     0E0h
        mov     dx, [dosver]
        test    dx, dx
        jz      .r
        mov     ax, 122Fh
        int     2Fh
.r:     pop     es
        pop     cx
        ret

ini_pass:                               ; BX = Abschnittsname, ES = Puffer
        mov     es, [ibuf]
        xor     si, si
        xor     bp, bp                  ; 1: Abschnitt gilt
.line:  mov     al, [es:si]
        test    al, al
        jz      .done
        cmp     al, ' '
        ja      .tx
        inc     si
        jmp     .line
.tx:    cmp     al, ';'
        je      .skip
        cmp     al, '['
        jne     .key
        inc     si
        xor     bp, bp
        mov     di, bx
.sc:    mov     al, [es:si]
        call    upcase
        cmp     al, ']'
        je      .se
        cmp     al, [di]
        jne     .skip
        inc     si
        inc     di
        jmp     .sc
.se:    cmp     byte [di], 0
        jne     .skip
        mov     bp, 1
        jmp     .skip
.key:   test    bp, bp
        jz      .skip
        mov     di, k_xms
        call    kmatch
        jc      .k2
        call    pnum
        mov     [xmskb], ax
        jmp     .skip
.k2:    mov     di, k_ems
        call    kmatch
        jc      .k3
        call    pnum
        mov     [emskb], ax
        jmp     .skip
.k3:    mov     di, k_ver
        call    kmatch
        jc      .k3b
        mov     dl, [es:si]             ; Hauptversion
        sub     dl, '0'
        xor     dh, dh
        inc     si
        cmp     byte [es:si], '.'
        jne     .vs
        inc     si
        mov     al, [es:si]
        sub     al, '0'
        cmp     al, 9
        ja      .vs
        mov     ah, 10
        mul     ah
        mov     dh, al
        inc     si
        mov     al, [es:si]
        sub     al, '0'
        cmp     al, 9
        ja      .vs
        add     dh, al
.vs:    cmp     dl, 2
        jb      .skip
        cmp     dl, 9
        ja      .skip
        mov     [dosver], dx
.skip:  mov     al, [es:si]
        test    al, al
        jz      .done
        inc     si
        cmp     al, 10
        jne     .skip
        jmp     .line
.done:  ret
.k3b:   mov     di, k_idle              ; IDLE_SENSITIVITY=n
        call    kmatch
        jc      .k4
        call    pnum
        inc     ax
        mov     [idles], ax
        jmp     .skip
.k4:    mov     di, k_hk                ; Schalter ON/OFF (1/0)
        mov     dl, 1
.k5:    push    di
        call    kmatch
        pop     di
        jnc     .k6
        add     di, 12
        shl     dl, 1
        cmp     dl, 64
        jb      .k5
        jmp     .skip
.k6:    mov     al, [es:si]
        call    upcase
        cmp     al, 'O'
        jne     .k7
        mov     al, [es:si+1]
        call    upcase
        cmp     al, 'N'
        je      .on
        cmp     al, 'F'
        je      .off
        jmp     .skip
.k7:    cmp     al, '1'
        je      .on
        cmp     al, '0'
        jne     .skip
.off:   not     dl
        and     [hk], dl
        jmp     .skip
.on:    or      [hk], dl
        jmp     .skip

kmatch:                                 ; [ES:SI] = Name an DI, dann "=" ?
        push    si                      ; -> SI hinter "=", CF = 0
.k:     mov     al, [di]
        test    al, al
        jz      .eq
        mov     ah, al
        mov     al, [es:si]
        call    upcase
        cmp     al, ah
        jne     .no
        inc     si
        inc     di
        jmp     .k
.eq:    cmp     byte [es:si], ' '
        jne     .e2
        inc     si
        jmp     .eq
.e2:    cmp     byte [es:si], '='
        jne     .no
        inc     si
.e3:    cmp     byte [es:si], ' '
        jne     .ok
        inc     si
        jmp     .e3
.ok:    add     sp, 2
        clc
        ret
.no:    pop     si
        stc
        ret

pnum:                                   ; Dezimalzahl an ES:SI -> AX
        mov     ax, 0FFFFh              ; -1 (jede negative Zahl) = so viel
        cmp     byte [es:si], '-'       ; wie moeglich, der Kernel kappt
        je      .x
        xor     ax, ax
.n:     mov     cl, [es:si]
        sub     cl, '0'
        cmp     cl, 9
        ja      .x
        mov     dx, 10
        mul     dx
        xor     ch, ch
        add     ax, cx
        inc     si
        jmp     .n
.x:     ret

; ---- Programm direkt starten ---------------------------------------------
; Endet das erste Wort der Befehlszeile (cmd, CX Zeichen) auf .COM oder
; .EXE, braucht es FreeCOM nicht (93 KB laden und einrichten - auf dem
; 386SX mehrere Sekunden): Rest als Befehlszeile nach tail, Name mit 0
; abschliessen -> DX = Name, CF = 0. Sonst CF = 1 (.BAT ueber FreeCOM).
direct: push    cx
        mov     bx, cx
        xor     bp, bp                  ; Ende des ersten Worts
.w:     cmp     bp, bx
        jae     .we
        cmp     byte [cmd+bp], ' '
        je      .we
        inc     bp
        jmp     .w
.we:    cmp     bp, 5
        jb      .no
        mov     ax, [cmd+bp-4]          ; "." und C/E
        mov     dx, [cmd+bp-2]          ; "OM" / "XE"
        and     dx, 0DFDFh
        and     ah, 0DFh
        cmp     al, '.'
        jne     .no
        cmp     ah, 'C'
        jne     .exe
        cmp     dx, 4D4Fh               ; "OM"
        je      .yes
.exe:   cmp     ah, 'E'
        jne     .no
        cmp     dx, 4558h               ; "XE"
        jne     .no
.yes:   push    cs
        pop     es
        lea     si, [cmd+bp]            ; Rest ab dem Leerzeichen
        mov     cx, bx
        sub     cx, bp
        mov     di, tail + 1
        mov     [tail], cl
        rep     movsb
        mov     al, 13
        stosb
        mov     byte [cmd+bp], 0
        mov     dx, cmd
        pop     cx
        clc
        ret
.no:    pop     cx
        stc
        ret

; ---- Laufwerk und Verzeichnis des Aufrufers (INT E0h 14h, 8.3) ----------
goto_cwd:
        push    cs
        pop     es
        mov     di, tail                ; tail ist hier noch frei
        mov     ah, 14h
        int     0E0h
        cmp     byte [tail+1], ':'
        jne     .ret
        mov     dl, [tail]
        and     dl, 0DFh
        sub     dl, 'A'
        mov     ah, 0Eh                 ; Laufwerk wechseln
        int     21h
        mov     dx, tail                ; Verzeichnis
        mov     ah, 3Bh
        int     21h
.ret:   ret

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
        cmp     word [si], 'BL'         ; BLASTER= schon gesetzt?
        jne     .e1p
        cmp     word [si+2], 'AS'
        jne     .e1p
        cmp     word [si+4], 'TE'
        jne     .e1p
        cmp     word [si+6], 'R='
        jne     .e1p
        mov     byte [cs:has_bl], 1
.e1p:   cmp     word [si], 'PA'         ; eigenes PATH= weglassen
        jne     .e2
        cmp     word [si+2], 'TH'
        jne     .e2
        cmp     byte [si+4], '='
        jne     .e2
.skip:  lodsb
        test    al, al
        jnz     .skip
        jmp     .e1
.e2:    lodsb
        stosb
        cmp     di, envbuf + ENVSZ - 80
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
        mov     si, t_path              ; PATH wie \OS2\MDOS bei OS/2
.e5:    lodsb
        stosb
        test    al, al
        jnz     .e5
        cmp     byte [has_bl], 0        ; virtuelle Sound Blaster (v86.c)
        jne     .e7
        mov     si, t_blaster
.e6:    lodsb
        stosb
        test    al, al
        jnz     .e6
.e7:    stosb                           ; zweite Null: Ende
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

;
; ---- LoL+45h: erweiterter Speicher in KB (MS-DOS-MEM nimmt das als
; Gesamtgroesse von XMS; FreeDOS setzt es nie). Wert aus CMOS 30h/31h,
; das der Kernel als HMA + XMS-Grenze dieser Box nachbildet.
set_xmssize:
        push    ax
        push    bx
        push    es
        mov     al, 31h
        out     70h, al
        in      al, 71h
        mov     ah, al
        mov     al, 30h
        out     70h, al
        in      al, 71h
        push    ax
        mov     ah, 52h
        int     21h
        pop     word [es:bx+45h]
        pop     es
        pop     bx
        pop     ax
        ret

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

t_path    db "PATH=X:\DOS\BOX;X:\DOS", 0
t_blaster db "BLASTER=A220 I5 D1 H5 T6", 0  ; SB16; IRQ 5 oder 7 geht beides
has_bl    db 0
t_ini     db "X:\DOS\BOX\DOSBOX.INI", 0
t_star    db "*", 0
t_cmdcom  db "COMMAND.COM", 0
k_xms     db "XMS_MEMORY_LIMIT", 0
k_ver     db "DOS_VERSION", 0
k_ems     db "EMS_MEMORY_LIMIT", 0
k_hk      db "POPF_PATCH", 0, 0       ; je 12 Byte, Bit 0/1/2
          db "SB_IRQ_IDLE", 0
          db "IF_WATCHDOG", 0
          db "PVI", 0, 0, 0, 0, 0, 0, 0, 0, 0
          db "ADLIB_COPY", 0, 0         ; Bit 4: Vorgabe aus (hk = 15)
          db "HW_TIMER", 0, 0, 0, 0     ; Bit 5: Vorgabe aus
hk        db 15
idles     dw 0
k_idle    db "IDLE_SENSITIVITY", 0
xmskb     dw 2048
emskb     dw 2048
dosver    dw 0
ibuf      dw 0
pname     times 13 db 0
t_comspec db "X:\DOS\BOX\COMMAND.COM", 0
t_cmd     db " X:\DOS\BOX\ /E:1024 /C ", 0
t_kauto   db "/K ", 0
t_autoexec db "X:\AUTOEXEC.BAT", 0
t_hello   db "DOS-Box von AT-DOS 6.0 (FreeDOS-Kern, FreeCOM) - EXIT beendet die Box.", 13, 10, "$"
drvpos    dw t_dosbox+7, t_setver, t_path+5, t_path+16, t_comspec, t_cmd+1
          dw t_autoexec, t_ini, 0
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
