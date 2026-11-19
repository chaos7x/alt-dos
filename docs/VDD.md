# Module für die DOS-Box (6.3) – Vorlage OS/2 2.0

Stand: 19.11.2026, Bestandsaufnahme, Abwägung und Entwurf (unten). Umfang 6.3
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

## PIC, Zeitgeber und CMOS als Module? (Abwägung, 19.11.2026)

Frage des Nutzers: „Zu PIC, Zeitgeber und CMOS in Modulen – da sollte
man schauen, was an Vorteilen überwiegt.“ Gemessen am Stand von 6.2
(wdis von `build/v86.o`, Größe je Funktion, KTEXT3 zusammen 61.980 Byte):

| Teil von v86.c                    | Code     |
|-----------------------------------|----------|
| DPMI (dp_*, ctx_*, rm_go)         | 9.133    |
| VGA/SVGA (vga_*, vbe_*, rom_*)    | 6.100    |
| XMS/UMB/HMA                       | 5.304    |
| EMS (ems_*, x57)                  | 5.076    |
| Ton (sb_*, opl_*, emu_*, mpu_*)   | 4.707    |
| Maus (mou*)                       | 2.165    |
| **Zeitgeber (pit_*, t8_ok, account, kpit_*, alrm)** | **1.758** |
| Tastatur                          | 805      |
| **PIC (inject, pic_*)**           | **776**  |
| COM/LPT                           | 764      |
| PCI-Dienste für Video-BIOSe       | 414      |
| **CMOS (cmos_*)**                 | **96** (dazu ~200 in port_in/port_out) |

Dafür (Module):
- Einheitlich wie OS/2; Ersatz denkbar (APIC/HPET in 7.0).
- Abschaltbar – aber eine Box ohne PIC oder Zeitgeber läuft nicht. Auch
  OS/2 lädt VPIC, VTIMER, VCMOS, VBIOS, VDMA, VKBD und VNPX nicht über
  die CONFIG.SYS, sondern immer („Basis-VDDs“).

Dagegen:
- Gewinn klein: zusammen unter 3 KB von 62 KB. Der Platz kommt von den
  großen Teilen (DPMI, VGA, XMS, EMS, Ton – zusammen über 30 KB).
- Heißer Pfad: `inject` läuft bei jedem Rücksprung in die Box, `t8_ok`/
  `account` bei jedem Takt, EOI an Port 20h kommt in DOOM ~700-mal je
  Sekunde. Ein Fernaufruf in ein Modul (CALL FAR mit DS-Wechsel, RETF)
  kostet auf dem 386 grob 40–60 Takte; eine Falle selbst einige hundert.
  Das wären 5–10 % mehr auf genau den Wegen, die wir für DOOM mühsam
  verkürzt haben (dp_more, sbc_*, pm_hk).
- Eng verzahnt: der Zeitgeber stellt den echten PIT des Kernels um
  (`kpit_set`, Zählung in start.asm `irq0`), die Takte der Box hängen an
  `irq0_n`; den PIC fragen Ton, Tastatur, Maus und Zeitgeber ab. Als
  Modul bräuchten sie eine breite Schnittstelle in beide Richtungen.
- CMOS ist mit ~0,3 KB zu klein, um einen eigenen Modulkopf (Treiber-
  rahmen, Registrierung, GDT-Plätze) zu rechtfertigen.

Ergebnis (Vorschlag): PIC, Zeitgeber und CMOS bleiben als Basis im
Kernel – wie die Basis-VDDs von OS/2 –, bekommen aber dieselbe
Schnittstelle, die Module benutzen (virtuellen IRQ melden, EOI-Rückruf,
Takt-Rückruf). Ausgelagert wird, was selten läuft und groß ist.

## Entwurf 6.3 (Stand 19.11.2026)

Ein Modul ist ein gewöhnlicher Ring-0-Treiber (`.VDD`, gebaut wie die
Treiber, Kopf „DRV1“), geladen mit `DEVICE=` in der CONFIG.SYS oder mit
DEVLOAD – kein eigener Lader. In INIT meldet es sich mit dem DevHlp
`DH_VDDREG` an (`include/vdd.h`): Name, welche Ports es will (Bereiche
oder „alle, die der Kernel nicht selbst behandelt“), welche Interrupts
(Vektoren, die sonst `svc` erledigt), welche Ereignisse (Box angelegt,
beendet, geparkt, vorn/hinten, Takt), dazu ferne Einsprünge (fent0–4
aus dcrt0.asm wie beim Netz). Der Monitor (v86.c) fragt bei einer Falle
erst seine eigenen Fälle, dann die Tabelle der Module; Aufruf fern mit
DS = Moduldaten, ES:BX = `struct vdd_call` (Task, Port/Vektor, Wert,
Breite, bei Interrupts die Register der Box zum Lesen und Schreiben).

