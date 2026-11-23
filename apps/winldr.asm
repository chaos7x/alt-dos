; SPDX-License-Identifier: GPL-2.0-or-later
;
; WINLDR.COM - Lader fuer Windows 3.1 im Standardmodus in der DOS-Box
; (Weg B in docs/DOSKERN.md, wie WIN-OS/2): statt DOSX.EXE, das nur VCPI
; oder den Real Mode kennt, laeuft der Windows-Kern (KRNL286/KRNL386)
; als DPMI-Programm auf dem Wirt des AT-DOS-Kernels. WINLDR uebernimmt,
; was DOSX dem Kern ausser DPMI bietet: die Uebersetzung der DOS-Aufrufe
; aus dem Protected Mode (Zeiger DS:DX usw. -> Puffer unter 1 MB, Seg-
; mente -> Selektoren). Die Erweiterung "MS-DOS" (INT 2Fh 168Ah, LDT-
; Alias) bietet der Wirt selbst.
;
; Aufruf: WINLDR Programm [Argumente]
; Ablauf: INT 2Fh 1687h abfangen -> der Kern bekommt den eigenen Ein-
; sprung; der ruft den Moduswechsel des Wirts, haengt im PM INT 21h ein
; und kehrt mit einem Codeselektor fuer den Aufrufer zurueck.
        cpu     386
        org     100h

BUFSZ   equ     4096                    ; Umkopierpuffer (unter 1 MB)

start:  jmp     init

; ---- residente Daten (bleiben im Speicher) --------------------------------
old2f   dd      0                       ; alter INT-2Fh-Vektor (RM)
host    dd      0                       ; Moduswechsel des Wirts
cli_ip  dw      0                       ; Aufrufer des Moduswechsels
cli_cs  dw      0, 0
rmseg   dw      0                       ; unser Segment
dsel    dw      0                       ; Datenselektor auf unser Segment
csel    dw      0                       ; Codeselektor fuer pm21
xsel    dw      0                       ; Daten, Basis je Kopie
old21   dd      0                       ; alter PM-Vektor 21h (Wirt)
old10   dd      0                       ; alter PM-Vektor 10h
old15   dd      0                       ; alter PM-Vektor 15h
rmint   dw      21h                     ; INT fuer callrm
pdta    dd      0                       ; DTA des Programms (Sel:Off)
c_ds    dw      0                       ; DS/ES des Aufrufers im PM
c_es    dw      0
ftype   dw      0                       ; Art der Funktion
fl_out  dw      0                       ; Rueckgabe: welche Register
                                        ; Bit 0 DX, 1 SI, 2 DI, 3 BX, 4 CX
; Register fuer DPMI 0300h (Real Mode aufrufen)
R:
R_edi   dd      0
R_esi   dd      0
R_ebp   dd      0
        dd      0
R_ebx   dd      0
R_edx   dd      0
R_ecx   dd      0
R_eax   dd      0
R_fl    dw      0
R_es    dw      0
R_ds    dw      0
R_fs    dw      0
R_gs    dw      0
R_ip    dw      0
R_cs    dw      0
R_sp    dw      0
R_ss    dw      0

; ---- Real Mode: INT 2Fh ------------------------------------------------
rm2f:   cmp     ax, 1687h
        jne     .chain
        pushf
        call    far [cs:old2f]
        test    ax, ax
        jnz     .r
        mov     [cs:host], di
        mov     [cs:host + 2], es
        push    cs
        pop     es
        mov     di, rm_entry
.r:     iret
.chain: jmp     far [cs:old2f]

; ---- Moduswechsel fuer den Kern (Fernaufruf im Real Mode) ---------------
rm_entry:
        pop     word [cs:cli_ip]
        pop     word [cs:cli_cs]
        call    far [cs:host]           ; zurueck im PM (CS = Selektor)
        jc      .fail
        pusha
        push    ds
        push    es
        mov     ax, 0002h               ; Datenselektor fuer uns
        mov     bx, [cs:rmseg]
        int     31h
        jc      .bad
        mov     ds, ax
        mov     [dsel], ax
        xor     ax, ax                  ; Arbeitsselektor fuer 3Fh/40h
        mov     cx, 1
        int     31h
        jc      .bad
        mov     [xsel], ax
        mov     bx, ax
        mov     ax, 0008h
        xor     cx, cx
        mov     dx, 0FFFFh
        int     31h
        mov     ax, 0204h               ; alter PM-Vektor 21h
        mov     bl, 21h
        int     31h
        mov     [old21], dx
        mov     [old21 + 2], cx
        xor     ax, ax                  ; eigener Codeselektor fuer pm21
        mov     cx, 1                   ; (den vom Wirt bekommt der Kern)
        int     31h
        jc      .bad
        mov     bx, ax
        mov     [csel], ax
        mov     dx, [rmseg]
        call    .setcode
        mov     ax, 0205h
        mov     bl, 21h
        mov     cx, [csel]
        mov     dx, pm21
        int     31h
        mov     ax, 0204h               ; INT 10h ebenso (Zeiger in ES)
        mov     bl, 10h
        int     31h
        mov     [old10], dx
        mov     [old10 + 2], cx
        mov     ax, 0205h
        mov     bl, 10h
        mov     cx, [csel]
        mov     dx, pm10
        int     31h
        mov     ax, 0204h               ; INT 15h C0h (ES:BX zurueck)
        mov     bl, 15h
        int     31h
        mov     [old15], dx
        mov     [old15 + 2], cx
        mov     ax, 0205h
        mov     bl, 15h
        mov     cx, [csel]
        mov     dx, pm15
        int     31h
        mov     word [pdta], 0          ; DTA erst mit 1Ah
        mov     word [pdta + 2], 0
        mov     [cli_cs + 2], cs        ; (Ablage: Selektor des Wirts)
        push    word [csel]             ; erst in den eigenen Selektor
        push    word .cont              ; wechseln: jede Falle laedt CS
        retf                            ; neu (IRET des Wirts)
.cont:  mov     bx, [cli_cs + 2]        ; der Selektor des Wirts zeigt
        mov     dx, [cli_cs]            ; danach auf den Kern - wie ohne
        call    .setcode                ; Lader (KRNL rechnet damit)
        mov     ax, [cli_cs + 2]
        mov     [cli_cs], ax
        pop     es
        pop     ds
        popa
        push    word [cs:cli_cs]        ; (CS liest noch den alten
        push    word [cs:cli_ip]        ;  Deskriptor aus dem Cache)
        clc
        retf
; BX = Selektor, DX = Segment: Code, 64 KB, DPL 3
.setcode:
        mov     cx, dx
        shl     dx, 4
        shr     cx, 12
        mov     ax, 0007h
        int     31h
        mov     ax, 0008h
        xor     cx, cx
        mov     dx, 0FFFFh
        int     31h
        mov     ax, 0009h
        mov     cx, 00FAh
        int     31h
        ret
.bad:   mov     ax, 4CFFh               ; im PM ohne Ausweg: Ende
        int     21h
.fail:  push    word [cs:cli_cs]
        push    word [cs:cli_ip]
        retf                            ; CF bleibt gesetzt

; ---- Protected Mode: INT 21h -------------------------------------------
; Tabelle: Funktion, Art. Unbekanntes geht unveraendert an den Wirt
; (der reicht es roh in den Real Mode).
T_PASS  equ     1                       ; nur Register
T_SZ    equ     2                       ; DS:DX Name hin
T_RD    equ     3                       ; 3Fh
T_WR    equ     4                       ; 40h
T_VGET  equ     5                       ; 35h
T_VSET  equ     6                       ; 25h
T_DSET  equ     7                       ; 1Ah
T_DGET  equ     8                       ; 2Fh
T_FIND  equ     9                       ; 4Eh (DS:DX) / 4Fh
T_CDIR  equ     10                      ; 47h DS:SI 64 Byte zurueck
T_REN   equ     11                      ; 56h DS:DX, ES:DI
T_XOPN  equ     12                      ; 6Ch DS:SI
T_TRUE  equ     13                      ; 60h DS:SI -> ES:DI
T_CTRY  equ     14                      ; 38h DS:DX 34 Byte zurueck
T_ESBX  equ     15                      ; 34h/52h ES:BX -> Selektor
T_PSP   equ     16                      ; 51h/62h BX -> Selektor
T_MEM   equ     17                      ; 48h/49h/4Ah
T_STR   equ     18                      ; 09h ('$')
T_TMP   equ     19                      ; 5Ah DS:DX hin und zurueck
T_FFFF  equ     20                      ; 4Ch: an den Wirt
T_PSET  equ     21                      ; 50h: BX Selektor -> Segment
T_NPSP  equ     22                      ; 26h/55h: DX Selektor -> Segment

