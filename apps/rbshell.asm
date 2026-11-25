; SPDX-License-Identifier: GPL-2.0-or-later
; RBSHELL.COM - SHELL= der Kompatibilitaetsbox von AT-DOS 5.1
;
; Haengt die AT-DOS-Laufwerke ab C: als Netzlaufwerke ein (CDS-Eintraege
; mit Flags C000h, INT 2Fh 11xxh geht ueber INT E0h AH = 10h an RBOX, das
; die Zugriffe mit FAPI-Aufrufen erledigt - wie BOXSHELL in 6.0), startet
; FreeCOM (A:\COMMAND.COM) und beendet nach dessen EXIT die Box mit seinem
; Rueckgabewert (INT E0h AH = 01h). RBSHELL bleibt als Elternprozess von
; FreeCOM im Speicher, der Haken also gueltig.
;
; INT E0h (nur in der Box): AH=01h Box beenden, AL = Rueckgabewert;
; AH=10h Redirector-Aufruf AL (ZF=1 erledigt, CF/AX Ergebnis); AH=11h SDA
; ist DS:SI; AH=12h -> DX:AX Bitmaske der AT-DOS-Laufwerke; AH=14h
; Laufwerk:Verzeichnis, aus dem RBOX gestartet wurde, nach ES:DI - FreeCOM
; beginnt dort; AH=13h -> AL = Startlaufwerk (FreeCOM aus X:\DOS\MDOS, A:/B:
; sind dann die Disketten von AT-DOS) oder 0 (vom Abbild A:); AH=15h Tastaturbelegung von AT-DOS
; (3 x 90 Zeichen) nach ES:DI -> AX = 0; AH=1Ah BLASTER-Text von SB16.SYS
; nach ES:DI -> AX = 0 (FFFFh: keine Karte).
cpu 8086
org 0x100

start:
        mov     sp, stack_top
        mov     bx, (trans_end - start + 0x100 + 15) / 16  ; Rest frei
        mov     ah, 0x4A
        int     0x21
        call    redir_install
        call    keyb_install
        mov     ax, cs
        mov     es, ax                          ; EXEC: ES:BX = Parameterblock
        mov     di, cdbuf                       ; Laufwerk und Verzeichnis,
        mov     ah, 0x14                        ; aus dem RBOX gestartet wurde
        int     0xE0
        mov     dl, [cdbuf]
        test    dl, dl
        jz      .nocd
        and     dl, 0xDF
        sub     dl, 'A'
        mov     ah, 0x0E
        int     0x21
        mov     dx, cdbuf
        mov     ah, 0x3B
        int     0x21
.nocd:  mov     ax, cs
        mov     es, ax
        mov     [pb_tail + 2], ax
        mov     [pb_fcb1 + 2], ax
        mov     [pb_fcb2 + 2], ax
        mov     dx, shell
        mov     ah, 0x13                        ; Startlaufwerk: FreeCOM aus
        int     0xE0                            ; X:\DOS\MDOS (A:/B: sind dann
        mov     [nstart], ah                    ; AH Bit 1: Notstart (F5)
        and     ah, 1
        mov     [perm], ah                      ; AH Bit 0: PROTECTONLY=NO
        test    al, al                          ; die Disketten von AT-DOS),
        jz      .abbild                         ; sonst vom Abbild (A:)
        mov     [shellx], al
        mov     [tailx + 2], al
        mov     [tailk + 2], al
        mov     [tailk + 26], al
        mov     [env1], al                      ; Umgebung fuer FreeCOM: PATH
        mov     [env2], al                      ; beginnt mit X:\DOS\MDOS (MEM
        mov     [env3], al                      ; von FreeDOS), deutsche MEM-
        mov     ax, cs                          ; Meldungen ueber NLSPATH/LANG
        mov     bx, envblk
        mov     cl, 4
        shr     bx, cl
        add     ax, bx
        mov     [pblock], ax
        mov     word [pb_tail], tailx
        call    other_dos               ; MS-DOS/PC DOS: dessen COMMAND.COM
        test    byte [nstart], 2        ; Notstart: ohne AUTOEXEC.BAT
        jnz     .noab
        call    ab_load                 ; PATH/SET/PROMPT aus X:\AUTOEXEC.BAT
.noab:
        mov     dx, shellx
.abbild:
        push    dx
        mov     bx, (stack_top - start + 0x100 + 15) / 16  ; ab_load frei
        mov     ah, 0x4A
        int     0x21
        call    mouse_load                      ; CuteMouse, wenn AT-DOS
        pop     dx                              ; eine Maus hat
        cmp     byte [perm], 1                  ; Box beim Start (OS/2 1.x):
        jne     .exec                           ; nach dem ersten Prompt
        cmp     word [pb_tail], tailx           ; meldet RBREADY.COM "bereit",
        jne     .exec                           ; AT-DOS kommt nach vorn
        cmp     byte [nok], 0                   ; DOS vor 6.0 kennt /K nicht
        je      .k                              ; (der Pfad dahinter gaelte
        push    dx                              ; als Suchpfad - dann erbt
        mov     ah, 0x17                        ; COMMAND.COM die Umgebung
        int     0xE0                            ; nicht): selbst melden
        pop     dx
        jmp     .exec
.k:     mov     word [pb_tail], tailk
.exec:  mov     bx, pblock
        mov     ax, 0x4B00
        int     0x21
        jnc     .ran
        cmp     ax, 15                          ; EXEC gescheitert: RBOX
        jbe     .e                              ; meldet D0h + DOS-Fehler
        mov     al, 15
.e:     or      al, 0xD0
        jmp     .done
.ran:
        push    cs
        pop     ds
        mov     ah, 0x4D                        ; AH = Art des Endes: 0 EXIT,
        int     0x21                            ; 1 Strg+C, 2 kritischer
        cmp     byte [perm], 0                  ; Box bleibt (wie OS/2 1.x):
        je      .noperm                         ; EXIT startet COMMAND.COM
        push    cs                              ; neu, ohne RBREADY
        pop     es
        cmp     word [pb_tail], tailk
        jne     .again
        mov     word [pb_tail], tailx
.again: mov     dx, shell
        cmp     word [pb_tail], tail
        je      .exec
        mov     dx, shellx
        jmp     .exec
.noperm:
        test    ah, ah                          ; Fehler/Absturz, 3 TSR ->
        jz      .done                           ; RBOX meldet F0h + Art
        mov     al, ah
        or      al, 0xF0
.done:  test    al, al                          ; nicht per EXIT 0 beendet:
        jz      .quit                           ; anhalten, damit die Meldung
        push    ax                              ; von FreeDOS (z. B. Invalid
        push    cs                              ; Opcode) lesbar bleibt
        pop     ds
        mov     dx, endmsg
        mov     ah, 9
        int     0x21
        xor     ah, ah
        int     0x16
        pop     ax
.quit:  mov     ah, 1                           ; Box beenden, AL = Code
        int     0xE0
        jmp     $

endmsg: db      13, 10, "RBSHELL: COMMAND.COM beendet (Code ungleich 0)."
        db      " Taste beendet die Box.$"

; ---- anderes DOS als FreeDOS (Abbild von MKDOSIMG) ----------------------
; RBOX meldet ueber INT E0h 19h den Namen des Abbilds (ohne .IMG, leer bei
; FreeDOS); dessen COMMAND.COM und Werkzeuge liegen in X:\DOS\MDOS\NAME\.
; Shell, Befehlszeilen und Umgebung (PATH mit NAME vorn, COMSPEC) werden
; dorthin umgestellt. Startlaufwerk aus shellx.
other_dos:
        push    ax
        mov     ah, 0x30
        int     0x21
        cmp     bh, 0xFD                ; FreeDOS: alles wie bisher
        je      .fd
        cmp     al, 6                   ; COMMAND.COM vor DOS 6.0: kein /K
        jae     .fd
        mov     byte [nok], 1
.fd:    pop     ax
        cmp     bh, 0xFD
        je      .r
        mov     al, [shellx]            ; Startlaufwerk (AL ist hier schon
        mov     [od_drv], al            ; ueberschrieben)
        push    cs
        pop     es
        mov     di, iname
        mov     ah, 0x19
        int     0xE0
        cmp     byte [iname], 0
        je      .r
        mov     di, obase               ; obase = X:\DOS\MDOS\NAME
        call    od_mdos
        mov     si, iname
        call    cpy
        mov     byte [di], 0
        mov     di, shellx              ; X:\DOS\MDOS\NAME\COMMAND.COM
        mov     si, obase
        call    cpy
        mov     si, s_com
        call    copyz
        mov     di, tailx + 1           ; " /E:1024"
        call    od_tail
        mov     ax, di
        sub     ax, tailx + 1
        mov     [tailx], al
        mov     byte [di], 13
        mov     di, tailk + 1           ; dazu " /K X:\DOS\MDOS\RBREADY.COM"
        call    od_tail
        mov     si, s_k
        call    cpy
        call    od_mdos
        mov     si, s_rbr
        call    cpy
        mov     ax, di
        sub     ax, tailk + 1
        mov     [tailk], al
        mov     byte [di], 13
        mov     di, envo                ; PATH=NAME;MDOS;DOS, COMSPEC
        mov     si, s_path
        call    cpy
        mov     si, obase
        call    cpy
        mov     al, ';'
        stosb
        call    od_mdos
        dec     di                      ; "\" am Ende weg
        mov     al, ';'
        stosb
        mov     al, [od_drv]
        stosb
        mov     si, s_dos
        call    copyz
        mov     si, s_cspec
        call    cpy
        mov     si, shellx
        call    copyz
        mov     si, s_dircmd
        call    copyz
        mov     si, s_prompt            ; MS-DOS 5 nimmt sonst $N$G
        call    copyz
        xor     al, al
        stosb
        mov     ax, cs
        mov     bx, envo
        mov     cl, 4
        shr     bx, cl
        add     ax, bx
        mov     [pblock], ax
.r:     ret
od_mdos:                                ; "X:\DOS\MDOS\" nach DI
        mov     al, [od_drv]
        stosb
        mov     si, s_mdos
        jmp     cpy
od_tail:                                ; " /E:1024" - ohne Verzeichnis:
        mov     si, s_e                 ; COMMAND.COM von MS-DOS erbt mit
        jmp     cpy                     ; einem Suchpfad die Umgebung nicht
cpy:    lodsb                           ; DS:SI -> ES:DI ohne die Null
        test    al, al
        jz      .e
        stosb
        jmp     cpy
.e:     ret
copyz:  lodsb                           ; ... mit der Null
        stosb
        test    al, al
        jnz     copyz
        ret
od_drv: db      0
nok:    db      0                       ; 1: COMMAND.COM ohne /K
iname:  times   13 db 0
obase:  times   32 db 0
s_mdos: db      ":\DOS\MDOS\", 0
s_dos:  db      ":\DOS", 0
s_com:  db      "\COMMAND.COM", 0
s_e:    db      " /E:1024", 0
s_k:    db      " /K ", 0
s_rbr:  db      "RBREADY.COM", 0
s_path: db      "PATH=", 0
s_cspec: db     "COMSPEC=", 0
s_dircmd: db    "DIRCMD=/OGN", 0
s_prompt: db    "PROMPT=$P$G", 0
a4409:  dw      0x08C2                  ; 4409h: MS-DOS 08C2h, PC DOS 48C2h

; ---- Maus: CuteMouse aus X:\DOS\MDOS (nur mit COMMAND.COM von dort) ------
; In der Box gehoert die Maus, solange sie vorn ist, CuteMouse (der Kernel
; maskiert IRQ 3/4/12 dort nicht); INT E0h 16h -> AX = Tasten der Maus von
; AT-DOS, 0 = keine.
mouse_load:
        cmp     word [pb_tail], tailx           ; nur mit X:\DOS\MDOS
        jne     .no
        mov     ah, 0x16
        int     0xE0
        test    ax, ax
        jz      .no
        mov     di, ctmtail + 1                 ; BL: Anschluss wie MOUSE.SYS
        cmp     bl, 'P'                         ; (sonst sucht CuteMouse selbst
        jne     .ser                            ; und nimmt zuerst PS/2)
        mov     word [di], ' /'
        mov     word [di + 2], 'P' | 13 << 8
        mov     byte [ctmtail], 3
        jmp     short .go
.ser:   test    bl, bl
        jz      .go
        mov     word [di], ' /'
        mov     byte [di + 2], 'S'
        mov     [di + 3], bl
        mov     byte [di + 4], 13
        mov     byte [ctmtail], 4
.go:    mov     al, [shellx]
        mov     [ctm], al
        mov     ax, [pb_tail]
        push    ax
        mov     word [pb_tail], ctmtail
        mov     ax, cs
        mov     es, ax
        mov     dx, ctm
        mov     bx, pblock
        mov     ax, 0x4B00
        int     0x21
        mov     ax, cs                          ; nach EXEC: SS:SP gueltig
        mov     es, ax                          ; (DOS 2+ stellt sie her)
        pop     ax
        mov     [pb_tail], ax
