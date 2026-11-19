# Module für die DOS-Box (6.3) – Vorlage OS/2 2.0

Stand: 19.11.2026, nur Bestandsaufnahme, noch kein Entwurf. Umfang 6.3
(Entscheidung des Nutzers, 19.11.2026): die Nachbildung der Geräte in der
DOS-Box wird nach dem Vorbild der VDDs von OS/2 2.0 in Module zerlegt.
Anlass: `kernel/v86.c` füllt KTEXT3 mit 61.980 Byte fast ganz.

Quelle: Dateiliste von OS/2 2.0 („OS/2 File Name's / Directory /
Usage“, eprm.ardent-tool.com/eprmx/h12455.htm, vom Nutzer genannt).
Nur Dateinamen und Zweck – kein Code von OS/2 (nicht freigegeben).

## Aufbau bei OS/2 2.0

- `\OS2\DLL\DOS.SYS` – Treiber, der die DOS-Sitzungen startet (VDM-
  Verwaltung im Kernel).
- `\OS2\MDOS\DOSKRNL` – das DOS in der Box (läuft größtenteils im
  Protected Mode, nur ein kleiner Teil in V86). Dazu COMMAND.COM und die
  üblichen DOS-Werkzeuge (APPEND, ASSIGN, DEBUG, EDLIN, JOIN, SUBST, MEM,
  DOSKEY, QBASIC, GRAFTABL), ANSI.SYS, EGA.SYS, HIMEM.SYS, EMM386.SYS
  (für eine „bestimmte DOS-Version“, VMB).
- VMB: `VMDISK.EXE` (Diskettenabbild aus einer DOS-Startdiskette),
  `FSFILTER.SYS` (Zugriff auf die OS/2-Laufwerke aus dem echten DOS),
  `FSACCESS.EXE` (welche Laufwerke das echte DOS sieht).
- Je Gerät ein virtueller Treiber `V*.SYS` in `\OS2\MDOS`, geladen über
  `DEVICE=` in der CONFIG.SYS von OS/2.
- Für bestimmte Programme Korrekturen: `FFIX.EXE` (FindFirst/Touch),
  `SQ4FIX.COM` (Space Quest 4), `FIXWP.EXE` (WordPerfect).

## Die VDDs und ihr Gegenstück bei uns

| OS/2 2.0      | Aufgabe                         | AT-DOS heute                        |
|---------------|---------------------------------|-------------------------------------|
| VBIOS.SYS     | BIOS-Dienste, BDA               | v86.c: INT 10h/11h/12h/13h/15h/16h/1Ah, Stummel F000 |
| VPIC.SYS      | virtueller Interruptcontroller  | v86.c: vimr/visr, OCW3, inject      |
| VTIMER.SYS    | PIT, Takt, INT 8                | v86.c: pit_*, account, kpit_set     |
| VCMOS.SYS     | CMOS/Echtzeituhr                | v86.c: cmos_ov, Port 70h/71h        |
| VKBD.SYS      | Tastatur, Port 60h/64h          | v86.c: Scancode-Ring, kb_pump, bios_shift |
| VMOUSE.SYS    | INT 33h                         | v86.c: mou33, mou_cb, mou_pmcb      |
| VCOM.SYS      | serielle Ports                  | v86.c: com_reg (Register, Ausgabe über FAPI) |
| VLPT.SYS      | Drucker                         | v86.c: lpt_reg, lpt_out             |
| VDMA.SYS      | DMA-Controller                  | v86.c: Kanal 1/5 nur für die SB     |
| VNPX.SYS      | Koprozessor                     | Kernel: fpu_nm (287/387 je Task)    |
| VDSK.SYS      | Plattenzugriffe (INT 13h)       | v86.c: v86_disk (nur Startabbild)   |
| VFLPY.SYS     | Diskettenlaufwerke              | Redirector (A:/B: als Laufwerk)     |
| VCDROM.SYS    | CD in der Box (MSCDEX)          | redir.c: mscdex, CDS wie MSCDEX     |
| VEMM.SYS      | EMS 4.0                         | v86.c: INT 67h, ems_*               |
| VXMS.SYS      | XMS, HMA, UMB                   | v86.c: xms(), emb_*, umb_*          |
| VDPMI.SYS     | DPMI 0.9                        | v86.c: dp_*                         |
| VDPX.SYS      | DOS-Erweiterung für DPMI (INT 21h im PM) | v86.c: dp_reflect, Übersetzungen |
| VVGA.SYS      | VGA, Hintergrund-Grafik         | v86.c vga_*, vgagfx.c raw_*         |
| VSVGA.SYS     | SVGA-Zustand je Chip            | vgagfx.c raw_sv*, vesa.c (4F04h)    |
| VCGA/VEGA/VMONO/V8514A/VXGA.SYS | andere Karten | – (nur VGA und Nachfolger)       |
| VVIDEO.SYS    | der gerade benutzte Videotreiber | –                                  |
| VWIN.SYS      | WIN-OS/2                        | –                                   |
| VTOUCH.SYS    | Berührungsbildschirm            | –                                   |
| –             | Sound Blaster, AdLib, AWE, MPU-401 | v86.c: vsb, opl_*, emu_*, mpu_* (OS/2 2.0 hatte keinen Ton-VDD; MMPM/2 kam später) |
| COMDD/LPTDD.SYS | DOS-Treiber in der Box für COM/LPT | ATBOX.SYS (ATIN$/ATOUT$/ATERR$, EMMXXXX0) |

Auffällig: OS/2 hat auch PIC, Zeitgeber und CMOS als eigene Module
gebaut. Bei uns liegen gerade diese Teile im heißen Pfad jeder Falle
(inject, t8_ok, dp_hwint) – sie bleiben im Kernel, ausgelagert wird,
was selten oder groß ist.

## Erste Überlegung zur Aufteilung (noch nicht abgestimmt)

- Fest im Kernel (KTEXT3): Monitor, PIC, PIT, Tastatur, BIOS-Grundlage,
  Fensterumlegen – alles, was je Falle läuft.
- Als Module (`.VDD`, Ring 0 wie Treiber, Einhängen über Port- und
  Interrupt-Tabellen): Ton (SB/OPL/AWE/MPU), EMS, DPMI, VGA/SVGA-
  Hintergrund, COM/LPT, Maus, MSCDEX/Redirector.
- Laden über CONFIG.SYS (`DEVICE=` wie OS/2) oder DOSBOX.INI je Box –
  zu klären.
- Offen: Schnittstelle (Port-Haken, INT-Haken, Seitenfehler-Haken,
  Takt-Haken, Zustand je Box), Kosten eines Fernaufrufs je Falle,
  Reihenfolge der Auslagerung.