Dienste für Module über DevHlp `DH_VDDSVC` (Unterfunktion in BX):
Speicher der Box lesen/schreiben (lineare Adresse, über das Fenster der
Box), FAPI im Namen der Box (wie `box_fapi`, Datei-/Gerätezugriff),
virtuellen IRQ melden, BDA lesen. Zustand je Box hält das Modul selbst
(Tabellen je Task, NTASK).

Reihenfolge:
1. Rahmen (`vdd.h`, DH_VDDREG/DH_VDDSVC, Tabelle im Kernel, Aufrufe aus
   port_in/port_out/svc/v86_destroy) mit COM/LPT als erstem Modul
   `VCOM.VDD` (Ports der BDA-Basen, INT 14h/17h, Schließen beim Ende) –
   klein, aber benutzt Ports, Interrupts, FAPI und Ereignisse.
2. Ton (`VSB.VDD`: SB/OPL/AWE/MPU, DMA-Kanäle 1/5, Takt-Rückruf zum
   Pumpen, IRQ melden, vorn/hinten) – braucht IRQ- und Takt-Dienste.
3. EMS und XMS (`VEMS.VDD`, `VXMS.VDD`) – brauchen Seitentabellen-
   Dienste (Seite einblenden/entfernen), Kosten genau messen.
4. VGA-Hintergrund und Maus. DPMI zuletzt (eng mit der Falle verwoben;
   evtl. besser ein eigenes Codesegment statt eines Moduls).

## Stand: Stufe 1 erledigt (19.11.2026)

- `include/vdd.h`: `struct vdd_reg` (Anmeldung), `struct vdd_call`
  (Aufruf: Port/Wert, Register bei Interrupts), `struct vdd_svc`
  (Dienste), DH_VDDREG 32, DH_VDDSVC 33, Ereignisse CREATE/DESTROY/PARK.
- `kernel/vdd.c` (KTEXT4): Tabelle `vt[NVDD]` (4 Module), `vdd_io` (nur
  wenn ein Modul VDDF_ANYPORT will, sonst kein Fernaufruf), `vdd_int`,
  `vdd_event`, `vdd_drop` (DEVLOAD /U, gescheitertes INIT), Dienste
  VS_RD (Box-Speicher lesen über `v86_xfer`, neu: Richtung wählbar –
  `v86_copy` schreibt) und VS_FAPI (FAPI im Namen der Box mit Kernel-
  sperre, Puffer auf dem Kernelstapel – die FAPI kann schlafen, andere
  Boxen rufen das Modul derweil auch).
- v86.c: unbekannte Ports -> `vdd_io`, INT 14h/17h -> `vdd_svc_int`
  (Register hin und zurück, ohne Modul „kein Port“), Ereignisse in
  v86_create/v86_destroy/v86_park.
- `VCOM.VDD` (drivers/vcom.c): COM1–4/LPT1–3 der Box wie vorher in
  v86.c (8250 ohne FIFO, Loopback, LPT-Latch mit STROBE, INT 14h/17h),
  Ausgabe über COMn/LPTn des Kernels. Mitgeliefert und in der
  CONFIG.SYS geladen (`DEVICE=VCOM.VDD`), entladbar mit `DEVLOAD /U
  VCOM`.
- KTEXT3 (v86.c) 61.980 -> 61.102 Byte; KTEXT4 32.682 Byte.
- Geprüft in QEMU: BIOSCMP (Box gegen FreeDOS) mit denselben 62
  Abweichungszeilen wie vorher (INT 14h/17h-Status gleich dem Bezug),
  Testprogramm PT.COM (INT 17h, INT 14h, LPT über Ports 378h/37Ah) mit
  `-parallel file:`/`-serial file:` – alles kommt an; nach `DEVLOAD /U
  VCOM` geht nichts mehr hinaus, die Box läuft weiter; erneut geladen.
  86Box 386DX/40 (4 MB, Textdrucker an LPT1): VCOM.VDD geladen, PT.COM in
  der Box druckt über INT 17h und über die Ports.