ftab:   db      02h, T_PASS, 06h, T_PASS, 07h, T_PASS, 08h, T_PASS
        db      09h, T_STR,  0Bh, T_PASS, 0Ch, T_PASS, 0Eh, T_PASS
        db      19h, T_PASS, 1Ah, T_DSET, 25h, T_VSET, 2Ah, T_PASS
        db      2Bh, T_PASS, 2Ch, T_PASS, 2Dh, T_PASS, 2Eh, T_PASS
        db      2Fh, T_DGET, 30h, T_PASS, 33h, T_PASS, 34h, T_ESBX
        db      35h, T_VGET, 36h, T_PASS, 38h, T_CTRY, 39h, T_SZ
        db      3Ah, T_SZ,   3Bh, T_SZ,   3Ch, T_SZ,   3Dh, T_SZ
        db      3Eh, T_PASS, 3Fh, T_RD,   40h, T_WR,   41h, T_SZ
        db      42h, T_PASS, 43h, T_SZ,   45h, T_PASS, 46h, T_PASS
        db      47h, T_CDIR, 48h, T_MEM,  49h, T_MEM,  4Ah, T_MEM
        db      4Ch, T_FFFF, 4Dh, T_PASS, 4Eh, T_FIND, 4Fh, T_FIND
        db      50h, T_PSET, 51h, T_PSP,  52h, T_ESBX, 55h, T_NPSP, 26h, T_NPSP, 54h, T_PASS, 56h, T_REN
        db      57h, T_PASS, 58h, T_PASS, 59h, T_PASS, 5Ah, T_TMP
        db      5Bh, T_SZ,   5Ch, T_PASS, 60h, T_TRUE, 62h, T_PSP
        db      66h, T_PASS, 67h, T_PASS, 68h, T_PASS, 6Ch, T_XOPN
        db      0

pm21:   push    ds
        mov     ds, [cs:dsel]
        pop     word [c_ds]
        mov     [c_es], es
        push    si
        mov     si, ftab
.look:  cmp     byte [cs:si], 0
        je      .raw
        cmp     [cs:si], ah
        je      .hit
        add     si, 2
        jmp     .look
.raw:   pop     si                      ; unbekannt: roh an den Wirt
        mov     es, [c_es]
        push    word [c_ds]
        pop     ds
        jmp     far [cs:old21]
.hit:   push    word [cs:si + 1]        ; Art (unteres Byte)
        pop     word [ftype]
        pop     si
        pushad                          ; Register des Programms
        mov     [R_eax], eax
        mov     [R_ebx], ebx
        mov     [R_ecx], ecx
        mov     [R_edx], edx
        mov     [R_esi], esi
        mov     [R_edi], edi
        mov     [R_ebp], ebp
        mov     word [fl_out], 1Fh      ; Vorgabe: alles aus R zurueck
        mov     word [rmint], 21h
        mov     ax, [rmseg]
        mov     [R_ds], ax
        mov     [R_es], ax
        xor     ax, ax
        mov     [R_ss], ax              ; Stapel vom Wirt
        mov     [R_sp], ax
        mov     [R_fs], ax
        mov     [R_gs], ax
        mov     eax, [R_eax]
        movzx   bx, byte [ftype]
        add     bx, bx
        mov     bx, [cs:bx + jtab - 2]
        mov     [ftype], bx             ; (jetzt Sprungziel)
        mov     ebx, [R_ebx]            ; BX des Programms zurueck
        jmp     [ftype]

jtab:   dw      f_pass, f_sz, f_rd, f_wr, f_vget, f_vset, f_dset, f_dget
        dw      f_find, f_cdir, f_ren, f_xopn, f_true, f_ctry, f_esbx, f_psp
        dw      f_mem, f_str, f_tmp, f_ffff, f_pset, f_npsp

; Real Mode aufrufen (INT 21h mit R), Ergebnis in die Register
callrm: push    es
        push    di
        push    bx
        push    cx
        push    ds
        pop     es
        mov     di, R
        mov     ax, 0300h
        mov     bx, [rmint]
        xor     cx, cx
        int     31h
        pop     cx
        pop     bx
        pop     di
        pop     es
        ret

; zurueck ins Programm: Register des Programms, AX und nach fl_out
; weitere aus R, CF aus R_fl
done:   popad
        mov     ax, [R_eax]
        test    byte [fl_out], 8
        jz      .nb
        mov     ebx, [R_ebx]
.nb:    test    byte [fl_out], 16
        jz      .nc
        mov     ecx, [R_ecx]
.nc:    test    byte [fl_out], 1
        jz      .nd
        mov     edx, [R_edx]
.nd:    test    byte [fl_out], 2
        jz      .ns
        mov     esi, [R_esi]
.ns:    test    byte [fl_out], 4
        jz      .ndi
        mov     edi, [R_edi]
.ndi:   push    bp                      ; CF in die Flags auf dem Stapel
        mov     bp, sp
        and     word [bp + 6], 0FFFEh   ; (BP, IP, CS, FLAGS)
        test    byte [R_fl], 1
        jz      .ok
        or      word [bp + 6], 1
.ok:    pop     bp
        mov     es, [c_es]
        push    word [c_ds]
        pop     ds
        iret

; AX = Offset beim Programm (DS) -> ES = xsel, Basis = Basis(DS) + AX
xmap:   pusha
        push    ax
        mov     bx, [c_ds]
        mov     ax, 0006h
        int     31h
        pop     ax
        add     dx, ax
        adc     cx, 0
        mov     bx, [xsel]
        mov     ax, 0007h
        int     31h
        popa
        mov     es, [xsel]
        ret

; esi/edi/edx der Register vor dem Aufruf (fuer Zeiger)
; Name DS:DX des Programms -> Puffer (bis 260 Byte), R_ds:R_edx = Puffer
cpsz_dx:
        movzx   esi, word [R_edx]
cpsz:   push    ds
        push    es
        push    ds
        pop     es
        mov     di, buf
        mov     ds, [c_ds]
        mov     cx, 260
.l:     lodsb
        stosb
        test    al, al
        loopnz  .l
        mov     byte [es:di - 1 + 0], al
        pop     es
        pop     ds
        ret

f_pass: call    callrm
        jmp     done

f_sz:   call    cpsz_dx
        mov     word [R_edx], buf
        call    callrm
        and     byte [fl_out], 0FEh     ; DX bleibt
        jmp     done

f_tmp:  call    cpsz_dx                 ; 5Ah: Verzeichnis hin, Name zurueck
        mov     word [R_edx], buf
        call    callrm
        mov     word [fl_out], 0
        push    es
        mov     es, [c_ds]
        mov     di, dx
        mov     si, buf
        mov     cx, 260
        rep     movsb
        pop     es
        jmp     done

f_str:  push    ds                      ; 09h: bis '$' (hoechstens 4 KB)
        push    es
        push    ds
        pop     es
        mov     di, buf
        mov     si, dx
        mov     ds, [c_ds]
        mov     cx, BUFSZ - 1
.l:     lodsb
        stosb
        cmp     al, '$'
        loopne  .l
        mov     byte [es:di], '$'
        pop     es
        pop     ds
        mov     word [R_edx], buf
        call    callrm
        and     byte [fl_out], 0FEh
        jmp     done

; 3Fh lesen: in Stuecken zu BUFSZ nach DS:DX
f_rd:   mov     bp, cx                  ; Rest
        xor     di, di                  ; gelesen
.l:     test    bp, bp
        jz      .end
        mov     cx, bp
        cmp     cx, BUFSZ
        jbe     .n
        mov     cx, BUFSZ
.n:     mov     word [R_eax], 3F00h
        mov     [R_ecx], cx
        mov     word [R_edx], buf
        push    cx
        call    callrm
        pop     cx
        test    byte [R_fl], 1
        jnz     .err
        mov     ax, [R_eax]             ; gelesen
        push    ax
        push    es
        push    cx
        push    di
        push    ax
        mov     ax, dx                  ; Ziel ueber xsel (wie DOSX: die
        add     ax, di                  ; Grenze des Segments gilt nicht)
        call    xmap
        pop     cx
        mov     si, buf
        xor     di, di
        rep     movsb
        pop     di
        pop     cx
        pop     es
        pop     ax
        add     di, ax
        sub     bp, ax
        cmp     ax, cx
        je      .l                      ; kurz gelesen: Ende der Datei
.end:   mov     [R_eax], di
        and     byte [R_fl], 0FEh
        mov     word [fl_out], 0
        jmp     done
.err:   test    di, di                  ; Fehler nach Teil: Teil melden
        jnz     .end
        mov     word [fl_out], 0
        jmp     done

; 40h schreiben: CX = 0 kuerzt (einmal durchreichen)
f_wr:   mov     bp, cx
        xor     di, di
        test    cx, cx
        jnz     .l
        call    callrm
        mov     word [fl_out], 0
        jmp     done
.l:     test    bp, bp
        jz      .end
        mov     cx, bp
        cmp     cx, BUFSZ
        jbe     .n
        mov     cx, BUFSZ
.n:     push    ds                      ; Stueck holen (ueber xsel)
        push    es
        push    cx
        push    di
        mov     ax, dx
        add     ax, di
        call    xmap
        push    es
        push    ds
        pop     es
        pop     ds
        xor     si, si
        mov     di, buf
        rep     movsb
        pop     di
        pop     cx
        pop     es
        pop     ds
        mov     word [R_eax], 4000h
        mov     [R_ecx], cx
        mov     word [R_edx], buf
        push    cx
        call    callrm
        pop     cx
        test    byte [R_fl], 1
        jnz     .err
        mov     ax, [R_eax]
        add     di, ax
        sub     bp, ax
        cmp     ax, cx
        je      .l
.end:   mov     [R_eax], di
        and     byte [R_fl], 0FEh
        mov     word [fl_out], 0
        jmp     done
.err:   test    di, di
        jnz     .end
        mov     word [fl_out], 0
        jmp     done

f_vget: mov     bl, al                  ; 35h -> DPMI 0204h, ES:BX
        mov     ax, 0204h
        int     31h
        mov     [c_es], cx
        mov     [R_ebx], dx
        mov     byte [R_fl], 0
        mov     word [fl_out], 8
        mov     eax, [R_eax]
        jmp     done

f_vset: mov     bl, al                  ; 25h -> DPMI 0205h, DS:DX
        mov     cx, [c_ds]
        mov     ax, 0205h
        int     31h
        mov     byte [R_fl], 0
        mov     word [fl_out], 0
        mov     eax, [R_eax]
        jmp     done

f_dset: mov     [pdta], dx              ; 1Ah: DTA des Programms merken,
        mov     ax, [c_ds]              ; im Real Mode gilt unser Puffer
        mov     [pdta + 2], ax
        mov     word [R_edx], dta
        call    callrm
        mov     word [fl_out], 0
        jmp     done

f_dget: mov     bx, [pdta]              ; 2Fh: die des Programms
        mov     ax, [pdta + 2]
        mov     [c_es], ax
        mov     [R_ebx], bx
        mov     byte [R_fl], 0
        mov     word [fl_out], 8
        mov     eax, [R_eax]
        jmp     done

f_find: cmp     ah, 4Eh
        jne     .nx
        call    cpsz_dx
        mov     word [R_edx], buf
.nx:    push    word [R_eax]            ; RM-DTA auf unseren Puffer
        mov     word [R_eax], 1A00h
        push    word [R_edx]
        mov     word [R_edx], dta
        call    callrm
        pop     word [R_edx]
        pop     word [R_eax]
        call    callrm
        mov     word [fl_out], 0
        cmp     word [pdta + 2], 0
        je      done
        push    es
        mov     di, [pdta]
        mov     es, [pdta + 2]
        mov     si, dta
        mov     cx, 43
        rep     movsb
        pop     es
        jmp     done

f_cdir: mov     word [R_esi], buf       ; 47h: DS:SI 64 Byte
        call    callrm
        mov     word [fl_out], 0
        test    byte [R_fl], 1
        jnz     done
        push    es
        mov     es, [c_ds]
        mov     di, si
        mov     si, buf
        mov     cx, 64
        rep     movsb
        pop     es
        jmp     done

f_ren:  call    cpsz_dx                 ; 56h: alt DS:DX, neu ES:DI
        mov     word [R_edx], buf
        push    ds
        push    es
        push    ds
        pop     es
        mov     si, di
        mov     di, buf2
        mov     ds, [c_es]
        mov     cx, 260
.l:     lodsb
        stosb
        test    al, al
        loopnz  .l
        pop     es
        pop     ds
        mov     word [R_edi], buf2
        call    callrm
        mov     word [fl_out], 0
        jmp     done

f_xopn: movzx   esi, si                 ; 6Ch: DS:SI
        call    cpsz
        mov     word [R_esi], buf
        call    callrm
        mov     word [fl_out], 16       ; AX, CX (Aktion), nicht SI
        jmp     done

f_true: movzx   esi, si                 ; 60h: DS:SI -> ES:DI (128)
        call    cpsz
        mov     word [R_esi], buf
        mov     word [R_edi], buf2
        call    callrm
        mov     word [fl_out], 0
        test    byte [R_fl], 1
        jnz     done
        push    es
        mov     es, [c_es]
        mov     si, buf2
        mov     cx, 128
        rep     movsb
        pop     es
        jmp     done

f_ctry: mov     word [R_edx], buf       ; 38h: DS:DX 34 Byte (DX FFFFh: setzen)
        cmp     dx, 0FFFFh
        jne     .q
        mov     word [R_edx], 0FFFFh
.q:     call    callrm
        mov     word [fl_out], 8        ; BX = Land
        test    byte [R_fl], 1
        jnz     done
        cmp     dx, 0FFFFh
        je      done
        push    es
        mov     es, [c_ds]
        mov     di, dx
        mov     si, buf
        mov     cx, 34
        rep     movsb
        pop     es
        jmp     done

f_esbx: call    callrm                  ; 34h/52h: ES:BX, ES -> Selektor
        mov     bx, [R_es]
        mov     ax, 0002h
        int     31h
        mov     [c_es], ax
        mov     word [fl_out], 8
        jmp     done

f_psp:  call    callrm                  ; 51h/62h: BX = Segment -> Selektor
        mov     bx, [R_ebx]
        mov     ax, 0002h
        int     31h
        mov     [R_ebx], ax
        mov     word [fl_out], 8
        jmp     done

f_mem:  cmp     ah, 48h                 ; 48h/49h/4Ah -> DPMI 0100h-0102h
        jne     .free
        mov     ax, 0100h
        int     31h
        jc      .err
        mov     [R_eax], dx             ; AX = Selektor (wie DOSX)
        mov     byte [R_fl], 0
        mov     word [fl_out], 0
        jmp     done
.err:   mov     [R_eax], ax
        mov     [R_ebx], bx
        mov     byte [R_fl], 1
        mov     word [fl_out], 8
        jmp     done
.free:  mov     dx, [c_es]
        cmp     ah, 49h
        jne     .size
        mov     ax, 0101h
        int     31h
        jc      .err
        mov     byte [R_fl], 0
        mov     word [fl_out], 0
        jmp     done
.size:  mov     ax, 0102h
        int     31h
        jc      .err
        mov     byte [R_fl], 0
        mov     word [fl_out], 0
        jmp     done

f_pset: mov     ax, 0006h               ; 50h: Basis des Selektors
        int     31h                     ; CX:DX -> Segment
        shrd    dx, cx, 4
        mov     [R_ebx], dx
        call    callrm
        mov     word [fl_out], 0
        jmp     done

f_npsp: mov     bx, dx                  ; 26h/55h: neuer PSP bei DX
        mov     ax, 0006h
        int     31h
        shrd    dx, cx, 4
        mov     [R_edx], dx
        call    callrm
        mov     word [fl_out], 0
        jmp     done

f_ffff: popad                           ; 4Ch u. a.: an den Wirt
        mov     es, [c_es]
        push    word [c_ds]
        pop     ds
        jmp     far [cs:old21]

; ---- Protected Mode: INT 10h mit Zeigern (wie DOSX) -------------------
; 1002h/1012h ES:DX hin, 1009h/1017h ES:DX zurueck, 1Bh ES:DI zurueck,
; 13h und 1100h/1110h ES:BP hin; alles andere unveraendert an den Wirt
pm10:   cmp     ah, 10h
        je      .p
        cmp     ah, 1Bh
        je      .go
        cmp     ah, 13h
        je      .go
        cmp     ax, 1100h
        je      .go
        cmp     ax, 1110h
        je      .go
.raw:   jmp     far [cs:old10]
.p:     cmp     al, 02h
        je      .go
        cmp     al, 09h
        je      .go
        cmp     al, 12h
        je      .go
        cmp     al, 17h
        jne     .raw
.go:    push    ds
        mov     ds, [cs:dsel]
        pop     word [c_ds]
        mov     [c_es], es
        pushad
        mov     [R_eax], eax
        mov     [R_ebx], ebx
        mov     [R_ecx], ecx
        mov     [R_edx], edx
        mov     [R_esi], esi
        mov     [R_edi], edi
        mov     [R_ebp], ebp
        mov     word [rmint], 10h
        mov     word [fl_out], 0
        mov     bx, [rmseg]
        mov     [R_ds], bx
        mov     [R_es], bx
        xor     bx, bx
        mov     [R_ss], bx
        mov     [R_sp], bx
        mov     [R_fs], bx
        mov     [R_gs], bx
        mov     word [R_fl], 0
        ; Laenge und Quelle/Ziel bestimmen: SI = Offset beim Programm,
        ; CX = Bytes, DI = R-Feld fuer den Pufferoffset, BL = 1 hin, 2 zurueck
        mov     si, dx
        mov     di, R_edx
        cmp     ah, 1Bh
        jne     .n1b
        mov     si, [R_edi]
        mov     di, R_edi
        mov     cx, 64
        mov     bl, 2
        jmp     .cp
.n1b:   cmp     ah, 13h
        jne     .n13
        mov     si, bp
        mov     di, R_ebp
        test    al, 2
        jz      .cp1
        add     cx, cx
.cp1:   mov     bl, 1
        jmp     .cp
.n13:   cmp     ah, 11h
        jne     .n11
        mov     si, bp
        mov     di, R_ebp
        movzx   ax, bh                  ; CX Zeichen zu BH Byte
        mul     cx
        mov     cx, ax
        mov     bl, 1
        jmp     .cp
.n11:   mov     cx, 17                  ; 10h: 02/09 17 Byte, 12/17 CX*3
        cmp     al, 09h
        jbe     .n10
        mov     ax, cx                  ; (CX = 17 hier ueberschrieben)
        mov     ax, [R_ecx]
        mov     cx, ax
        add     cx, ax
        add     cx, ax
.n10:   mov     bl, 1
        mov     al, [R_eax]
        cmp     al, 09h
        je      .out
        cmp     al, 17h
        jne     .cp
.out:   mov     bl, 2
.cp:    cmp     cx, BUFSZ
        jbe     .cpo
        mov     cx, BUFSZ
.cpo:   mov     word [di], buf
        push    cx
        push    si
        cmp     bl, 1
        jne     .call
        push    ds                      ; hin: ES:SI -> buf
        push    es
        push    ds
        pop     es
        mov     di, buf
        mov     ds, [c_es]
        rep     movsb
        pop     es
        pop     ds
.call:  push    bx
        call    callrm
        pop     bx
        pop     di                      ; Offset beim Programm
        pop     cx
        cmp     bl, 2
        jne     .end
        push    es                      ; zurueck: buf -> ES:DI
        mov     es, [c_es]
        mov     si, buf
        rep     movsb
        pop     es
.end:   popad
        mov     ax, [R_eax]
        test    byte [fl_out], 8
        jz      .e1
        mov     bx, [R_ebx]
.e1:    mov     es, [c_es]
        push    word [c_ds]
        pop     ds
        iret

; ---- Protected Mode: INT 15h C0h (Konfigurationstabelle, ES:BX) --------
pm15:   cmp     ah, 0C0h
        je      .go
        jmp     far [cs:old15]
.go:    push    ds
        mov     ds, [cs:dsel]
        pop     word [c_ds]
        pushad
        mov     [R_eax], eax
        mov     [R_ebx], ebx
        mov     word [rmint], 15h
        xor     bx, bx
        mov     [R_ss], bx
        mov     [R_sp], bx
        mov     [R_fs], bx
        mov     [R_gs], bx
        mov     [R_fl], bx
        mov     bx, [rmseg]
        mov     [R_ds], bx
        mov     [R_es], bx
        call    callrm
        mov     bx, [R_es]              ; Segment -> Selektor
        mov     ax, 0002h
        int     31h
        mov     [c_es], ax
        popad
        mov     ax, [R_eax]
        mov     bx, [R_ebx]
        push    bp                      ; CF in die Flags auf dem Stapel
        mov     bp, sp
        and     word [bp + 6], 0FFFEh
        test    byte [R_fl], 1
        jz      .ok
        or      word [bp + 6], 1
.ok:    pop     bp
        mov     es, [c_es]
        push    word [c_ds]
        pop     ds
        iret

dta:    times   128 db 0
buf:    times   BUFSZ db 0
buf2:   times   264 db 0
res_end:

; ---- nicht resident: Start --------------------------------------------
init:   mov     [rmseg], cs
        mov     sp, stk_top
        mov     bx, (stk_top - start + 100h + 15) / 16
        mov     ah, 4Ah
        int     21h
        mov     si, 81h                 ; Programmname und Rest
.sp:    lodsb
        cmp     al, ' '
        je      .sp
        cmp     al, 13
        je      usage
        mov     di, prog
.nm:    stosb
        lodsb
        cmp     al, ' '
        je      .ne
        cmp     al, 13
        jne     .nm
.ne:    mov     byte [di], 0
        dec     si                      ; Rest als Befehlszeile
        mov     di, tail + 1
        xor     cx, cx
.t:     lodsb
        stosb
        cmp     al, 13
        je      .te
        inc     cx
        jmp     .t
.te:    mov     [tail], cl
        mov     ax, 352Fh
        int     21h
        mov     [old2f], bx
        mov     [old2f + 2], es
        mov     dx, rm2f
        mov     ax, 252Fh
        int     21h
        mov     [pb + 4], cs
        mov     [pb + 8], cs
        mov     [pb + 12], cs
        push    cs
        pop     es
        mov     dx, prog
        mov     bx, pb
        mov     ax, 4B00h
        int     21h
        pushf
        push    ax
        push    ds
        lds     dx, [old2f]
        mov     ax, 252Fh
        int     21h
        pop     ds
        pop     ax
        popf
        jc      .err
        mov     ah, 4Dh
        int     21h
        mov     ah, 4Ch
        int     21h
.err:   mov     dx, m_err
        mov     ah, 9
        int     21h
        mov     ax, 4C01h
        int     21h
usage:  mov     dx, m_use
        mov     ah, 9
        int     21h
        mov     ax, 4C01h
        int     21h

pb:     dw      0, tail, 0, 5Ch, 0, 6Ch, 0
m_use:  db      'WINLDR Programm [Argumente] - Windows-Kern auf dem DPMI', 13, 10
        db      'der DOS-Box starten (statt DOSX.EXE)', 13, 10, '$'
m_err:  db      'WINLDR: Programm nicht gefunden', 13, 10, '$'
prog:   times   128 db 0
tail:   times   130 db 0
        times   256 db 0
stk_top:
