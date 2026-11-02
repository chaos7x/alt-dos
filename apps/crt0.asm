; SPDX-License-Identifier: GPL-2.0-or-later
; Startcode fuer FAPI-Anwendungen (Ring 3)
; CS, DS, ES, SS und SP sind bereits vom Kernel ueber das TSS gesetzt.
;
; Aufbau des Abbilds (FX-Version 2): Code (CGROUP) und Daten (DGROUP)
; sind getrennte Segmente, beide ab Offset 0 - gelinkt als DOS-EXE wie der
; Kernel, mkfx.py nimmt den Kopf ab. Der Kernel gibt jeder Instanz ein
; eigenes Datensegment (initialisierte Daten, BSS, Stack) und teilt das
; Codesegment zwischen Instanzen desselben Programms. crt0.o muss als
; erste Datei gelinkt werden (Reihenfolge der Segmente, Einsprung 0).
;
; Bei DS:0002 steht der Gate-Zeiger: alle FAPI-Aufrufe gehen ueber
; `call far [2]` (apps/fapi.h). Unter AT-DOS zeigt er auf das Call-Gate
; 0023:0000; unter MS-DOS traegt der gebundene Stub (stub/) dort seine
; Uebersetzungsschicht ein - so laeuft dieselbe .EXE auf beiden (wie die
; Family API von OS/2 mit BIND).

cpu 286

segment _TEXT  public class=CODE align=1 use16
segment BEGDATA public class=BEGDATA align=16 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
group CGROUP _TEXT
group DGROUP BEGDATA CONST CONST2 _DATA _BSS

extern main_

segment BEGDATA
        dw      0                       ; Offset 0 frei (Nullzeiger)
global _fapi_gate
_fapi_gate:
        dw      0x0000, 0x0023          ; Offset 2: Gate (Offset, Segment)

segment _TEXT
..start:
global _start
_start:
        call    main_
        xor     bx, bx                  ; Rueckgabewert 0 (main ist void;
        mov     ax, 3                   ; sonst DosExit(n)) - FAPI_EXIT
        call    far [0x0002]
.hang:  jmp     .hang