- Unbekannte Ports kosten jetzt einen Fernaufruf ins Modul; VCOM lehnt
  alles außerhalb 278h–3FFh sofort ab, ohne den BDA zu lesen (Joystick
  201h fragen Spiele in Schleifen ab).

## Stand: Stufe 2 erledigt – Ton als `VSB.VDD` (19.11.2026)

- Neue Dienste (`include/vdd.h`, Kernelseite `v86_vsvc` in v86.c, über
  vdd.c): `VS_IRQ` (virtuelle Leitung 2–7 der Box; der Monitor stellt
  sie in inject/dp_hwint zu wie seine eigenen, auch an PM-Handler;
  Leitungen ohne Handler ruhen – `vidle`, gesetzt in dp_trap, gelöscht
  durch einen Real-Mode-Handler oder DPMI 0205h; vorher `sb.noirq`),
  `VS_VECDEF` (Vektoren 08h–0Fh auf dem Stummel / im PM eingehängt),
  `VS_TICK` (Takt-Rückruf `tick_off`, höchstens einmal je Kerneltakt,
  2 = auch gleich beim nächsten Rücksprung), `VS_IOPERM` (Ports unter
  3E0h in der I/O-Bitmap der Box frei/gesperrt), `VS_FGRES` (Modul gab
  der Box Hardware: beim Wechsel nach hinten kommt `VDD_EV_BG`),
  `VS_AUD` (Audio-FAPI im Namen der Box, Puffer im DS des Moduls –
  `box_aud` nimmt jetzt den Selektor), `VS_HK` (DOSBOX.INI-Schalter),
  `VS_WR` (Box-Speicher schreiben). VS_RD/VS_WR nur unter 4 MB (eine
  Seitentabelle `pt_box`).
- Neue Ereignisse: `VDD_EV_BG`, `VDD_EV_RESUME` (nach dem Einfrieren,
  Pause nicht nachholen), `VDD_EV_PMHOOK` (0205h mit Vektor),
  `VDD_EV_PMEND`. Port-Aufrufe tragen in `flags` Bit 0 „Box vorn“;
  Wortzugriffe auf Ports über 3FFh gehen als `VDD_INW`/`VDD_OUTW` an die
  Module (EMU8000 verträgt keine Bytes), sonst wie bisher in Bytes.
- `VSB.VDD` (drivers/vsb.c): virtuelle SB bis DSP 4.05 mit DMA 1/5,
  Mixer, E2h, Aufnahme (Stille), DSP-MIDI; OPL an 388h–38Bh und
  220h–223h/228h/229h mit Durchgriff über die Bitmap und Registerkopie
  (jetzt 512 Byte je Box im Modul statt einer eigenen Seite), Teilen
  mit FAPI-FM-Programmen; MPU-401; EMU8000 durchgereicht. Code fast
  unverändert aus v86.c; DMA-Daten über VS_RD/VS_WR statt VWIN.
  `DEVICE=VSB.VDD` in der CONFIG.SYS, `DEVLOAD /U VSB` verstummt alle
  Boxen. VCOM liest den BDA jetzt höchstens einmal je Takt und Box
  (MIDI an 330h fiel sonst bei jedem Byte durch einen Lesezugriff).
- KTEXT3 (v86.c) 61.102 -> 56.350 Byte (−4,7 KB), DGROUP 61.370 Byte.
- Geprüft in QEMU (`-device sb16`, WAV-Mitschnitt): DOOM 1.9
  `-timedemo demo1` mit SB-Effekten – Lautstärkeverlauf wie der gute Lauf
  vor der Umstellung (leise Abschnitte, kein Dauerbrei); mit AdLib-Musik
  (`-device adlib`) spielt die Musik die ganze Demo durch. BIOSCMP
  unverändert (dieselben 31 Abweichungen).
  86Box DX4-100 (486SP3C, SB16, Start von Diskette): DOOM 1.9
  `-timedemo demo1` – Mitschnitt der Karte mit demselben Verlauf wie der
  Bezugslauf vor der Umstellung, Spiel läuft.
- Nächste Stufe: EMS/XMS (`VEMS.VDD`, `VXMS.VDD`) – braucht Dienste für
  Seitentabellen der Box.