.no:    ret

ctm:    db      "C:\DOS\MDOS\CTMOUSE.COM", 0
ctmtail: db     0, 13, 0, 0, 0, 0

; ---- Redirector ----------------------------------------------------------
redir_install:
        push    ds
        mov     ax, 0x5D06              ; DS:SI = SDA
        int     0x21
        mov     [cs:sda], si            ; fuer den Lesepuffer
        mov     [cs:sda + 2], ds
        mov     ah, 0x30                ; welches DOS? (BH: FDh FreeDOS,
        int     0x21                    ; FFh MS-DOS, 00h PC DOS) - DS:SI
        mov     bl, bh                  ; bleibt
        test    bh, bh                  ; PC DOS ab 6.30 meldet bei 4409h
        jnz     .nibm                   ; Bit 14 mit
        cmp     al, 6
        jb      .nibm
        ja      .ibm
        cmp     ah, 30
        jb      .nibm
.ibm:   mov     word [cs:a4409], 0x48C2
.nibm:  mov     ah, 0x11                ; RBOX melden (BL = OEM)
        int     0xE0
        pop     ds
        mov     ah, 0x12                ; welche Laufwerke hat AT-DOS?
        int     0xE0
        mov     [dmask], ax
        mov     [dmask+2], dx
        mov     ah, 0x52                ; Liste der Listen
        int     0x21
        mov     cl, [es:bx+0x21]        ; LASTDRIVE
        les     di, [es:bx+0x16]        ; CDS
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
        mov     [es:di], al             ; "X:\"
        mov     word [es:di+1], 0x5C3A
        mov     byte [es:di+3], 0
        mov     word [es:di+0x43], 0xC000   ; Netz + physisch
        mov     word [es:di+0x4F], 2    ; Stamm-Backslash
.next:  add     di, 0x58
        inc     si
        jmp     .d
.hook:  mov     ax, 0x352F
        int     0x21
        mov     [old2f], bx
        mov     [old2f+2], es
        mov     dx, int2f
        mov     ax, 0x252F
        int     0x21
        mov     ax, 0x3521              ; Laufwerksabfragen (IOCTL)
        int     0x21
        mov     [old21], bx
        mov     [old21+2], es
        mov     dx, int21
        mov     ax, 0x2521
        int     0x21
        ret

; INT 21h: die AT-DOS-Laufwerke wie lokale melden (Wunsch des Nutzers,
; BIOSCMP gegen MS-DOS 6.22) - FreeDOS beantwortet IOCTL fuer Netzlauf-
; werke mit Fehlern oder "fern". Abgefangen: 4408h (Wechselmedium),
; 4409h (fern?), 440Ah (Handle fern?), 440Dh 0860h/0866h/0846h (Parameter,
; Seriennummer lesen/setzen), 440Eh/440Fh (logische Laufwerke), 6900h/
; 6901h. Die Werte kommen vom Kernel (INT E0h 18h -> dbuf, RBOX). CD-ROMs und
; Laufwerke ohne Medium bleiben, was sie waren.
int21:  cmp     ah, 4Bh                 ; EXEC/Laufwerkswechsel: CDS mit
        je      .sy                     ; den AT-DOS-Laufwerken abgleichen
        cmp     ah, 0Eh                 ; (NET USE, LOOP, DEVLOAD, waehrend
        je      .sy                     ; die Box hinten war)
        cmp     ah, 0Ch                 ; Tastatur (Eingabezeile von
        ja      .n4b                    ; COMMAND.COM): hoechstens einmal
        cmp     ah, 6                   ; je Sekunde
        je      .n4b
        cmp     ah, 1
        jb      .n4b
        push    ax
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ds, [0E0h*4+2]          ; Stummel des Kernels: Laufwerke
        cmp     byte [94], 0A5h         ; bei jedem Eintritt in [90]
        jne     .old                    ; (sonst je Sekunde wie frueher -
        mov     ax, [90]                ; jeder Ausflug kostet auf dem 286
        cmp     ax, [cs:km]             ; einen CPU-Reset, Spiele wie
        jne     .chg                    ; Digger verlieren ihren Takt)
        mov     ax, [92]
        cmp     ax, [cs:km+2]
        je      .same
.chg:   mov     ax, [90]
        mov     [cs:km], ax
        mov     ax, [92]
        mov     [cs:km+2], ax
        pop     ds
        pop     ax
        jmp     short .sy
.same:  pop     ds
        pop     ax
        jmp     short .n4b
.old:   xor     ax, ax
        mov     ds, ax
        mov     ax, [046Ch]
        pop     ds
        sub     ax, [cs:sy_t]
        cmp     ax, 18
        pop     ax
        jb      .n4b
.sy:    call    cds_sync
.n4b:   cmp     ah, 44h
        je      .io
        cmp     ah, 69h
        jne     .ch
        cmp     al, 1
        jbe     .dr
.ch:    jmp     far [cs:old21]
.io:    cmp     al, 0Ah
        je      .rh
        cmp     al, 08h
        je      .dr
        cmp     al, 09h
        je      .dr
        cmp     al, 0Eh
        je      .dr
        cmp     al, 0Fh
        je      .dr
        cmp     al, 0Dh
        jne     .ch
        cmp     ch, 08h                 ; Kategorie Platte
        jne     .ch
        cmp     cl, 60h
        je      .dr
        cmp     cl, 66h
        je      .dr
        cmp     cl, 46h
        jne     .ch
.dr:    call    dmine                   ; Laufwerk BL ein AT-DOS-Laufwerk?
        jc      .ch                     ; nein: wie bisher
        cmp     ah, 69h
        je      .sn
        cmp     al, 08h
        jne     .n9
        mov     ax, 1                   ; fest = 1, Wechselmedium = 0
        cmp     byte [cs:dbuf], 2
        jne     .ok
        dec     ax
        jmp     .ok
.n9:    cmp     al, 09h
        jne     .ne
        mov     dx, [cs:a4409]          ; Geraeteattribute wie MS-DOS,
        jmp     .ok                     ; Bit 12 (fern) aus
.ne:    cmp     al, 0Dh
        je      .gd
        xor     al, al                  ; 440Eh/440Fh: nur ein Buchstabe
        jmp     .ok
.gd:    cmp     cl, 60h
        je      .par
        cmp     cl, 46h
        je      .deny
.mid:   push    di                      ; 0866h/6900h: Info-Stufe, Serie,
        push    si                      ; Bezeichnung, Dateisystem
        push    cx
        push    es
        push    ds
        pop     es
        mov     di, dx
        xor     ax, ax
        stosw
        mov     si, dbuf + 8
        mov     cx, 4 + 11 + 8
.cp:    mov     al, [cs:si]
        stosb
        inc     si
        loop    .cp
        pop     es
        pop     cx
        pop     si
        pop     di
        xor     ax, ax
        jmp     .ok
.sn:    test    al, al
        jz      .mid
.deny:  mov     ax, 5                   ; setzen: nicht erlaubt
        stc
        sti
        retf    2
.par:   push    bx                      ; 0860h: Geraeteparameter mit BPB
        push    di
        mov     di, dx
        mov     al, 5                   ; fest: 5, Diskette 1,44 MB: 7
        cmp     byte [cs:dbuf], 2
        jne     .p1
        mov     al, 7
.p1:    mov     [di + 1], al
        xor     ax, ax
        cmp     byte [cs:dbuf], 2
        je      .p2
        inc     ax                      ; nicht wechselbar
.p2:    mov     [di + 2], ax
        mov     word [di + 4], 1024     ; Zylinder (geschaetzt)
        mov     byte [di + 6], 0
        mov     ax, [cs:dbuf + 2]
        mov     [di + 7], ax            ; Bytes je Sektor
        mov     al, [cs:dbuf + 1]
        mov     [di + 9], al            ; Sektoren je Cluster
        mov     word [di + 10], 1       ; reservierte Sektoren
        mov     byte [di + 12], 2       ; FATs
        mov     word [di + 13], 512     ; Stammeintraege
        mov     word [di + 15], 0       ; Sektoren (16 Bit)
        mov     al, [cs:dbuf + 31]
        mov     [di + 17], al           ; Medienbyte
        mov     ax, [cs:dbuf + 4]       ; Sektoren je FAT ~ Cluster/256
        mov     bx, [cs:dbuf + 6]
        mov     al, ah
        mov     ah, bl
        inc     ax
        mov     [di + 18], ax
        mov     word [di + 20], 63      ; Sektoren je Spur
        mov     word [di + 22], 16      ; Koepfe
        mov     word [di + 24], 0       ; versteckte Sektoren
        mov     word [di + 26], 0
        mov     ax, [cs:dbuf + 4]       ; Sektoren gesamt = Cluster *
        mov     bx, [cs:dbuf + 6]       ; Sektoren je Cluster
        mov     cl, [cs:dbuf + 1]
.mul:   shr     cl, 1
        jz      .mdn
        shl     ax, 1
        rcl     bx, 1
        jmp     .mul
.mdn:   mov     [di + 28], ax
        mov     [di + 30], bx
        mov     cx, 0860h
        pop     di
        pop     bx
        xor     ax, ax
.ok:    clc
        sti
        retf    2
.rh:    pushf                           ; 440Ah: Handle fern? FreeDOS
        call    far [cs:old21]          ; setzt Bit 15 (SFT ueber den
        jc      .rhe                    ; Redirector) - lokal melden
        and     dh, 7Fh
        clc
.rhe:   sti
        retf    2

; Laufwerk BL (0 = aktuelles, 1 = A:) ein AT-DOS-Laufwerk? -> CF = 0 und
; dbuf gefuellt, sonst CF = 1; Register bleiben
;
; AT-DOS-Laufwerke in die CDS eintragen (wie BOXSHELL von 6.x): schon
; eingetragene behalten ihr Verzeichnis, nicht mehr vorhandene (Flags
; C000h) werden ungueltig. Alle Register bleiben.
cds_sync:
        pushf
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [046Ch]
        pop     ds
        mov     [cs:sy_t], ax
        mov     ah, 12h                 ; welche Laufwerke hat AT-DOS?
        int     0E0h
        mov     [cs:dmask], ax
        mov     [cs:dmask+2], dx
        mov     ah, 52h                 ; Liste der Listen
        pushf
        call    far [cs:old21]
        mov     cl, [es:bx+21h]         ; LASTDRIVE
        les     di, [es:bx+16h]         ; CDS
        xor     ch, ch
        xor     si, si
.d:     cmp     si, cx
        jae     .done
        mov     bx, si
        mov     ax, [cs:dmask]
        mov     dx, [cs:dmask+2]
.sh:    test    bx, bx
        jz      .tst
        shr     dx, 1
        rcr     ax, 1
        dec     bx
        jmp     .sh
.tst:   test    al, 1
        jz      .gone
        mov     ax, [es:di+43h]
        and     ax, 0FF7Fh
        cmp     ax, 0C000h
        je      .next                   ; schon da
        mov     ax, si
        add     al, 'A'
        mov     [es:di], al             ; "X:\"
        mov     word [es:di+1], 5C3Ah
        mov     byte [es:di+3], 0
        mov     word [es:di+43h], 0C000h
        mov     word [es:di+4Fh], 2
        jmp     .next
.gone:  mov     ax, [es:di+43h]
        and     ax, 0FF7Fh
        cmp     ax, 0C000h
        jne     .next
        mov     word [es:di+43h], 0     ; ausgehaengt
.next:  add     di, 58h
        inc     si
        jmp     .d
.done:  pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        popf
        ret

dmine:  push    ax
        push    bx
        push    cx
        push    di
        push    es
        mov     al, bl
        dec     al
        test    bl, bl
        jnz     .k
        mov     ah, 19h
        pushf
        call    far [cs:old21]
.k:     cmp     al, 31
        ja      .no
        mov     bl, al
        xor     bh, bh
        mov     cl, bl
        and     cl, 7
        shr     bx, 1
        shr     bx, 1
        shr     bx, 1
        mov     ch, 1
        shl     ch, cl
        test    [cs:dmask + bx], ch
        jz      .no
        push    cs
        pop     es
        mov     di, dbuf
        mov     ah, 18h                 ; (20h-25h ist XMS von RBHMA)
        int     0E0h                    ; CF = 1: CD, leer, unbekannt
        jmp     .e
.no:    stc
.e:     pop     es
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret


int2f:  cmp     ah, 0x15                ; MSCDEX: RBOX (INT E0h 1Bh)
        je      .cd
        cmp     ah, 0x10                ; SHARE: Freigabe pruefen
        je      .share                  ; RBOX (Redirector) und Kernel
        cmp     ah, 0x11
        jne     .chain
        test    al, al                  ; Installationsabfrage
        jnz     .op
        mov     al, 0xFF
        iret
.op:    cmp     al, 0x23                ; Namen pruefen: fuer jeden Pfad,
        je      .chain                  ; nie unser (spart einen Ausflug)
        cmp     al, 0x08                ; READ: Lesepuffer (rd_cache)
        jne     .inval
        cmp     cx, CSZ
        ja      .inval
        jmp     rd_cache
.share: test    al, al                  ; 1000h: geladen (FFh); die
        jnz     .shf                    ; Aufrufe des FreeDOS-Kerns fuer
        mov     al, 0xFF                ; lokale Dateien (Startabbild A:)
        iret                            ; 10A0h-10A6h: erlaubt
.shf:   xor     ax, ax
        iret
.inval: mov     byte [cs:cvalid], 0     ; alles andere: Puffer verwerfen
.pass:  push    ax
        mov     ah, 0x10
        int     0xE0                    ; RBOX: ZF=1 erledigt
        jnz     .not
        inc     sp                      ; AX bleibt das Ergebnis; INC
        inc     sp                      ; laesst CF stehen (ADD nicht!)
        sti
        retf    2                       ; CF von RBOX zurueckgeben
.not:   pop     ax
.chain: jmp     far [cs:old2f]
.cd:    cmp     al, 0x10                ; nur 1500h-1510h
        ja      .chain
        push    ax
        push    dx
        push    si
        cmp     al, 0x01                ; 1501h: DX:SI = Treiberkopf
        jne     .cd1
        mov     dx, cdhdr
        mov     si, cs
.cd1:   mov     ah, 0x1B
        int     0xE0                    ; ZF=1 erledigt, CF Fehler
        jnz     .cdn
        pop     si
        pop     dx
        inc     sp                      ; AX ist das Ergebnis; INC
        inc     sp                      ; laesst CF stehen
        pushf
        cmp     ah, 0x1B                ; AX nicht gesetzt: wie vorher
        jne     .cdr
        mov     ah, 0x15
.cdr:   popf
        sti
        retf    2
.cdn:   pop     si
        pop     dx
        pop     ax
        jmp     far [cs:old2f]

; Kopf eines CD-Treibers fuer 1501h (Zeichengeraet C800h, "MSCD001",
; Strategie/Interrupt = RETF; Laufwerk +20, Einheiten +21 traegt RBOX ein)
cdhdr:  dd      0FFFFFFFFh
        dw      0C800h, cdretf, cdretf
        db      "MSCD001 "
        dw      0
        db      0, 0
cdretf: retf

; ---- Lesepuffer fuer kleine READs ----------------------------------------
; Jeder Redirector-Aufruf ist ein Ausflug zu AT-DOS (beim 286 mit CPU-
; Reset). Programme, die byteweise lesen (MEM von FreeDOS liest MEM.DE so,
; 5 KB = 5.000 Ausfluege), holen sich hier 1 KB auf einmal: READ mit
; CX <= CSZ an derselben SFT innerhalb des Puffers wird ohne Ausflug
; bedient. Jeder andere Redirector-Aufruf (WRITE, CLOSE, ...) verwirft den
; Puffer; die Lage steht in der SFT (+15h), Seek setzt sie dort direkt.
CSZ     equ     1024
rd_cache:
        push    bx
        push    dx
        push    si
        push    ds
        mov     [cs:cwant], cx
        mov     byte [cs:cfail], 0
.again: cmp     byte [cs:cvalid], 0
        je      .fill
        cmp     di, [cs:csft]
        jne     .fill
        mov     bx, es
        cmp     bx, [cs:csft + 2]
        jne     .fill
        mov     bx, [es:di + 0x0B]      ; Handle von RBOX
        cmp     bx, [cs:chnd]
        jne     .fill
        mov     ax, [es:di + 0x15]      ; Lage - Pufferanfang
        mov     dx, [es:di + 0x17]
        sub     ax, [cs:cpos]
        sbb     dx, [cs:cpos + 2]
        jnz     .fill                   ; davor oder weit dahinter
        cmp     ax, [cs:clen]
        jae     .fill2                  ; hinter dem Puffer
        mov     bx, [cs:clen]
        sub     bx, ax                  ; BX = noch im Puffer
        mov     cx, [cs:cwant]
        cmp     cx, bx
        jbe     .serve
        cmp     word [cs:clen], CSZ     ; Puffer endet vor dem Dateiende:
        je      .fill                   ; neu fuellen; sonst Dateiende
        mov     cx, bx
.serve: push    es                      ; CX Byte ab cbuf + AX in die DTA
        push    di
        lds     si, [cs:sda]
        les     di, [si + 0x0C]
        push    cs
        pop     ds
        mov     si, cbuf
        add     si, ax
        push    cx
        cld
        rep     movsb
        pop     cx
        pop     di
        pop     es
        add     [es:di + 0x15], cx      ; Lage weiter
        adc     word [es:di + 0x17], 0
        pop     ds
        pop     si
        pop     dx
        pop     bx
        clc
        sti
        retf    2
.fill2: cmp     word [cs:clen], CSZ     ; Dateiende schon im Puffer: 0
        je      .fill
        xor     cx, cx
        xor     ax, ax
        jmp     short .serve
.fill:  cmp     byte [cs:cfail], 0
        jne     .fail
        mov     ax, [es:di + 0x15]      ; Lage merken
        mov     [cs:cpos], ax
        mov     ax, [es:di + 0x17]
        mov     [cs:cpos + 2], ax
        lds     si, [cs:sda]            ; DTA kurz auf den Puffer
        mov     ax, [si + 0x0C]
        mov     [cs:cdta], ax
        mov     ax, [si + 0x0E]
        mov     [cs:cdta + 2], ax
        mov     word [si + 0x0C], cbuf
        mov     [si + 0x0E], cs
        mov     cx, CSZ
        mov     ax, 0x1008              ; RBOX: READ in den Puffer
        int     0xE0
        lds     si, [cs:sda]            ; DTA zurueck (Flags bleiben)
        mov     ax, [cs:cdta]
        mov     [si + 0x0C], ax
        mov     ax, [cs:cdta + 2]
        mov     [si + 0x0E], ax
        jnz     .fail                   ; nicht unser
        jc      .fail                   ; Fehler: normaler Weg meldet ihn
        mov     ax, [cs:cpos]           ; Lage wie vorher
        mov     [es:di + 0x15], ax
        mov     ax, [cs:cpos + 2]
        mov     [es:di + 0x17], ax
        mov     [cs:clen], cx
        mov     [cs:csft], di
        mov     [cs:csft + 2], es
        mov     ax, [es:di + 0x0B]
        mov     [cs:chnd], ax
        mov     byte [cs:cvalid], 1
        mov     byte [cs:cfail], 1      ; zweimal fuellen: nicht noch mal
        jmp     .again
.fail:  mov     byte [cs:cvalid], 0
        mov     byte [cs:cfail], 0
        mov     cx, [cs:cwant]
        pop     ds
        pop     si
        pop     dx
        pop     bx
        mov     ax, 0x1108
        jmp     int2f.pass

cvalid: db      0
cfail:  db      0
cwant:  dw      0
clen:   dw      0
cpos:   dd      0
csft:   dd      0
chnd:   dw      0
cdta:   dd      0
sda:    dd      0
cbuf:   times   CSZ db 0

; ---- Tastaturbelegung wie AT-DOS (kernel/kbd.c) --------------------------
; RBOX liefert die aktive Belegung (KEYB GR/US/GB). Der Haken sitzt im
; Tastaturabfang des BIOS (INT 15h/4Fh, AL = Scancode): Zeichentasten
; legt er selbst mit dem Zeichen der Belegung in den BIOS-Puffer und
; meldet CF = 0 (das BIOS uebergeht die Taste). Umschalt-, Strg-, Alt-,
; Funktions- und Ziffernblocktasten, Alt+Taste und Umschalt+Tab bleiben
; beim BIOS; Alt+Esc und Strg+Alt+Entf gehen weiter an den Stummel.
; AltGr (E0 38) merkt sich der Haken selbst in 496h Bit 3 (das AT-BIOS
; haelt die rechte Alt-Taste fuer Alt); der Kernel gleicht 417h/418h/496h
; bei jedem Wechsel mit seinem Tastaturtreiber ab.
keyb_install:
        push    cs
        pop     es
        mov     di, kb_norm
        mov     ah, 0x15
        int     0xE0
        test    ax, ax
        jnz     .no                     ; keine Belegung: BIOS (US)
        mov     ax, 0x3515
        int     0x21
        mov     [old15], bx
        mov     [old15+2], es
        mov     dx, int15
        mov     ax, 0x2515
        int     0x21
.no:    ret

int15:  cmp     ah, 0x4F
        je      .kb
        jmp     far [cs:old15]
.kb:    push    ax
        push    bx
        push    dx
        push    ds
        push    cs
        pop     ds
        cmp     al, 0xE0
        jne     .ne0
        mov     byte [e0], 1
        jmp     .out
.ne0:   cmp     byte [e0], 0
        je      .noe0
        mov     byte [e0], 0            ; erweiterte Taste: nur AltGr merken
        push    es                      ; AltGr in 496h Bit 3 (rechte Alt-
        xor     bx, bx                  ; Taste): der Kernel gleicht es mit
        mov     es, bx                  ; AT-DOS ab
        cmp     al, 0x38
        jne     .e0r
        or      byte [es:0x496], 8
        pop     es
        jmp     .out
.e0r:   cmp     al, 0xB8
        jne     .e0x
        and     byte [es:0x496], 0xF7
.e0x:   pop     es
        jmp     .out
.noe0:  test    al, 0x80                ; Loslassen: BIOS
        jnz     .out
        cmp     al, 0x5A
        jae     .out
        mov     bl, al
        xor     bh, bh                  ; BX = Scancode
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ah, [0x417]             ; AH = Umschaltzustand des BIOS
        mov     dl, [0x496]             ; Bit 3: AltGr
        pop     ds
        test    dl, 8
        jz      .noag
        mov     al, [kb_altg + bx]
        test    al, al
        jnz     .put
        jmp     .eat                    ; AltGr ohne Zeichen: nichts
.noag:  test    ah, 8                   ; Alt+Taste: BIOS (Scancode)
        jnz     .out
        mov     al, [kb_norm + bx]
        test    al, al
        jz      .out
        mov     dl, al                  ; DL = Grundzeichen
        test    ah, 4                   ; Strg+Buchstabe: 01h-1Ah
        jz      .noct
        cmp     dl, 'a'
        jb      .out
        cmp     dl, 'z'
        ja      .out
        sub     dl, 'a' - 1
        mov     al, dl
        jmp     .put
.noct:  cmp     bl, 0x0F                ; Umschalt+Tab: BIOS (0F00h)
        jne     .nt
        test    ah, 3
        jnz     .out
.nt:    test    ah, 3
        jz      .ns
        mov     al, [kb_shft + bx]
.ns:    test    ah, 0x40                ; Feststelltaste: nur Buchstaben
        jz      .chk
        cmp     dl, 'a'
        jb      .um
        cmp     dl, 'z'
        jbe     .tog
.um:    cmp     dl, 0x84                ; ae oe ue
        je      .tog
        cmp     dl, 0x94
        je      .tog
        cmp     dl, 0x81
        jne     .chk
.tog:   mov     al, [kb_shft + bx]
        test    ah, 3
        jz      .chk
        mov     al, dl
.chk:   test    al, al
        jz      .out
.put:   mov     ah, bl                  ; AX = Zeichen | Scancode << 8
        push    si
        push    di
        push    es
        xor     si, si
        mov     es, si
        mov     si, [es:0x41C]
        mov     di, si
        add     di, 2
        cmp     di, [es:0x482]
        jb      .w
        mov     di, [es:0x480]
.w:     cmp     di, [es:0x41A]
        je      .full                   ; Puffer voll
        mov     [es:0x400 + si], ax
        mov     [es:0x41C], di
.full:  pop     es
        pop     di
        pop     si
.eat:   pop     ds
        pop     dx
        pop     bx
        pop     ax
        push    bp                      ; CF = 0: das BIOS uebergeht sie
        mov     bp, sp
        and     byte [bp + 6], 0xFE
        pop     bp
        iret
.out:   pop     ds
        pop     dx
        pop     bx
        pop     ax
        jmp     far [cs:old15]

e0:     db      0
old15:  dd      0
kb_norm: times  90 db 0                 ; von RBOX (INT E0h 15h)
kb_shft: times  90 db 0
kb_altg: times  90 db 0

cdbuf:  times   130 db 0
dmask:  dd      0
sy_t:   dw      0
km:     dw      0FFFFh, 0FFFFh          ; zuletzt gesehene Laufwerke
old21:  dd      0
dbuf:   times   32 db 0
old2f:  dd      0
shell:  db      "A:\COMMAND.COM", 0
tail:   db      12, " A:\ /E:1024", 13
shellx: db      "C:\DOS\MDOS\COMMAND.COM", 0
        times   24 db 0                 ; Platz fuer \NAME (anderes DOS)
        align   16
envblk: db      "PATH="
env1:   db      "C:\DOS\MDOS;"
env2:   db      "C:\DOS", 0
        db      "NLSPATH="
env3:   db      "C:\DOS\MDOS", 0
        db      "LANG=DE", 0
        db      "DIRCMD=/OGN", 0, 0             ; DIR: Verzeichnisse zuerst
tailx:  db      21, " C:\DOS\MDOS\ /E:1024", 13
        times   12 db 0
tailk:  db      48, " C:\DOS\MDOS\ /E:1024 /K C:\DOS\MDOS\RBREADY.COM", 13
        times   12 db 0
        align   16
envo:   times   160 db 0                ; Umgebung fuer ein anderes DOS
perm:   db      0
nstart: db      0                       ; Bit 1: Notstart (F5)
fcb:    db      0, "           ", 0, 0, 0, 0
pblock: dw      0                               ; Umgebung des Aufrufers
pb_tail: dw     tail, 0
pb_fcb1: dw     fcb, 0
pb_fcb2: dw     fcb, 0

        align   2
        times   256 db 0
stack_top:

; ---- PATH, SET und PROMPT aus X:\AUTOEXEC.BAT ---------------------------
; COMMAND.COM fuehrt die AUTOEXEC.BAT in der Box nicht aus (DOS 5 kennt
; kein /K, die Box startet COMMAND.COM mit eigener Umgebung). Was Setup-
; Programme dort eintragen (Word: PATH C:\WORD;%PATH%), uebernimmt
; RBSHELL selbst: nur die Zeilen PATH, SET und PROMPT (auch mit @ vorn),
; %NAME% wird ersetzt, alles andere bleibt unbeachtet; SET COMSPEC nicht.
; Vorher BLASTER aus den Einstellungen von SB16.SYS (INT E0h 1Ah), wenn
; der Treiber eine Sound Blaster gefunden hat.
; Arbeitsblock per 48h: Umgebung vorn (bis AB_ENV Byte), dahinter Datei
; (bis 4 KB) und Puffer; zuletzt per 4Ah auf die Umgebung gekuerzt - sie
; bleibt fuer jeden Neustart von COMMAND.COM. Der Code liegt hinter
; stack_top und wird danach freigegeben (RBSHELL bleibt resident).
AB_ENV  equ     0x200
AB_FILE equ     0x200
AB_FSZ  equ     0x1000
AB_LINE equ     0x1210
AB_VAL  equ     0x1300
AB_VSZ  equ     0x1F0
AB_NAME equ     0x1500
AB_TNAM equ     0x1580
AB_PARA equ     0x160

ab_load:
        mov     ah, 0x48
        mov     bx, AB_PARA
        int     0x21
        jc      .r
        mov     [abseg], ax
        mov     es, ax
        mov     si, [pblock]            ; bisherige Umgebung kopieren
        mov     ax, cs
        sub     si, ax
        mov     cl, 4
        shl     si, cl
        xor     di, di
.cp:    lodsb
        stosb
        test    al, al
        jnz     .cp
        cmp     di, AB_ENV - 2
        jae     .cpe
        cmp     byte [si], 0
        jne     .cp
.cpe:   xor     al, al
        stosb
        push    es                      ; BLASTER aus SB16.SYS (INT E0h
        pop     ds                      ; 1Ah), AUTOEXEC.BAT geht vor
        mov     di, AB_VAL
        mov     ah, 0x1A
        int     0xE0
        test    ax, ax
        jnz     .nobl
        mov     bx, k_blaster
        call    ab_kname
        call    env_add
.nobl:  push    cs
        pop     ds
        mov     al, [shellx]            ; Startlaufwerk
        mov     [abname], al
        mov     dx, abname
        mov     ax, 0x3D00
        int     0x21
        push    es
        pop     ds
        jc      .done                   ; keine AUTOEXEC.BAT
        push    ds
        pop     es
        mov     bx, ax
        mov     dx, AB_FILE
        mov     cx, AB_FSZ - 1
        mov     ah, 0x3F
        int     0x21
        jnc     .rd
        xor     ax, ax
.rd:    mov     si, ax
        mov     byte [si + AB_FILE], 0
        mov     ah, 0x3E
        int     0x21
        push    ds                      ; ES gilt nach dem Redirector nicht
        pop     es                      ; mehr (Lesepuffer von RBSHELL)
        mov     si, AB_FILE
.ln:    cmp     byte [si], 0
        je      .done
        call    ab_getline
        push    si
        call    ab_doline
        pop     si
        jmp     .ln
.done:  call    env_end                 ; auf die Umgebung kuerzen
        inc     di
        add     di, 15
        mov     cl, 4
        shr     di, cl
        mov     bx, di
        mov     ah, 0x4A
        int     0x21
        mov     ax, es
        mov     [cs:pblock], ax
.r:     push    cs
        pop     ds
        push    cs
        pop     es
        ret

ab_getline:                             ; DS:SI -> Zeile nach AB_LINE
        mov     di, AB_LINE
        mov     cx, 127
.g:     lodsb
        test    al, al
        jz      .e0
        cmp     al, 0x1A
        je      .e0
        cmp     al, 13
        je      .eol
        cmp     al, 10
        je      .z
        jcxz    .g                      ; zu lang: Rest uebergehen
        stosb
        dec     cx
        jmp     .g
.eol:   cmp     byte [si], 10
        jne     .z
        inc     si
        jmp     .z
.e0:    dec     si
.z:     mov     byte [di], 0
        ret

ab_skipws:
        cmp     byte [si], ' '
        je      .s
        cmp     byte [si], 9
        jne     .r
.s:     inc     si
        jmp     ab_skipws
.r:     ret

ab_up:  cmp     al, 'a'                 ; AL gross
        jb      .r
        cmp     al, 'z'
        ja      .r
        sub     al, 0x20
.r:     ret

ab_kw:                                  ; CS:BX Schluesselwort an DS:SI?
        push    si                      ; CF = 0: ja, SI dahinter
.m:     mov     al, [cs:bx]
        test    al, al
        jz      .ok
        mov     ah, al
        lodsb
        call    ab_up
        cmp     al, ah
        jne     .no
        inc     bx
        jmp     .m
.ok:    mov     al, [si]                ; dahinter Trenner
        cmp     al, ' '
        je      .y
        cmp     al, 9
        je      .y
        cmp     al, '='
        je      .y
        cmp     al, ';'
        je      .y
.no:    pop     si
        stc
        ret
.y:     add     sp, 2
        clc
        ret

ab_doline:
        mov     si, AB_LINE
        call    ab_skipws
        cmp     byte [si], '@'
        jne     .k
        inc     si
        call    ab_skipws
.k:     mov     bx, k_path
        call    ab_kw
        jnc     .path
        mov     bx, k_prompt
        call    ab_kw
        jnc     .prompt
        mov     bx, k_set
        call    ab_kw
        jnc     .set
        ret
.path:  mov     bx, k_path
        call    ab_eq
        cmp     byte [si], 0            ; PATH allein zeigt nur an
        je      .r
        call    ab_kname
        cmp     word [si], ';'          ; "PATH ;" loescht
        je      .del
        call    ab_expand
        mov     si, AB_VAL              ; Pfade gross wie COMMAND.COM
.pu:    lodsb
        call    ab_up
        mov     [si - 1], al
        test    al, al
        jnz     .pu
        jmp     .add
.prompt: mov    bx, k_prompt
        call    ab_eq
        call    ab_kname
        cmp     byte [si], 0
        je      .del
        call    ab_expand
        jmp     .add
.set:   call    ab_skipws
        mov     di, AB_NAME
.sn:    lodsb
        test    al, al
        jz      .r
        cmp     al, '='
        je      .se
        call    ab_up
        stosb
        cmp     di, AB_NAME + 120
        jb      .sn
        ret
.se:    mov     byte [di], 0
        cmp     di, AB_NAME
        je      .r
        push    si
        mov     si, AB_NAME
        mov     bx, k_cspec
        call    ab_kw0
        pop     si
        jnc     .r                      ; COMSPEC gehoert der Box
        cmp     byte [si], 0
        je      .del
        call    ab_expand
.add:   call    env_add
.r:     ret
.del:   mov     si, AB_NAME
        jmp     env_del

ab_eq:  call    ab_skipws               ; Leerraum und ein "=" ueberspringen
        cmp     byte [si], '='
        jne     .r
        inc     si
        call    ab_skipws
.r:     ret

ab_kname:                               ; CS:BX -> AB_NAME, SI bleibt
        push    si
        mov     di, AB_NAME
.c:     mov     al, [cs:bx]
        stosb
        inc     bx
        test    al, al
        jnz     .c
        pop     si
        ret

ab_kw0:                                 ; DS:SI genau gleich CS:BX? CF = 0
.m:     mov     al, [cs:bx]
        cmp     al, [si]
        jne     .no
        test    al, al
        jz      .y
        inc     si
        inc     bx
        jmp     .m
.no:    stc
        ret
.y:     clc
        ret

ab_expand:                              ; DS:SI -> AB_VAL, %NAME% ersetzt
        mov     di, AB_VAL
.e:     lodsb
        test    al, al
        jz      .end
        cmp     al, '%'
        je      .pct
.put:   cmp     di, AB_VAL + AB_VSZ - 1
        jae     .e
        stosb
        jmp     .e
.pct:   cmp     byte [si], '%'          ; %% = %
        jne     .var
        inc     si
        jmp     .put
.var:   mov     bx, si
.f:     mov     al, [bx]
        test    al, al
        jz      .lit
        cmp     al, '%'
        je      .got
        inc     bx
        jmp     .f
.lit:   mov     al, '%'
        jmp     .put
.got:   push    di
        mov     di, AB_TNAM
.n:     cmp     si, bx
        je      .nd
        lodsb
        call    ab_up
        stosb
        jmp     .n
.nd:    mov     byte [di], 0
        pop     di
        inc     si
        push    si
        mov     si, AB_TNAM
        push    di
        call    env_find                ; aendert DI
        pop     di
        jc      .nv
        mov     si, bx
.cv:    lodsb
        test    al, al
        jz      .nv
        cmp     di, AB_VAL + AB_VSZ - 1
        jae     .nv
        stosb
        jmp     .cv
.nv:    pop     si
        jmp     .e
.end:   mov     byte [di], 0
        ret

env_find:                               ; DS:SI Name -> DI Eintrag, BX Wert
        xor     di, di                  ; CF = 1: fehlt
.l:     cmp     byte [di], 0
        je      .nf
        push    si
        push    di
.c:     mov     al, [si]
        test    al, al
        jz      .eq
        cmp     al, [di]
        jne     .ne
        inc     si
        inc     di
        jmp     .c
.eq:    cmp     byte [di], '='
        jne     .ne
        lea     bx, [di + 1]
        pop     di
        pop     si
        clc
        ret
.ne:    pop     di
        pop     si
.s:     cmp     byte [di], 0
        je      .s2
        inc     di
        jmp     .s
.s2:    inc     di
        jmp     .l
.nf:    stc
        ret

env_end:                                ; DI -> abschliessende Null
        xor     di, di
.l:     cmp     byte [di], 0
        je      .r
.s:     inc     di
        cmp     byte [di - 1], 0
        jne     .s
        jmp     .l
.r:     ret

env_del:                                ; Eintrag DS:SI entfernen
        call    env_find
        jc      .r
        mov     si, di
.s:     lodsb
        test    al, al
        jnz     .s
.m:     cmp     byte [si], 0
        je      .fin
.c:     lodsb
        stosb
        test    al, al
        jnz     .c
        jmp     .m
.fin:   mov     byte [di], 0
.r:     ret

env_add:                                ; AB_NAME=AB_VAL setzen
        mov     si, AB_NAME
        call    env_del
        call    env_end
        mov     ax, di
        mov     si, AB_NAME
        call    .len
        mov     si, AB_VAL
        call    .len
        add     ax, 2
        cmp     ax, AB_ENV - 1
        ja      .r                      ; kein Platz: auslassen
        mov     si, AB_NAME
.n:     lodsb
        test    al, al
        jz      .q
        stosb
        jmp     .n
.q:     mov     al, '='
        stosb
        mov     si, AB_VAL
.v:     lodsb
        stosb
        test    al, al
        jnz     .v
        mov     byte [di], 0
.r:     ret
.len:   cmp     byte [si], 0
        je      .lr
        inc     si
        inc     ax
        jmp     .len
.lr:    ret

abseg:  dw      0
abname: db      "C:\AUTOEXEC.BAT", 0
k_path: db      "PATH", 0
k_prompt: db    "PROMPT", 0
k_set:  db      "SET", 0
k_cspec: db     "COMSPEC", 0
k_blaster: db   "BLASTER", 0
trans_end:
