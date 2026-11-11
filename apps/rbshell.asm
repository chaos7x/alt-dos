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
; Laufwerk:Verzeichnis, aus dem RBOX gestartet wurde, nach ES:DI (leer bei
; A:/B:) - FreeCOM beginnt dort; AH=15h Tastaturbelegung von AT-DOS
; (3 x 90 Zeichen) nach ES:DI -> AX = 0.
cpu 8086
org 0x100

start:
        mov     sp, stack_top
        mov     bx, (stack_top - start + 0x100 + 15) / 16  ; Rest frei
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
        mov     bx, pblock
        mov     ax, 0x4B00
        int     0x21
        jnc     .ran
        cmp     ax, 15                          ; EXEC gescheitert: RBOX
        jbe     .e                              ; meldet D0h + DOS-Fehler
        mov     al, 15
.e:     or      al, 0xD0
        jmp     .done
.ran:
        mov     ah, 0x4D                        ; AH = Art des Endes: 0 EXIT,
        int     0x21                            ; 1 Strg+C, 2 kritischer
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

; ---- Redirector ----------------------------------------------------------
redir_install:
        push    ds
        mov     ax, 0x5D06              ; DS:SI = SDA
        int     0x21
        mov     ah, 0x11                ; RBOX melden
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
        ret

int2f:  cmp     ah, 0x11
        jne     .chain
        test    al, al                  ; Installationsabfrage
        jnz     .op
        mov     al, 0xFF
        iret
.op:    cmp     al, 0x23                ; Namen pruefen: fuer jeden Pfad,
        je      .chain                  ; nie unser (spart einen Ausflug)
        push    ax
        mov     ah, 0x10
        int     0xE0                    ; RBOX: ZF=1 erledigt
        jnz     .not
        inc     sp                      ; AX bleibt das Ergebnis; INC
        inc     sp                      ; laesst CF stehen (ADD nicht!)
        sti
        retf    2                       ; CF von RBOX zurueckgeben
.not:   pop     ax
.chain: jmp     far [cs:old2f]

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
old2f:  dd      0
shell:  db      "A:\COMMAND.COM", 0
tail:   db      12, " A:\ /E:1024", 13
fcb:    db      0, "           ", 0, 0, 0, 0
pblock: dw      0                               ; Umgebung des Aufrufers
pb_tail: dw     tail, 0
pb_fcb1: dw     fcb, 0
pb_fcb2: dw     fcb, 0

        align   2
        times   256 db 0
stack_top:
