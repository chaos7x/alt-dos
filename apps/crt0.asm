; Startcode fuer FAPI-Anwendungen (Ring 3)
; CS, DS, ES, SS und SP sind bereits vom Kernel ueber das TSS gesetzt.

cpu 286

segment _TEXT  public class=CODE align=1 use16
segment CONST  public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA  public class=DATA align=2 use16
segment _BSS   public class=BSS  align=2 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS

extern main_

segment _TEXT
..start:
global _start
_start:
        call    main_
        mov     ax, 3                   ; FAPI_EXIT
        call    0x23:0x0000
.hang:  jmp     .hang
