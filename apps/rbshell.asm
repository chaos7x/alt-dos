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
; A:/B:) - FreeCOM beginnt dort.
cpu 8086
org 0x100

start:
        mov     sp, stack_top
        mov     bx, (stack_top - start + 0x100 + 15) / 16  ; Rest frei
        mov     ah, 0x4A
        int     0x21
        call    redir_install
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
        mov     al, 0xFF
        jc      .done
        mov     ah, 0x4D
        int     0x21
.done:  mov     ah, 1                           ; Box beenden, AL = Code
        int     0xE0
        jmp     $

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
