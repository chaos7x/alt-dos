# alt-dos

Hobby-Betriebssystem aus einer alternativen Zeitlinie: Microsoft führt das
Multitasking-DOS 4.0 (1986) als offizielle DOS-Linie fort, statt auf OS/2 zu
setzen. Spezifikation der ganzen Zeitlinie (DOS 5.0–9.0, FAPI, NT-Linie):
https://claude.ai/code/artifact/43b28dce-1011-46c8-ad1a-e3a2255adff8

Sprache im Projekt: Deutsch (Kommentare, Commits, Doku).

Lizenz (Entscheidung des Nutzers): **GPL-2.0-or-later** für das ganze
Projekt, wie der FreeDOS-Kernel (`LICENSE`, SPDX-Zeile in jeder Quelldatei
– neue Dateien bekommen sie auch). Grund: Ableger sollen offen bleiben,
die Arbeit geht nicht verloren, wenn jemand anderes fortsetzt. Fremder
Code darf übernommen werden – Herkunft je Übernahme vermerken (Datei,
Projekt, Lizenz): FreeDOS (meist GPLv2+), 86Box (Lizenz je Datei prüfen),
Linux (GPLv2-only: nicht zusammen mit GPLv3-Code), GRUB (GPLv3+: macht das
Ganze zu v3 – vorher fragen). Wine (LGPL) als abgegrenztes Teilsystem
neben dem Kernel. Grundsatz bleibt: fremder Code passt selten zum
16-Bit-System – meist selbst schreiben, fremden Code als Nachschlagewerk.

Produktname in der Zeitlinie: **AT-DOS** (AT = Rechnerklasse und Alternative).

## Aktueller Stand: AT-DOS 6.0 in Arbeit (`main`), 5.0 fertig (Zweig `5.x`)

README aktuell halten (Wunsch des Nutzers): jede Neuerung gleich im
README („Was läuft“, FAPI-Tabelle, Meilenstein in der Überschrift)
nachtragen, im selben Commit.

Meilensteine (Zählung des Nutzers): 7 = Werkzeuge wie MS-DOS 5/6 und
Treiber; 8 = lange Namen komplett (Stufen 1–4, EDIT), Platten-Cache im
Kernel, ANSI, JOIN, CD auf andere Laufwerke, Stapelsprache komplett
(MS-DOS 6.22 + OS/2 CMD), Dateimanager FM, DISKCOMP/COMP/REPLACE/EXPAND;
9 = 5.0 fertig: Textwerkzeuge nach GNU (CUT, HEAD, TAIL, WC, UNIQ, TR),
287 je Task, CD ohne Medium „nicht bereit“, FAPI-Programme unter MS-DOS
(gebundener Stub), gemeinsame Codesegmente (FX-Version 2), Joliet, Ton
(SB16.SYS: digital SB 1.0–SB16/AWE, FM OPL2/OPL3, MPU-401, Mixer; PLAY,
MIXER), Lizenz GPL-2.0-or-later. Zweig `5.x` = Stand 5.0 für
Fehlerbehebungen (Tag `v5.0` konnte aus der Umgebung nicht gepusht
werden – HTTP 403 –, der Nutzer legt ihn in GitHub an).

**Plan 6.0** (vom Nutzer abgestimmt): Phase 0 Umstellung (erledigt, s. u.)
-> **NCR8XX.SYS** (PCI-SCSI 53C8xx, vom Nutzer vorgezogen: sein Standard
auf PCI-Maschinen, steckt in seiner 486) -> Phase 1 Speicher (E820h,
4-KB-Seiten, Paging 1:1; erledigt) -> Phase 2 präemptiv (erledigt) +
NCR8XX mit IRQ (erledigt)
-> Phase 3 DOS-Boxen (Stufe 1 V86-Grundlage erledigt; DOS darin: **FreeDOS-Kernel** mit INT-2Fh-
Redirector auf die AT-DOS-Laufwerke, vom Nutzer gewählt; später echtes
MS-DOS aus Abbildern) -> Phase 4 EMS/XMS/DPMI, virtuelle VGA und SB,
mehrere Boxen (Rest, abgestimmt: Platz im ersten Codesegment, XMS über
3 MB je Box, IDLE_SENSITIVITY, Grafik-Boxen im Hintergrund mit Schatten,
AdLib-Registerkopie der Box, Kleinkram nach Bedarf) -> Phase 5 zuerst
Fenster statt voller Spiegelung (Kernel-Alias reicht nur für 2 GB) mit
Speicher bis ~3,5 GB (Wunsch des Nutzers: AT-DOS auf einer 4-GB-VM; heute
startet es dort schon und nutzt 1 GB), gleich danach VESA mit linearem
Bildspeicher (braucht denselben Umbau), dann FAT32 + VFAT-Schreiben,
IDE-Busmaster, APM -> Phase 6 (Entscheidung des Nutzers: Rest von Phase 2 ans Ende)
Sperren je Gerät statt der einen Kernelsperre (DMABUF, `req`,
SCRATCH-Fenster und statische FAPI-Puffer je Task/Gerät), echt parallele
Plattenzugriffe auf verschiedenen Laufwerken; danach nach `5.x` zurück.

Testmaschinen 6.0 (Entscheidung des Nutzers, ab Phase 3 – „Hauptsache
stabil und ressourcenschonend“): **Hauptmaschine die 486 in 86Box mit
8–16 MB**; **Untergrenze 386 (SX oder DX) mit 4 MB** – dort muss alles
laufen, auch zwei DOS-Boxen; **386SX mit 2 MB nur noch Rauchtest**
(starten, FAPI-Programme, Kopieren, CHKDSK, eine Box), Platz und Tempo
werden nicht mehr darauf zugeschnitten; Obergrenze der P3B-F. Grund:
eine Box kostet ~660 KB (mit FreeDOS mehr), jeder Trap und jeder TLB-
Verlust beim Taskwechsel (CR3 je Task) kostet auf dem SX spürbar;
Vorbilder der Zeit (Windows 3.1 erweitert, OS/2 2.0) brauchten real
4–8 MB. Der Kernel bleibt trotzdem schlank (Leitlinie 6.0).

Diskettenabbild heißt seit 6.0 `build/atdos-fd.img` (vorher `dos5.img`,
Wunsch des Nutzers), OEM-Kennung `ALTDOS60`.

Regeln auf `main` (6.0); für `5.x` gelten weiter die 5.0-Regeln (nur
286-Befehle, kooperativ):

- **Kernel, Treiber, IFS: 386-Befehle** (`cpu 386`, Watcom `-3`,
  build.sh `KCFLAGS`), keine 486-Befehle (BSWAP, XADD, CMPXCHG, INVD,
  WBINVD, INVLPG). Code bleibt 16 Bit und segmentiert; 32-Bit-Register
  und -Portzugriffe (PCI CF8h/CFCh) sind erlaubt, 32-Bit-Code nur wo die
  CPU ihn erzwingt (V86-Eintritt/-Austritt), als kleine Stücke.
- **Programme bleiben 286** (`-2`, `CFLAGS`, crt0 `cpu 286`): FAPI wie in
  5.0, sie laufen unverändert auch unter 5.x.
- `start.asm` prüft ganz am Anfang mit 8086-Befehlen auf einen 386
  (Flags-Bits 12–15) und hält sonst mit Meldung an.
- Version: Titel/VER/MSD „AT-DOS 6.0“, `FAPI_VERSION` 0600h.
- Diskettenbootsektor liest so viele Sektoren, wie `ATDOS.SYS` groß ist
  (Dateigröße aus dem Verzeichniseintrag; vorher fest 160 = 80 KB) –
  `ATDOS.SYS` wird nicht mehr aufgefüllt, Grenze in build.sh 448 KB
  (Laden ab 10000h, darüber Real-Mode-Reste/EBDA).
- **Präemptiv seit Phase 2 Schritt 1** (nur aus Ring 3, siehe unten), **FAPI einziger Weg zur
  Hardware**, **IFS** wie bisher.

Was zu einer späteren Version gehört (präemptiv, Paging/Swap, V86, Journaling-FS,
Win32s), nicht vorziehen, sondern nachfragen.

## Aufbau

- `boot/` – `boot.asm`: Diskette, Standard-FAT12-BPB, lädt `ATDOS.SYS` (erste
  Datei, zusammenhängend ab Cluster 2, auf 64 KB aufgefüllt) nach 1000:0000;
  `hdboot.asm`: FAT16-Partition, sucht `ATDOS.SYS` und folgt der FAT-Kette
  (CHS per INT 13h/08h-Geometrie); `mbr.asm`: MBR nur für `atdos-hd.img`
- `kernel/preload.c` – Start von Platte: liest im **Real Mode** per BIOS
  `CONFIG.SYS` und die `DEVICE=`-Dateien nach 0600h–7BFFh; das
  Pseudo-Dateisystem `BOOT` stellt sie während `CONFIG.SYS` als `Z:` bereit.
  Danach findet `find_boot_vol()` die Startpartition (Startsektor +
  Seriennummer aus dem BPB bei 0000:7C00) und setzt `sys_drive`; `vol_move()` schiebt sie danach auf C: (übrige rücken nach, nur beim Start, keine offenen Dateien).
  Code in `preload()` darf keine Selektoren und kein `vio` benutzen.
  `preload()` sucht im Stamm, in `\DOS\SYSTEM`, dann in `\DOS`.
- Ablage (Entscheidung des Nutzers, wie OS/2): Stamm nur `ATDOS.SYS`,
  `CMD.EXE`, `CONFIG.SYS`, `AUTOEXEC.CMD` (+ `ATSYS.COM` auf der
  Diskette); `\DOS` Werkzeuge, die man aufruft; `\DOS\SYSTEM` was das System
  nachlädt (Treiber, IFS, `U*.EXE` für CHKDSK, `.KBL`); `\DEMO` TASKA–C.
  `build.sh`: `FILES`, `DOSFILES`, `SYSFILES`, `DEMOFILES`. `load_driver()`
  und `KEYB` fallen bei reinem Dateinamen auf `\DOS\SYSTEM`, dann `\DOS`
  zurück; CHKDSK sucht `U*.EXE` fest in `\DOS\SYSTEM` des Startlaufwerks,
  erst danach im PATH. Standard-PATH `X:\DOS;X:\`. `ATSYS` kopiert die
  Unterverzeichnisse aus der Tabelle `subdirs` (Eltern zuerst).
- `tools/atsys.asm` – `ATSYS.COM` für MS-DOS (wie SYS); `tools/sysboot.py`
  macht dasselbe für Abbilder beim Bauen
- `kernel/` – `start.asm` (Einstieg, Gate, ISRs, CPU-Hilfsroutinen), `kernel.c`,
  `mem.c` (physische Speicherverwaltung, Bitmap in 4-KB-Seiten),
  `floppy.c` (Diskettentreiber), `fat.c` (FAT12/16 und IFS-Verteiler),
  `vio.c` (Sitzungen, Konsole), `kbd.c` (Tastatur), `ifs.h`, `kern.h`
  (gemeinsame Definitionen). Blockgeräte, Treiberlader, DevHlp, CONFIG.SYS
  und Partitionen stehen in `kernel.c`.
- `drivers/` – Gerätetreiber (Ring 0): `dcrt0.asm` (Kopf, Einsprung,
  DevHlp-Aufruf), `drvlib.h`, `aha154x.c`, `ide.c`; gebaut mit `-zu`, weil
  SS (Kernelstack) ungleich DS ist – keine Adressen lokaler Variablen
  weitergeben, Puffer `static` anlegen
- `include/drv.h` – Schnittstelle Kernel ↔ Treiber (Anforderung, DevHlp)
- `ifs/` – nachladbare Dateisysteme (Ring 0, gebaut wie Treiber mit `-zu`),
  `iso9660.c` -> `ISO9660.IFS`; Schnittstelle `include/ifsmod.h`
  (`struct ifs_file`, `ifs_dirent`, `ifs_req`). Im Kernel überbrückt
  `mod_driver` (kernel.c) die `struct ifs_driver`-Aufrufe zum Modul;
  `cd_drives()` gibt CD-Einheiten (bsize 2048) nach `CONFIG.SYS` Buchstaben.
- `CONFIG.SYS` – `DEVICE=` lädt Treiber, `IFS=` Dateisystem-Module (beides
  wird beim Plattenstart vorgeladen)
- `apps/task_cmd.c` – `CMD.EXE`, der Befehlsinterpreter (bis 6.0 hieß er
  `COMMAND.EXE`; umbenannt wie bei OS/2 und NT – Wunsch des Nutzers –, damit
  er nicht mit `COMMAND.COM` der DOS-Box verwechselt wird; auch auf `5.x`;
  steht in einer alten CONFIG.SYS noch `PROTSHELL=COMMAND.EXE`, nimmt der
  Kernel `CMD.EXE /P`), mit
  Programmsuche über `PATH`, Stapelverarbeitung (Datei je Stufe in einem
  `DosAllocSeg`-Segment, bis 4 Stufen) und `AUTOEXEC.BAT`. Umgebung je Task
  im Kernel (`env_*` in kernel.c, ein Kernelsegment für alle Tasks plus
  Systemumgebung)
- `AUTOEXEC.CMD` – Startdatei des nativen Befehlsinterpreters (`PROTSHELL=`
  in CONFIG.SYS, Vorgabe `CMD.EXE /P`). Trennung wie OS/2: `.CMD` und
  `PROTSHELL=` nativ; `AUTOEXEC.BAT`, `.BAT` und `SHELL=` gehören ab 6.0 der
  DOS-Box (V86) – AT-DOS 5.0 rührt `AUTOEXEC.BAT` nicht an.
- `apps/` – Ring-3-Anwendungen, `crt0.asm`, `fapi.h`; `util.c`/`util.h`
  gemeinsame Hilfen der neueren Werkzeuge (Zeichenketten, Zahlen,
  Platzhalter, `abs_path`, `copy_file`, `walk_tree` – Baum ohne Rekursion
  im 2-KB-Stack, Rückruf mit WT_DESCEND/WT_REMOVED). In `build.sh` in
  `UTILAPPS` eintragen; util.c setzt `FAPI_NO_IMPL` (fapi.h definiert
  sonst `puts` doppelt). Ältere Programme haben eigene Kopien.
- `tools/` – `stub.asm` (DOS-Stub), `mkfx.py` (Abbild + Map -> FX-`.EXE`),
  `mkkbl.py` (alle Tastaturbelegungen: `.KBL`-Dateien und `kbd_gr.h` für den
  Kernel – Belegungen nur dort ändern)
- `include/fx.h` – Programmformat: MZ-Stub, `e_lfanew` -> FX-Kopf
- `include/fapi_abi.h` – FAPI-Funktionsnummern, gemeinsam für Kernel und Apps

## Konventionen

- Programme (FX-Version 2): Code (CGROUP) und Daten (DGROUP) getrennt, je
  bis 64 KB, beide ab Offset 0 – gelinkt als DOS-EXE (`ALINK` in build.sh,
  `format dos` wie der Kernel), `mkfx.py` nimmt den Kopf ab (bricht bei
  Segment-Relokationen ab), liest Code-/Datengröße aus der Map und füllt
  den FX-Kopf (18 Byte, `code_size`) bis zur Paragraphengrenze auf (der
  DOS-Stub nimmt das Abbild an Ort und Stelle!). Kernel `load_image`: Code
  einmal laden und teilen, wenn ein laufender Task dasselbe Programm hat
  (`path_key` aus Laufwerk + kanonischem Pfad, dazu Dateigröße), jede
  Instanz eigenes Datensegment (Daten, BSS, Stack); `task_end` gibt den
  Code erst mit dem letzten Benutzer frei. In QEMU geprüft: jede weitere
  COMMAND-Sitzung kostet 42 statt 62 KB. Treiber/IFS bleiben Version 1
  (ein Segment, `format raw`). crt0: BEGDATA zuerst in DGROUP (Gate-Zeiger
  bei DS:2), _TEXT zuerst im Code (Einsprung 0).
- Der Kernel hat getrennte Code- und Datensegmente wie ein Small-Modell-
  Programm: CGROUP (`_TEXT`) ab linear 10000h, DGROUP (CONST.._BSS) ab dem
  nächsten Paragraphen dahinter (`_DSTART` align 16), je bis 64 KB.
  `start.asm` setzt DS = CS + Codegröße, merkt sie in `kdata_seg`;
  `KDATA_LIN`/`LIN()` in `kern.h` rechnen damit, KCODE/KDATA zeigen dorthin.
  Gelinkt als **DOS-EXE** (`format dos`), weil `format raw` Offsets linear
  statt gruppenrelativ vergibt; `build.sh` schneidet den Kopf ab und bricht
  ab, wenn Segment-Relokationen auftauchen (kein `seg`-Bezug im Kernel!)
  oder DGROUP nicht direkt hinter dem Code liegt. Kein Code darf Daten
  über CS lesen oder Codeadressen als Datenoffsets benutzen.
  Die Speicherverwaltung beginnt direkt hinter den Kerneldaten
  (`mem_init(low, ...)`), MEM zeigt die echte Kernelgröße.
- Jeder Task hat eine eigene LDT: Eintrag 0 Code, 1 Daten/Stack, ab 2 per
  `DosAllocSeg`. Anwendungen sehen keine GDT-Segmente außer dem FAPI-Gate.
  Der Kernel greift über das GDT-Fenster `SCRATCH` (30h) auf beliebigen
  Speicher zu (zweites Fenster `SCRATCH2`, 40h), Laufwerke lesen per DMA in
  `DMABUF` (38h, linear 8000h, 2 KB für CD-Blöcke; Platten nutzen 512).
- Blockgeräte haben eine Blockgröße (`blk[].bsize`, Treiber melden sie in
  `drv_req.bsize`): 512 für Platten/Diskette, 2048 für CD-ROMs. FAT und
  Partitionen nur auf 512er-Geräten, CDs nur lesend.
- Kernelstack je Task 768 Worte: FAPI -> IFS-Modul -> DevHlp -> Treiber
  schachtelt tief. Die Kernelstacks bleiben in DGROUP (Kernel-C setzt
  SS = DS voraus). Seit der Trennung: Code ~33 KB, Daten ~27 KB, je 64 KB.
- Was mit der Konfiguration wächst, gehört nicht nach DGROUP, sondern in
  eigene Kernelsegmente: `kseg_alloc(bytes)` -> Selektor (GDT-Plätze
  `KSEG_IDX`, `NKSEG`), Zugriff über `__far`-Zeiger. Beispiel: Dateitabelle
  `files` (Größe `FILES=`). `KSEG_IDX(0)` ist fest die Belegungs-Bitmap der
  Speicherverwaltung bei linear 9000h–AFFFh (`MEMMAP_SEL`).
  `struct ifs_file` wird überall als `__far`-Zeiger übergeben.
- Kernel ab 10000h (Code, dann Daten); `ATDOS.SYS` (Code + initialisierte Daten) darf
  höchstens 80 KB groß sein (Diskettenbootsektor lädt 160 Sektoren nach
  1000:0000–23FFF; vorher 128 bzw. 96 – Disketten mit altem Bootsektor
  vor dem Überschreiten von 64 KB mit SYS erneuern), BSS dahinter
  nullt `start.asm` selbst.
- Zweites Kernel-Codesegment KCODE2 (Selektor 1C8h, `KCODE2_IDX` am Ende
  der GDT): selten gebrauchte, abgeschlossene Teile werden mit
  `wcc -nt=KTEXT2` übersetzt (build.sh `K2MODS`, heute `floppy`, `vgagfx`, `bus`, `mem`, `paging`, `v86`, `redir`,
  `k2misc` – Echtzeituhr `rtc_stamp`/`rtc_set` und `rom_str` für MSD; erstes
  Segment damit 62,0 KB).
  Ihre Einsprünge sind `__far`; sie selbst erreichen Kernelhilfen des
  ersten Segments nur über Fern-Thunks (`kernel/k2thunk.c`, umgelenkt per
  `#define` in `kernel/k2.h` – k2.h als letztes Include!), Ports und
  CLI/STI inline. Gleiche Daten (DS). start.asm: Segmente KTEXT2/KTEXT2END
  in Gruppe CGROUP2 hinter CGROUP, DS = CS + beide Größen. build.sh setzt
  die Segmentwerte der Fernaufrufe fest auf KCODE (08h) bzw. KCODE2 ein
  (Kernel liegt immer bei 10000h) und bricht bei anderen Relokationen ab.
  Nahe Aufrufe aus KTEXT2 ins erste Segment fängt der Linker nicht immer
  ab – neue Hilfen immer über k2.h. Grafik aus vio.c steht in
  `kernel/vgagfx.c`, gemeinsame Sitzungsdaten in `kernel/vioint.h`.
  Drittes Codesegment KCODE3 (Selektor 218h, `KCODE3_IDX` = letzter
  GDT-Eintrag): `v86.c` und `redir.c` (build.sh `K3MODS`, `-nt=KTEXT3`,
  Gruppe CGROUP3 hinter CGROUP2, DS = CS + alle drei Größen; start.asm
  ruft v86_exc/dpmi_trap/v86_tick über KCODE3). Vorher prüfen, dass ein
  verschobenes Modul nah nur eigene Funktionen ruft (dmpobj: „Self“-
  Fixups auf globale EXTDEFs). Stand danach: Code 65.463 + 13.823 +
  44.510 Byte – das erste Segment bleibt voll, KTEXT2 hat wieder ~50 KB
  frei (Ziel für Seltenes aus kernel.c), KTEXT3 ~20 KB für die Boxen.
  Stand vorher: Code 48 + 8 KB (vorher 56 KB in einem Segment). `hdboot.asm` folgt der FAT-Kette, ATSYS/SYS
  kopieren in Stücken – beide ohne feste Grenze.
- Bildschirm nur über Sitzungen (`vio.c`): Zeile 0 Titel, 1–23 Sitzung,
  24 Status (bei 43/50 Zeilen entsprechend tiefer). Programme schreiben in
  den Puffer ihrer Sitzung (Puffer immer 50 Zeilen groß). `VioSetMode(1)`
  (Entscheidung des Nutzers, wie MS-DOS EDIT/OS/2-Vollbild): Sitzung hat
  alle Bildschirmzeilen, Titel- und Statusleiste
  fehlen, solange sie im Vordergrund ist; der normale Inhalt wird in eigenem
  Speicher gesichert und bei `VioSetMode(0)` oder Programmende
  (`vio_task_end` in `task_end`) wiederhergestellt. `ses[].top` (1/0) und
  `rows` statt fester Konstanten benutzen.
- Logischer Bildschirmpuffer wie OS/2 1.x (FAPI 3Dh/3Eh VioGetBuf/
  VioShowBuf): LDT-Alias auf den Sitzungspuffer (`ses[].unit`), in
  `ldt_alias[t]` vermerkt – `seg_free` gibt dann nur den Eintrag frei,
  nicht den Speicher (die reservierten Deskriptorbytes sind beim 386 Basis/
  Grenze, daher eigene Bitmaske). Schreiben in den Puffer markiert nichts;
  VioShowBuf(von, bis) markiert fertige Zeilen (nur im Vordergrund, sonst
  zeichnet redraw beim Wechsel alles). EDIT leitet VioWrtStrAtt auf
  `lvb_str` um und zeigt in `getkey_m` vor dem Warten – ein FAPI-Aufruf je
  Bild, nie halbfertige Bilder (Anlass: Flackern auf dem SCAT; vom Nutzer
  auf dem SCAT bestätigt: Öffnen-Dialog mit Maus läuft ruhig). Grundsatz
  (Wunsch des Nutzers): wo man sparen kann, nutzen; hat OS/2 für ein
  Problem eine bewährte Lösung, das gleich sagen.
- VGA-Grafik, Stufe 1 (Modus 13h), in QEMU geprüft: FAPI 3Fh–41h
  VioSetGfx/VioGetPhysBuf/VioSetPal. Register aus den IBM-Standardtabellen
  (`vga_mode13`, `vga_mode03`, `vga_regs`), kein BIOS. Anders als OS/2 1.x
  (VioSavRedrawWait: Programm muss selbst sichern/neu zeichnen) sichert der
  Kernel beim Wechsel (`gfx_out` in vio_focus vor dem fg-Wechsel) 64 KB
  Bild + 768 Byte Palette in `gfx_save` und biegt den LDT-Alias aus
  VioGetPhysBuf mit `ldt_set` in den Puffer um; `gfx_in` (in redraw)
  stellt Karte, Bild, Palette und Selektor wieder her. Zurück auf Text:
  `hw_text` (Register 03h, Text-DAC 0–63 von der ersten Umschaltung,
  `hw_lines = 0` -> set_hw_lines lädt den Zeichensatz neu, 13h überschreibt
  Ebene 2). Textausgaben einer Grafiksitzung landen im Textpuffer.
  Programmende/VioSetGfx(0): `ldt_drop`. Demo `\DEMO\GRAFIK.EXE` (zeichnet
  im Hintergrund weiter). In 86Box vom Nutzer bestätigt (Grafik und
  Sitzungswechsel; ET4000 und Diamond SpeedStar 24X).
  Stufe 2, Modus 12h (640×480×16), in QEMU geprüft: vier Ebenen zu 38400
  Byte, FAPI 42h VioSetPlane(p) = Map Mask (SEQ 2) und Read Map (GC 4) im
  Vordergrund, im Hintergrund biegt `gfx_alias` den Selektor auf die Ebene
  im Puffer (`save_part`, ohne 32-Bit-Multiplikation). Attributregister
  0–15 direkt durchgeschaltet (Farbe n = DAC n), daher VioSetPal in beiden
  Modi gleich. Sichern/Zurückladen Ebene für Ebene (`gfx_copy`). Demo
  `GRAFIK 12` (16 Balken, wachsender Balken wechselt je Runde die Farbe –
  im Hintergrund weitergezeichnet geprüft). In 86Box vom Nutzer bestätigt
  (Farbbalken und Sitzungswechsel).
  Maus in Grafik, in QEMU geprüft: `MouReadEvent(&ev, MOU_XY)` liefert
  zusätzlich x/y in Bildpunkten (Mickeys 1:1 bei 12h, halbiert bei 13h).
  Zeiger (Pfeil 10×16, weiß/schwarz) zeichnet der Kernel in vio_flush
  (`gptr_update`) direkt in den Bildspeicher; 12h über Bitmaske GC 8 und
  Latches, danach Map Mask/Read Map auf die Ebene des Programms zurück.
  Beim Entfernen kommt je Punkt nur zurück, was noch genau wie der Zeiger
  aussieht (Programm darf jederzeit selbst malen). `gfx_out` entfernt ihn
  vor dem Sichern. Gegen Flackern (vom Nutzer auf dem SCAT gemeldet):
  Entfernen und neu Zeichnen in einem Durchgang über beide Flächen
  (`gcomb`, zwei Zustände `gst[2]` im Wechsel, jeder Punkt höchstens
  einmal geschrieben; ohne Überlappung erst neu, dann alt). Mode X: beim
  Seitenwechsel erst auf die neue Seite malen, umschalten, dann die alte
  aufräumen. Alle 50 ms ohne Bewegung derselbe Durchgang an derselben
  Stelle: übermalte Zeigerpunkte werden neuer Hintergrund, der Zeiger
  kommt zurück. Danach flackerte es auf dem SCAT weiter unten im Bild
  (Strahl kreuzt den halb gezeichneten Zeiger): gezeichnet wird jetzt im
  Strahlrücklauf (3DAh Bit 3) – im Leerlauf des Schedulers (`vio_idle`
  vor HLT) wartet der Kernel auf dessen Beginn (höchstens 2 Takte), aus
  vio_flush nur, wenn gerade Rücklauf ist oder die Änderung 30 ms wartet. Demo: GRAFIK malt mit links, rechts wechselt die Farbe.
  Stufe 3, Mode X (`VIO_MODEX` = 58h, 320×240×256), in QEMU geprüft:
  Register aus 13h ohne Chain-4, CRTC im Bytebetrieb, 480 Rasterzeilen
  (`vga_modex`); Speicher wie 12h (vier Ebenen zu 38400 = zwei Seiten zu
  19200, `PLANAR()`), daher gleiche Sicherung und VioSetPlane. FAPI 45h
  VioSetPage(0/1): CRTC 0Ch/0Dh, wartet vorn auf den Strahlrücklauf
  (Frist 2 Takte über get_ticks), Zeiger zieht mit (`gp_page`). Demo
  `GRAFIK X` (Farbverlauf + Quadrat, je Bild verdeckt gezeichnet, Klick
  setzt das Quadrat). In 86Box vom Nutzer bestätigt (Bild und
  Sitzungswechsel). Mauszeiger nimmt in 256-Farben-Modi die Farben 15
  und 0 der Palette (wie der Microsoft-Treiber).
- Zeilenzahl je Sitzung (`MODE CON LINES=25|43|50`, FAPI VioSetLines/
  VioGetRows): `ses[].lines`, Puffer immer für 50 Zeilen (`SLINES_MAX`,
  `SBYTES` 8000), normal `lines - 2` Zeilen, Vollbild alle, Statuszeile
  `hw_lines - 1`. `redraw()` stellt die Karte bei Bedarf um
  (`set_hw_lines`: 400 Rasterzeilen + 8x16 für 25, + 8x8 für 50; 350 +
  8x8 für 43; Sync-Polarität im Misc-Register, CRTC 06h–16h, Zeichenhöhe
  09h, Cursor 0Ah/0Bh) und lädt den Zeichensatz in Ebene 2 (`load_font`).
  Nur VGA: `start.asm` fragt im Real Mode INT 10h 1A00h (VGA?) und 1130h
  (ROM-Zeichensätze 8x8 in zwei Hälften, 8x16) ab -> `vga_ok`,
  `vga_font*`. Geänderte Zeilen als Bereich `dlo..dhi` statt 32-Bit-Maske.
  `VIDEO`-Deskriptor reicht für 50 Zeilen. Programme fragen die Zeilen mit
  `VioGetRows()` bzw. dem Ergebnis von `VioSetMode(1)` ab, nie fest 23/25.
  Neue Sitzungen (START) erben die Zeilenzahl der startenden Sitzung
  (Wunsch des Nutzers).
  In QEMU geprüft (50, 43, EDIT mit 43, Sitzungswechsel 50 <-> 25),
  vom Nutzer auf dem SCAT mit ET4000 bestätigt. EGA (43 Zeilen, andere Register) nicht
  unterstützt.
- Dateimanager `FM.EXE` (`apps/task_fm.c`, Wunsch des Nutzers „so etwas wie
  der Norton Commander“ – eigener Entwurf, kein Nachbau; in QEMU geprüft):
  Ansicht (Wunsch des Nutzers „so wie beim echten NC“): Fenster ab Zeile 0,
  Pfad mittig im oberen Rahmen (aktiv: cyan hinterlegt), Spalten Name
  (8.3 als „NAME     EXT“, lange Namen gekürzt mit »), Größe (ohne
  Punkte, Verzeichnisse ►VERZ◄/►HOCH◄), Datum, Zeit mit gelben Köpfen und
  ╤/│/┴-Strichen, Kurzanzeige unter ╟──╢, Befehlszeile R-2, Tastenleiste
  (Ziffer weiß auf schwarz, Text schwarz auf cyan, 6 Zeichen).
  Vollbild über VioGetBuf/VioShowBuf wie EDIT, zwei Fenster (`struct
  panel`, je 350 Einträge + 8-KB-Namenspool, Shellsort, Verzeichnisse
  zuerst, versteckte/System nicht gezeigt), Befehlszeile und Programme
  über `COMSPEC /C` im Verzeichnis des aktiven Fensters (VioSetMode(0)
  davor, danach VioSetMode(1), MouOpen neu, alles neu einlesen), CD in der
  Befehlszeile wechselt das Fenster. Kopieren/Löschen von Bäumen über
  `walk_tree` (`copy_visit`/`del_visit`), Verschieben auf demselben
  Laufwerk per DosMove, sonst kopieren + löschen. Alt+F1/F2: der
  Tastaturtreiber liefert Alt+F-Taste als normalen Scancode (nicht 68h/
  69h wie das BIOS) -> `KbdShift()` prüfen. Zahlen ohne 32-Bit-Division
  (`fmt_num` über Zehnerpotenzen).
  Betrachter F3 (Wunsch des Nutzers: wie mcview, nur Verhalten nach-
  gebaut): 32-KB-Fenster in einem DosAllocSeg-Segment, `vb(off)` lädt
  nach. Lehre: DosRead liest nur ins eigene DS (der Puffer ist nur ein
  Offset) – in fremde Segmente über einen DS-Puffer kopieren, sonst
  überschreibt der Kernel Programmdaten. Text mit Umbruch (`vnext`/`vprev`,
  CR LF nach genau 80 Zeichen ist kein Umbruch) oder waagerecht blättern,
  Hex (16 Byte je Zeile), Suchen (Text ohne Groß/klein, Hex-Bytes) mit
  Hervorhebung, Gehe zu Zeile/0x-Offset, Prozent ohne 32-Bit-Division.
  Dialoge mit Mausknöpfen (`buttons`/`button_hit`); `box()` hat einen
  eigenen Puffer, weil der Text des Dialogs oft in `line` steht (Fehler
  vom Nutzer gemeldet). Vom Nutzer in 86Box bestätigt: Kopieren und
  Verschieben, CHKDSK danach ohne Fehler.
  Abbrechen (Wunsch des Nutzers): `abort_check` (Esc oder Klick auf den
  Knopf im Fortschrittsfenster, Rückfrage) zwischen Dateien und über den
  neuen util-Rückruf `copy_abort` auch zwischen den 4-KB-Blöcken von
  copy_file (gibt -8, das Ziel wird gelöscht); `op_abort` statt op_err,
  Verschieben löscht die Quelle nur, wenn alles ankam. Vom Nutzer in 86Box
  bestätigt: Abbruch mitten in NWIND.MDB (Access) – die halbe Datei ist
  weg, vorher Kopiertes bleibt.
- DISKCOMP, COMP, REPLACE, EXPAND (in QEMU geprüft): DISKCOMP wie
  DISKCOPY (Quelle in 64-KB-Segmente, mehrere Durchgänge, ein Laufwerk
  mit Wechsel; Seriennummer 27h–2Ah im Bootsektor wird übergangen), COMP
  (Muster auf Datei 2 übertragen, `apply_pat`), REPLACE (/S über
  walk_tree im Ziel), EXPAND (SZDD: Kopf `SZDD 88 F0 27 33`, 'A', letztes
  Zeichen, Größe; LZSS 4 KB, Fenster mit Leerzeichen, Start 4096-16;
  getestet mit einer in Python gepackten Datei mit Rückverweisen; bei
  mehreren Quellen erst zählen, dann prüfen, ob das Ziel ein Verzeichnis
  ist). COMPRESS (Wunsch des Nutzers): eigener LZSS-Packer, Hash der
  3-Byte-Anfänge mit Ketten (48 Schritte), Treffer höchstens so lang wie
  der Abstand (sonst läse der Entpacker gerade Geschriebenes); /R ->
  letztes Zeichen '_'; Größe wird am Ende per DosSeek in den Kopf
  geschrieben. Geprüft: Hin und zurück mit EXPAND + COMP, und ein
  unabhängiger Python-Entpacker liefert das Original.
- CUT (Wunsch des Nutzers, „wie unter Linux“; in QEMU gegen GNU cut auf
  dem Host verglichen): -c/-b/-f, -d, -s, --complement,
  --output-delimiter=, Listen N/N-M/N-/-M; Zeilen bis 4 KB (länger wird
  gekürzt), CR LF bleibt erhalten, ^Z beendet. Testfalle q.py: `|` ist
  auf der deutschen Tastatur AltGr+< (`alt_r-less`).
- HEAD, TAIL, WC, UNIQ, TR (Wunsch des Nutzers, GNU-Vorbild; in QEMU mit
  den GNU-Werkzeugen auf dem Host verglichen – gleich bis auf CR LF und
  die Spaltenbreite von WC): gemeinsamer gepufferter Ein-/Ausgabeteil
  `apps/textin.h` (per `#include`, je 2 KB; `ti_num`/`ti_file` nur mit
  `TI_NUM`/`TI_FILE`, sonst W202). TAIL liest durch einen 64-KB-Ring
  (DosAllocSeg). Lehre bei `tail -f`: die Dateigröße steht je offenem
  Handle (`struct ifs_file`), Angehängtes eines anderen Tasks sieht ein
  offenes Handle nicht – TAIL öffnet daher alle 500 ms neu und springt
  per DosSeek an die alte Stelle (wie `tail -F`).
- Stapelsprache (Wunsch des Nutzers: komplett, MS-DOS 6.22 + OS/2 CMD;
  in QEMU geprüft): `cmd_for` (FOR %v/%%v IN (...) DO, Platzhalter über
  DosFindEntry, nur sichtbare Dateien, nicht schachtelbar), `run_chain`
  (& && || ^, statt execute_line in Hauptschleife, Stapel und /C;
  && prüft ERRORLEVEL 0, nicht gefundener Befehl setzt ERRORLEVEL 1),
  `setlocal`/`endlocal` (Umgebung + aktuelles Verzeichnis in DosAllocSeg,
  bis 4 tief, Ende der Stapeldatei räumt auf), EXTPROC (erste Zeile),
  GOTO :EOF, Strg+C: SIG_BREAK-Handler während Stapeldateien bzw. Kind
  mit Rückgabewert 255 -> „Stapelverarbeitung abbrechen (J/N)?“ (vor der
  nächsten Zeile; `ran_prog` wird am Anfang und Ende jeder Stapeldatei
  gelöscht – sonst fragte der nächste Aufruf, Fehler vom Nutzer gemeldet)
  (`batch_abort` über alle Ebenen), TRUENAME (Pfad normalisieren, SUBST
  und JOIN auflösen), BREAK/VERIFY (nur Merker), CHCP (437), CTTY
  (Handles 0–2 auf das Gerät, `read_line_tty`), KEYS ON/OFF/LIST, LH,
  DETACH (EXEC_BG). Argumente in "..." bleiben ein Wort. COMMAND ist bei
  ~62 KB (cbuf auf 4 KB verkleinert) – auf „mehr als 64 KB“ achten.
- ANSI wie OS/2 fest in der Bildschirmausgabe statt ANSI.SYS (Wunsch des
  Nutzers, in QEMU geprüft): `tty_char` -> `ansi_char` (vio.c) wertet in
  jeder TTY-Ausgabe (DosWrite auf die Konsole, VioWrtTTY) ESC[ Zeile;Spalte
  H/f, n A/B/C/D, s/u, J (ganz löschen), K und m (0/1/22/5/25/7/8/30–37/39/
  40–47/49) aus; Tastenbelegung (`p`, auch mit "..."), Modi (h/l, `=`/`?`)
  und anderes ESC x werden geschluckt. Je Sitzung `attr` (Textfarbe, auch
  für Löschen und Rollen), `ansi`, Zustand/Parameter. Vorgabe ANSI ON,
  `ANSI [ON|OFF]` (ANSI.EXE, FAPI 4Eh VioSetAnsi, 2 = abfragen), OFF setzt
  die Farbe zurück. COMMAND: `PROMPT $E` = ESC (`$e[1;33m$p$e[0m$g`).
  Vom Nutzer in 86Box bestätigt: farbiger Prompt geht.
- TEMP/TMP (Wunsch des Nutzers): Kernel setzt sie neben COMSPEC/PATH in
  der Systemumgebung auf X:\TEMP (Startlaufwerk; Start von Diskette und
  C: eingehängt -> C:), nur wenn CONFIG.SYS sie nicht per SET setzt;
  CMD /P macht DosMkDir(%TEMP%). Bisher nutzt kein AT-DOS-Werkzeug
  temporäre Dateien (Pipes laufen im Kernel) – gedacht für DOS-Box/
  Compiler.
- Pfade: der Kernel löst sie je Task auf (aktuelles Laufwerk und Verzeichnis)
  und reicht den IFS-Treibern kanonische Pfade `\DIR\DATEI` weiter.
- Sektornummern sind 32 Bit (`unsigned long`); Multiplikation mit der
  Clustergröße per Schieben, keine Watcom-Hilfsroutinen.
- Dateisysteme nur über die IFS-Schnittstelle (`struct ifs_driver`); außer
  `fat.c` kennt kein Kernelteil FAT-Interna. Blockgeräte nie per BIOS ansprechen.
- Platten-Cache wie SMARTDRV, aber im Kernel wie OS/2 `DISKCACHE=`
  (Wunsch des Nutzers; kein Programm, Anzeige über MEM): `CACHE=kb[,s]`
  in CONFIG.SYS (bis 8 MB; ohne Angabe nach Speicher: ab 4 MB erweitert
  1 MB, ab 1,5 MB 256 KB, sonst 128/32 KB; `BUFFERS=` wirkt nur noch als
  Untergrenze). kernel.c `bc_*`: Daten am Stück per `mem_alloc` im
  erweiterten Speicher (`bc_lin`), Blöcke zu 2 KB (4 Sektoren, Anfang
  lba & ~3), Zugriff über ein eigenes GDT-Fenster (`bc_wsel`, vor jedem
  Zugriff mit `bc_win(i)` umgelegt); Verwaltung (10 Byte je Block,
  valid/dirty/ref-Bits, Hash-Kette, 1024 Hash-Köpfe) in einem
  Kernelsegment. Verdrängen nach dem Uhr-Verfahren, Vorausgelesenes
  („Strom“, `blk_read_n` bis 4 Sektoren) ohne ref-Bit -> geht zuerst.
  Verzögertes Schreiben für Festplatten (s Sekunden, Vorgabe 5, 0 =
  sofort): `blk_write` legt nur in den Cache; geschrieben wird beim
  Verdrängen, nach Ablauf der Frist (`bc_due`: am Ende jedes FAPI-Aufrufs
  mit Sperre und in `sleep_ticks`, also auch beim Warten auf Tasten) und
  in `blk_flush` (SHUTDOWN, Strg+Alt+Entf, Entsperren nach CHKDSK/FORMAT/
  FDISK). `bc_wb` schreibt direkt aus dem Cache (buf_phys/buf_sel aufs
  Fenster), DMABUF bleibt unberührt. Vorausgelesenes überschreibt nie
  verzögert Geschriebenes. Disketten sofort (eigener Spurpuffer), ihre
  Einträge fliegen bei Wechsel raus. Sauber-Bit (`mark_in_use`,
  `ifs_shutdown`) mit `bc_sync` sofort; `system_down` leert erst alle
  Daten, setzt dann das Sauber-Bit. FAPI 4Dh DosCacheInfo, MEM zeigt
  Größe, Frist und Trefferquote. In QEMU geprüft: XCOPY + SHUTDOWN (fsck
  sauber, Inhalte gleich), Strom weg nach COPY (Datei fehlt, Dateisystem
  stimmig, Sauber-Bit gelöscht -> AUTOCHECK), CHKDSK /F, zwei XCOPY
  gleichzeitig, Diskette mit 4 MB RAM.
  Vorbereitung dynamischer Cache (Wunsch des Nutzers, für 6.0): Daten in
  Stücken zu 16 KB (`bc_unit[]`, 8 Blöcke je Stück, `BC_LIN(i)`), nicht
  mehr am Stück; fehlende Stücke = Blöcke mit dev -2. mem.c ruft bei
  Platzmangel `mem_reclaim` (= `bc_reclaim`: saubere Stücke von hinten
  freigeben, nur Tabellen, keine Plattenzugriffe – mem_alloc kommt auch
  ohne Kernelsperre), `bc_regrow` holt sie in `sleep_ticks` zurück (alle
  5 s, 256 KB bleiben frei; `bc_growing` verhindert Rückruf in sich
  selbst). MEM zeigt die aktuelle Größe. In QEMU geprüft (3 MB, CACHE=
  1536, TASKB: Cache 1.536 -> 1.472 KB, danach wieder 1.536; XCOPY,
  SHUTDOWN, fsck sauber). In 6.0 dann wie NT: Seiten statt Stücke, Cache
  = freier Speicher.
  Vom Nutzer auf dem SCAT in 86Box
  bestätigt (beschnitten auf 2 MB, 1.664 KB erkannt): Vorgabe 128 KB,
  XCOPY auf die Adaptec-Platte (Busmaster-DMA direkt aus dem Cache
  oberhalb 1 MB) mit 85 % Treffern, IDE ebenso (88 % Treffer).
  DEFRAG auf der 100-MB-IDE-Platte vom Nutzer bestätigt (117 Dateien, 34
  fragmentiert, CHKDSK danach sauber). Strom weg
  nach dem Schreiben: AUTOCHECK lief beim nächsten Start sofort an.
- CHKDSK (`apps/task_chkdsk.c`) ist nur der Verteiler: `DosQFSInfo` liefert
  den Namen des Dateisystems (FAT: `fat_driver.name`, nachgeladene IFS:
  Dateiname des Moduls ohne Endung), gestartet wird `U<Name>.EXE` aus dem
  PATH mit `X: [/F] [/V] [/AUTO]`. `UFAT.EXE` (`apps/task_ufat.c`) liest
  über `DosOpenDrive` (OF_DASD in kernel.c, `dasd_rw`), FAT ganz im
  Speicher, Verzeichnisse mit eigenem Stapel (App-Stack nur 2 KB).
  Sperre: `vol[].locked` – solange gesetzt, lehnt `vol_ok()` alle
  IFS-Zugriffe ab; beim Schließen `vol_reset()` (FAT-/Verzeichnis-Cache
  weg, neu einhängen, freier Platz neu zählen). Gesperrt wird nur, wenn
  keine Datei auf dem Laufwerk offen ist (aktuelle Verzeichnisse zählen
  nicht). `AUTOCHK.LST` im Stamm des Startlaufwerks: Zeilen `X: /F`,
  `CMD /P` löscht die Datei und ruft `CHKDSK X: /F /AUTO`.
  Sauber-Bit: FAT16 (nicht Diskette) FAT[1] Bit 15. `mark_in_use()` (fat.c)
  löscht es vor dem ersten Schreiben (Hook in den `ifs_*`-Schreibfunktionen,
  nicht in `wr()` – DMABUF!), `ifs_shutdown()` setzt es in `system_down()`.
  `fat_mount` merkt den Zustand beim Einhängen in `was_dirty` ->
  `DosQFSInfo` FSI_DIRTY. `UFAT /F` setzt das Bit nach der Prüfung.
  `AUTOCHECK=` (CONFIG.SYS) landet als Variable in der Systemumgebung.
- Schreiben (`fat.c`): Daten und Verzeichniseinträge sofort (`blk_write`),
  FAT nur im 2-Sektor-Cache bis `fat_flush()` am Ende jeder Operation – nach
  jedem FAPI-Aufruf ist das Dateisystem stimmig. Zeiger aus `dir_entry()`
  zeigen in `DMABUF` und gelten nur bis zum nächsten Plattenzugriff.
  Disketten: `revalidate` fragt vor jeder Operation `blk_media` (Wechsel-
  leitung im Treiber), eingehängt wird nur nach einem Wechsel neu.
- FORMAT/SYS: `FORMAT.EXE` ruft `U<FS>.EXE X: /FORMAT [/Q] [/V:x]`
  (Code in `apps/ufat_fmt.c`, in task_ufat.c eingebunden), danach ggf.
  `SYS.EXE`. Bootcode für beide kommt aus `build/bootcode.h` (build.sh
  erzeugt es aus boot.bin/hdboot.bin). Unformatierte Laufwerke:
  `vol[].present` (Laufwerk da) neben `used` (Dateisystem eingehängt),
  `psize`/`ptype` aus dem MBR; `vol_present()` = da, `vol_ok()` = da und
  eingehängt. DosQFSInfo meldet dann "RAW". Nach dem Entsperren hängt
  `vol_reset()` neu ein (frisch formatiert sofort benutzbar) und
  `of_release` behält `in_use`, damit SHUTDOWN das Sauber-Bit setzt.
  Diskette: ATDOS.SYS muss bei Cluster 2 zusammenhängend liegen – SYS nur
  auf leere Disketten; der Bootsektor prüft die ersten 16 Stammeinträge.
- Rückgabewerte: `DosExit(rc)` setzt `task.rc`, `task_end` legt ihn beim
  Elternteil in `child_rc` ab (`DosQChildRC`); Ausnahme und Strg+C ergeben
  255. `crt0` beendet nach `main` mit 0 (`main` ist `void`) – wer einen Wert
  liefern will, ruft `DosExit(n)`. COMMAND merkt ihn in `errorlevel`.
- Zeitscheibe: `fapi_dispatch` ruft nach jedem FAPI-Aufruf
  `schedule_next()`, wenn die Scheibe (`SLICE`, 50 ms seit `slice_start`)
  um ist oder Strg+C/Alt+Esc/Neustart anstehen. So bleiben Strg+C und der
  Sitzungswechsel auch bei `grep -r` über große Platten bedienbar. Rechnen
  ohne FAPI-Aufrufe hält das System weiter an (präemptiv erst 6.0).
  6.0, Phase 2 Schritt 1 (in QEMU geprüft): präemptiv. start.asm `irq0`
  prüft nach dem EOI, ob der unterbrochene Code in Ring 3 lief (CS-Bits
  0–1 im Rahmen); nur dann pusha/es, CLD, `preempt()` (kernel.c): bei
  abgelaufener Scheibe oder Strg+C/Alt+Esc/Neustart/resched_req
  `schedule_next()` – wie beim FAPI-Rücksprung. Begründung: ein Task in
  Ring 3 hält nie die Kernelsperre, alle anderen stehen an normalen
  Wartepunkten, also braucht der 16-Bit-Kernel keine Wiedereintritts-
  festigkeit (Kernel wird nie unterbrochen, wie OS/2 1.x). Der
  unterbrochene Task wird im ISR-Zustand (IF=0, EOI schon geschickt) im
  TSS gesichert und läuft später dort weiter bis zum IRET. Programme
  sind 286-Code: keine 32-Bit-Register, FS/GS – das 286-TSS reicht (für
  V86 später 386-TSS). Demo `\DEMO\BUSY [Runden] [/W]` (65536 Schritte
  je Runde, ohne FAPI). Geprüft: BUSY in Sitzung 2, Alt+Esc und VER in
  Sitzung 1 sofort; Strg+C bricht BUSY ab; zwei BUSY gleichzeitig kommen
  beide voran (Vordergrund 12 statt 8 s, nicht genau halbe-halbe –
  QEMU-Messung grob); zwei BUSY per DETACH + XCOPY über NCR8XX in 22 s,
  SHUTDOWN beendet sie, fsck sauber. Vom Nutzer auf dem 486 in 86Box
  bestätigt: mit zwei BUSY lässt sich weiter gut tippen und EDIT starten
  (eine Bevorzugung interaktiver Tasks war daher nicht nötig).
  Auf dem 386SX/2 MB in 86Box bestätigt: `grep -r` über die 2-GB-Platte im
  Hintergrund, Vordergrund bleibt bedienbar, Alt+Esc schnell, Strg+C bricht ab.
  `MEM` dort: 1.664 KB erkannt, 231 KB belegt, 1.433 KB frei.
- Treiber mit Interrupt (DevHlp in `include/drv.h`, Hilfen in drvlib.h):
  `DH_SETIRQ` (IRQ 3–15 außer 6) trägt die Routine in `irq_hoff/hcs/hds`
  ein (start.asm `irq_common`: Aufruf fern mit DS = Treiberdaten, AX = IRQ,
  IF aus, danach EOI; Geister-IRQ 7/15 über das ISR-Register erkannt).
  `DH_PROCBLOCK` mit CLI rufen, nachdem die Bedingung geprüft ist (kein
  verlorenes Wecken), Zustand `T_BLOCK`, Frist `block_until`, kehrt mit STI
  zurück; vor dem Start der Tasks (`current < 0`) HLT-Schleife.
  `DH_PROCRUN` setzt nur Zustände (auch aus dem Interrupt) und `run_flag`/
  `resched_req` – der Scheduler prüft `run_flag` vor HLT bei CLI.
  Ereignis = Treibernummer << 16 | BX. `devhlp_entry_` sichert
  `devhlp_dx` auf dem Stack (Aufrufe aus Interruptroutinen).
  Kernelsperre `klock` (kernel.c): jeder FAPI-Aufruf außer `fapi_nolock()`
  (VIO, KBD, Umgebung, Zeit, Konsole/Pipe-READ/WRITE ...) nimmt sie;
  `schedule_next()` gibt sie ab und der Scheduler gibt sie dem Task zurück
  (`want_lock`), nur ProcBlock (`sched(1)`) behält sie. Strg+C, Neustart
  und `task_end` anderer Tasks warten, solange jemand sie hält. Kernelteile
  ohne Sperre (vio) sichern SCRATCH/SCRATCH2 mit `win_save/win_restore`.
  Im Zweifel neue FAPI-Aufrufe nicht in `fapi_nolock` aufnehmen.
  Lehre (Schreibfehler beim Doppel-XCOPY in 86Box, mit /NOIRQ nie): Alles,
  was vor dem Warten auf die Sperre in globalen Variablen liegt, muss vorher
  gesichert sein – `fapi_dx` (DX, z. B. Länge bei DosWrite) wurde erst in
  `fapi_inner` gelesen und war dann von einem anderen Task überschrieben.
  Jetzt liest `fapi_dispatch` es als Erstes. QEMU zeigte den Fehler nie.
  In 86Box bestätigt (Board-IDE, Master 63 MB CHS + Slave 504 MB, IRQ 14):
  3× zwei XCOPY gleichzeitig ohne Fehler, CHKDSK C:/D: sauber.
  IDE: `wait_irq()` schläft je 2 Takte, prüft danach immer den Status
  (Interrupt beschleunigt nur), `chan[].done` vor Befehl und vor jedem
  Datentransfer löschen; 3 Fehltreffer -> Abfragebetrieb. In QEMU geprüft:
  zwei Sitzungen kopieren und greppen gleichzeitig auf einer Platte,
  `fsck.fat` sauber, 18/18 Kopien gleich; CD über IRQ 15; Rückfall mit nIEN.
- Signale (`sig_deliver` in kernel.c): Rahmen des FAPI-Aufrufs liegt fest
  oben auf dem Kernelstack (`FRAME(t)`, 14 Worte, Aufbau in start.asm
  `fapi_entry_`). Zustellung beim Rücksprung aus der FAPI: Rahmen und
  Ergebnis in `sig_save`, IP auf die Weiche `sig_tramp` (apps/fapi.h),
  AX = Signal; `DosSigReturn` legt alles zurück. Wartende FAPI-Aufrufe müssen
  bei `sig_pending & sig_mask` zurückkehren (KbdCharIn tut es).
  `end_programs()` (SHUTDOWN, Strg+Alt+Entf): SIG_SHUTDOWN, bis 5 s warten,
  Rest mit `task_end` beenden.
- Handles (kernel.c): Einträge der Dateitabelle zählen Verweise (`refs`),
  Eintrag 0 (`CONS`) ist die Konsole, `OF_FILE/OF_PRD/OF_PWR/OF_CON`.
  Programm-Handles 0–2 zeigen über `task.std[]` auf Einträge, eigene ab 3
  (`HBASE`). Freigabe nur über `of_release`. Programme schreiben über
  `puts`/`DosWrite(STDOUT)`, nicht über VioWrtTTY – sonst wirkt keine
  Umleitung (VioWrtTTY nur für Bildschirm-Dialoge wie „-- Mehr --“).
- SHUTDOWN/POWEROFF/REBOOT (Wunsch des Nutzers): ohne Schalter Rückfrage
  J/N, `/NOW` sofort, `/T:n` Countdown (Esc bricht ab), `/R` Neustart
  (`cmd_shutdown` in task_cmd.c, in QEMU geprüft).
- Strg+Alt+Entf ist zweistufig: Die Taste setzt `reboot_count`, der
  Scheduler schließt beim nächsten Durchlauf alle Dateien, ruft `blk_flush()`
  und startet neu; IRQ 0 startet nach 5 s trotzdem neu.
- Schreiben auf dem SCAT bestätigt: IDE-Master, IDE-Slave und Adaptec,
  danach ScanDisk unter MS-DOS ohne Fehler. Alle Platten der Testmaschine
  sind Kopien, Schreiben ist überall erlaubt. Den Adaptec emuliert QEMU
  nicht – Änderungen an `AHA154X.SYS` nur in 86Box prüfbar.
- IDE: Vor einem neuen Befehl nur auf BSY warten (`wait_idle`), nicht auf
  ERR – ein abgebrochener Befehl (FLUSH CACHE kennen die Platten in 86Box
  nicht) lässt ERR stehen bis zum nächsten Befehl. Sonst ist das Laufwerk
  nach dem ersten `blk_flush()` tot (auf dem SCAT nach `CHKDSK /F`). QEMU
  kennt FLUSH CACHE; nachstellen, indem man testweise 00h (NOP) schickt.
- MFM/RLL (WD1003) und ESDI (WD1007) in `IDE.SYS`: IDENTIFY abgebrochen
  (ERR + ABRT, keine ATAPI-Signatur) -> `old_disk()`: Geometrie aus der
  BIOS-Tabelle (`DH_BIOSDISK`; `start.asm` kopiert INT 41h/46h und
  0040:0075 im Real Mode nach `bios_fdpt`/`bios_hdcount`, weil die Tabelle
  im vergebenen RAM liegen kann), 91h einstellen, Sektor 0 und letzten
  Sektor der ersten Spur lesen. Kein FLUSH an diese Platten (`unit.old`).
  CHS ohne Wort 53: BIOS-Geometrie, wenn sie in die Platte passt (ESDI),
  sonst Grundgeometrie, beides per 91h eingestellt. In QEMU nachgestellt,
  indem `IDE.SYS` testweise 00h statt ECh schickt (QEMU bricht ab wie der
  WD1003): Start, CHKDSK, Kopieren über CHS mit SeaBIOS-Geometrie 130/16/63
  geprüft. ESDI in 86Box bestätigt (WD1007V, Master 504 MB + Slave
  63 MB, beide „(CHS, BIOS)“, also INT 41h und 46h gültig): FORMAT der
  500-MB-Platte unter AT-DOS ohne Fehler (jeder Sektor gelesen), Start von
  der ESDI-Platte (Bootsektor per BIOS, danach IDE.SYS). WD1003 ebenfalls
  bestätigt (Weg ohne IDENTIFY): Master 130/16/63 und Slave 1024/16/63 aus
  INT 41h/46h, Start von C:. Anzeige MFM (≤17 Sektoren/Spur), RLL (≤26),
  sonst „MFM/RLL-Platte“ – im BIOS (Typ 47) und in 86Box geht auch 63.
  Lehre: `line[]` im Treiber war 80 Byte, eine lange Meldung überschrieb
  die Einheitentabelle (jetzt 128).
- BusLogic ISA (laut Nutzer alle ISA-Modelle, die das aktuelle 86Box-
  master zeigt) wird von `AHA154X.SYS` erkannt. Die Startmeldung nennt die
  Karte (Wunsch des Nutzers): Befehl 8Bh (Modellnummer) kennt nur BusLogic
  -> „BusLogic BT-545C“; ein Adaptec lehnt ihn sofort ab (HACC + ungültig,
  dann kein Längenbyte nachschicken – es gälte als Befehl 04h), dann
  Board-ID aus 04h: A = 1540/1542A/B, D = 1542C, E = 1542CF, F = 1542CP.
  Die Board-ID allein unterscheidet nicht (BusLogic meldet auch „A…“).
  In 86Box bestätigt: Adaptec-Modell wird richtig erkannt; BusLogic wird
  als BusLogic erkannt, die Modellnummer kam aber leer – 86Box nimmt sie
  aus dem NVRAM der Karte (alte NVR-Datei). Rückfall: „BusLogic BT-54x“ und
  Firmware aus 04h (Haupt-, erste Nebenziffer), 84h (zweite), 85h
  (Buchstabe), z. B. „3.35“ (2.xx = BT-542B). Nach dem Löschen der NVR-
  Datei in 86Box wird auch das BusLogic-Modell erkannt (vom Nutzer
  bestätigt).
- BusLogic BT-545C (86Box) läuft mit `AHA154X.SYS` im Adaptec-Kompatibilitätsmodus:
  Platte (2 GB, D:) und CD erkannt und eingehängt; Schreiben bestätigt
  (COPY nach D:, Datei von dort ausgeführt). Meldung
  lautet noch „Adaptec 154x“ – BusLogic-Erkennung (Board-ID aus INQUIRY)
  wäre nur Kosmetik; der native BusLogic-Modus lohnt erst ab 6.0.
- CD über den Adaptec (SCSI-ID 6) auf dem SCAT bestätigt: ISO9660.IFS
  liest eine echte CD von 1994/95 mit korrekten Datumsangaben.
- Schreibtests in QEMU mit beschreibbaren Kopien der Abbilder, danach
  `fsck.fat -n` (Partition vorher mit `dd skip=63` herauslösen) und Inhalte
  mit `cmp` vergleichen.
- Watcom-Registerkonvention: Argumente in AX, DX, BX, CX; Assembler-Routinen
  erhalten alle Register außer dem Rückgabewert (AX bzw. DX:AX).
- Kein C-Laufzeitsystem (`-zl`): keine Bibliotheksaufrufe, keine
  32-Bit-Division/-Multiplikation (würde `__U4D` etc. ziehen), keine
  Struct-Zuweisungen.
- FAPI-Aufruf: `call far 0023:0000`, AX = Funktion, BX/CX = Argumente.
  Neue FAPI-Funktionen immer in `include/fapi_abi.h` und im README eintragen.
- FAPI-Schnittstellen für Dateien müssen lange Namen und Pfade annehmen
  können (bis 260 Zeichen, wie bei OS/2), auch wenn FAT nur 8.3 kennt – der
  IFS-Treiber lehnt ab, was er nicht kann. So laufen Programme später unter
  DOS 6.0 mit langen Namen ohne Neuübersetzung.
- Hardware-Erkennung liest immer erst, bevor sie schreibt: Eine Adresse gilt
  nur als Treffer, wenn die gelesenen Werte zur gesuchten Karte passen. Nie
  blind auf einen Port schreiben – dort kann ein anderes Gerät sitzen.
- Bildschirmtexte in CP437: Umlaute als Escape (`\x84` ä, `\x94` ö, `\x81` ü,
  `\xE1` ß), auf folgende Hex-Ziffern achten.

## Bauen und testen

    ./build.sh          # NASM + Open Watcom -> build/atdos-fd.img

Werkzeuge in einer neuen Umgebung einrichten (wie `.github/workflows/build.yml`):

    sudo apt-get install -y nasm mtools qemu-system-x86 dosfstools
    curl -sSL -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz
    mkdir -p ~/watcom && tar -xJf ow.tar.xz -C ~/watcom
    export WATCOM=~/watcom INCLUDE=~/watcom/h PATH=~/watcom/binl64:$PATH

QEMU-Tests ohne Bildschirm: `-display none -monitor unix:...`, Tasten per
`sendkey`, Bild per `screendump` (PPM -> PNG mit Pillow) ansehen.

Referenzmaschine in 86Box (Konfiguration: `86box/scat-286.cfg`): GW-286CT
GEAR, SCAT-Chipsatz (C&T 82C235), SCAT-BIOS 1.5.0, 286 mit 25 MHz (genauer
Interpreter, kein Dynarec; laut cfg des Nutzers, zeitweise auch 12 MHz
probiert), 287XL, 2 MB RAM (vom Nutzer von 16 MB auf zeitgemäße Werte für
1988/89 verkleinert; MEM: 1.664 KB), vier Everex EV-159 als EMS
(zusammen 11 MB, Ports 208h/218h, 258h/268h, 2A8h/2B8h, 2E8h; von DOS 5.0
nicht genutzt, erst für V86-Sitzungen in DOS 6.0 interessant), NE2000,
Tseng ET4000/W32i, Diskette A: 3,5" 1,44 MB, SCSI Adaptec AHA-154xC
(Port 334h, IRQ 11, DMA 6, BIOS C800h), Sound Blaster 16 (220h, MPU-401 330h,
IRQ 5, DMA 1/5, DSP 4.05), ROM-Karte mit IBM BASIC für Nachbauten bei
CC000h, Diskette B: 5,25" 1,2 MB. Mit 16 MB meldete das SCAT-BIOS
640 KB + 15232 KB erweitert (384 KB Shadow-RAM für System- und Video-BIOS).
Platten (Stand der cfg): IDE am Board, Master `atdos-hd.img` (130/16/63),
Slave 100 MB (776/8/33); am Adaptec SCSI-ID 6 CD-ROM (für Adaptec-Tests
hängt der Nutzer zeitweise eine SCSI-Platte an ID 0).

Start von Platte in QEMU: `qemu-system-i386 -hda build/atdos-hd.img -boot c`.
Der Nutzer will AT-DOS auch in Proxmox (KVM) laufen lassen: SeaBIOS,
i440fx, Platte an IDE oder am „LSI 53C895A“ (NCR8XX.SYS), nicht VirtIO.
`ATSYS.COM` lässt sich in DOSBox-X testen (`imgmount d ziel.img -t hdd -size
512,63,16,130`, Diskette als A:), danach das Ziel in QEMU starten.

Falle beim Umstecken von Platten zwischen Adaptec und IDE: FDISK schreibt
den Partitionsanfang auch als CHS mit der Geometrie des Controllers
(Adaptec 64/32, IDE z. B. 16/63). Der MS-DOS-MBR lädt per CHS und meldet am
anderen Controller „Betriebssystem fehlt“, obwohl MS-DOS, AT-DOS und
`hdboot.asm` (alle über die Sektornummer) die Partition lesen. Abhilfe:
am Zielcontroller neu partitionieren. Auf dem SCAT bestätigt.

Für Plattentests in QEMU dient `IDE.SYS` mit einer FAT16-Testplatte
(`-drive file=hdd.img,if=ide,snapshot=on`, angelegt mit `mpartition`/`mformat`).
QEMU emuliert keinen Adaptec – `AHA154X.SYS` lässt sich nur in 86Box testen.

Eingaben im QEMU-Test per Monitor `sendkey`; QEMU benennt Tasten nach
US-Belegung, der Kernel wertet deutsch aus (`y`/`z` vertauscht).

Schnelltest headless in QEMU (`qemu-system-i386 -fda build/atdos-fd.img`), der
echte Test ist eine 286-Maschine in 86Box. QEMU emuliert keinen echten 286
und prüft Segmentgrenzen nicht – ein Zugriff über die Grenze läuft in QEMU
durch und endet auf dem SCAT mit #GP. Deshalb bei jedem Zugriff über ein
Fenster (`SCRATCH`, `SCRATCH2`, Anwendungssegmente) die Grenze selbst prüfen.

CI baut bei jedem Push und veröffentlicht `atdos-fd.img` als Release `latest`.
Build-Nummer (Wunsch des Nutzers, damit klar ist, welches Abbild getestet
wurde): build.sh schreibt `build/version.h` (ATDOS_BUILD = in der CI
`GITHUB_RUN_NUMBER` wie auf der Actions-Seite, lokal Anzahl der Commits
+ „lokal“, ATDOS_REV = Kurz-Hash, `+` bei ungesicherten Änderungen,
ATDOS_DATE), VER in CMD zeigt „Build n (hash, Datum)“; die CI checkt mit
`fetch-depth: 0` aus (sonst zählte sie 1).
Auf `5.x` (Wunsch des Nutzers, wie Windows 10 19045.n): fest 240 (CI-Lauf
von 5.0, 8d7f54f), hinter dem Punkt die Commits seither, z. B. 240.5.

Nachschlagewerke (Lizenz beachten): Microsoft hat 2024 MS-DOS 1.25/2.0/
4.0 unter MIT veröffentlicht (github.com/microsoft/MS-DOS, Klon unter
/home/user/microsoft/ms-dos falls vorhanden): `v4.0/src` vollständig,
darin `MEMM` (EMS-Manager im V86-Modus für den 386, Vorläufer von
EMM386; ELIM.INC nennt vdmad.inc aus Win386 2.03) und SMARTDRV;
INT 2Fh-Kennungen in INC/MULT.INC (16h WIN386, 17h Zwischenablage).
`v4.0-ozzie`: Multitasking-DOS-4-Beta (Grundlage dieser Zeitlinie, auf
MS-DOS-2-Quellen) als Disketten + 11 Handbücher als gescannte PDFs (ohne
Textebene). Für virtuelle Geräte außerdem DOSEMU2 (GPL, V86 wie wir),
DOSBox (GPL, SB/OPL), ReactOS NTVDM (GPL). OS/2-Quellen sind nicht
freigegeben (Lecks nicht verwenden).

Zweite Meinung: Der Nutzer hat einen Gemini-MCP-Server verbunden
(`gemini_chat_completion`). Funktioniert mit `model: gemini-3.5-flash`;
3.1 Pro ist im kostenlosen Kontingent gesperrt (Limit 0), die 2.x-Modelle
gibt Google nicht mehr heraus. Projektbeschreibung als `systemInstruction`
mitgeben – Gemini kennt AT-DOS nicht. Antworten gegen den Code prüfen:
beim CHKDSK-Review waren 2 von 8 Punkten echte Fehler (Verzeichnisschleifen,
unlesbare FAT 1), der Rest war schon gelöst oder passte nicht.

## Geplant (noch nicht umgesetzt)

- EDIT ist da (`apps/task_edit.c`): Lückenpuffer in einem 64-KB-Segment,
  daher Text bis knapp 64 KB – offen: größere Dateien (mehrere Segmente),
  horizontale Bildlaufleiste. Erledigt: Öffnen-Dialog mit Dateiliste
  (`open_dialog`, Verzeichnisse zuerst, `..`, Muster, Laufwerk `D:`),
  Nachfrage bei SIG_SHUTDOWN: Signal setzt nur `shut_req`, `getkey()`
  (statt KbdCharIn überall in EDIT) ruft `shutdown_now()` beim nächsten
  Warten; bei Änderungen `DosShutdownHold` (FAPI 37h: Sitzung nach vorn,
  `end_programs` wartet ohne Frist, solange ein Task `hold` hat; zweites
  Strg+Alt+Entf während `going_down` startet sofort neu). Danach nächster
  Schritt mit dem Nutzer absprechen. FORMAT, SYS, FDISK, DISKCOPY, SUBST, UNDELETE und SCANDISK sind da. Später (vom
  Nutzer nach hinten gestellt): DEFRAG mit Clusterkarte und Verzeichnissen.
  MODE COM/LPT (in QEMU mit `-serial file:`/`-parallel file:` geprüft):
  FAPI 4Ah DosComCtl stellt den UART ein (Teiler aus Tabelle, LCR) bzw.
  liest ihn zurück, LPT nur Wiederholen + Status. COM1-4/AUX als Geräte
  (`OF_COM`, Abfragebetrieb mit yield, Senden 30 s Frist, mit RETRY=P
  endlos; Lesen wartet bis 5 s aufs erste Zeichen). Belegt ein Treiber den
  IRQ des Ports (COM1/3: 4, COM2/4: 3 – z. B. serielle Maus), lehnt der
  Kernel ab. MODE LPT schickt SI/DC2 und ESC 0/ESC 2 an den Drucker.
  In 86Box vom Nutzer geprüft: `ver > com2` geht, COM1 mit serieller Maus
  gesperrt, `MODE LPT1:132,8,P` meldet „Drucker: bereit“.
  DEFRAG (in QEMU an einer absichtlich fragmentierten Diskette geprüft,
  danach fsck sauber, Inhalte gleich; vom Nutzer in 86Box an einer
  Diskette bestätigt, 4 Dateien verschoben, CHKDSK danach ohne Fehler): `apps/ufat_defrag.c`, Belegung als
  Bitmap in `used`, Dateien (Eintragssektor, Startcluster, Länge) in einem
  DosAllocSeg-Segment; Reihenfolge kopieren -> FAT -> Eintrag -> alte
  Kette frei. UFAT ist knapp an 64 KB (VCHUNK auf 16 gesenkt) – beim
  Bauen auf „mehr als 64 KB“ achten, build.sh bricht dann still ab.
  Drucker (Nutzer hat keinen, gehört aber rein; in QEMU mit `-parallel
  file:` geprüft): `dev_name()` in OPEN/CREATE erkennt PRN/LPT1-3/NUL
  (Basisname, Endung egal), `OF_LPT`/`OF_NUL`, Ports aus dem BIOS-
  Datenbereich (`bios_ports`, start.asm). `lpt_write` wartet auf BUSY mit
  `yield()`, Status Bit 5/3 -> FAPI_ERR_NOTREADY (0xFFF9), HT_DEVICE.
  PRINT: Schlange `\DOS\SYSTEM\PRINT.QUE` („Gerät Pfad“ je Zeile),
  Hintergrund-Task `PRINT /WORK` über EXEC_BG, läuft-Merker PRINT.LCK
  (offen gehalten -> DosCreate meldet INUSE); prüft vor jedem 2-KB-Stück,
  ob seine Zeile noch vorne steht (/T, /C). util `same()` vergleicht nur
  das erste Wort – für ganze Zeilen eigener Vergleich.
  MSD (in QEMU geprüft): FAPI 49h DosSysInfo – start.asm fragt im Real
  Mode 286/386 (FLAGS Bit 12–15), Koprozessor (FNINIT/FNSTSW; 287 oder
  387-Kern wie 287XL über +inf = -inf nach FNINIT, nur beim 287 gleich), INT 11h,
  BIOS-Datenbereich (COM/LPT), BIOS-Datum/Modellbyte ab; der Kernel sucht
  in F000 und C000:0000–07FF den Namen (`rom_str`: „(C)“/„Copyright“ vor
  „BIOS“ vor dem längsten Text, mit Leerzeichen, ohne '%'). 386 und 486
  unterscheiden ginge nur mit 32-Bit-Befehlen – Anzeige „80386 oder neuer“.
  `MSD /S` Bericht, sonst Vollbild mit Knöpfen.
  UNDELETE (in QEMU geprüft): Verteiler `task_undel.c` -> `UFAT X:
  /UNDELETE \Pfad\Muster [/LIST] [/ALL]` (`apps/ufat_undel.c`), ohne
  Löschschutz wie MS-DOS 5: Startcluster frei -> Startcluster + nächste
  freie Cluster nach Größe. SCANDISK (Entscheidung des Nutzers: gehört in
  CHKDSK, wie NT): `CHKDSK /R` = /F + Oberflächentest (`apps/ufat_scan.c`):
  freie defekte Cluster FFF7h, belegte verlegt (Vorgänger in der FAT
  suchen, neuer Cluster, Unlesbares als Nullen), erster Cluster einer
  Datei nur gemeldet. `SCANDISK.EXE` ruft `CHKDSK X: /F /R`. In QEMU mit
  `blkdebug` (Lesefehler auf festen Sektoren einer IDE-Testplatte)
  geprüft, danach fsck sauber.
  SUBST (in QEMU geprüft): `subst_drv[]`/`subst_sel` in kernel.c,
  `resolve()` = `resolve_v()` (Sicht des Programms) + `subst_map()`;
  CHDIR speichert die X:-Sicht, prüft am Ziel. FREESPACE und LABEL-Lesen
  nehmen das Ziel (`subst_real`), DosOpenDrive/QFSInfo lehnen SUBST-
  Laufwerke ab (CHKDSK/FORMAT also auch). Initialisierung ganz am Anfang
  von kmain – sonst zeigt jedes Laufwerk auf A: (statische Null).
  Disketten (Wunsch des Nutzers: „old-school“, alle Formate bis 2,88 MB),
  in QEMU geprüft: `floppy.c` neu – A:/B: aus CMOS 10h (B: ist Geräte-
  nummer 1, `IS_FLOPPY(dev)` = `blk[].drv < 0`, `floppy_lazy` Bit 0/1),
  Formattabelle `fmts` (Rate, GAP3, Format-GAP, Doppelschritt, ED mit
  PERPENDICULAR MODE 12h), Erkennung: READ ID je möglicher Rate + letzter
  Sektor der Spur 0 lesbar (QEMU prüft die Rate bei READ ID nicht), ohne
  Diskette Controller-Reset (`ctl_reset`). Wechselleitung DIR Bit 7 (Typ 1
  = 360-KB-Laufwerk hat keine: jedes Mal neu erkennen). Spurpuffer 18 KB
  über `kseg_dma` (doppelt belegen, Stück ohne 64-KB-Grenze), Spur einer
  Seite mit einem Befehl lesen, Schreiben verzögert bis `fd_sync` – das
  läuft in `fat_flush`, nach DASD-Schreiben, in `blk_flush` und nach jedem
  FAPI-Aufruf mit Sperre (`fapi_dispatch`); vor dem Lesen einer Spur wird
  Geändertes zuerst geschrieben. Motoren 3 s nach dem letzten Zugriff aus
  (`fd_tick` im Scheduler). FAPI 46h DosFmtTrack (FORMAT ohne /Q, DISKCOPY
  formatiert beim Kopieren). In QEMU geprüft: Lesen/Schreiben aller fünf
  Formate (auch 360 KB im 1,2-MB-Laufwerk), FORMAT 1,44/2,88, DISKCOPY
  A: A: mit 2 MB RAM (mehrere Durchgänge) und A: -> B: 2,88 MB mit /V.
  QEMU kennt keine unformatierten Disketten und keinen Doppelschritt
  wirklich – beides nur in 86Box prüfbar.
  Vom Nutzer in 86Box bestätigt (SCAT, zwei 1,44-MB-Laufwerke): B: wird
  erkannt, `DISKCOPY A: B: /V` kopiert.
  FDISK (apps/task_fdisk.c), in QEMU geprüft: Menü wie MS-DOS 5/6
  (anlegen primär/erweitert/logisch, aktiv, löschen, anzeigen, nächste
  Platte), `/STATUS`, `/MBR` (Startcode aus build/mbrcode.h = mbr.asm).
  Kernel: FAPI 43h/44h DosDiskInfo/DosOpenDisk (OF_DASD mit f.attr =
  DASD_DISK, ganze Platte ab Sektor 0; Sperre nur ohne offene Dateien auf
  ihren Laufwerken, sperrt alle bis zum Schließen, dann vol_reset).
  Logische Laufwerke in erweiterten Partitionen (05h/0Fh, EBR-Kette)
  hängt mount_disk nach den primären ein („logisch n“). Ausrichtung wie
  MS-DOS (erste Partition Spur 1, sonst Zylindergrenze, EBR eine Spur vor
  dem logischen Laufwerk), Typ nach Größe (01h < 16 MB, 04h < 32 MB, 06h,
  höchstens 2 GB); 32-Bit über mul/div-Inline. Geschrieben wird sofort,
  Laufwerksbuchstaben erst nach Neustart (FDISK bietet ihn an).
  Geometrie: Erster Test in 86Box am Adaptec – MS-DOS-FDISK sah das
  logische Laufwerk nicht, weil FDISK auf der leeren Platte 16/63 annahm,
  der Adaptec aber 64/32 übersetzt (logisches Laufwerk nicht auf einer
  Spurgrenze, CHS in der EBR-Kette falsch). Jetzt wie MS-DOS/FreeDOS:
  start.asm fragt im Real Mode INT 13h/08h für 80h–83h (`bios_geo_*`),
  `disk_geo()` ordnet die BIOS-Platten über die Größe zu (Zyl×Köpfe×Sekt.
  höchstens 4 Zylinder unter der echten Größe, bei 1024 Zylindern gekappt),
  DosDiskInfo liefert heads/spt; FDISK nimmt BIOS-Geometrie, sonst die aus
  vorhandenen Einträgen, sonst 16/63 bzw. 255/63. In QEMU mit 8/32 geprüft
  (CHS = LBA in MBR und EBR). FreeDOS-FDISK (GPL) nur gelesen. Vom
  Nutzer in 86Box bestätigt: MS-DOS erkennt das mit AT-DOS-FDISK angelegte
  logische Laufwerk (D: und E: unter MS-DOS).
  Oberfläche (Wunsch des Nutzers) wie das deutsche MS-DOS-6-FDISK:
  Vollbild, zentrierte Titel, Felder in [ ] (nach „…ein:“ direkt
  dahinter, sonst Punkte bis Spalte 66), Fehler Zeile 22, „ESC drücken,
  um zu den FDISK-Optionen zurückzukehren“, Tabellen mit Laufwerks-
  buchstabe (über DosQFSInfo Start/Größe), Bezeichnung und System aus dem
  Bootsektor, eigene Bildschirme für logische Laufwerke und Plattenwechsel,
  „maximal und aktiv (J/N)“, Größe in MByte oder %, Löschen mit
  Datenträgerbezeichnung, nach Änderungen Neustart-Bildschirm. Bedienung
  nachgebildet, kein fremder Code; Copyright-Zeile weggelassen.
  Schon da: CHKDSK, XCOPY, ATTRIB, LABEL, MORE, FIND, SORT, FC, TREE,
  MOVE, DELTREE, CHOICE; VOL/DATE/TIME in COMMAND.
  Grundsatz: alles, was zur Kompatibilität gehört, wie bei MS-DOS (Namen,
  Schalter mit `/`, Meldungen, Verhalten). GNU-Vorbild nur für Werkzeuge, die
  MS-DOS nicht hatte. Wichtigste Werkzeuge für den Nutzer: `LESS` und `GREP`. `MORE` bleibt aus
  Kompatibilität (einfach, nur vorwärts, wie MS-DOS); `FIND` ebenso neben `GREP`.
  Vorbild für das Verhalten sind die GNU-Werkzeuge, besonders `less`:
  vor- und zurückblättern, Suche mit `/` und `n`, Sprung an Anfang/Ende,
  Zeilennummern – statt des einfachen MORE von MS-DOS. Nur Verhalten und
  Bedienung übernehmen, keinen GNU-Code (Lizenz offen, siehe oben).
  `DIR /P` blättert seitenweise.
- `EDIT` gehört zu den wichtigsten Werkzeugen (neben `LESS`, `GREP`).
- Maus, Stand: `MOUSE.SYS` (drivers/mouse.c) sucht zuerst PS/2 (Wunsch des
  Nutzers: die verbreitetste Maus), dann seriell (Microsoft 2 Tasten,
  Logitech `M3` mit 3. Taste im 4. Byte) an COM1/COM2; `/PS2`, `/COM1`,
  `/COM2`. PS/2: DevHlp `DH_KBDLOCK` maskiert IRQ 1 während der Erkennung
  (Takt läuft weiter, Fristen über DH_TICKS); A9h vor A8h (AT-Controller
  ohne Hilfsport kennen A8h nicht), bei Fehlschlag A7h; Bit 1 von
  DH_KBDLOCK sagt dem Kernel, dass der AUX-Kanal an ist – erst dann lässt
  `irq1` Bytes mit Statusbit 5 liegen (beim AT-Controller heißt Bit 5
  Sende-Zeitüberschreitung!). In QEMU geprüft (PS/2 und seriell). SCAT
  (GW-286CT) hat vermutlich keinen PS/2-Hilfsport – Test auf 386SX/P3B-F. Treiber ohne Blockgeräte melden beim INIT `DRV_OK` mit
  0 Einheiten (load_driver behält sie). DevHlp `DH_MOUREG`/`DH_MOUEVENT`
  -> vio.c: Position in Mickeys (8 je Spalte, 16 je Zeile wie Microsoft),
  Warteschlange je Sitzung (8, nur Vordergrund mit MouOpen), Zeiger als
  Zelle mit Attribut XOR 77h nur im Bildspeicher (`ptr_remove`/`ptr_draw`
  in vio_flush und redraw). FAPI 38h–3Ah MouOpen/MouReadEvent/MouShowPtr
  (ohne Kernelsperre), `vio_task_end` gibt die Maus frei. Test:
  `\DEMO\MAUSTEST.EXE`; in QEMU mit `-serial msmouse` und Monitor
  `mouse_move`/`mouse_button` geprüft. Serielle Microsoft-Maus an COM1
  in 86Box erkannt (IRQ 4). Vom Nutzer auf dem ASUS P3B-F in
  86Box bestätigt: Maus wird erkannt. EDIT nutzt sie: Klick setzt den
  Cursor, Ziehen markiert (über den Rand: rollen), Menüleiste per Klick,
  Loslassen über einem Eintrag führt ihn aus, Klick daneben schließt,
  Bildlaufleiste (Pfeile zeilenweise, Fläche seitenweise). `getkey_m(1)`
  liefert `K_MOUSE` (Hauptfenster, Menüs, Öffnen-Dialog: Klick wählt,
  zweiter Klick öffnet; msgbox-Knöpfe per `button_hit`); `input()`
  (Suchen/Ändern/Speichern unter): Klick ins Feld setzt den Cursor,
  Klick auf OK/Abbrechen drückt (in QEMU geprüft).
  Busmaus (nach seriell, `/BUS`), in 86Box vom Nutzer bestätigt (wird
  erkannt; QEMU hat keine): InPort an 23Ch/238h über
  die Kennung (Basis+2 abwechselnd DEh/Version, nur lesen), Modus 11h
  (30 Hz, Interrupt), beim Lesen 31h (halten); Logitech (8255) an 23Ch
  erst lesen (alles FFh = nichts), dann A5h/5Ah ins Kennungsregister.
  Werte wie im Linux-Treiber (nur gelesen). IRQ: IRR beider PICs ohne
  und mit Interrupt der Karte vergleichen (2, 3, 4, 5, 7; 2 = 9), genau
  ein neuer -> der; sonst `/IRQ:n`, Rückfall 5.
  Lehre (Flackern des Öffnen-Dialogs auf dem SCAT): `getkey_m` meldet
  bloßes Bewegen ohne Taste nicht (den Zeiger zeichnet der Kernel), und
  Dialoge, in denen nur Drücken zählt, überspringen Loslassen/Ziehen ohne
  Neuzeichnen – sonst baut sich der Dialog bei jeder Bewegung neu auf, und
  vio_flush zeigt auf dem 286 halbfertige Zustände.
- Konsolenmaus wie gpm (Wunsch des Nutzers), erledigt: Hat in der
  Vordergrund-Sitzung niemand MouOpen gerufen, gehört die Maus dem Kernel
  (vio.c): Zeiger nach der ersten Bewegung, linke Taste ziehen markiert
  (Zellen XOR 77h, `sel_paint` nur auf frisch kopierten Zeilen, Änderungen
  über `mark()`), Loslassen -> `sel_finish` in vio_flush: Text aus dem
  Sitzungspuffer in die Zwischenablage (Kernelsegment 16 KB, CR LF,
  Leerzeichen am Zeilenende weg; bloßer Klick kopiert nichts). Rechte oder
  mittlere Taste: `vio_paste` liefert die Zeichen über kbd_get/kbd_peek,
  solange die Sitzung im Vordergrund ist (kein Überlauf des 32er-Tasten-
  puffers); CR/LF/CR LF -> Enter. FAPI 3Bh/3Ch DosClipPut/DosClipGet
  (Selektor 0 = eigenes DS); EDIT kopiert dorthin und fügt von dort ein
  (über 16 KB nur intern, Kernel-Ablage dann leer). In QEMU geprüft:
  Markieren in COMMAND, Einfügen in COMMAND und in EDIT. Vom Nutzer auf
  dem ASUS P3B-F (PS/2-Maus) bestätigt: Kopieren aus der Konsole geht.
  Testfalle: `ver` gibt vor dem Text eine Leerzeile aus – Bildschirm-
  ausgaben beim Testen nicht mit `grep -v "^$"` filtern.
- Maus: ein Treiber `MOUSE.SYS`, der wie MOUSE.COM von Microsoft/Logitech
  selbst sucht und nimmt, was da ist: PS/2 (Hilfsport des Tastatur-
  controllers, IRQ 12), Busmaus (InPort/Logitech, 23Ch, ID-Register lesen),
  seriell (COM1/COM2: UART erst lesend prüfen, dann DTR/RTS umschalten, die
  Maus antwortet mit 'M'). Optional per Argument festlegen
  (`DEVICE=MOUSE.SYS /COM2`). Braucht Treiber mit Interrupt (DevHlp SetIRQ).
  FAPI wie das Mou-API von OS/2: Ereignisse je Sitzung (nur Vordergrund),
  Zeiger als invertierte Textzelle. Erster Nutzer: EDIT. Test zuerst mit
  serieller Microsoft-Maus in 86Box.
- CHKDSK: Datei-Fragmentierung anzeigen (`CHKDSK *.*`), Oberflächentest
  gehört zu einem späteren ScanDisk; FORMAT, LABEL, DISKCOPY über
  dieselbe DosOpenDrive-Schnittstelle, FORMAT ebenfalls über
  `U<Dateisystem>.EXE` (wie OS/2: `UFAT`/`UHPFS` machen CHKDSK und FORMAT).
- CD ohne Medium (in QEMU mit `change`/`eject` geprüft): neuer
  Treiberstatus `DRV_ERR_NOTREADY` (drv.h). IDE.SYS: nach einem Lesefehler
  REQUEST SENSE (`atapi_pkt`/`atapi_sense`): 2/3Ah beim zweiten Mal ->
  NOTREADY (QEMU und manche Laufwerke melden 3Ah nach einem Wechsel
  einmal), 2/04h je 0,5 s bis 10 s warten, 6 (Unit Attention) nochmal.
  AHA154X.SYS: Auto-Sense liegt hinter der CDB (`ccb.cdb + cdblen`, nicht
  `ccb.sense`!); NCR5380.SYS schickt REQUEST SENSE selbst – beide nur in
  86Box prüfbar. Kernel: `blk_nm[dev]` aus dem letzten Lesen,
  `blk_nomedia` auch für CDs, `mod_mount` setzt `ifs_notready`;
  SELECTDISK, LABEL und QFSInfo (FSI_NOMEDIA) versuchen bei CDs ein
  Einhängen. COMMAND (`not_ready_d`), CHKDSK, FM: „keine CD eingelegt“
  bei bsize 2048. Offen: ISO9660.IFS liefert keine Datenträgerbezeichnung
  (VOL zeigt „keine Bezeichnung“).
- CHKDSK auf CD-ROM (Entscheidung des Nutzers, festgehalten): heute „CHKDSK
  kann CD-ROM-Laufwerke nicht prüfen“, solange es kein U*.EXE gibt. Später
  denkbar: Lesbarkeitsprüfung (Oberflächentest) für CDs, und ein Dateisystem
  für beschreibbare Medien bringt sein eigenes U*.EXE mit – CHKDSK nimmt
  es dann automatisch (Reihenfolge in task_chkdsk.c: erst U*.EXE suchen,
  Meldung nur, wenn keins da ist).
- Lange Namen auf CD (Wunsch des Nutzers): Joliet erledigt (in QEMU mit
  `genisoimage -J` geprüft, auch Wechsel auf eine CD ohne Joliet, XCOPY
  von der CD auf C: mit EA-Namen, fsck sauber). `ISO9660.IFS`: `mount`
  nimmt den Joliet-Baum (SVD Typ 2, Escape `%/@`, `%/C`, `%/E` bei 88),
  `take` wandelt UCS-2 -> CP437 (`lat2cp`, Unbekanntes `_`). 8.3-
  Ersatznamen wie Windows 95: `basis()` (6 Zeichen + Endung), `~k` = k-ter
  Eintrag mit gleicher Grundform (`fill_alias` zählt mit `scan(SC_COUNT)`,
  nur für READDIR/NAMEPATH); `scan(SC_NAME)` findet lange Namen ohne
  Groß/klein und Ersatznamen (`STAMM~k.EXT`). Neue Modulaufrufe
  `IFSM_LABEL` (Bezeichnung aus dem PVD, VOL) und `IFSM_NAMEPATH` (CD
  zeigt `D:\Neuer Ordner`, alte Programme bekommen 8.3) -> `mod_label`/
  `mod_namepath` im Kernel. Offen: ISO 9660:1999 (Level 4, bis 207
  Zeichen) und Rock Ridge. Kopieren auf FAT braucht dann die
  `.LONGNAME`-EAs (siehe unten), sonst 8.3-Ersatznamen. UDF (DVD, ab 1996)
  als eigenes `UDF.IFS` – zeitlich DOS 6.0/7.0.
- `COPY` mit Verketten (`A+B [Ziel]`, ohne Ziel an A anhängen) und Datum/
  Uhrzeit der Quelle (wie MS-DOS) – erledigt, in QEMU geprüft. Verketten
  binär (kein ^Z-Textmodus). `HELP Befehl` = `Befehl /?`, `HELP` zeigt
  interne Befehle und die Programme in `\DOS`.
- `ATSYS`: CHS-Anfang der Partition im MBR gegen die BIOS-Geometrie prüfen
  und warnen (siehe Falle oben). Erledigt: `mbr.asm` (nur `atdos-hd.img`)
  rechnet den Startsektor aus LBA und INT-13h/08h-Geometrie um, CHS aus der
  Tabelle nur als Rückfall – in QEMU mit 16/32 statt 16/63 geprüft (alter
  MBR hängt, neuer startet). Auf dem SCAT bestätigt: `atdos-hd.img` startet
  direkt am Adaptec (64/32). Ein eigener FDISK sollte denselben MBR schreiben.
- `JOIN` (Wunsch des Nutzers, wie MS-DOS 3.1–5.0), erledigt, in QEMU
  geprüft: `JOIN A: C:\DISK` (Verzeichnis leer, fehlt es, wird es
  angelegt; nicht der Stamm), `JOIN` zeigt, `JOIN A: /D` hebt auf. FAPI
  4Bh/4Ch DosJoin/DosQJoin. kernel.c: `join_drv[]`, Pfade in der hinteren
  Hälfte von `subst_sel` (`JOIN_OFF`), `join_map()` läuft in `resolve()`
  nach `subst_map()` (`C:\DISK\X` -> `A:\X`), `resolve_v` lehnt das
  eingehängte Laufwerk ab, ebenso SELECTDISK/QFSInfo/OpenDrive/
  FreeSpace/Label (`join_hidden`). Nicht einhängbar: Startlaufwerk,
  SUBST-Laufwerke, aktuelle Laufwerke, Laufwerke mit offenen Dateien oder
  mit SUBST/JOIN darauf. Aufheben nur, wenn kein Task darin steht
  (`join_under`) und nichts offen ist. CHDIR/QCURDIR holen die
  Schreibweise über die Grenze (`namepath_j`). DIR/TREE laufen hindurch.
  Vom Nutzer in 86Box bestätigt: JOIN funktioniert.
  DIR zeigt JOIN-Verzeichnisse wie Windows als `<JUNCTION>` mit Ziel
  (`disk [A:\]`), COMMAND fragt dazu DosQJoin ab (`join_mask`, `join_at`).
  CD auf andere Laufwerke wie MS-DOS (in QEMU geprüft): je Task merkt sich
  der Kernel das Verzeichnis von bis zu 4 weiteren Laufwerken (`dcwd()`,
  `NDCWD`, im Pfadsegment `subst_sel` hinter SUBST/JOIN, `DCWD_OFF`);
  CHDIR auf fremdes Laufwerk speichert dort, SELECTDISK tauscht (fehlt das
  Verzeichnis inzwischen: Stamm), `resolve_v` nimmt es für relative Pfade,
  `in_use` schützt es vor RD, Kinder erben die Tabelle. DosQCurDir mit
  CX = Laufwerk + 1 (`DosQCurDirD`), COMMAND `CD A:` zeigt es. TREE zeigt
  wie MS-DOS keine versteckten/System-Dateien.
- Leeres Diskettenlaufwerk (vom Nutzer in 86Box gemeldet: A:/B: ohne
  Diskette „zugreifbar“): `floppy.c` merkt `nomedia` (Wechselleitung
  bleibt nach dem Schritt gesetzt, oder READ ID ohne Index), `fd_read`/
  `fd_write` scheitern dann. `revalidate` und `vol_present` setzen
  `ifs_notready`, `fapi_dispatch` macht aus jedem Fehler dann
  FAPI_ERR_NOTREADY; QFSInfo meldet FSI_NOMEDIA, SELECTDISK lehnt ab.
  COMMAND/CHKDSK/MSD: „Laufwerk nicht bereit - keine Diskette
  eingelegt.“ In QEMU mit `eject`/`change` geprüft. Dabei behoben:
  `disk_blk` zählte B: (Gerät 1) als „Platte 1“ (MSD, FDISK!).
- Treiber mit Interrupt: Grundlage und IDE erledigt (siehe Konventionen).
  `AHA154X.SYS` mit IRQ auf dem SCAT bestätigt (Anzeige „IRQ 11, DMA 6“,
  Start von der Adaptec-Platte über den Interrupt, 3× zwei XCOPY
  gleichzeitig über IDE/Adaptec/CD ohne Fehler). IRQ aus Befehl 0Bh
  (Byte 2: Bit 0–3 IRQ 9–12, Bit 5/6 IRQ 14/15), Routine quittiert bei
  INTV mit IRST (sonst bleibt die ISA-Leitung oben) und weckt; fertig ist,
  wenn `mb[1].status` gesetzt ist (`wait_mbox`). In 86Box (`scsi_x54x.c`)
  löst START kein HACC aus, MBIF kommt nach HACC erst nach IRST.
  Diskette schläft ebenfalls (proc_block), `MOUSE.SYS` erledigt. `has_opt()` und `hex()` in drvlib.h.
- `NCR5380.SYS` (drivers/ncr5380.c), erste Stufe, in 86Box vom Nutzer
  bestätigt (auf Anhieb): T130B an 350h, zwei IBM-Platten (63/504 MB) und
  CD erkannt, Start von der Trantor-Platte (C:). Das Trantor-BIOS hängt
  bei einer 2-GB-Platte (BIOS-Übersetzung 64/32, 1 GB) – AT-DOS braucht
  das Karten-BIOS nicht (in 86Box „Disabled“). Trantor T130B (53C400,
  Ports 350h/340h/250h/240h, 5380-Register ab Basis+8), PIO mit REQ/ACK je
  Byte, Arbitrierung + Auswahl ohne ATN, Phasen nach CSBS, langes Warten
  mit ProcBlock-Schlaf (1 Takt). Erkennung lesend (MR Bit 6 und TCR Bit
  4–6 = 0), dann TCR als Probe (wirkt nur im Zielmodus). `/P:340` usw.
  86Box-Karten (scsi_ncr53c400.c, scsi_t128.c, scsi_aic6360.c): T130B per
  I/O; LCS-6821N, RT1000B, LS2000, T128 mit Registern im 16-KB-Fenster
  des Karten-BIOS (C800h–DC00h) -> braucht DevHlp für physischen Speicher
  (OS/2: PhysToVirt/PhysToGDTSelector); AHA-1520A und SB16-SCSI: AIC-6360.
  Zweite Stufe (Nutzer: PIO zu langsam). Erster Test in 86Box: Busfehler
  auf D: (Lesen 40h, Schreiben 4F8h) – Reihenfolge war falsch: 86Box (und
  das Datenblatt) beginnen beim Laden des Blockzählers nur, wenn der 5380
  schon im DMA-Betrieb ist. Jetzt wie Linux: MR=DMA|Busüberwachung, DMA-
  Start, dann Richtung, dann Blockzähler. Nach einem gescheiterten Block-
  versuch bleibt der 53C400 in 86Box auf „Zähler geladen“, dann scheitert
  auch PIO (tx_mode bleibt DMA). Nach der Korrektur vom Nutzer in 86Box
  bestätigt: XCOPY von 28 Dateien auf die Trantor-Platte fehlerfrei, in
  annehmbarer Geschwindigkeit.
  Datenphasen in Vielfachen von 128 Byte im 53C400-Blockbetrieb – Richtung
  in Basis+0 (Bit 6 lesen), Blockzahl in Basis+1, 5380 MR=DMA und Start
  über Register 7 (empfangen) bzw. 5 (senden, ICR mit DBUS), je Block
  warten bis Status Bit 2 = 0, dann `rep insb/outsb` über Basis+4; am Ende
  auf Bit 7 (5380 erreichbar) warten, MR=0. Scheitert es: Bus-Reset,
  `blockmode = 0`, Meldung, Wiederholung per PIO; `/PIO` erzwingt PIO.
  Ohne IRQ (Pufferwechsel je 128 Byte sind kurz). Nächste Stufen:
  Speicherfenster-Karten, AIC6360.SYS. NDRV auf 6 erhöht (MEMMAP_SEL 168h).
- `NCR8XX.SYS` (drivers/ncr8xx.c, 6.0, in QEMU mit `-device lsi53c810`
  und `lsi53c895a` geprüft: Platte + CD, XCOPY auf die SCSI-Platte,
  CHKDSK, SHUTDOWN, fsck sauber, Inhalte gleich). PCI Mechanismus 1 mit
  Suche über den PCI-Dienst des Kernels (bus.c), Chiptabelle Hersteller
  1000h, BAR 0 = I/O, Befehlsregister I/O + Busmaster. SCRIPTS in `scr[]`
  (je Befehl neu gebaut, `build()`): SELECT ATN -> Phasenschleife (JUMP
  WHEN) -> MSG OUT IDENTIFY 80h, Befehl, Daten (Busmaster direkt nach
  `buf_phys`), Status, MSG IN: 00h fertig (INT 1), 04h DISCONNECT ->
  WAIT DISCONNECT, WAIT RESELECT, IDENTIFY lesen, weiter; anderes
  quittieren. Lehren aus QEMU: der LSI geht nach der Auswahl immer nach
  MSG OUT (ohne IDENTIFY INT 2) und trennt vor den Daten trotz 80h
  (ohne Wiederauswahl Zeitüberschreitung). Abfragebetrieb über ISTAT,
  Frist über DH_TICKS; STO (SIST1 Bit 2) = kein Gerät. IRQ (in QEMU
  geprüft, `info irq`: 1.185 Interrupts auf IRQ 11 bei XCOPY): PCI-IRQ
  aus Register 3Ch, `ncr_irq` -> `collect()` liest ISTAT, DSTAT/DSPS,
  SIST0/1 (löscht den pegelgesteuerten Interrupt), sammelt per ODER,
  `done`, proc_run; `run()` schläft je 2 Takte und prüft danach selbst
  (wie IDE: verlorener/geteilter IRQ hält nichts auf). `/NOIRQ` oder
  belegter IRQ: Abfragebetrieb wie vorher (Startmeldung nennt den
  Grund). Vom Nutzer auf dem 486 in 86Box bestätigt: MSD zeigt „IRQ 9
  belegt NCR8XX.SYS“ (vorher altes Abbild ohne IRQ – Release `latest`
  neu laden!); mit IRQ den DOS-Ordner per FM mehrfach auf D: (250 MB,
  1.128 Dateien) kopiert und per DELTREE gelöscht, CHKDSK /F ohne
  Fehler. Offen: synchrone/Wide-Übertragung. Vom Nutzer auf seiner 486 in 86Box
  bestätigt: Platte und ZIP-Laufwerk am NCR erkannt, `\DEMO` aufs ZIP
  kopiert (Programme laufen von dort), CHKDSK ohne Fehler.
  Vorladen beim Plattenstart dafür vergrößert: preload.c nimmt bei
  >= 600 KB konventionell 8000:0000 (60 KB, 16 Dateien) statt 0060:0000
  (30 KB, 8); kmain gibt der Speicherverwaltung erst nach CONFIG.SYS den
  Bereich ab 80000h (`mem_init(..., 512, ...)`, dann `mem_free`).
- Busdienste (Vorschlag mit dem Nutzer abgestimmt: im Kernel statt
  PCI.SYS/ISA.SYS, wie OEMHLP$ bei OS/2), in QEMU geprüft (PC mit NCR
  und SB16, `isapc` ohne PCI): `kernel/bus.c` in KTEXT2 (erstes
  Codesegment ist mit ~61 KB fast voll). `pci_init` beim Start nach
  fd_init: CF8h erst lesen – Bits 0–1 ungleich 0 = kein PCI-Host,
  nichts schreiben (ISA dekodiert 10 Bit, CF8h wäre 0F8h =
  Koprozessor); Tabelle `pci_tab` (32, Bus 0 + hinter Brücken,
  Mehrfunktion). DevHlp 14–18: PCIFIND (Hersteller/Gerät, n),
  PCICLASS, PCIREAD/PCIWRITE (Wert über 4 Byte im Treiber-DS), CLAIM
  (RES_IO/RES_DMA; DH_SETIRQ trägt RES_IRQ ein, UNSETIRQ und
  gescheitertes INIT löschen). drvlib: `claim`, `pci_find`, `pci_rd`,
  `pci_wr`. Alle Treiber melden ihre Ports an. FAPI 51h DosBusInfo
  (BUS_PCI/BUS_RES) -> MSD: Computer-Seite PCI-Liste mit Hersteller-
  und Klassennamen, IRQ-Seite Besitzer, Treiber-Seite Ports/DMA/IRQ.
  Offen: ISA-PnP (`ISAPNP.SYS`, früh laden) zusammen mit SB16 PnP.
- Weitere SCSI-Treiber, Ziel NCR-Familie (in 86Box vorhanden): zeitlich
  passend für 5.0 der ISA-NCR 53C400 (z. B. Trantor T130B, PIO); die
  PCI-Chips 53C810/825/875 (SCRIPTS-Prozessor, PCI-Konfiguration mit
  32-Bit-Portzugriffen) gehören zu 6.0 auf dem 386/486. Die 53C8xx sind
  die Standard-SCSI-Controller des Nutzers auf PCI-Maschinen: für 6.0 der
  wichtigste neue Plattentreiber (`NCR8XX.SYS`, mit Interrupt).
- Ton (Wunsch des Nutzers: lieber SB16 als AIC-6360; Vorrang Vordergrund
  für Spiele), Stufe 1 und 2 erledigt, in QEMU mit `-device sb16` und
  `-audiodev wav` geprüft (Mitschnitt nach Frequenz je Viertelsekunde
  ausgewertet): `SB16.SYS` (drivers/sb16.c) – DSP-Reset (vorher lesen:
  alle Leseports FFh = nichts), Version E1h, beim SB16 IRQ/DMA aus Mixer
  80h/81h; 16-KB-DMA-Puffer (neuer DevHlp `DH_DMABUF` = kseg_dma, physische
  Adresse in den Treiber), zwei Hälften, Auto-Init (SB16 41h + B6h/C6h,
  DSP 2/3: Zeitkonstante 40h + 48h + 1Ch), IRQ füllt eine nicht volle
  Hälfte mit Stille. Mixer SB16 30h–3Ah, SB Pro 22h/04h/26h/28h/2Eh.
  Kernel: `DH_AUDREG`, `FAPI_AUDIO` (4Fh, ohne Kernelsperre – Schreiben
  wartet im Treiber per ProcBlock), `DRV_AUDIO` mit `struct aud_req` (je
  Programm eine, ein schlafender Schreiber behält seine; Treiber prüft
  `gen` nach dem Aufwachen). Mehrere Programme (NAUD 4): `aud_pick` =
  Vordergrund-Sitzung, sonst zuletzt geöffnet; beim Wechsel AR_STOP +
  AR_START; wer nicht dran ist, schläft Länge / Bytes je Takt (Echtzeit,
  Daten verworfen) – geprüft: Hintergrund 6 s 440 Hz + Vordergrund 2 s
  880 Hz -> 880, dann 440 ab der Echtzeitstelle, zusammen 6 s.
  `PLAY.EXE` (WAV PCM 8/16, VOC Block 1/2/9; nur Esc bricht ab, damit ein
  DETACH-PLAY COMMAND die Tasten nicht wegnimmt), `MIXER.EXE`. Testfalle:
  q.py tippt ~0,45 s je Taste – zeitkritische Abläufe per Stapeldatei.
  Stufe 3/4 erledigt (FM in QEMU mit `-device adlib,iobase=0x380` geprüft
  – QEMU legt die AdLib auf iobase+8 = 388h; MPU-401 nur in 86Box
  prüfbar): `SB16.SYS` erkennt OPL an 388h (Zeitgeber-Test, Status 06h =
  OPL2, sonst OPL3 – QEMU meldet fälschlich OPL3) und MPU-401 (`/M:`,
  Reset FFh mit Bestätigung FEh, UART 3Fh), lädt auch ohne DSP; `/NOFM`,
  `/NOMPU`. FAPI: AUD_CAPS, AUD_FMOPEN/FMWRITE (struct fapi_fmreg)/
  FMCLOSE, AUD_MIDIOPEN/MIDIWRITE/MIDICLOSE, FAPI_QTICKS (50h, Stub:
  BIOS-Takt * 5,5). Kernel: `struct aclient` fmc/mdc, gleiche Vorrangregel
  (`cl_pick`); FM: 512 Register je Programm in einem Kernelsegment
  (`fm_sel`), beim Wechsel `fm_load` (AR_FMRESET, Register mit Tasten
  los) und danach immer auch der laufende Auftrag (sonst fehlt die erste
  Note – Fehler beim ersten Test); MIDI: Programm/Lautstärke je Kanal aus
  dem Strom mitgelesen (`md_track`, laufender Status), beim Wechsel
  AR_MIDIRESET + neuer Stand. PLAY: `apps/midiplay.h` – SMF 0/1 (bis 64 KB,
  32 Spuren, Tempo, SysEx an den MPU), eigene FM-Instrumente (16
  Familien nach GM-Programm/8, 5 Schlagzeugklänge auf Kanal 10), 9/18
  Stimmen, älteste wird abgelöst; IMF (560/700 Hz). Zeit in 10 ms über
  DosQTicks, Rest in µs (`time_add_us`, mul16/div32, `sdiv` begrenzt).
  Geprüft: IMF 440 Hz 1 s, MIDI A3/E4/A4 mit richtigen Frequenzen.
  Klangbeispiele in `\DEMO` (tools/mkaudio.py, beim Bauen erzeugt):
  KLANG.WAV (Arpeggio, 11 kHz 8 Bit), ODE.MID / ODE.IMF (Ode an die
  Freude, gemeinfrei, mit Bass) – in QEMU alle drei hörbar.
  Vom Nutzer auf dem SCAT in 86Box bestätigt: `PLAY AKKORD.WAV` (aus
  Windows 3.1) spielt über den SB16, ebenso über Sound Blaster 2.0;
  nach dem Umbau für alte DSP auch ohne Endlosschleife, FM auf SB 2.0.
  Alte Karten (vom Nutzer gemeldet: SB 1.5 spielte endlos, SB 1.0 stumm):
  DSP 1.xx kennt kein Auto-Init (1Ch) und keine Blockgröße (48h) ->
  `single_block` (14h je Hälfte, im IRQ neu gestartet, der 8237 läuft im
  Auto-Init weiter); vor dem SB16 hält D0h Auto-Init bei DSP 2.00 nicht an
  (DAh erst ab 2.01) -> Anhalten per DSP-Reset. In QEMU mit dem
  Testschalter `/V:105` bzw. `/V:200` (alte DSP-Version vortäuschen)
  nachgestellt: Ton 1 s, danach Stille. Auf SB 1.0/1.5 in 86Box noch zu
  bestätigen (86Box: SB 1.0 = DSP 1.05, SB 1.5 = DSP 2.00).
  IMF/MID auf SB16/AWE in 86Box stumm (vom Nutzer gemeldet): echter OPL3 –
  im OPL3-Betrieb (105h = 1) braucht jeder Kanal in C0h die Ausgänge L/R
  (Bits 4–5); IMF ist für den OPL2 geschrieben -> PLAY schaltet für IMF
  den OPL3-Betrieb nicht ein (`fm_start(0)`), MIDI über FM nutzt ihn mit
  C0h | 30h. QEMUs AdLib (OPL2, 38Ah fehlt) zeigte das nicht. MIDI geht
  bei erkanntem MPU-401 dorthin – in 86Box nur hörbar mit eingestelltem
  MIDI-Ausgang, sonst `PLAY /FM`. SB16.SYS stellt einen FM-Regler, der
  beim Start auf 0 steht, auf 75 %. Danach vom Nutzer in 86Box bestätigt:
  IMF/MID spielen auf SB16/AWE.
  ATDOS.SYS jetzt 73,6 KB (Grenze 80 KB des Diskettenladers) – Platz wird
  knapp, Seltenes nach KTEXT2 verschieben.
  Offen: ISA-PnP
  (SB16 PnP/AWE32 PnP, nach der Intel/Microsoft-Spezifikation), AWE32-
  Wavetable (6.0). SBEMU/VSBHDA (GPL) nur als Nachschlagewerk für die
  virtuelle SB der DOS-Boxen in 6.0.
- SCSI-Wechselplatten (vom Nutzer auf dem 486 gemeldet, für später):
  ZIP ohne Medium beim Start lässt sich danach nicht mehr einlegen –
  NCR8XX/AHA154X/NCR5380 legen eine Platte nur an, wenn READ CAPACITY
  klappt (sonst „(nicht bereit)“, keine Einheit, kein Buchstabe). Nötig:
  Einheit trotzdem anlegen, wenn INQUIRY Byte 1 Bit 7 (RMB) gesetzt ist
  (Größe offen), und Wechsel erkennen wie bei der Diskette (`blk_media`,
  neu einhängen; SCSI meldet ihn als Unit Attention 6/28h, kein Medium
  2/3Ah -> DRV_ERR_NOTREADY). Gilt für ZIP, JAZ, MO, SyQuest.
- Laufwerkszugriffe asynchron statt synchron (andere Tasks laufen weiter)
- Idee „AT-DOS 5.1“ (vom Nutzer zurückgestellt, DOS-Boxen in 6.0 gehen
  vor): Kompatibilitätsbox wie bei OS/2 1.x – eine einzelne
  Real-Mode-DOS-Sitzung unter 640 KB (z. B. MS-DOS 6 von der SCSI-Platte),
  Rückweg in den Real Mode per CPU-Reset mit Shutdown-Code im CMOS, Box im
  Hintergrund eingefroren. Erweiterung der Zeitlinie (Spezifikation sieht
  Altkompatibilität erst mit DOS 6.0/V86 vor).
- Codesegmente zwischen Instanzen desselben Programms teilen (FX-Kopf muss
  Code und Daten getrennt beschreiben)
- FAPI unter MS-DOS (Wunsch des Nutzers, „EXE darf größer werden“), wie
  die Family API von OS/2 mit BIND, erledigt: Programme rufen die FAPI
  über `call far [2]` (apps/fapi.h, 4 Byte); crt0 beginnt mit `jmp short`
  über den Gate-Zeiger bei Offset 2 (`dw 0, 0023h`) – crt0.o muss zuerst
  gelinkt werden. `mkfx.py --bind=dstub.bin,dstub.map`: statt tools/
  stub.asm kommt `stub/` davor (scrt.asm + fapidos.c, Real Mode, `-0`),
  aufgefüllt um BSS und 1,5 KB Stack (MZ-SS:SP zeigt ans Stubende),
  Stub-Offset 2 = Abstand zum FX-Kopf, FX-Flag `FX_FLAG_DOS`, MZ-minalloc
  bis zum Ende des 64-KB-Programmsegments. DOS lädt den ganzen Rumpf;
  der Stub nimmt das Abbild an Ort und Stelle (image_off - 64 ab CS),
  verkleinert seinen Block (4Ah), nullt BSS, trägt `gate_entry` in den
  Gate-Zeiger ein, setzt INT 23h (Strg+C nur merken) und INT 24h (ohne
  Rückfrage „Fehler“, Code merken -> NOTREADY/WP) und springt hinein.
  `gate_entry` wechselt auf den Stub-Stack und ruft `fapi_dos()`;
  Signale wie im Kernel (Rahmen merken, in sig_tramp springen,
  DosSigReturn stellt zurück). Übersetzt: Dateien (Konsole: LF -> CR LF),
  FindEntry (fortlaufend 4Fh), QCurDir, Umgebung aus dem PSP, ExecPgm
  (4Bh, .EXE anhängen, BG geht nicht), Kbd über INT 16h (Strg+C/Untbr ->
  SIG_BREAK oder Ende 255), Vio direkt in B800/B000, VioSetMode(1)
  sichert den Bildschirm, VioGetBuf als DOS-Block, Grafik über INT 10h +
  Register (Mode X wie im Kernel), Maus INT 33h, QFSInfo (MSCDEX für CD),
  SysInfo aus BIOS. Nicht: Sitzungen, Pipes, DASD, SUBST/JOIN, SetEnv,
  Tastaturbelegung, Zwischenablage (liefert 0). `BINDAPPS` in build.sh.
  Geprüft in DOSBox (Textwerkzeuge mit Umleitung; DOSBox-Shell kann keine
  `|`, ATTRIB ist dort ein interner Befehl) und DOSBox-X mit `autotype`
  und `tools/snap.asm` (TSR: F12 schreibt B800 nach SNAPn.BIN, Anzeige
  `tools/snapshow.py`): EDIT, FM (Betrachter, Befehlszeile über COMSPEC),
  LESS, GRAFIK in allen drei Modi. Unter AT-DOS unverändert.
  Strg+C und Maus unter DOS noch nicht geprüft.
- Lange Namen auf FAT – Entscheidung des Nutzers: Bonus, erst wenn 5.0 alle
  nötigen Funktionen hat, aber auf jeden Fall vor der DOS-Box von 5.1.
  Reihenfolge also: 5.0 komplett -> lange Namen -> 5.1 (Kompatibilitätsbox).
  IBM/OS/2-Modell, also
  `.LONGNAME` als erweitertes Attribut in `EA DATA. SF`, nicht VFAT. Grund:
  MS-DOS-Werkzeuge (CHKDSK, ScanDisk) sehen nur eine normale Datei, während
  sie VFAT-Einträge als Fehler „reparieren“. Dazu ein Prüfprogramm, das
  verwaiste EAs aufräumt. Zusätzlich (Entscheidung des Nutzers): `fat.c`
  liest VFAT-Namen (Windows 95) mindestens, damit Platten und Disketten von
  dort lange Namen zeigen; geschrieben wird weiter per EA. Merker im
  FX-Kopf „kann lange Namen“ (Entscheidung des Nutzers, wie OS/2 1.2):
  Programme ohne Merker bekommen bei der Dateisuche nur 8.3-Kurznamen
  (`BEWERB~1.TXT`), damit ihre Puffer nicht überlaufen; eigene Werkzeuge
  bekommen den Merker, sobald sie lange Namen sauber verarbeiten.
  Entscheidung des Nutzers (Verfahren): **EA-Datei mit Rückverweis**. Grund:
  OS/2 legt die EA-Nummer im Verzeichniseintrag bei Offset 14h ab – genau
  diese zwei Bytes sind bei FAT32 (Pflicht in 6.0) die oberen 16 Bit des
  Startclusters. Deshalb steht im Verzeichniseintrag nichts; `EA DATA. SF`
  speichert zu jedem Satz, wem er gehört (Startcluster des Verzeichnisses +
  8.3-Kurzname), gleich auf FAT12/16 und FAT32. Folgen für den Bau: Cache
  im Kernel (Verzeichnis -> lange Namen), sonst kostet jede Suche
  Plattenzugriffe; Umbenennen/Verschieben/Löschen über das FAPI pflegt den
  Rückverweis mit; das Prüfprogramm (UFAT) findet verwaiste Sätze, deren
  Kurzname im Verzeichnis fehlt, und Dateien, die alte DOS-Werkzeuge
  umbenannt haben. Nicht OS/2-kompatibel (OS/2 sieht die langen Namen
  nicht), dafür FAT32-fest. Hintergrund, warum
  Microsoft VFAT statt EAs nahm: Suche nach langem Namen ohne zusätzliche
  Plattenzugriffe (Name im selben Verzeichnissektor), keine verwaisten EAs
  durch alte DOS-Werkzeuge, Name gehört ins Verzeichnis (wie HPFS/NTFS),
  dazu der Bruch mit IBM 1990–92.
  Stufe 1 erledigt (in QEMU mit einer per mtools beschriebenen VFAT-
  Diskette geprüft): `fat.c` sammelt VFAT-Einträge (`lfn_feed`/
  `lfn_valid`, UCS-2 -> CP437 über `lat2cp`, Prüfsumme `sum83`),
  `lookup_name` findet Pfadteile über 8.3 oder langen Namen (ASCII ohne
  Groß/klein), `fat_readdir` liefert `name` (lang) und `sname` (8.3, ""
  wenn gleich; NT-Kleinschreibungsbits Byte 12 beachtet), `drop_lfn` in
  remove/rmdir/move, neuer IFS-Aufruf `shortpath`. Kernel: IFS_PATHLEN
  261, `task.lfn` aus `FX_FLAG_LONGNAMES` (0x0002, `mkfx.py --lfn`,
  build.sh `LFNAPPS`), FINDENTRY gibt alten Programmen `sname`, QCURDIR
  ihnen den 8.3-Pfad (höchstens 130 Byte). COMMAND und util.c: `"..."` in
  word/next_word, Endung nach dem letzten Punkt, FAPI_CURDIRLEN 263.
  Vom Nutzer in 86Box bestätigt: auf dem Mac angelegter Ordner mit langem
  Namen erscheint unter AT-DOS.
  Aktuelles Verzeichnis in Originalschreibung (IFS-Aufruf `namepath`,
  shortn=0): CHDIR speichert die Namen vom Datenträger, `resolve_v` macht
  daraus beim Auflösen wieder Großbuchstaben, `path_in` vergleicht ohne
  Groß/klein. `CD Neuer Ordner` ohne Anführungszeichen nimmt wie bei
  Windows den Rest der Zeile (Wunsch des Nutzers nach Test in 86Box).
  In 86Box bestätigt (Ordner vom Mac): `cd Neuer Ordner\`, Aufforderung
  `A:\Neuer Ordner`. macOS legt `.`/`..` versteckt an – DIR zeigt genau
  diese beiden immer, andere versteckte (z. B. `.fseventsd`) nur mit /A.
  Merker `FX_FLAG_LONGNAMES` (build.sh `LFNAPPS`): COMMAND, ATTRIB, TREE,
  DELTREE, GREP, PRINT, XCOPY, MOVE, JOIN, EDIT.
  Musterpuffer auf FAPI_PATHLEN, `.`/`..` nur noch genau erkennen (nicht
  jeder Name mit Punkt vorn), Endung nach dem letzten Punkt auch in GREP.
  ATTRIB mit Verzeichnisnamen zeigt/setzt wie NT das Verzeichnis selbst.
  Kernelgröße: erledigt mit KCODE2 (siehe Konventionen).
  Stufe 2 erledigt (in QEMU geprüft, mtools liest alles, fsck sauber):
  Kernel reicht den letzten Pfadteil in Originalschreibung in `ifs_orig`
  (`set_orig` vor create/mkdir/move), `LONGNAMES=` -> `lfn_cfg`. fat.c:
  `vol[].vfat` beim Einhängen (VFAT oder AUTO + lange Einträge im Stamm);
  `name_for`: 8.3 ganz groß -> nur 8.3, ganz klein je Teil -> NT-Bits
  Byte 12, gemischt oder lang -> VFAT-Einträge (`new_entry_lfn`,
  `dir_slots` n freie Plätze am Stück, Unterverzeichnis wächst) und
  Kurzname `gen_short` (NAME~n.EXT, eindeutig). Move gibt den alten Platz
  vorher frei; nur Schreibung ändern (REN kurz.txt Kurz.TXT) erlaubt.
  Stufe 3 erledigt (in QEMU geprüft: frische Diskette und C:, XCOPY von
  VFAT nach EA, REN/MOVE/DEL, Neustart, CHKDSK und fsck sauber):
  `EA DATA. SF` im Stamm (versteckt+System), Sätze zu 32-Byte-Einheiten:
  'L'/0 frei, Einheiten, Verzeichnis-Startcluster (32 Bit), 8.3-Name
  (Rückverweis), Länge, Name. `ea_open` (legt beim ersten langen Namen
  an), `ea_load` (Namen eines Verzeichnisses in ein 16-KB-Kernelsegment,
  vor jeder Schleife über ein Verzeichnis – nie mitten drin, DMABUF!),
  `ea_find`, `ea_add` (freien Satz wiederverwenden, sonst anhängen),
  `ea_del`; Pflege in create/mkdir (`new_entry_n`), remove/rmdir, move.
  `vol[].vfat = 0` heißt EA-Laufwerk. AUTO beim Einhängen: EA-Datei im
  Stamm -> EA (geht vor), sonst lange VFAT-Einträge -> VFAT, sonst EA.
  `dir_cache_reset` vergisst EA-Datei und Namen (Wechsel, Neueinhängen).
  Gemischte Schreibung bei 8.3-Namen wird ebenfalls als Satz gespeichert.
  Merker jetzt auch XCOPY und MOVE; `abs_path` (util.c) schreibt nicht
  mehr groß (nur den Laufwerksbuchstaben). FAPI_ARGLEN 128 (127 Zeichen
  wie MS-DOS), COMMAND-Eingabe bis 126 Zeichen, `\b` am Zeilenanfang
  springt in vio.c in die vorige Zeile. DIR /B und /W. Stufe 4 erledigt
  (in QEMU geprüft): `apps/ufat_lfn.c` – jedes CHKDSK prüft VFAT-Ketten
  (Ordnung, Prüfsumme, direkt vor dem passenden 8.3-Eintrag) und die Sätze
  in `EA DATA. SF` (Verzeichnis + 8.3-Name vorhanden?); /F gibt Verwaistes
  frei. Tab-Ergänzung in COMMAND quotet wie bash: Namen mit Leerzeichen
  bekommen `"`, Datei mit schließendem `"`, Verzeichnis bleibt offen;
  `type "T`+Tab sucht ohne das Anführungszeichen. EDIT hat den Merker
  (in QEMU geprüft): Dateiliste als Namenspool (12 KB, `ent_off[]`),
  Anzeige 12 Zellen, länger -> 11 Zeichen + `\xAF`; Feld und Speichern
  unter mit FAPI_PATHLEN, Pfade nicht mehr groß geschrieben, Titel nur
  bei 8.3-Namen groß; `EDIT Brief an Hans.txt` nimmt wie CD den Rest der
  Zeile. Vom Nutzer in 86Box bestätigt (Dialog mit langem Namen).
  Schreibformat je Laufwerk (Entscheidung des Nutzers): `LONGNAMES=AUTO|
  VFAT|EA` in CONFIG.SYS, Vorgabe AUTO. FAT32 immer VFAT (MS-DOS 6 liest
  FAT32 ohnehin nicht, Austausch mit Windows 95/98 in beide Richtungen).
  FAT12/16: `EA DATA. SF` vorhanden -> EA; VFAT-Einträge vorhanden (beim
  Einhängen im Stamm geprüft) -> VFAT; sonst EA. Nie beide Formate auf
  einem Laufwerk (sonst wechselt eine verschobene Datei das Format, UFAT
  müsste beides prüfen). VFAT-Schreiben braucht: Kurznamen `~1`,
  Prüfsumme, zusammenhängend freie Verzeichnisplätze, lange Einträge beim
  Löschen/Umbenennen mit entfernen, CP437/850 <-> UCS-2. Die EA-Datei mit
  Rückverweis gilt damit praktisch für FAT12/16 und Disketten.
- 287 je Task (in QEMU geprüft): Der 286 setzt bei jedem Hardware-
  Taskwechsel MSW.TS – vorher führte daher jeder 287-Befehl eines
  Programms zu #NM und Abbruch. Jetzt `exc7` (start.asm): CLTS,
  `fpu_nm()` (kernel.c) speichert den Zustand des bisherigen Besitzers
  (`fpu_owner`, FNSAVE 94 Byte in `fpu_area[NTASK]`) und lädt den eigenen
  bzw. FNINIT beim ersten Mal (`fpu_used`); ohne 287 Abbruch wie bisher.
  `fpu_pm_init` beim Start: MSW.MP (auch WAIT prüft TS), FSETPM (der 287
  muss den Protected Mode kennen), IRQ 13 frei. `irq13`: Port F0h
  beschreiben (sonst hält das AT-Latch BUSY und der nächste 287-Befehl
  wartet ewig), aus Ring 3 wie Ausnahme 16 „#MF Koprozessorfehler“ –
  Programm wird beendet. `task_end` -> `fpu_task_end` (Besitz weg,
  `fpu_reset` = CLTS, FNINIT, TS wieder setzen). Demo `\DEMO\FPUTEST
  Start Schritte [/W] [/E]`: zählt im 287 abwechselnd +1/-1 mit DosYield
  dazwischen; zwei gleichzeitig (Stapeldatei mit `start fputest 100
  300000 /w` + `fputest 7 300000`) – Gegenprobe ohne Tausch meldet FEHLER
  bei Schritt 1. `/E`: 1/0 mit unmaskierter Ausnahme -> Abbruch, 287 geht
  danach weiter. Vom Nutzer auf dem SCAT (287XL) bestätigt: 300.000
  Schritte OK, `/e` dreimal hintereinander jeweils #MF. QEMU löst IRQ 13
  nur beim ersten Mal aus (IGNNE nach dem Schreiben auf F0h) – Eigenheit
  von QEMU, nicht des Kernels. Auf dem 286 dauern 300.000 Schritte
  einige Minuten.
- Dateien gleichzeitig: Leser und Schreiber schließen sich aus (FAPI_OPEN
  prüft `open_for_write`, CREATE/Anhängen `in_use`), daher können die
  Größen in den Handles nie auseinanderlaufen (geprüft: `echo >>` auf eine
  Datei, die TYPE gerade liest -> „wird gerade benutzt“). Für Protokolle
  mit `tail -f` bräuchte es Freigabemodi (wie OS/2 DENYNONE) und eine
  gemeinsame Dateigröße – erst bei Bedarf.
- Zweite Referenzmaschine: 286 mit 10–12 MHz für zeitgemäße Geschwindigkeit.
  Nutzer: SCAT mit 12 MHz in 86Box bootet AT-DOS schnell.
- Leitlinie DOS 6.0 (Entscheidung des Nutzers): ein **stabiles 16-Bit-OS**,
  das aus dem 386 herausholt, was geht – Paging (Schutz, virtueller Speicher),
  V86-Boxen für echte DOS-Programme, präemptives Multitasking. Stabilität vor
  Tempo der Entwicklung, daher OS/2-2.0-Art statt WfW-Art: kein 16-Bit-Kern
  als gemeinsame „System-VM“ über einem 32-Bit-VMM, sondern ein Kernel, in
  dem FAPI-Programme und DOS-Boxen gleichberechtigt, voneinander geschützt
  und präemptiv laufen. Für Programme bleibt alles 16 Bit (FAPI wie 5.0).
  **Kein 32-Bit-Umbau wie OS/2 2.0** (der hat die Rechner seiner Zeit
  überfordert): Kernel bleibt 16 Bit und schlank; 386-Strukturen
  (Seitentabellen, 32-Bit-TSS/Gates) sind nur Daten, 32-Bit-Code nur dort,
  wo die CPU ihn erzwingt (Eintritt/Austritt V86, Interrupts aus V86 über
  32-Bit-Gates) – als kleine, abgegrenzte Stücke. Zielhardware: läuft gut
  auf einem 386SX mit 2 MB. Flache 32-Bit-Programme erst mit 7.0.
  Vorarbeit in 5.0 erledigt: Kernel-Code und -Daten in getrennten Segmenten
  (wie der 16-Bit-Kernel von OS/2 1.x); weitere Tabellen, Stacks und Puffer
  können später in eigene Segmente (`kseg_alloc`).
- Hardware-Zeitraum DOS 6.0 (Entscheidung des Nutzers): Die Zeitlinie muss
  nicht genau stimmen. Obergrenze, bis zu der DOS laufen muss: das Board des
  Nutzers **ASUS P3B-F** (i440BX, PIIX4E, Slot 1, Pentium II/III, AGP, USB,
  APM/ACPI, bis 1 GB SDRAM, Super-I/O W83977EF; in 86Box als `p3bf`).
  Folgen für 6.0: Speicher per E820h bis 1 GB (Bitmap in 4-KB-Seiten:
  32 KB; Anheben auf 2 GB und mehr – Entscheidung des Nutzers – nach
  hinten, erst wenn das Wichtige durch ist: bis 2 GB wachsen nur Bitmap
  und Seitentabellen, darüber stößt der Kernel-Alias ab 80000000h an,
  dann Fenster statt voller Spiegelung bzw. 7.0; XMS der Boxen bekommt
  trotzdem gleich die 32-Bit-Funktionen 88h/89h/8Fh); PIIX4-IDE im Kompatibilitätsmodus wie heute, dazu Busmaster-DMA
  (SFF-8038i/UDMA); Partitionen über 2 GB -> FAT32 ist Pflicht
  (Entscheidung des Nutzers, wegen des Austauschs mit Windows 95/98),
  im Kernel neben FAT12/16 inkl. VFAT-Namen lesen; AGP-Grafik mit VBE 2.0/3.0 und
  linearem Bildspeicher oberhalb 16 MB (über Seitentabellen/Selektoren);
  Tastatur/Maus an USB über die Legacy-Emulation des BIOS (sieht aus wie
  Port 60h/64h), eigener USB-Stack (UHCI) höchstens später; Ausschalten über
  die 16-Bit-Protected-Mode-Schnittstelle von APM 1.2 (passt zum 16-Bit-
  Kernel, ACPI wäre zu groß); Warteschleifen immer über den PIT messen, nie
  über Befehlszählen (Pentium III ist hunderte Male schneller als der 286).
  Dazu PCI, 53C8xx, VESA VBE 1.2/2.0.
  Test des Nutzers: AT-DOS 5.0 läuft auf dem P3B-F in 86Box (IDE, Diskette,
  Tastatur, Sitzungen); nur der PCI-SCSI-Controller NCR 53C8xx wird nicht
  erkannt – erwartet, dafür gibt es noch keinen Treiber (`NCR8XX.SYS`, 6.0).
- DOS-Sitzungen in DOS 6.0 wie OS/2 2.x (MVDM), Wunsch des Nutzers:
  jede Sitzung eine eigene V86-Maschine (1 MB + HMA, präemptiv, geschützt –
  ein Absturz trifft nur die Sitzung); virtuelle Gerätetreiber (VDDs) für
  Bildschirm, Tastatur, Maus, COM/LPT, Zeitgeber, DMA fangen Ports und
  Speicher ab; eingebautes DOS in der Box, das Dateizugriffe an den Kernel
  weiterreicht (alle AT-DOS-Laufwerke sichtbar, auch FAT32/IFS, für DOS-
  Programme als 8.3); DOS-Einstellungen je Sitzung (EMS/XMS/DPMI_MEMORY_
  LIMIT, DOS_HIGH/UMB, IDLE_SENSITIVITY gegen Warteschleifen, VIDEO_
  RETRACE_EMULATION, HW_TIMER, COM_HOLD …), EMS/XMS/DPMI vom Kernel statt
  EMM386/HIMEM in der Box; „bestimmte DOS-Version“ wie VMB: echtes DOS
  (MS-DOS 6, FreeDOS) aus einem Disketten-/Plattenabbild in einer Sitzung
  starten; Windows 3.1 in einer Sitzung (WIN-OS/2-Weg), später seamless.
- Win16, Optionen (noch nicht entschieden): 1. echtes Windows 3.1 in
  einer DOS-Sitzung (kommt mit den Sitzungen, braucht Windows-Lizenz);
  2. eigenes Win16-Teilsystem auf Basis von Wine (krnl386/user/gdi, wie
  otvdm/winevdm): Win16-Programme sind 16-Bit-PM (NE-Format) und passen
  direkt zum Kernel – NE-Lader legt die Segmente in die LDT, kein V86,
  keine Emulation; Aufwand ist USER/GDI auf einer eigenen Grafikschicht,
  die auch ein Win32-Teilsystem (Wine, 7.0) bräuchte; LGPL als abgegrenzter
  Teil; 3. ReactOS nur als Nachschlagewerk (NTVDM für DOS, WOW unfertig).
  Vorschlag: erst 1, dann 2 als eigenes Vorhaben zusammen mit Win32.
- Grafik in DOS 6.0: Video-BIOS/VESA über eine V86-Umgebung für Moduswechsel
  (allgemeiner VESA-Treiber), Spezialtreiber je Chip (ET4000 usw.) für Tempo.
  DOS-Boxen brauchen einen virtuellen Bildschirm wie VDD (Windows 3.0) bzw.
  VVGA (OS/2 2.0): Hintergrund-Box schreibt per Seitentabelle in einen
  Schatten bei A0000h–BFFFFh, VGA-Ports 3C0h–3DFh über die I/O-Bitmap
  abfangen, Kartenzustand beim Umschalten sichern/herstellen (SVGA-Erweiterungen
  je Chip). Grafik-Boxen im Hintergrund notfalls einfrieren (wie OS/2 2.0 anfangs).
- Reihenfolge nach dem ESDI-Test (abgestimmt mit dem Nutzer): 1. 43/50
  Textzeilen mit `MODE` (erledigt), 2. EDIT fertig (SIG_SHUTDOWN-Nachfrage,
  Dateiliste – erledigt), 3. Treiber mit Interrupt (DevHlp SetIRQ; IDE
  und AHA154X erledigt, Diskette offen), 4. `MOUSE.SYS`,
  danach der ISA-Bereich für 5.0 (Entscheidung des Nutzers): ISA-SCSI
  (NCR 53C400/Trantor T130B, evtl. AHA-1520/AIC-6360) und VGA-Grafik
  wie OS/2 1.x – Standardmodi (12h 640×480×16, 13h 320×200×256) direkt
  über die VGA-Register (kein BIOS im Protected Mode), FAPI-Zugang zum
  Bildspeicher für das Vordergrundprogramm (wie `VioGetPhysBuf`), beim
  Sitzungswechsel sichern oder Programm einfrieren. PCI (53C8xx) und
  VESA bleiben bei 6.0. Später: FDISK, DISKCOPY, SUBST, UNDELETE usw.,
  lange Namen auf FAT, dann 5.1.
- Hardware-Referenz 86Box (Quellcode, GPL v2): lesen, um Register und
  Verhalten zu verstehen, nie Code übernehmen (Lizenz offen). Datenblätter
  bleiben erste Quelle; der Emulator ist nachsichtiger als echte Hardware.
  Interessant für 5.0: NCR 5380/53C400 (Trantor T130B,
  Rancho RT1000B, Longshine LCS-6821N), AHA-1520A/AIC-6360 (PIO).
  Adaptec 154x und BusLogic laufen in 86Box über denselben Code (`scsi_x54x.c`).
- Speicher in DOS 6.0, Phase 1a erledigt (Entscheidung des Nutzers:
  durchgehend 32 Bit bis 1 GB, alles in 4-KB-Seiten – wegen DOS-Boxen mit
  DPMI/XMS und dem Weg zu 7.0; der SX ist dafür nicht mehr Maßstab).
  start.asm fragt im Real Mode E820h ab (`e820_map`, bis 32 Einträge zu
  20 Byte, `e820_n`), sonst E801h (`e801_lo` KB 1–16 MB, `e801_hi`
  64-KB-Blöcke darüber), zuletzt 88h. mem.c: `pg_t` = unsigned long
  (Seitennummer), `mem_alloc(n)` -> erste Seite (0 = keiner),
  `mem_alloc_low` unter 16 MB (kseg_dma, ISA-DMA), `mem_free(p, n)`,
  `PAGES(bytes)`, `PG_LIN(p)`, Suche ab Merker `hint`. Bitmap bis 256 MB
  (8 KB) fest bei 9000h, darüber im ersten passenden RAM ab 1 MB
  (Seiten reserviert); `MEM_MAX_PG` = 1 GB. Unter 640 KB frei ab
  Kernelende (auf Seiten gerundet). `put_desc` setzt Basis 24–31 (Byte
  7), `desc_base()` liest sie (seg_free). Alle Felder mit Seitennummern
  sind `pg_t` (task img/code_unit, ses unit/save/gfx_save, bc_unit,
  ld_cu); Zähler bleiben `unsigned` (Segmente <= 16 Seiten, nur
  GFX-Puffer mehr). Lehre: Watcom kürzt unsigned long -> unsigned ohne
  Warnung, `(unsigned long)u << 8` in vio.c (Sitzungspuffer) übersehen
  -> #GP erst, als der Speicher über 16 MB lag. Test: `hint` testweise
  auf 256 MB setzen (alles wird oben vergeben). FAPI_MEMINFO: CX =
  Größe, ab 18 Byte `total_kb32`/`free_kb32` (alte Programme 10 Byte,
  gekappt; fapi.h übergibt sizeof). In QEMU geprüft: 2 MB bis 1 GB, 2 GB
  -> 1 GB, isapc; mit allem oberhalb 256 MB: XCOPY über NCR, PLAY (DMA
  unter 16 MB), GRAFIK 12 mit Sitzungswechsel, fsck sauber. Rundung auf
  4 KB kostet bei 2 MB rund 45 KB (frei 976 statt 1.026 KB).
  Erstes Kernel-Codesegment danach bei 63,8 KB -> mem.c nach KTEXT2
  (Einsprünge `__far` in mem.h, `set_desc` und der Cache-Rückruf
  `mem_reclaim` über Thunks `kf_set_desc`/`kf_reclaim`), jetzt 60,6 KB.
  `-os` statt `-ox` brachte nur 0,7 KB – nicht genommen. Geprüft: 3 MB,
  CACHE=1536, TASKB bekommt 640 KB, Cache 1.536 -> 1.408 KB.
  Phase 1b erledigt (in QEMU geprüft): `kernel/paging.c` in KTEXT2,
  `paging_init()` direkt nach mem_init: Seitenverzeichnis + je 4 MB eine
  Seitentabelle (einzeln per mem_alloc, beschrieben über SCRATCH/
  SCRATCH2), 1:1 von 0 bis `mem_top_pg()` (mindestens das erste MB),
  alle Seiten P|W|U (Schutz vorerst weiter über Segmente), dann CR3 und
  CR0.PG (pragma aux `pg_enable`). Die 286-TSS haben kein CR3: ein
  Seitenverzeichnis für alle Tasks. Kosten: 8 KB bei 2 MB, 1 MB bei
  1 GB. start.asm: Vektor 14 jetzt EXC_ERR (386 legt einen Fehlercode
  ab), `fault()` meldet „#PF Seitenfehler bei CS:IP, Adresse“ aus CR2
  (eigener Text, die statische Meldung bleibt unverändert). Startzeile
  zeigt „Paging an (n Seitentabellen)“, freier Speicher ab 64 MB in MB.
  Geprüft: CR0 = 8000001Bh, Zugriff auf 256 MB bei 4 MB RAM ->
  #PF mit Adresse 10000004h; mit allen Vergaben oberhalb 256 MB XCOPY
  über NCR, PLAY, GRAFIK 12 + Sitzungswechsel, SHUTDOWN, fsck sauber;
  isapc mit 2 MB. Vom Nutzer in 86Box bestätigt: 486 mit 64 MB („Paging
  an (16 Seitentabellen)“) und ASUS P3B-F mit 256 MB (255 MB frei, 64
  Seitentabellen; mit FM Dateien über NCR8XX aufs ZIP kopiert, CHKDSK
  ohne Fehler); auf dem P3B-F MEM 261.676 KB erkannt, 259.944 KB frei,
  MSD listet alle PCI-Geräte inkl. S3 auf Bus 1 hinter der AGP-Brücke und
  NCR 53C875 (IRQ 10). BIOS-Name zeigte dort „. Awar,Copyright (C)
  1984-98, Award Softwa“ – `rom_str` beginnt jetzt bei „(C)“/„Copyright“,
  wenn die Folge eines davon enthält (davor steht oft Müll); Cache
  bleibt ohne CACHE= bei 1 MB (dynamischer Cache = freier Speicher kommt
  später). Damit ist Phase 1 fertig; nächste Phase 2
  (präemptiv, Sperren je Gerät, asynchrone Plattenzugriffe).
- DOS-Boxen, Phase 3 Stufe 1 erledigt (in QEMU geprüft): Grundlage V86.
  Umbau des Kerns dafür: (1) **Kernel-Alias**: paging.c trägt jede
  Seitentabelle zusätzlich bei Verzeichniseintrag 512+i ein (physisch
  ab linear 80000000h = `KALIAS`), `alias_on()` rechnet danach alle
  Speicherdeskriptoren der GDT um (Gates nicht) und lädt GDTR/IDTR neu,
  `put_desc` addiert KALIAS selbst, sobald `k_alias` gesetzt ist,
  `desc_base` zieht ihn ab. Physische Adressen (`buf_phys`, DMA) bleiben
  physisch. Linear 0–4 MB gehört damit frei der laufenden Box.
  Lehre: LGDT/LIDT im 16-Bit-Code laden nur 24 Bit Basis – Präfix 66h
  nötig (`k2_lgdt`), sonst bleibt die GDT bei 0002xxxxh. (2) **Alle TSS
  386** (`struct tss386` in kern.h, 104 Byte, `tss_init`, `ACC_TSS`
  89h): CR3 je Task (die CPU lädt es beim Wechsel, schreibt es aber nie
  zurück – deshalb auch `tss_k.cr3` nach paging_init setzen), iomap
  hinter der Grenze (jeder Port aus Ring 3/V86 = #GP). (3) **Alle Gates
  386-Interrupt-Gates** (8Eh): nur sie führen aus V86 in den Kernel.
  Rahmen 32 Bit (EIP, CS, EFLAGS je 4 Byte, aus V86 dazu ESP, SS, ES,
  DS, FS, GS), alle Handler enden mit IRETD, sichern mit PUSHAD (Programme
  in Boxen nutzen 32-Bit-Register). irq0/irq13 prüfen „aus Ring 3“ jetzt
  als EFLAGS.VM oder CS-RPL 3. EXC legt einen 4-Byte-Fehlercode ab.
  Watcom benutzt im Kernel FS/GS (sieht man im Registerabzug) – die
  386-TSS sichern sie jetzt mit.
  Box (`kernel/v86.c`, KTEXT2): .COM -> `load_com` (kernel.c; COMMAND
  lehnt .COM nicht mehr ab), `v86_create`: Seitenverzeichnis (0 = eigene
  Tabelle, ab 512 wie der Kernel), Seitentabelle mit 640 KB eigenen
  Seiten (einzeln, nicht zusammenhängend), ROM C0000–FFFFF 1:1 nur
  lesen, A0000–BFFFF fehlt (Stufe 2), eigene Seite F0000 mit Stummeln
  „ARPL n; IRET“ je Vektor (ARPL ist in V86 #UD -> Dienst n, wie die
  BIOS-Haken von Windows 386), HMA = Umlauf auf 0–64 KB. PSP bei
  1000:0000 (Befehlszeile mit Leerzeichen vorn wie DOS), TSS mit VM|IF,
  IOPL 0. Monitor `v86_exc` aus exc_common (EFLAGS.VM): #GP -> CLI/STI/
  PUSHF(D)/POPF(D)/INT n/INT3/INTO/IRET(D)/HLT/IN/OUT/INS/OUTS mit
  Präfixen, virtuelles IF/IOPL/NT in `vfl[t]`; INT n über die IVT der
  Box, zeigt der Vektor noch auf den Stummel, gleich den Dienst (ein
  Trap statt drei); #DE/#DB/INT3/INTO/BOUND in die IVT der Box; sonst
  (#PF, unbekannter Befehl) beendet `fault()` die Box (`fault_vm`).
  Dienste über die Sitzung des Tasks (`box_ctl` in kernel.c für Task,
  Sitzung, Schlafen, Ende). Box-Speicher über das rohe Fenster `VWIN`
  (Basis ohne Alias, nur im Kontext der Box gültig). `v86_destroy`
  schaltet auf das CR3 des Kernels um, wenn die Box sich selbst beendet,
  bevor ihr Seitenverzeichnis frei wird.
  Lehren: in exc_common erst PUSHAD, dann BP setzen (sonst verliert die
  Box BP); nach `pop ds` ist DS = 0 (aus V86 genullt) – QEMU liest dann
  ohne #GP aus linear 0 der Box, echte CPUs lösen #GP aus: danach nur
  über SS zugreifen. Geprüft: HALLO (Ausgabe, IF, eigener INT 60h,
  32-Bit-Register über Taktinterrupts, Befehlszeile, ERRORLEVEL 5), zwei
  Boxen mit Rechenschleife gleichzeitig in eigenen Sitzungen (COMMAND
  bleibt bedienbar, Strg+C beendet je eine, Speicher kommt ganz zurück),
  #PF auf B800 und `mov eax,cr0` beenden nur die Box; Regression XCOPY
  IDE <-> NCR8XX, CHKDSK, GRAFIK 12, SHUTDOWN, fsck sauber; isapc 2 MB,
  512 MB, alle Vergaben über 256 MB. Erstes Codesegment jetzt 63,3 KB –
  vor weiterem Code Seltenes nach KTEXT2 verschieben. Offen: MEM zählt
  Box-Speicher unter „BIOS, Boot, Puffer“. Vom Nutzer in 86Box
  bestätigt: drei HALLO-Boxen gleichzeitig, dabei Dateien über NCR8XX
  kopiert, CHKDSK ohne Fehler; auf dem 386SX mit 2 MB zwei HALLO-Boxen
  gleichzeitig, dabei kopiert.
  Lehre (Kernel-Panik auf der 486 in 86Box: #GP bei `task_jump`, Fehler-
  code 00E0h = Puffer von Sitzung 3): das 386-TSS sichert ES/FS/GS und
  lädt sie beim Zurückwechseln; Watcom hält Fernzeiger in FS/GS (z. B.
  Sitzungspuffer), `vio_close` löscht den Deskriptor -> beim Wechsel
  zurück in einen Task mit altem FS ein leerer Deskriptor. `task_jump`
  legt jetzt ES = KDATA, FS = GS = 0 ins TSS und holt FS/GS danach vom
  Stack zurück, nur wenn noch gültig (`sanitize_sel`). QEMU traf die Lage
  im Nachstellversuch nicht. Danach vom Nutzer auf der 486 bestätigt:
  drei Box-Sitzungen beendet, dabei kopiert, CHKDSK ohne Fehler, keine
  Panik mehr.
  Stufe 2 erledigt (in QEMU geprüft): Bildschirm – `v86_start` (aus
  task_create, sobald die Sitzung feststeht) schaltet die Sitzung ins
  Vollbild (`vio_setmode(s, 1, t)`, wie EDIT) und trägt die zwei Seiten
  des Sitzungspuffers (`vio_buf_lin`) bei B8000/B9000 in die Seiten-
  tabelle der Box ein; A0000–BFFFF sonst eine Ersatzseite. Änderungen
  erkennt `v86_poll` (im Scheduler vor vio_flush) am Dirty-Bit der beiden
  PTE: löschen, `vio_dirty(s)`; läuft gerade die Box selbst, CR3 neu
  laden (sonst hält der TLB D=1 und die CPU setzt es nie wieder). Ohne
  Kernelsperre, SCRATCH mit win_save/restore. `v86_destroy` gibt B8/B9
  nicht frei, merkt sich den Text bis zur Cursorzeile (`capture`), nach
  `vio_task_end` gibt `v86_replay` ihn in der Sitzung des Aufrufers aus
  (wie die NT-Konsole; sonst verschwände die Ausgabe eines DOS-Programms
  mit dem Vollbild). INT 10h/21h-Ausgabe schreibt selbst in B800 (`vtty`,
  Cursor im BIOS-Datenbereich 450h, `cur_put` -> vio_setcur), kein
  vio_tty/ANSI mehr. BDA beim Anlegen aus dem echten (physisch 400h)
  kopiert, Bildschirmfelder 80x25, Takt 46Ch aus der Echtzeituhr
  (`rtc_ticks`, ohne 32-Bit-Division). Takt: `account` rechnet 100 Hz in
  18,2 Hz um (vt_acc in Tausendsteln); solange INT 8 und 1Ch auf den
  Stummeln stehen, zählt nur 46Ch weiter, sonst virtueller INT 8 (eigener
  Stummel F000:0400 „ARPL 8; INT 1Ch; IRET“). Tastatur: kbd_irq ruft
  `v86_key` (außer Alt+Esc – sonst beendete Esc die Box beim Wegschalten,
  im Test passiert) -> Scancode-Ring je Box im Vordergrund; INT 9 nur
  zugestellt, wenn umgebogen, sonst holt Port 60h aus dem Ring; INT 16h
  weiter über kbd_get (deutsche Belegung). Zustellung `inject` beim
  Rücksprung (Ende von v86_exc und aus irq0 nach preempt: `v86_tick`,
  Zeiger auf EIP im Rahmen), nur bei virtuellem IF, `vimr`/`visr`
  (EOI an Port 20h löscht das niedrigste Bit, die Standard-Stummel für 8/9
  auch). Ports: 20h/21h, 60h/64h, 61h (Refresh-Bit kippt), 40h–43h
  (Zähler läuft), 3DAh/3BAh (Rücklauf kippt), 3D4h/3D5h Cursor 0Eh/0Fh;
  Rest FFh/verpufft. BIOS: INT 10h 00/01/02/03/05/06/07/08/09/0A/0E/0F/
  11/12/13/1A, 11h, 12h, 13h (CF), 15h (86h warten, 88h = 0), 16h (00h
  liefert graue Tasten mit AL 0 wie das AT-BIOS), 1Ah 00–04, 33h AX=0;
  INT 21h dazu 2Ah/2Ch. Test `\DEMO\BOXTEST.COM` (apps/boxtest.asm):
  B800 mit Farben, INT 1Ch zählt (131 Takte in ~7 s), INT 9 mit Port 60h
  und EOI, Rücklauf-Schleife, Rollen per INT 10h; im Hintergrund per
  START läuft der Takt weiter, Alt+Esc hin und zurück, Strg+C, Speicher
  danach wie vorher; XCOPY über NCR8XX neben BOXTEST, fsck sauber; isapc
  2 MB. Beobachtung: eine Box, die an 3DAh wartet, trappt pausenlos und
  nimmt anderen ihre Zeitscheibe – später IDLE_SENSITIVITY wie OS/2.
  Vom Nutzer in 86Box (386SX, 4 MB) bestätigt: zwei BOXTEST gleichzeitig, dabei Dateien
  auf D: kopiert, CHKDSK ohne Fehler.
  Stufe 3, erster Schritt erledigt (in QEMU geprüft): FreeDOS in der
  Box. FreeDOS-Kern (d6791ad, 386, FAT32, ohne UPX) und FreeCOM deutsch
  (d52ba90) vom Nutzer erlaubt aus dem Quelltext gebaut (GitHub-Releases
  und ibiblio sperrt das Netz der Umgebung, git geht), als Binärdateien in
  `freedos/` (README.txt mit Herkunft, COPYING, `tools/freedos.sh` baut
  neu). build.sh legt `build/freedos.img` an (360 KB, `mformat -B
  fat12com.bin`, KERNEL.SYS zuerst, COMMAND.COM, BOXSHELL.COM, CONFIG.SYS
  mit `SWITCHES=/F /N` und `SHELL=A:\BOXSHELL.COM`, AUTOEXEC.BAT) ->
  `\DOS\BOX\FREEDOS.IMG`; `DOSBOX.CMD` = `START %DOSBOX%`, die Variable
  setzt der Kernel wie COMSPEC. Kernel: Endung `.IMG` -> `load_img`
  (Bootsektor nach 0:7C00, DL = 0, Abbild bleibt offen in `box_img[t]`),
  `box_ctl(BOX_READ)` liest einen Sektor nach `box_sec` (mit Kernel-
  sperre; erst danach legt v86.c VWIN um – ifs_read kann schlafen, eine
  andere Box verbiegt sonst das Fenster). v86.c: INT 13h nur Laufwerk 0
  (00/02/04/08/15/16/17/18, 03 = schreibgeschützt), CHS mit der Geometrie
  aus dem BPB (`v86_disk`), Diskettenparameter bei F000:0800; BDA: ein
  Diskettenlaufwerk, keine Platten (FreeDOS meldet „no hard disks“).
  INT E0h = Box-Schnittstelle (00h Befehlszeile der Box nach ES:DI, 01h
  Box beenden). `apps/boxshell.asm` ist SHELL=: holt die Befehlszeile,
  startet `A:\COMMAND.COM A:\ /E:1024 /K A:\AUTOEXEC.BAT` bzw. `/C
  Befehl`, beendet die Box mit dessen Rückgabewert -> EXIT schließt die
  Box (FreeDOS selbst meldete sonst „Bad or missing Command Interpreter“).
  Strg+C: `v86_fgbox()` – in einer Box mit DOS gehört es dem DOS (kbd.c
  setzt kbd_break_req nicht). Leerlauf: 16 leere INT-16h-Abfragen
  hintereinander, INT 28h und INT 2Fh 1680h schlafen einen Takt; XCOPY
  neben einer wartenden FreeDOS-Box ist nicht langsamer. Geprüft: Start
  bis zur Eingabe, VER, DIR, TIME, deutsche Tasten, Strg+C bricht nur
  die Zeile ab, EXIT. Offen: Box mit DOS hängt -> bisher kein Abschuss
  (Strg+C gehört dem DOS; später eigene Taste oder über die Sitzungs-
  liste); Schreiben aufs Abbild; MEM zählt Box-Speicher unter „BIOS“.
  Vom Nutzer in 86Box (386, AT-DOS386) bestätigt: DOSBOX startet, VER,
  DIR, TIME gehen; COPY auf A: meldet wie vorgesehen „Laufwerk ist
  schreibgeschützt“ (A: ist das gemeinsame Startabbild aller Boxen –
  Schreiben dorthin würde sich zwischen gleichzeitigen Boxen in die
  Quere kommen; beschreibbar werden die AT-DOS-Laufwerke über den
  Redirector).
  Redirector erledigt (in QEMU geprüft: MD/CD/RD, COPY in beide
  Richtungen mit 25/27 KB – Inhalte gleich, REN, DEL *.*, TYPE, Umleitung
  `>`, Programm von C:, VOL; fsck sauber): BOXSHELL trägt die AT-DOS-
  Laufwerke (INT E0h 12h) als Netzlaufwerke in die CDS ein (Flags C000h,
  Backslash-Offset 2), meldet die SDA (5D06h -> INT E0h 11h) und biegt
  INT 2Fh 11xxh auf INT E0h 10h um (`retf 2` mit `inc sp` – ADD zerstört
  CF). `kernel/redir.c` (KTEXT2) erledigt die Unterfunktionen mit den
  FAPI-Funktionen über `box_fapi` (Kernelpuffer: `fapi_kcall`, sonst
  setzt fapi_inner RPL 3 auf den Selektor), Box-Speicher über das
  Fenster `RWIN_SEL(t)` je Task. FreeDOS-Eigenheiten: Argument auf dem
  Stack (beim TSR [SP+8]), Datenpuffer bei READ/WRITE ist die DTA der SDA
  (FreeDOS setzt sie auf den Puffer), CLOSE zählt die SFT selbst herunter,
  DOS prüft vor jedem Schreiben den Modus im SFT (bei CREATE ist das
  Argument das Attribut -> Modus 2 eintragen). Lehren: FreeCOM-COPY setzt
  erst die Größe (Schreiben mit 0 Byte -> FAPI 52h DosNewSize); DEL *.* löscht zwischen
  FINDFIRST/NEXT – Suche zählt Einträge, daher letzten Namen je Suche
  merken (`slast`) und zurückgehen, wenn er fehlt. Bezeichnung (Attribut
  08h) über FAPI_LABEL. Lesen+Schreiben (in QEMU geprüft, auch Kürzen
  über mehrere Cluster, fsck sauber): `DosOpen` CX = `OPEN_RW` (2, Datei
  muss da sein, allein offen wie CREATE – `in_use`; Schreibschutz/
  Verzeichnis -> DENIED), Handle bekommt IFS_F_WRITE, gelesen wird über
  dasselbe Handle. FAPI 52h `DosNewSize` (wie OS/2) -> `ifs_setsize`
  (fat.c): kürzen über den neuen Treiberaufruf `truncate` (letzter Eintrag
  in `struct ifs_driver`, `fat_truncate` gibt die Kette hinter dem
  Cluster mit Byte size-1 frei), verlängern für jedes IFS mit Nullen über
  `write`. Stub: OPEN_RW = 3D02h, NewSize = Seek + Schreiben mit 0 Byte.
  Weiter offen: dieselbe Datei in mehreren Handles gleichzeitig (Leser und
  Schreiber schließen sich aus, Größe je Handle).
  SETVER (Wunsch des Nutzers): `SETVER.EXE` (FAPI, gebunden – läuft auch
  in der Box), Tabelle als Text in `X:\DOS\BOX\SETVER.LST`
  (Startlaufwerk aus %DOSBOX%, sonst %COMSPEC%), höchstens 48 Einträge
  (BOXSHELL liest 1023 Byte). BOXSHELL holt das Startlaufwerk (INT E0h
  13h), liest die Liste und legt die Tabelle im MS-DOS-Format (Länge,
  Name, Haupt-, Nebenversion, 0 = Ende) resident ab, Zeiger in LoL+37h –
  FreeDOS wertet sie beim EXEC aus (task.c `SetverGetVersion`). BOXSHELL
  gibt FreeCOM eine Umgebung mit `DOSBOX=X:\DOS\BOX\FREEDOS.IMG` mit.
  Start aus CMD.EXE erledigt (in QEMU geprüft: HALLO, eine DOS-.EXE
  (MZ ohne FX), .BAT, ERRORLEVEL 5/7 kommt an, `COMMAND` interaktiv im
  aktuellen Verzeichnis, DOSSTART=SESSION; FX-Programme, gebundene und
  .CMD weiter nativ): kernel.c `dos_kind()` (.COM, .BAT, .EXE ohne
  FX-Signatur „CH“ bei e_lfanew) -> `load_dos()`: Befehlszeile der Box =
  8.3-Pfad (`ifs_namepath(..., 1)`) + Argumente, `load_img` von
  `X:\DOS\BOX\FREEDOS.IMG` des Startlaufwerks (fehlt es, ein .COM wie
  früher in der nackten Box über `load_com`). Gilt für jedes ExecPgm,
  also auch START/FM. `DOSSTART=SESSION` (`dos_session`): EXEC_SYNC wird
  für DOS-Programme (`dos_cmd`) zu EXEC_ASYNC. BOXSHELL: bei /C erst
  INT 10h 0003h (sonst gäbe `v86_replay` die FreeDOS-Startmeldungen an
  CMD zurück), dann INT E0h 14h (QCURDIR des Box-Tasks, der Laufwerk und
  Verzeichnis vom Aufrufer erbt; 8.3, weil die Box kein LFN-Merker hat)
  -> 0Eh + 3Bh, A:/B: nicht (gibt es in der Box nicht). CMD: `.BAT` ist
  Typ 4 (geht an ExecPgm, nicht mehr an run_batch), `COMMAND[.COM]
  [/C x]` -> %DOSBOX% mit den Argumenten. Erstes Codesegment danach
  62,7 KB. Tempo (vom Nutzer gemeldet: auf dem 386SX dauert es, bis die
  Ausgabe kommt, auf der 486 deutlich kürzer): gezählt je HALLO-Aufruf
  ≈ 8.400 Monitor-Traps (CLI/STI je ~1.600, INT n 3.300, IRET 1.600)
  und 348 Sektoren vom Abbild – fast alles Start von FreeDOS/FreeCOM.
  BOXSHELL startet `.COM`/`.EXE` jetzt direkt (4B00h, Rest der Zeile als
  Befehlszeile, `direct`), FreeCOM nur für `.BAT`/interaktiv: 167
  Sektoren, ≈ 6.000–6.700 Traps. Der Rest ist der FreeDOS-Start (u. a.
  Startmeldungen über INT 10h je Zeichen). Tasten, die während des Starts
  getippt werden, gehen an die Box und verfallen.
  Box offen halten erledigt (wie NTVDM, in QEMU geprüft: mehrere DOS-
  Programme hintereinander mit Verzeichniswechsel, ERRORLEVEL, .BAT,
  COMMAND nimmt dieselbe Box, nach EXIT des Besitzers ist der Speicher
  wieder frei): BOXSHELL meldet nach jedem Programm INT E0h 15h (AL =
  Rückgabewert) und springt danach an den Anfang (`next`, Stack neu).
  Kernel `BOX_PARK`: hat die Box einen Aufrufer in derselben Sitzung,
  `v86_park` (Text sichern, Vollbild weg, Text ausgeben, `box_ses = -1`
  -> keine Tasten/Strg+C/Bildschirmabfrage mehr), child_rc und Aufrufer
  wecken, `park_owner[t] = Aufrufer + 1`, T_WAIT; sonst wie BOX_EXIT.
  `box_reuse` in EXECPGM (nur EXEC_SYNC): geparkte Box des Aufrufers
  bekommt Befehlszeile (`dos_line`), Name, Laufwerk/Verzeichnis/dcwd,
  `v86_start`, T_READY; `%DOSBOX%`-Abbild = interaktiv (COMMAND).
  `task_end` des Besitzers beendet seine geparkte Box. Erstes Code-
  segment jetzt 63,2 KB.
  Kleinere Punkte erledigt (in QEMU geprüft): COMMAND.COM auch in
  `X:\DOS\BOX` (BOXFILES), BOXSHELL nimmt es von dort (COMSPEC) und
  wechselt beim Start aufs Startlaufwerk – danach braucht die Box das
  Abbild nicht mehr, und `BOX_DRIVES` meldet A:/B: mit (nur „Laufwerk da“
  über vol[].used/present/floppy_lazy, kein Medienzugriff): die Disketten
  von AT-DOS sind in der Box A:/B: (`a:prog` aus CMD geht). PATH der Box
  `X:\DOS\BOX;X:\DOS` (build_env ersetzt PATH=, Laufwerksbuchstaben über
  die Tabelle `drvpos`), interaktiv `/K X:\AUTOEXEC.BAT` nur, wenn es sie
  gibt (4300h), Meldung von BOXSHELL; freedos/AUTOEXEC.BAT entfällt. MEM:
  `v86_pages` (vp_alloc/vp_free in v86.c), `fapi_meminfo.box_kb` ab CX ≥
  22 (`MEMINFO_V60` = 18 für total/free_kb32), Zeile „DOS-Boxen“.
  Strg+Alt+Ende (auch grau, E0-Zweig in kbd.c) bei einer Box im Vorder-
  grund -> kbd_break_req -> do_break beendet sie (in QEMU mit `jmp $`
  geprüft). Erstes Codesegment 63,3 KB.
  Phase 4, erster Schritt erledigt: XMS/HMA/UMB (in QEMU mit einem
  Testprogramm geprüft: Erkennung, Version 3.00, Anlegen/Kopieren hin und
  zurück/Sperren -> 110000h/Freigeben, DOS in der HMA (3306h DH Bit 4),
  48 KB mehr konventionell, UMB-Block bei CF01h, Speicher kommt beim Ende
  der Box ganz zurück). v86.c: INT 2Fh 4300h/4310h fängt `do_int` ab
  (FreeDOS reicht 43h nicht an den alten Vektor weiter!), Einsprung
  F000:0500 „ARPL E1h; RETF“ -> `xms()` ohne FLAGS-Schreiben (Fernaufruf,
  kein INT-Rahmen). A20 je Box = PTE 100h–10Fh: eigene Seiten (`hma_pg`,
  bei Bedarf) oder Spiegel von 0–FFFF, danach `tlb()` (CR3 neu). EMBs im
  Adressraum der Box ab Seite 110h (eine Seitentabelle, `XMS_KB` 2048 je
  Box, Lücken über `emb_gap`, 16 Handles), Seiten mit `PTE_OWN` (Bit 9)
  -> v86_destroy gibt sie frei; Move über VWIN + RWIN(t) mit linearer
  Basis (`lwin`). UMB: `umb_scan` einmal (55AA-ROMs ab C0000 in 2-KB-
  Schritten, sonst Seite einheitlich 00/FF), 10h vergibt eigene Seiten,
  11h stellt den ROM-Eintrag 1:1 wieder her. CONFIG.SYS: `DOS=` setzt
  `dos_high`/`dos_umb` (ohne = LOW,NOUMB wie MS-DOS; ausgeliefert mit
  HIGH,UMB), `RMSIZE=` -> `box_kb`. Box-CONFIG.SYS hat fest
  `DOS=HIGH,UMB` und `DEVICE=A:\ATBOX.SYS` (apps/atbox.asm, tut nichts –
  FreeDOS ruft umb_init nur nach einer DEVICE=-Zeile!). Offen: XMS-Grenze
  einstellbar erledigt: `\DOS\BOX\DOSBOX.INI` (Entscheidung des
  Nutzers: keine Oberfläche -> Datei; beides: CONFIG.SYS für den Start
  der Box, INI je Programm). BOXSHELL liest sie vor jedem Programm
  (`get_pname` = Name aus der Befehlszeile, `ini_apply`/`ini_pass`: erst
  `[*]`, dann `[NAME.EXT]`, Schlüssel ohne Groß/klein, 4 KB per 48h),
  `XMS_MEMORY_LIMIT` -> INT E0h 16h (`xms_lim[t]` = Seiten + 1, 0 =
  Vorgabe 2048, bis 63.424 – seit Phase 4 Rest), `DOS_VERSION` -> INT 2Fh 122Fh (FreeDOS gibt
  os_setver jedem neuen PSP mit, SETVER-Tabelle geht vor), nach dem
  Programm zurück (DX = 0). In QEMU geprüft ([*] 1024, [XT.COM] 512,
  [DOSVER.COM] 6.22, anderes Programm wieder 7.10). Pro Programm nicht
  möglich, weil die Box offen bleibt: DOS=, RMSIZE (beim Start der Box).
  EMS 4.0 erledigt (in QEMU geprüft: Erkennung am Vektor F060:000A und
  über das Gerät EMMXXXX0 = ATBOX.SYS, Rahmen D000h, 128 Seiten, Version
  4.0, zwei logische Seiten im Wechsel behalten ihren Inhalt, Freigabe;
  Speicher nach dem Ende der Box zurück): INT 67h -> F060:0020 „ARPL 67h;
  IRET“, `vec_def(67h)` kennt das, also ein Trap je Aufruf. Rahmen:
  `ems_pick_frame` nach umb_scan (erste 16 freie Seiten ab C8, auf 4
  Seiten ausgerichtet), `umb_run` lässt ihn aus. Logische Seite = 4
  zusammenhängende Seiten (`mem_alloc(4)`, in v86_pages gezählt),
  Einblenden über die PTEs (`ems_show`, leer = Ersatzseite A0), Verwaltung
  je Box in einer Seite (`ems_tab`: Erstseiten, Besitzer, Namen,
  gesicherte Zuordnung), Handles/Zählung/Zuordnung in DGROUP. Funktionen
  40–51h, 53h, 54h (nur 02), 58h, 59h (01), 5Ah. INI `EMS_MEMORY_LIMIT`
  -> INT E0h 17h (`ems_lim`). 57h (kopieren/tauschen, AL 0/1) über
  `x57_win`: konventionell -> VWIN/RWIN mit linearer Basis, EMS -> SCRATCH/
  SCRATCH2 auf die physischen Seiten (Ziel zuerst holen – die Tabellen-
  suche benutzt SCRATCH), Stücke bis zur 16-KB-Grenze, Tauschen über
  512 Byte in KDATA; geprüft mit 20.000 Byte über die Seitengrenze, EMS
  -> EMS und zurück. 55h/56h (Phase 4, Rest; in QEMU mit E56.COM
  geprüft, direkt und über einen eigenen INT-67h-Haken): `ems_maplist`
  (aus 50h herausgelöst), `ems_jmpcall` – INT 67h kommt auf zwei Wegen:
  aus do_int (Vektor unverändert: kein Rahmen, CS:IP = Rücksprung) oder
  über den Stummel F060:0020 (IRET-Rahmen bei SP, FLAGS schreibt der
  Aufrufer danach bei SP+4 – deshalb für 56h den Rahmen nach unten
  umbauen: Ziel, FLAGS, EMS_RET, Aufrufer). 56h merkt die alte Liste
  (`ems_cs`, 3 tief), das RETF des Ziels landet bei F000:0560 „ARPL
  E6h; RETF“ -> `ems_ret` blendet sie ein. 5602h: BX = 8.
  `EMS_FRAME_LOCATION=NONE` (CONFIG.SYS, `ems_none` in kernel.c):
  `ems_pick_frame` wählt keinen Rahmen, INT 67h meldet 80h, die 64 KB
  bleiben UMB (in QEMU: größter freier UMB 68 statt 4 KB). DPMI 0600h–
  0603h/0702h/0703h (Sperren, Auslagern) gab es schon ohne Wirkung.
  DPMI Stufe 1 erledigt (in QEMU mit eigenem Testprogramm DP/DP32
  geprüft: Moduswechsel 16/32 Bit, INT 10h an den Real Mode, 0300h mit
  INT 21h, 0400h, 0501h + Deskriptor schreiben/lesen, 0205h + INT 60h im
  PM nachgebildet, Ende mit 4Ch aus dem PM, ERRORLEVEL; Speicher nach drei
  Läufen gleich). Aufbau (v86.c, Abschnitt DPMI): 1687h in `do_int`,
  Einsprung F000:0510 „ARPL E2h; RETF“ -> `dp_switch` (LDT-Seite, GDT-
  Eintrag LDT_IDX(t) umgebogen – Basis KALIAS + physisch – und `lldt`,
  Selektoren CS/DS/SS/PSP/Umgebung/Stummel, Standard-PM-Vektoren =
  Stummel „HLT; n; IRET(D)“ bei F000:0C00+4n). start.asm: V86-Weg Rück-
  gabe 2 = Segmente aus den V86-Feldern laden, IRETD mit 5 Doppelworten;
  `.pm` mit CS-RPL 3 -> `dpmi_trap` (FS/GS gesichert, unter dem Rahmen),
  Rückgabe 2 = nach V86 (die vier Segmentfelder liegen über SS – dafür
  `esp0` der Boxen 16 Byte tiefer, load_img). INT n aus Ring 3 = #GP mit
  Fehlercode n*8+2 (IDT-Tore DPL 0, über 48 jenseits der Grenze). Real
  Mode: `ctx_push` (Zustandsseite, bis 6 Ebenen) + `rm_go` (Stapel im
  Host-Bereich des Programms, 1 KB, Rücksprung F000:0520 „ARPL E3h“) ->
  `dp_back`. Speicher 0501h ab Seite 400h (4 MB) bis 1 GB, eigene
  Seitentabellen mit PTE_OWN im Seitenverzeichnis, v86_destroy gibt sie
  frei. Stufe 2 teilweise (Test mit DOOM 1.2 Shareware, DOS/4G
  eingebunden, aus archive.org „Doom_Knee_Deep_in_the_Dead_Shareware“ –
  nur zum Testen, nicht im Repository; DOOM läuft bis „ST_Init“, also
  durch die ganze Initialisierung mit 8 MB DPMI-Speicher): 0306h Raw
  Switch (PM -> RM: HLT bei Stummel:0530, RM -> PM: F000:0534 „ARPL
  E4h“, Register wie in der Spezifikation), 0202h/0203h Ausnahmen
  (Tabelle in der Zustandsseite bei 1536, Standard = HLT bei Stummel:
  0600+4n -> Box beenden, `dp_exc` legt den DPMI-0.9-Rahmen auf den
  Host-Stapel – eine Seite DPMI-Speicher, `dp_exstack` –, Rücksprung per
  RETF nach Stummel:0540 -> `dp_exret`), 0303h/0304h Callbacks (16, bei
  1728, Einsprung F000:0700+4k „ARPL E5h“ -> `dp_cbcall`: Register in die
  Struktur, DS:(E)SI = RM-Stapel über `dp_cbss`, IRET-Rahmen nach
  Stummel:0548 -> `dp_cbret` lädt den Real Mode aus ES:(E)DI), 0702h/
  0703h ohne Wirkung, Präfixe (66h …) vor IN/OUT/CLI/STI im PM.
  Sicherheit: das Seitenverzeichnis der Box bekommt die Einträge ab 512
  (Kernel-Alias) ohne U-Bit – DOS/4G legt flache 4-GB-Selektoren an,
  Ring 3 käme sonst an den Kernel. Lehre: DOS/4G ruft 0202h in einer
  Schleife über alle 32 Ausnahmen und lädt AX nicht neu – ein fehlender
  Dienst (AX = 8001h) brachte die ganze Kette durcheinander. Fehlersuche:
  Meldungen in einem Puffer sammeln und in `v86_replay` ausgeben (im
  Vollbild geht vio_kputs verloren). Nächste Schritte: virtuelle VGA
  (Vordergrund direkt auf der Hardware: Ports 3C0h–3DFh über die
  I/O-Bitmap, A0000–BFFFF physisch, Moduswechsel über das echte
  Video-BIOS, Hintergrund einfrieren + sichern), virtueller PIT (DOOM
  stellt 140 Hz ein), IRQ 0/1 an PM-Handler (auch während Real Mode),
  Takt im PM weiterzählen (46Ch steht im PM),
  mehr als 2,9 MB XMS (erledigt, s. u.),
  Lock für DPMI, EMS, DPMI. Weitere Hebel: Kosten je
  Trap senken (`kf_box_ctl(BOX_TASK)` je Trap, `vwin` vor jedem Byte,
  Präfix-Schleife). QEMU mit `-icount` bildet die teuren 386-Ausnahmen
  nicht nach – Zählen ist der bessere Maßstab. Offen: Programme auf A:/B:.
  Virtuelle VGA, Stufe 1 erledigt (in QEMU geprüft: Modus 13h mit Bild,
  CGA-Modus 4, `start g` in eigener Sitzung, Alt+Esc hin und zurück –
  Bild, Palette und Tasten kommen wieder, Textsitzung mit Zeichensatz
  heil): v86.c `vga_want` vor dem Dienst INT 10h (auch über einen
  umgebogenen Vektor, ARPL-Weg): Grafikmodus (00h nicht 0–3/7, 4F02h) ->
  `vga_enter`: `vio_setgfx(s, t, RAWGFX)` (vgagfx.c, 0x80; Puffer 4×64 KB
  + Register), PTE A0–BF physisch (Ersatzseite in `vga_spare`), IVT 1Fh/
  43h/6Dh aus der echten IVT, Ports 3B0h–3DFh frei (`tss[t].iob`, jedes
  TSS hat jetzt eine I/O-Bitmap für 0–3DFh, Vorgabe alles gesperrt; `tss`
  nicht mehr static), dann ins echte INT 10h (Vektor aus physisch 40h).
  Solange roh, geht jedes INT 10h ans BIOS, ein Textmodus -> `vga_leave`
  und Nachbildung. Rohmodus in vgagfx.c: `raw_out` (Register wie die
  61-Byte-Tabellen, DAC, Ebenen über `raw_flat`), `raw_in`, gfx_plane = 1
  heißt frisch (nichts zurückladen). `vga_freeze` in inject, v86_tick
  (irq0 ruft es jetzt auch für DPMI-Ring-3) und dpmi_trap: im Hintergrund
  schlafen, bis die Sitzung vorn ist und hw_gfx == RAWGFX. v86_poll
  übergeht rohe Boxen, park/destroy rufen vga_leave (vor dem Freigeben –
  A0 würde sonst als Box-Seite freigegeben). Lehre: das SeaVGABIOS von
  QEMU schreibt nach E856Ch (SeaBIOS-Daten im Schattenbereich) -> #PF;
  `rom_cow` gibt jeder Box beim Schreiben auf eine ROM-Seite (C0–FF außer
  F0) eine eigene Kopie (PTE_OWN). Erstes Codesegment 65.202 Byte – voll!
  Virtueller PIT erledigt (Anlass: Digger lief „suboptimal“; in QEMU
  geprüft: 1000 Hz eingestellt, 3000 eigene INT 8 in ~3 s; Lautsprecher
  440 Hz im WAV-Mitschnitt mit `-machine pcspk-audiodev=`): v86.c
  `pit_elapsed` misst echte Zeit (Kerneltakt + gelatchter echter Zähler
  von Kanal 0, Teiler 11932), `pit_run` zählt `pit_acc` gegen den Teiler
  des Programms (`pit_rl`, 0 = 65536) -> `vt_pend` (bis 32) und setzt den
  Zähler `pit_cnt`; Lesen 40h mit Latch und lo/hi-Folge, Schreiben 43h/
  40h stellt den Teiler. `account`: mit eigenem INT 8 oder Teiler die
  Rate des Programms, sonst wie bisher 18,2 Hz für 46Ch. Kanal 2 (43h mit
  Kanal 2, 42h) und 61h Bits 0–1 (`spk`) nur im Vordergrund an die
  Hardware (`box_fg`), hinten/eingefroren/Ende/Parken stumm. Park setzt
  den Teiler zurück.
  IRQ 0/1 an PM-Handler erledigt – DOOM 1.2 (DOS/4GW) läuft in QEMU
  (Demo, Menü, Spiel; hing vorher in DMX_Init): `dp_hwint` aus v86_tick
  (irq0, Ring-3-PM-Rahmen) und nach jedem PM-Trap, `pm_hooked(t, n)`
  (0205h-Vektor nicht der Stummel) -> `dp_push` legt den IRET-Rahmen auf
  den Stapel des Programms. Lehren: (1) nachgeholte Takte direkt
  hintereinander -> DOS/4GW „transfer stack overflow on interrupt 08h“
  -> Takt höchstens einmal je irq0 (`from_tick`), vt_pend bis 2 (über
  100 Hz also gedeckelt – DOOM will 140); (2) das IRET des Handlers
  trappt im PM nicht – ein beim Zustellen gelöschtes virtuelles IF blieb
  bis zum nächsten STI aus (Tasten kamen Sekunden zu spät); jetzt bleibt
  IF an, Schachteln verhindert visr bis zum EOI; (3) Port 60h liefert bei
  PM-Handler für 9 nur vcur (nicht aus dem Ring nehmen). „Y“ in der Beenden-Abfrage wirkte doch (danach
  der Neustart, s. u.); echter Takt über 100 Hz (PIT des Kernels hochstellen).
  Schneller echter Takt (Anlass: Digger fror beim Highscore ein, wartet
  dort auf viele schnelle Takte; DOOM will 140 Hz): `kpit_set` stellt
  den echten PIT-Kanal 0 auf den Teiler der Box im Vordergrund (höchstens
  1 kHz, `KPIT_MIN`), irq0 (start.asm) zählt den Kerneltakt dann nur je
  11932 Schritte (`kpit_div`/`kpit_acc`), preempt/v86_tick laufen bei
  jedem Interrupt; `pit_elapsed` rechnet mit Takt + kpit_acc + (Teiler -
  Zähler). Zurück auf 100 Hz: Box hinten (inject, v86_poll prüft je
  Durchlauf), eingefroren, Ende, Parken. In QEMU geprüft: 1000 Hz -> 3000
  Takte in ~3 s (vorher gedeckelt 30 s), Uhr läuft richtig, DOOM läuft.
  Tasten in Spielen (vom Nutzer: Leertaste/Strg in DOOM wirkungslos):
  Protokoll zeigte Drücken und Loslassen innerhalb eines Spieltakts ->
  DOOM sieht die Taste nie. `key_ready`: Loslassen-Code frühestens 3
  Takte nach dem vorigen Code, IRQ 1 nicht, solange IRQ 0 bedient wird
  (wie der PIC; sonst löschte das EOI des Tastatur-Handlers das Takt-Bit).
  DOOM startete nach dem Beenden endlos neu (vom Nutzer gemeldet; Beenden
  auf deutscher Tastatur mit Z, DOOM liest die Tastenposition): die im
  Spiel gedrückten Tasten landen auch im Tastaturpuffer des Kernels, CMD
  las danach Pfeil hoch (letzter Befehl) + Enter. `v86_park`/`v86_destroy`
  leeren den Puffer (`kf_kbd_flush`), wenn die Box vorn war.
  Digger (Remastered, DOS, von digger.org – in QEMU nachgestellt) fror am
  Highscore ein: es wartet in einer Schleife, die den PIT latcht/liest und
  INT 16h 11h/INT 21h 0Bh ruft; dabei trappt die Box so oft, dass irq0
  fast immer im Kernel landet – und der Takt kam nur aus irq0 heraus.
  Jetzt `t8_ok`: aus irq0 immer, sonst höchstens ein INT 8 je Kerneltakt
  (`t8_last`). HLT (`hlt_wait`): schläft nur, wenn kein Interrupt ansteht,
  und erlaubt danach den Takt. Spezifisches EOI (60h+n) am virtuellen PIC.
  Danach läuft Digger bis ins Spiel, DOOM weiter. Offen: PC-Lautsprecher
  bei Digger im QEMU-Mitschnitt stumm (Nutzer hört Knacksen).
  Verwaiste Haken (Nutzer: Digger endet unter MS-DOS mit Strg+C unsauber,
  Ton läuft weiter): die Box bleibt offen, ein abgebrochenes Spiel ließe
  INT 8/9 in freigegebenen Speicher zeigen. INT E0h 00h (Programmstart)
  merkt die Vektoren 08/09/0B–0F/1C/70–77 (`hwvec`, `ivt_save`), INT E0h
  15h stellt sie vor dem Parken zurück und setzt den virtuellen PIC/Takt/
  Tastenring zurück. In QEMU geprüft: Digger mit F10 (Titel, dann Ende)
  beendet, danach PT (eigener INT 8, 1000 Hz) in derselben Box richtig.
  Digger lief auf dem P3B-F zu langsam (Nutzer): die Leerlauf-Erkennung
  (16 leere INT-16h-Abfragen -> 1 Takt schlafen) traf Diggers Warte-
  schleife; sie gilt nicht mehr, sobald das Programm den PIT selbst
  umstellt (`pit_rl`), wie IDLE_SENSITIVITY 100 bei OS/2.
  Digger-Ton/-Spiel etwas zu schnell gegenüber MS-DOS (Nutzer, P3B-F):
  der Kernel-PIT lief in Modus 3 (36h, Rechteck – der Zähler läuft je
  halber Periode durch, in Zweierschritten), `pit_elapsed` nimmt einen
  linearen Zähler an -> Zeit innerhalb eines Takts doppelt geschätzt.
  Jetzt Modus 2 (34h) in pic_pit_init und kpit_set. Geprüft: PT 1000 Hz
  ~3 s, Digger und DOOM laufen.
  Digger bei viel Action langsamer (Nutzer, P3B-F): jeder Ton-Befehl an
  42h/61h war ein Trap. `spk_ports`: nach dem ersten Zugriff im Vorder-
  grund gibt die I/O-Bitmap 42h und 61h direkt frei (43h bleibt gefangen,
  Kanal 0 gehört dem Kernel); hinten (inject, v86_poll, vga_freeze),
  Parken, Ende: wieder gesperrt und Lautsprecher aus.
  Digger-Knacksen untersucht (Phase 4, Rest; QEMUs PC-Lautsprecher gibt
  Diggers Technik nicht wieder – stumm, nur in 86Box hörbar): Digger
  stellt Kanal 2 per B6h auf Modus 3 und schreibt ständig Teiler 0028h
  (29,8 kHz Träger) nach 42h, misst Zeit über Latch (43h = 04h) + zwei
  Lesezugriffe auf 40h und stellt den Teiler von Kanal 0 laufend um –
  direkt über 40h, ohne Befehl an 43h. Versuch „40h/43h frei“ wie OS/2
  HW_TIMER: Uhr des Kernels lief 40 % vor (Hardware schneller, der Kernel
  sah die Teiler nicht) -> verworfen. Jetzt DOSBOX.INI `HW_TIMER=ON`
  (BOXSHELL hk Bit 5, `HK_HWT`, Vorgabe aus): Ports bleiben gefangen,
  aber `kpit_check` setzt `kpit_slow` – der echte Kanal 0 läuft vorn mit
  dem Teiler der Box auch unter 100 Hz, jeder INT 8 kommt von einem
  echten irq0 statt aus dem 100-Hz-Raster. start.asm irq0 zählt dafür je
  Interrupt so viele Kerneltakte, wie 11932er-Schritte voll sind
  (`kpend`, Überlauf über 65536 = 5 × 11932 + 5876). In QEMU geprüft:
  Uhr richtig mit Digger (50 s Uptime in 50 s), SLOW.COM (4000h), DOOM
  (140 Hz) läuft mit Ton. Ob es das Knacksen behebt, nur in 86Box
  prüfbar. Vermutung zum Haken am Anfang bei DOOM + PLAY: DOOM lädt beim
  Start die WAD über den Redirector mit Kernelsperre – PLAY wartet dann
  bei FAPI-Aufrufen mit Sperre (Phase 6: Sperren je Gerät).
  Absturz einer Box sichtbar machen (Anlass: Digger stürzt beim Nutzer
  in 86Box ohne Meldung ab, in QEMU und Bochs nicht nachstellbar): die
  Meldung von `fault()` ging mit dem Vollbild der Box verloren. Jetzt
  merkt fault() sie in `box_fmsg` (kernel.c, `scat`), `v86_replay` gibt
  sie nach dem Text der Box in der Sitzung des Aufrufers aus („DOS-Box
  abgebrochen: #GP … bei CS:IP (Fehlercode …)“). Nur beim Start aus CMD
  (mit START hat die Box keinen Aufrufer in derselben Sitzung).
  Dazu eine Spur (v86.c `trace`/`trace_out`): die letzten 8 Fallen der
  Box (Ausnahme bzw. 80h + zugestellter Interrupt, CS:IP/AX) und die
  Vektoren 08h/09h/1Ch im Moment des Abbruchs, unter der Meldung.
  Anlass: COMMAND und DIGGER brechen beim Nutzer auf dem P2 mit #UD bei
  0000:000B bzw. 0000:0024 ab (HALLO und DOOM laufen) – in QEMU (bis
  1 GB, Pentium II, D: mit 1,8 GB FAT16) und Bochs nicht nachstellbar.
  Ursache (mit der Spur gefunden, danach in QEMU mit `sendkey ret 100`
  nachgestellt): Digger biegt INT 9 um und speichert den alten Vektor
  erst nach `STI; POP DS`. Der Monitor stellte beim emulierten STI eine
  wartende Taste (Loslassen von Enter, `key_ready` verzögert es 3 Takte)
  sofort zu -> Diggers Handler sprang über den leeren Zeiger nach
  0000:0000. Jetzt STI-Schatten wie die CPU (`sti_sh`): schaltet STI IF
  ein und wartet ein Interrupt, läuft der nächste Befehl im Einzelschritt
  (TF, `sti_step`), dessen #DB stellt ihn zu (jede andere Falle löscht TF
  vorher, damit PUSHF es nicht sieht). Erster Versuch „bis zur nächsten
  Falle“ verzerrte Diggers Ton: in seiner Schleife „CLI; Zähler lesen;
  STI“ ist die nächste Falle wieder ein CLI. Das allein reicht für Digger
  nicht (die Taste käme nach `POP DS`, wie auf echter Hardware zu früh):
  der eigentliche Fehler war, dass Tastencodes im Ring liegen blieben,
  solange INT 9 auf dem Stummel stand (dort hätte sie das BIOS längst
  verarbeitet; INT 16h holt seine Tasten aus dem Kernel). Jetzt verwirft
  inject den Ring, sobald INT 9 umgebogen wird (`k9_def`, nicht bei
  PM-Handler). Geprüft: Digger mit gehaltenem Enter (100/300/600 ms),
  BOXTEST (INT 9 + Port 60h), DOOM 1.2 Timedemo, COMMAND /C VER.
  Digger-Ton „unter DOS klarer“ (Nutzer, P2; mit und ohne HW_TIMER
  gleich): Digger schaltet den Lautsprecher in seinem INT 8 um (61h Bit 1,
  Ton = halbe Taktrate; Musik mit Teilern 0D59h–1FBFh = 150–350 Hz). Die
  Takte kamen aus der gemessenen Zeit (`pit_elapsed`) – ein gerade
  fälliger Takt oft erst mit der nächsten Falle, also mit Zittern. Jetzt
  `pit_run`: läuft der echte Kanal 0 mit dem Teiler der Box (kpit_div ==
  Teiler, Box vorn), ist jeder echte irq0 genau ein INT 8 (`pit_n0`
  gegen `irq0_n`), und 40h liefert den echten Zähler (`pit_lastc`). Test
  JT.COM (Zähler im eigenen INT 8 bei 349 Hz): vorher über alle Phasen
  verteilt, jetzt fast immer kurz nach dem Nachladen. Digger, Uhr (Uptime
  gleich), DOOM 1.2 Timedemo geprüft; hörbar nur in 86Box.
  Danach (Build 355) „immer noch leicht verzerrt“: Digger misst die Zeit
  in einer Schleife „CLI; Latch; IN 40h; IN 40h; STI“ – vier Fallen mit
  virtuellem IF aus, ein Takt in diesem Fenster kam erst nach dem STI.
  Jetzt VME (CR4 Bit 0, Pentium+, CPUID 1 EDX Bit 1 wie PVI) für Boxen
  ohne DPMI: CLI/STI/PUSHF/POPF/IRET (16 Bit) laufen ohne Falle auf
  EFLAGS.VIF, INT n fängt weiter der Monitor (Umleitungs-Bitmap `irb[32]`
  im TSS vor der I/O-Bitmap, ganz 1). `vme_in` (Eintritt v86_exc/
  v86_tick, nur wenn `vme_on[t]`) holt VIF nach vfl, `vme_out` (Ausgang
  rc 0 und v86_tick) legt vfl nach VIF, VIP bei wartendem Interrupt
  (dann trappt das STI/POPF/IRET, das IF einschaltet). Andere Ausgänge
  (PM, DPMI) lassen VME aus (vme_in löscht das Bit in `pvi_t`, der
  nächste pvi_cr4 stellt CR4). `pvi_t`/`pvi_cur` sind jetzt Bitmasken
  (1 VME, 2 PVI), pvi_sw `and cl,0FCh`. Mit TF (STI-Einzelschritt) kein
  VME. INI `PVI=OFF` schaltet beides ab. QEMU-TCG kennt VME nicht (dort
  unverändert, Digger/BOXTEST/COMMAND geprüft); in Bochs (p2_klamath)
  geprüft: VME aktiv (Zähler über Port E9h), Digger-Titel läuft, DOOM 1.9
  Timedemo durch.
  Phase 5, Schritt 1 erledigt – Speicher bis knapp 4 GB über Fenster
  (in QEMU geprüft: 3 GB erkannt, DOOM 1.2 Timedemo mit Box-/DPMI-
  Speicher über 2 GB, XCOPY über den Cache über 2 GB + CHKDSK +
  SHUTDOWN, fsck sauber, Inhalte gleich; isapc 4 MB, 512 MB; -m 3584
  liefert unter i440fx 3 GB, der Rest liegt über 4 GB = ohne PAE nicht
  erreichbar): mem.h `MEM_MAX_PG` 0xFFFFF, `MEM_LOW_PG` 0x7FC00 (2 GB -
  4 MB). Der Alias spiegelt nur noch darunter (paging.c: höchstens 511
  Seitentabellen); PDE 1023 (linear FFC00000h, kopieren die Boxen mit)
  zeigt auf `win_pt` mit vier Plätzen zu 32 Seiten: SCRATCH, SCRATCH2,
  Cache-Fenster (`win_csel` = bc_wsel), Rest. `pg_win(sel, base,
  limit)` legt den Platz auf die 64-KB-Gegend um (nur wenn sie sich
  ändert, dann CR3 neu laden – kein INVLPG, 386) und liefert die Basis
  für set_desc; kernel.c `swin` nimmt es für scratch_map/scratch2_map/
  bc_win, sobald base + limit über MEM_LOW_PG liegt. mem.c: `mem_alloc`
  nur unter MEM_LOW_PG (Segmente/Deskriptoren über den Alias),
  `mem_alloc_hi` zuerst darüber (`hint_hi`, erst nach paging_init:
  `mem_hi_ok`) – nur für Speicher, den der Kernel ausschließlich über
  diese Fenster oder Seitentabellen einer Box anfasst: Box-Seiten
  (`vp_alloc_hi`/`own_page_hi`: konventionell, HMA, UMB, EMB, DPMI,
  ROM-Kopien), EMS-Seiten, Platten-Cache. Seitentabellen, Verzeichnisse,
  DPMI-LDT/Zustandsseiten, OPL-Kopie bleiben unten. Bitmap bis 128 KB
  über zwei Selektoren (KSEG_IDX(0)/(1), kseg_alloc überspringt belegte).
  Regel: was ohne Kernelsperre über SCRATCH geht (vio, v86_poll), bildet
  nur Speicher unter MEM_LOW_PG ab – win_restore stellt nur Deskriptoren
  zurück, nicht die Seiten eines Platzes. Dabei gefunden: DPMI 0500h
  meldete 3 GB frei, in Byte über 2^31 – DOS/4GW las das als negativ
  („DOS/16M error [6] not enough memory“); jetzt gekappt auf den DPMI-
  Bereich der Box (64 MB–1 GB). Erstes Codesegment 65.421 Byte.
  Phase 5, Schritt 2 begonnen – VESA mit linearem Bildspeicher, zuerst
  in den Boxen (in QEMU geprüft: `\DEMO\VBETEST.COM` (apps/vbetest.asm), DPMI-Programm, 4F02h 4101h
  im Real Mode, 0800h auf PhysBasePtr FD000000h, Deskriptor 0000h/0007h/
  0008h, 640×480×256 Streifen, zurück auf 03h; dazu START VBETEST, Alt+
  Esc zu CMD + VER, zurück -> Bild wieder da): v86.c DPMI 0800h/0801h,
  `dp_phys[NTASK][NDPP=4]` (lpg/npg/ppg), Lücke über `dmem_gap` (beachtet
  jetzt auch dp_phys), PTEs ohne PTE_OWN (v86_destroy gibt nichts frei),
  gleiche Anfrage -> gleiche Adresse; nur Seiten ab `mem_top_pg()` (nie
  RAM), `dphys_unmap` in 0801h und dpmi_end. `vga_ports` gibt im
  Rohbetrieb zusätzlich 1CEh/1CFh frei (VBE-Register der Bochs-/QEMU-/
  Proxmox-Standard-VGA). Offen: SVGA-Zustand beim Sitzungswechsel auf
  echten Karten (raw_out sichert nur die Standardregister; S3 hat eigene
  CRTC-Register) – nur in 86Box prüfbar; VESA für FAPI-Programme (Moduswechsel
  braucht das Video-BIOS, also einen V86-Aufruf aus dem Kernel). DGROUP
  jetzt 65.002 Byte – fast voll.
  Nutzer danach (P2): Verzerrung unverändert – zurückgestellt („löst sich
  evtl. in einem anderen Fall“). VME bleibt (weniger Fallen für alle
  Boxen). Weitere Verdächtige: die Fallen auf 43h/40h im Messfenster und
  86Boxs Nachbildung des PC-Lautsprechers (Vergleich unter MS-DOS auf
  derselben Maschine wäre der nächste Anhaltspunkt).
  Farbiger Text nach dem Ende (Nutzer: DOOMs ENDOOM unter MS-DOS rot
  hinterlegt, bei uns grau): `capture` merkt Zeichen + Attribut (cap[]
  25×80 Worte, Zeilen bis zum letzten Zeichen oder farbigen Hintergrund),
  `v86_replay` gibt sie als ANSI-Farbfolgen (`ansi_seq`, VGA-BGR ->
  ANSI-RGB, 1 = hell, 5 = Blinken) über vio_tty aus, Zeilenende mit
  ESC[0m, volle 80er-Zeilen ohne \n; bei ANSI OFF (kf_vio_ansi) nur Text.
  In QEMU geprüft (ENDOOM rot/gelb/cyan). DGROUP jetzt 61 KB.
  Musik (AdLib/OPL, Wunsch des Nutzers, Schritt 1 zum Ton für DOOM):
  `opl_ports` gibt 388h–38Bh nach dem ersten Zugriff im Vordergrund
  direkt frei (wie spk_ports), hinten/Parken/Ende gesperrt, alle Stimmen
  aus (`opl_wr` B0h–B8h beider Bänke, BDh). In QEMU (`-device adlib,
  iobase=0x380` + DEFAULT.CFG mit snd_musicdevice 2) startet DOOMs Musik
  hörbar, verstummt aber nach ~3 s – vermutlich QEMUs AdLib (meldet OPL3,
  hat 38Ah nicht); in 86Box prüfen. Konflikt mit FAPI-FM-Programmen
  (SB16.SYS/fm_load) noch nicht geregelt: die Box im Vordergrund schreibt
  direkt. Nächster Schritt: virtuelle Sound Blaster (DSP 220h, DMA 1,
  IRQ 5/7) über SB16.SYS.
  Virtuelle Sound Blaster, Stufe 1 (in QEMU mit `-device sb16` für
  SB16.SYS + DOOM-Konfiguration SB 220h/IRQ 5/DMA 1 geprüft: DOOM läuft,
  Effekte im Mitschnitt): v86.c `struct vsb sb[]` – DSP 2.01 an 226h/22Ah/
  22Ch/22Eh (Reset AAh, E1h Version, 40h Zeitkonstante -> Rate über
  `div3216`, 48h Blocklänge, 14h/91h einmal, 1Ch/90h Auto-Init, D0/D4/DA,
  D1/D3/D8 Lautsprecher, E0/E4/E8, F2 IRQ), 8237-Kanal 1 virtuell (02h/
  03h/0Ah/0Bh/0Ch/83h, andere DMA-Ports verpuffen – nie an die Hardware).
  `sb_pump` (aus inject und dp_hwint): fällige Bytes je 10 ms aus dem
  Box-Speicher (VWIN über `lwin`, 512-Byte-Stücke in `sb_buf`) über
  `kf_box_aud` -> `box_aud` (kernel.c, aud_fapi mit KDATA, ohne Sperre)
  an SB16.SYS, nur vorn und mit Lautsprecher an; Blockende -> `irqp`,
  Leitung = zuletzt per DPMI 0205h eingehängter Vektor 0Dh/0Fh (`irql`;
  DOS/4GW hängt alle PM-Vektoren ein, „0Fh eingehängt“ taugt nicht; ohne
  0205h: 7, wenn 0Fh in der IVT umgebogen ist, sonst 5), `irqp` als
  Bitmaske, RM-Handler über die IVT, PM-Handler über dp_push; Standard-
  Stummel 0Dh/0Fh löschen ihr visr-Bit. DOOM fror beim Levelstart ein:
  DMX macht im PM PUSHF/CLI/POPF – POPF trappt bei IOPL 0 nicht, das
  virtuelle IF blieb aus. Wachhund `dp_cli_t` (PM-CLI merkt die Zeit):
  nach 3 Takten mit IF aus setzt dp_hwint es wieder. Lehren: Leitung über
  die Maske an 21h erkennen geht nicht (DOS/4GW setzt sie nicht);
  Zustellung auf beiden Leitungen ließ DOOM im Demo still beenden. In
  QEMU mit IRQ 5 und 7 geprüft (Spiel läuft, Ton im Mitschnitt).
  Waffe ~2 s verzögert (Nutzer, P3B-F; PC-Lautsprecher ohne): SB16.SYS
  spielte erst los, wenn 8 KB im Puffer lagen, und hielt zwei Hälften zu
  8 KB. Neu `AUD_SMALL` (Bit 7 von `fapi_audfmt.bits`): Hälfte etwa
  1/16 s (rate/16 × Kanäle × Bytes, 512–8192, `half` im Treiber, 8237-
  Zähler und DSP-Blocklänge danach); die Box öffnet damit. Geprüft mit
  `LAT.COM` (Piepser, dann 8.000 Byte per DSP 14h, WAV mit pcspk und
  sb16 auf einem audiodev): neu folgt der SB-Ton direkt, alt kam er gar
  nicht (unter 8 KB, nie gestartet).
  Vom Nutzer auf dem ASUS P3B-F in 86Box bestätigt: DOOM funktioniert
  (Spiel, Tasten, Ton der Waffe ohne Verzögerung).
  BLASTER und Länderdaten (Wunsch des Nutzers; in QEMU geprüft):
  BOXSHELL hängt `BLASTER=A220 I5 D1 T3` an die Umgebung der Box, wenn
  sie keins hat (IRQ 5 oder 7 geht beides, siehe `irql`). COMMAND.COM
  zeigte Datum/Zeit im US-Format: `freedos/COUNTRY.SYS` (FreeDOS-
  Submodul country 7f83e04, GPL, mit nasm gebaut) im Startabbild, Box-
  CONFIG.SYS `COUNTRY=049,437,A:\COUNTRY.SYS` -> DATE 03.11.2026, TIME
  17:36:39,83. Fest Deutschland – AT-DOS wertet COUNTRY= selbst noch
  nicht aus.
  Virtuelle SB16 (Wunsch des Nutzers; in QEMU geprüft): DSP 4.05 (E1h),
  41h/42h Rate, Bxh/Cxh (Modus: 20h stereo, 10h mit Vorzeichen; Länge
  in Werten, Stereo zählt beide Kanäle, 16 Bit = Worte), D5/D6/D9, F3,
  Quittung 22Eh (8 Bit) / 22Fh (16 Bit), Mixer 224h/225h (80h IRQ – ein
  geschriebenes 2/4 setzt `irql` 5/7 –, 81h = 22h für DMA 1+5, 82h
  anstehende IRQ, 30h–47h gespeichert, Lautstärken ohne Wirkung).
  16 Bit über DMA 5 (C4h/C6h/D8h/8Bh, Adresse/Zähler in Worten, Seite &
  FEh); die übrigen DMA-Ports verpuffen. Format je Block: Box öffnet
  Rate/Kanäle/Bits passend (`orate/och/obits`); kann die Karte es nicht
  (SB 2.0), `conv`: 8 Bit mono aus dem linken Kanal. Vorzeichen in
  `sb_conv` an die Karte angepasst. DSP 4: Lautsprecher-Befehle wirken
  nicht (`dsp4`). BLASTER jetzt `A220 I5 D1 H5 T6`. Geprüft: eigenes
  Testprogramm 44,1 kHz 16 Bit stereo Auto-Init mit IRQ 5 (78 IRQs in 2 s
  bei 25-ms-Blöcken, 1 kHz auf beiden Kanälen im Mitschnitt), DOOM 1.2
  mit Ton. Ein DOOM-Nachbau hängt bei I_Init (siehe ganz am Ende).
  Offen: Aufnahme,
  SB-Pro-Stereo (Mixer 0Eh), DSP-MIDI.
  AdLib teilen (Wunsch des Nutzers; in QEMU geprüft): greift eine Box im
  Vordergrund auf 388h–38Bh zu, meldet `opl_ports` das dem Kernel
  (`kf_box_aud(BOX_AUD_FM, 1)` -> `fm_box`): FAPI-FM-Programme werden
  angehalten (AR_FMRESET, `fm_cur = -1`) und schreiben, solange
  `fm_boxed`, nur in ihre Registerkopie; gibt die Box den Chip ab
  (hinten, Parken, Ende), lädt der nächste FMWRITE den Stand neu
  (fm_load) – PLAY spielt an der Stelle weiter. Erst melden, dann den
  ersten Wert der Box schreiben (sonst löscht AR_FMRESET ihn). Grenze:
  die Box schreibt direkt (Fallen auf 388h kosteten zu viel, Musik-
  treiber lesen den Status Dutzende Male je Register) – Registerkopie
  nur bei Bedarf, siehe unten. Dabei gefunden: QEMUs AdLib (OPL2) galt als OPL3 –
  ein OPL2 dekodiert nur A0, 38Ah/38Bh spiegeln 388h/389h, fm_load
  überschrieb mit Bank 1 (Nullen) die Instrumente. `opl_detect` startet
  jetzt bei Status „OPL3“ noch Zeitgeber 1 über die zweite Bank (102h/
  104h): läuft er, ist es ein Spiegel -> OPL2. Vermutlich auch der
  Grund, warum DOOMs Musik in QEMU nach 3 s verstummte. Test: `PLAY
  ODE.IMF` in einer DETACH-Schleife, vorn eine Box mit 440 Hz für 3 s
  -> Mitschnitt: ODE, 440 (PLAY still), ODE ab der laufenden Stelle.
  Registerkopie der Box (Phase 4, Rest; in QEMU geprüft): v86.c
  `opl_pg[t]` (eigene Seite, je Bank 256 Byte), `opl_trk[t]` (Bit 0
  mitschreiben, 1 Kopie gilt, 2 Bank 1 benutzt), `opl_idx`. Mitschreiben
  heißt: Ports 388h–38Bh bleiben gesperrt, port_out merkt Index/Wert und
  reicht durch, port_in liest den Chip – nur wenn beim Nehmen des Chips
  (`opl_ports(t, 1)`) ein FAPI-Programm FM offen hat (`BOX_AUD_FMQ` ->
  `fm_count`), die Kopie schon gilt oder DOSBOX.INI `ADLIB_COPY=ON`
  (BOXSHELL hk Bit 4, Vorgabe aus -> `HK_ADLIB`, als einziges Bit „an“).
  Nimmt die Box den Chip wieder und die Kopie gilt: `opl_restore`
  (08h, WSE kurz an, 20h–F5h nur gültige Register `OPL_REG`, Tasten/
  Rhythmus los, 01h zuletzt; Bank 1 und 104h/105h nur, wenn benutzt –
  sonst überschreibt beim OPL2 der Spiegel Bank 0). Kopie gilt bis zum
  Programmende (xms_reset), Seite frei in v86_destroy. Lehre: D0h–DFh
  gibt es nicht, der OPL-Kern von QEMU (MAME) nimmt D0h–D8h aber als
  C0h–C8h – Nullen dort machten aus additiv FM (Obertöne 1320/2640 Hz);
  fm_load (FAPI) schreibt dort jetzt den Wert von C0h–C8h. Test `\DEMO\OPLTEST.COM`
  (440 Hz 1 s, Taste abwarten, dann nur B0h = Taste an): DETACH PLAY
  ODE.IMF, START OPLR, weg, PLAY spielt, zurück, Taste -> 440 Hz sauber
  (ohne Kopie stumm); `ADLIB_COPY=ON` ohne PLAY ebenso; DOOM unverändert.
  Vom Nutzer in 86Box bestätigt: IMF spielt sauber weiter, der Ton der
  Box kommt nach dem Zurückschalten richtig.
  Kernel-Panik mit `START DOOM` (Nutzer, P3B-F; in QEMU nachgestellt:
  #GP bei 0008:03EF = exc_common `.topm`, Fehlercode 1Ch = LDT-Selektor
  1Fh): das TSS einer Box hat `ldt = 0` (nur FX-Programme bekamen
  LDT_SEL), dp_switch lud die DPMI-LDT nur per LLDT – beim nächsten
  Hardware-Taskwechsel zurück in die Box lud die CPU LDTR aus dem TSS
  = 0. Synchron (CMD schläft) gab es keinen Wechsel, mit START läuft die
  zweite Sitzung. Jetzt `tss[t].ldt = LDT_SEL(t)` in dp_switch, 0 in
  dpmi_end. Gefunden mit Ausgaben auf Port E9h (`-debugcon file:`):
  SLDT zeigte 0000 beim Absturz. Geprüft: START DOOM, Alt+Esc zu CMD
  (VER), zurück, spielt weiter.
  Vom Nutzer auf dem P3B-F bestätigt: START DOOM (Musik auf AdLib) und
  `PLAY /FM` mit MIDI laufen nebeneinander, Sitzungswechsel geht; der
  Ton hakte am Anfang kurz, danach sauber (noch nicht untersucht).
  SB-Effekte nach dem Wegschalten weg (Nutzer): `vga_freeze` rief
  `sb_stop` – die virtuelle DSP stand, DMX wartete danach ewig auf den
  Block-IRQ (AdLib-Musik lief weiter). Jetzt bleibt die SB beim
  Einfrieren an (sb_pump gibt hinten ohnehin nichts aus), nach dem
  Auftauen `sb[t].last` = jetzt (Pause nicht nachholen). In QEMU geprüft:
  START DOOM, 6 s weg, zurück -> Effekte im Mitschnitt weiter.
  DOOM 1.9 (Shareware, archive.org „doom19s_202607“, nur zum Testen) in
  QEMU geprüft: synchron und per START mit Alt+Esc hin und zurück, Demo
  läuft, Ton (SB + AdLib) vor und nach dem Wechsel.
  DOOM 1.9 mit Sound Blaster „nicht rund“ (Nutzer; ohne Effekte rund):
  Zähler über Port E9h zeigten 82 statt 140 Takte je Sekunde – DMX
  macht PUSHFD; POP EAX; CLI … PUSH EAX; POPFD, das POPF trappt im PM
  nicht, das virtuelle IF blieb bis zum Wachhund (3 Takte) aus. Versuche:
  Stapelstelle merken (Flags liegen im Register – nichts), Einzelschritt
  mit TF bis POPF/IRET (zu teuer: ~16.000 Fallen/s, die Mischschleife
  hat mehr als 1000 Befehle mit CLI), Hardware-Haltepunkte (DR0/DR1 auf
  das POPF/IRET – gingen in QEMU, 10.345 Ticks, aber 86Box prüft sie
  und mit dem Dynarec auch TF nur mit der Option „Debug-Register bei
  486-Prozessoren und neueren“, src/cpu/386_common.c/386_dynarec.c;
  beim Nutzer blieb es zäh: Menü spät, Ton abgehackt). Lösung jetzt:
  beim CLI im PM nach `PUSHF; POP reg` das erste `PUSH reg` (dasselbe
  Register); `POPF(D)` der nächsten 1 KB suchen (`dp_patch_popf`, Suche
  in `sb_buf`) und das 9Dh im Speicher der Box durch HLT (F4h) ersetzen
  (höchstens 8 Stellen je Box, `dp_patch`, beim Ende von DPMI vergessen).
  Das HLT trappt (#GP), dp_trap bildet POPF(D) nach (Flags vom Stapel,
  IF wie vor dem CLI: `dp_popf_if`). Das passende Register macht einen
  Treffer in Daten praktisch unmöglich; sonst bleibt der Wachhund. In
  QEMU: `-timedemo demo1` mit Ton 7.241 Ticks (ohne Ton 9.040, mit
  Haltepunkten 10.345, vorher nach 5 min nicht fertig); DOOM 1.2 und
  1.9 (START + Alt+Esc) mit Ton. Lehre: Emulatoren bilden Debug-
  Register oft nicht nach – nicht darauf bauen.
  DOOM 1.9 blieb beim Nutzer trotzdem „unspielbar“ (1.2 rund). Fallen
  je 2 s gezählt (Port E9h): 1.9 ~3.900 statt ~2.500, Ports 1.700 statt
  800, V86 550 statt 100, Umleitungen in den Real Mode 150 statt 14. Der
  Grund: DMX 1.9 hat gar keinen IRQ-Handler (liest die DMA-Stelle), die
  virtuelle SB löste trotzdem 55-mal je Sekunde IRQ 7 aus (`irql` aus
  DOS/4GWs eigenem 0205h für 0Fh); DOS/4GW reichte ihn in den Real Mode
  zum leeren Stummel weiter, mit Fragen an den zweiten PIC (A0h). Jetzt:
  landet IRQ 3–7 am PM-Stummel und steht im Real Mode nur der Standard-
  Stummel (`vec_def`), quittiert dp_trap sofort ohne Moduswechsel; für
  5/7 merkt sich die SB die Leitung als ruhend (`sb[].noirq`), bis ein
  Real-Mode-Handler oder 0205h kommt (`sb_line`). Box-CONFIG.SYS hat
  `STACKS=0,0`, damit FreeDOS sich nicht mit Stapel-Handlern vor die
  Stummel hängt (IVT 0Dh zeigte auf 0070:006B). Ergebnis: Umleitungen
  11/s statt 75/s; in QEMU kaum schneller (Fallen sind dort billig),
  der Gewinn zeigt sich erst in 86Box.
  Nutzer danach: „gravierende Verbesserung“, nur bei Kämpfen (viele
  Geräusche) wird 1.9 langsamer, 1.2 nicht. Gemessen (je s, Demo):
  DOOM bekommt ~80 statt 140 Takte, ~60 gehen verloren (vt_pend voll);
  Grund bei 60 % der Versuche: virtuelles IF aus (DMX mischt mit CLI,
  darin weitere Fallen: DMA-Stelle, PIC). `t8_ok` jetzt wie der PIC:
  jeder echte irq0 (`irq0_n`, start.asm) gibt höchstens einen INT 8 frei,
  auch wenn er den Kernel traf (vorher außerhalb von irq0 nur einer je
  Kerneltakt) – in QEMU ohne Wirkung, in 86Box (teurere Fallen) evtl.
  Versuch verworfen: Rückstand bis 8 und Nachholen außerhalb des laufenden
  Handlers -> DOOM 1.9 hing in I_StartupTimer. Nächster großer Hebel:
  PVI (CR4 Bit 1, ab Pentium, 86Box bildet es für P2/P3 nach,
  src/cpu/x86_ops_flag.h) – CLI/STI ohne Falle (~700/s); braucht dann
  einen anderen Weg zum POPF-Patch (CLI trappt nicht mehr).
  Die drei Hilfen sind je Programm abschaltbar (Wunsch des Nutzers, weil
  sie vom echten Verhalten abweichen): DOSBOX.INI `POPF_PATCH`,
  `SB_IRQ_IDLE`, `IF_WATCHDOG` = ON/OFF (auch 1/0), BOXSHELL schickt die
  Maske per INT E0h 18h (BL Bit 0–2 = an), v86.c `box_hk[t]` (Bit = aus,
  0 = Vorgabe alle an, zurück bei jedem Programmstart und in xms_reset).
  Nutzer danach mit DOOM 1.9: wieder besser, bei Kämpfen (Monster auf dem
  Podium in E1M1, Gegner vor dem Levelende) geht es noch deutlich runter.
  PVI erledigt (CR4 Bit 1, CPUID 1 EDX Bit 1 = VME; nur in Bochs geprüft –
  QEMU-TCG kennt VME nicht, auch nicht mit `+vme`): CLI/STI im PM setzen
  nur EFLAGS.VIF. v86.c `pvi_in` (Eintritt dpmi_trap/v86_tick: VIF ->
  vfl, Übergang 1->0 merkt dp_cli_t), `pvi_out` (Ausgang dpmi_trap,
  v86_tick, v86_exc mit rc 2: vfl -> VIF, VIP nur wenn vt_pend/SB-IRQ/
  Taste wartet – dann trappt das nächste STI). CR4 gilt für alle Tasks:
  `pvi_sw` (start.asm) in switch_to stellt es nach `pvi_t[t]` (FAPI-
  Programme behalten #GP bei CLI), v86.c ändert es selbst über `pvi_cr4`;
  `pvi_cur` = Stand in CR4. POPF ändert VIF nicht – der POPF-Patch bleibt
  nötig, findet seine Stellen aber nur über ein trappendes CLI: die erste
  Sekunde nach dp_switch ohne PVI (`pvi_back`), und feuert der Wachhund,
  wieder 1 s ohne (nur solange die Tabelle nicht voll ist). Beim
  gepatchten POPFD nimmt die Nachbildung IF aus Bit 19 (VIF) des vom
  Programm gesicherten Werts (PUSHFD sichert VIF mit). INI `PVI=OFF`.
  Dabei gefunden (wichtiger als PVI selbst): die Patch-Tabelle (8) war
  sofort voll, DOOM 1.9 braucht 9 Stellen – der Wachhund feuerte deshalb
  auch ohne PVI 1–2-mal je s, jedes Mal 3 Takte ohne Interrupts, DOOMs
  Takt verlor Schritte und das Spiel lief in Echtzeit langsamer (je mehr
  Geräusche, desto mehr Mischpfade). Jetzt `NPATCH` 24; der Wachhund
  feuert im Timedemo nicht mehr. QEMU-Timedemo zeigt deshalb mehr
  „realtics“ (8804 statt 6042 bei gleicher Laufzeit – vorher fehlten
  Takte). Fallen je 2 s in Bochs (virtuelle SB, Timedemo): ~7.400 ohne,
  ~4.250 mit PVI. Testumgebung Bochs (apt `bochs bochs-x bochsbios
  vgabios`, Debugger-Build: `c` über stdin, `display_library: x` unter
  Xvfb, Bilder mit `import`, `BIOS-bochs-legacy` – das latest-BIOS stürzt
  in rombios32 ab, `port_e9_hack`, Bochs-eigene SB16 aus: DOOM hing damit
  auch ohne PVI). Dabei gesehen und behoben: nach `DOOM > T.TXT` meldete
  `TYPE T.TXT` „wird gerade geschrieben“ – die geparkte Box hielt die
  Umleitung offen. Jetzt `std_set(t, from)` (kernel.c): BOX_PARK gibt
  die Handles 0–2 frei (Konsole), box_reuse übernimmt die des Aufrufers;
  task_create/task_end nutzen dieselbe Hilfe. In QEMU geprüft: zweimal
  `HALLO.COM > X.TXT` + `TYPE X.TXT`, danach `DEL X.TXT`.
  Vom Nutzer auf dem P3B-F (Pentium II) in 86Box bestätigt: DOOM 1.9 mit
  Sound Blaster läuft flüssig, auch bei Kämpfen.
  XMS über 3 MB (Phase 4, Rest; in QEMU geprüft): EMBs im Adressraum
  der Box von 110000h bis 64 MB (`XMS_TOP` 4000h, `XMS_MAXPG` 63.424 KB,
  INI `XMS_MEMORY_LIMIT` bis 63424, Vorgabe weiter 2048). `emb_map`/
  `emb_unmap` gehen über `lpte_set`/`lpte_get` (Seitentabellen ab 4 MB
  bei Bedarf, PTE_OWN), DPMI-Speicher (`dmem_gap`) deshalb erst ab
  64 MB. Dabei behoben: v86_destroy gab nicht freigegebene EMBs unter
  4 MB nicht zurück (Schleife über pt_box nur bis 100h, jetzt bis 400h
  mit PTE_OWN). Geprüft: X2.COM (16 MB anlegen, Muster bei 15 MB hin
  und zurück, Lock 110000h, Freigabe), Box per START mit belegtem EMB
  beendet -> Speicher ganz zurück, DOOM 1.9 Timedemo (DPMI ab 64 MB).
  IDLE_SENSITIVITY (Phase 4, Rest; in QEMU geprüft): v86.c `idle_poll`
  zählt je Kerneltakt leere INT-16h-01/11-Abfragen, INT 1Ah 00h und
  Lesen von 3DAh/3BAh; ab sens/4 + 1 schläft die Box einen Takt
  (BOX_SLEEP). INI `IDLE_SENSITIVITY=n` -> BOXSHELL INT E0h 19h (BX =
  n + 1, 0 = Vorgabe 75), `idle_sens[t]`. Ohne INI-Wert bleibt die
  Erkennung bei Spielen mit eigenem Takt (`pit_rl`) aus (Digger). Test
  POLL.COM (fragt 3DAh und INT 16h 01 ab) per START im Hintergrund,
  BUSY 60000 vorn: 10 s mit Vorgabe, mit 100 nach über 2 min nicht fertig.
  `XMS_MEMORY_LIMIT=-1` bzw. `EMS_MEMORY_LIMIT=-1` (Wunsch des Nutzers)
  = so viel, wie die Box ansprechen kann (63.424 KB XMS, 4.096 KB EMS):
  BOXSHELL `pnum` liefert bei `-` FFFFh, der Kernel kappt.
  Speicher je Box (vom Nutzer auf dem 386 mit 4 MB gemeldet: zwei
  Boxen 2.600 KB, 304 KB frei): in QEMU nachgestellt – eine Box am
  FreeCOM-Prompt kostet ~830 KB, dazu kam eine **geparkte** Box (DOS-
  Programm vorher direkt aus CMD, BOX_PARK hält sie wie NTVDM offen).
  Jetzt gibt `load_img` vor dem Anlegen einer neuen Box die geparkten
  frei, wenn weniger als 4 × RMSIZE (2,5 MB) frei sind. Danach in QEMU
  (4 MB): zwei Boxen 1.656 KB, 1.132 KB frei. Platz dafür: `sys_info`
  (MSD) bis auf die Treiberliste nach k2misc.c (`sys_info2`). Lehre: das
  erste Codesegment darf höchstens 65.520 Byte haben – start.asm rundet
  `code_end` auf 16 auf, darüber läuft die DS-Rechnung über und der
  Kernel hängt beim Start; build.sh bricht jetzt ab. Vom Nutzer auf dem
  386 mit 4 MB bestätigt (Build 336): zwei Boxen per START 1.800 KB,
  1.104 KB frei (vorher 2.600/304). MS-DOS-MEM zeigte in der Box
  „Erweiterung (XMS) 0K“: MEM liest die Handle-Tabelle von HIMEM (INT
  2Fh 4309h, AL = 43h, ES:BX -> Kopf: Kennung, Eintragsgröße 10, Anzahl,
  Fernzeiger; Einträge Art 1 frei/2 belegt/4 leer, Sperren, Adresse und
  Größe in KB). Jetzt `xms_table` (v86.c) bei F000:0900 (`XMS_TAB`), je
  Aufruf neu: belegte Blöcke, dahinter der freie Rest der Box als ein
  Block. Mit HT.COM geprüft (2.048 frei; nach 1 MB anlegen 1.024/1.024);
  MS-DOS-MEM selbst nur beim Nutzer prüfbar.
  AUD_CLOSE nur im
  eigenen Task (`sb_stop`), sonst räumt aud_task_end auf. Offen: BLASTER-
  Variable in der Box, 16-Bit/SB16-DSP, Aufnahme, Mixer-Ports 224h/225h.
  Zweite Grafiksitzung in derselben Box (#UD im SeaVGABIOS bei C000:8008):
  `rom_uncow` verwirft beim Verlassen der Grafik die per rom_cow kopierten
  ROM-Seiten (UMB/EMS-Rahmen bleiben); `umb_scan` lässt bei SeaBIOS
  (Kennung im F-Segment) E0000–EFFFF aus (zonelow des BIOS).
  MS-DOS 6.22 MEM/MSD in der Box (vom Nutzer): XMS 0 K bzw. Extended
  65535K -> CMOS 70h/71h nachgebildet (17h/18h, 30h/31h = HMA + XMS der
  Box, sonst echte CMOS lesend); MSD blinkte -> INT 10h 1003h schaltet
  Attribut-Modusregister Bit 3 (`blink_hw`, bei Ende/Parken zurück).
  MEM zeigte danach weiter „Erweiterung (XMS) 0K“ (auch mit der 4309h-
  Tabelle): MEM nimmt die Gesamtgröße aus LoL+45h („erweiterter Speicher
  beim Start“, MS-DOS-4.0-MEM.C `ExtendedMemory`), Frei/Belegt kappt es
  darauf – FreeDOS setzt das Feld nie. BOXSHELL (`set_xmssize`) trägt
  vor jedem Programm CMOS 30h/31h dort ein; die Nachbildung liefert HMA
  + XMS-Grenze, gekappt auf belegte EMBs + freien Systemspeicher (sonst
  bei -1 riesige „belegt“-Werte). In QEMU geprüft: 2.112 KB (Vorgabe),
  mit -1 bei 16 MB 13.660 KB = 64 + 13.596 frei.
  Vom Nutzer in 86Box bestätigt (Build 339): 386/4 MB mit zwei Boxen
  1.204K/92K/1.112K, 16-MB-VM mit einer und zwei Boxen 2.036K/92K/
  1.944K (92K = vermutlich FreeCOM „XMS_Swap“). Danach (Wunsch des
  Nutzers): `xms_table` meldet jede Lücke zwischen EMBs als eigenen
  freien Eintrag (Gang wie emb_gap, gekappt auf den freien System-
  speicher, Adresse/Größe 32 Bit über `xms_ent`), damit frei + belegt
  die Grenze trifft – in QEMU mit HT2.COM geprüft (100/200/100 KB
  anlegen, mittleren frei: 4 Einträge, 1.848 + 200 = 2.048 KB).
  Virtuelle VGA, Stufe 2 – Grafik-Boxen im Hintergrund (in QEMU geprüft:
  `\DEMO\BGTEST.COM` (apps/bgtest.asm, `4` = CGA, `12` = planar) 13h zeichnet je BIOS-Takt eine Zeile und stellt bei Zeile 100
  Farbe 1 per 3C8h/3C9h auf Rot – START, Alt+Esc weg bei Zeile 71, nach
  5 s zurück bei Zeile 169, ganzes Bild rot; BG4.COM dasselbe in CGA-
  Modus 4: 70 -> 195 Zeilen; CMD vorn sauber; DOOM (Mode Y) hält wie
  bisher an und läuft danach weiter): wie VVGA, aber nur für Modi, deren
  Bild für die CPU linearer Speicher ist. v86.c `vga_allow` merkt beim
  Moduswechsel übers BIOS im Puffer (RAW_ALLOW, Byte 62 hinter den 61
  Registern), ob der Modus taugt (13h -> 1, 4–6 -> 3). vgagfx.c
  `raw_out`: `raw_kind` prüft die Register (Chain-4 bzw. B8000, GC1/GC3
  0, Schreib-/Lesemodus 0, Bitmaske FFh, bei CGA Read Map < 2) und
  kopiert vor `raw_flat` die Sicht der CPU (64 KB A0000 bzw. 32 KB
  B8000) nach RAW_LIN (Puffer jetzt 4×64 KB + 4 KB + 64 KB), Art in
  RAW_KIND; `raw_in` schreibt nach den Registern den Schatten zurück
  (Map Mask kurz 0Fh/03h). v86.c `vga_freeze` (vor jedem Rücksprung in
  die Box): vorn Karte (`vga_map(t, 0)`), hinten mit Art -> `vga_map(t,
  k)`: PTE des Fensters auf den Schatten, Rest Ersatzseite, Ports
  gesperrt; sonst `vga_wait` (wie früher). Ports hinten (`vga_pin`/
  `vga_pout`) gehen an die gesicherte Registerkopie im Puffer, Zustand
  (Indizes, Attribut-Flipflop, DAC-Zähler) bei RAW_ST; was die
  Abbildung ändert (SEQ 4 Chain-4/Odd-Even, GC 1/3/5/6/8 nicht einfach)
  -> `vga_wait`, dann an die Karte. Jedes INT 10h ans Video-BIOS wartet
  ebenfalls (vga_want), auch ein Grafikmodus, der hinten verlangt wird.
  Lehre: `for (o = 0; o < 2 * n; ...)` mit n = 8000h – 2 * n ist in
  16 Bit 0, die Schleife lief nie (der Schatten blieb leer, beim
  Zurückholen überschrieb er das Bild).
  Vom Nutzer in 86Box bestätigt: BGTEST wächst hinten weiter und wird
  rot. `BGTEST 4` blieb cyan – Testfehler: in CGA-Modus 4 zeigt Farbe 1
  über Attributregister 1 auf DAC 13h, nicht DAC 1; BGTEST liest jetzt
  das Attributregister (3DAh, 3C0h = 21h, 3C1h) und stellt diesen
  Eintrag um (prüft so auch das Lesen der Attributkopie hinten; in QEMU
  danach rot).
  Planare Modi hinten (Phase 4, Rest; in QEMU mit `BGTEST 12` geprüft:
  54 -> 200 Zeilen, rot): Art 2 in `raw_kind` (BIOS-Modus 0Dh/0Eh/10h/
  12h -> RAW_ALLOW 2, 13h -> 1 erlaubt auch Mode X; Chain-4 aus, SEQ 4
  Bit 2, GC5 ohne Odd/Even, A0000). Kein Schatten nötig – die vier Ebenen
  liegen schon im Puffer (raw_copy holt sie in raw_in zurück, raw_lin nur
  für Art 1/3). v86.c `vga_plmap`: A0–AF zeigen auf die Ebene der Map
  Mask im Puffer, wenn die Register einfach sind und genau eine Ebene
  gewählt ist (Map Mask 0: Read Map nur lesend), sonst nicht eingeblendet;
  neu nach jedem Schreiben auf SEQ 2 und GC 0–8 (außer 6). Seitenfehler
  dort (`vga_pf`, V86 und dp_trap) -> vga_wait, danach läuft der Befehl
  auf der Karte. DOOM 1.9 (Mode Y) wartet damit gleich bei der ersten
  Latch-Kopie (`mov cl,[ebx]` … `mov [edx],cl` in Schreibmodus 1, GC5 =
  41h, Map Mask 0Fh) – nachbilden hieße einen Befehls-Nachbildner je Byte;
  lohnt nicht (DOOM spielt man nicht hinten, Ton ist dort stumm). In QEMU
  geprüft: START DOOM, weg, VER, zurück, läuft weiter.
  INT 10h hinten (`vga_bgint` vor vga_wait in vga_want): 0Fh aus dem BDA,
  1000h/1001h/1002h/1007h/1008h Attribute, 1010h/1012h/1015h/1017h DAC
  in der Registerkopie – in QEMU mit einer Abwandlung von BGTEST geprüft
  (Zeile 100: 0Fh + 1010h grün, 200 Zeilen, grün).
  Offen: Latch-Kopien und SVGA hinten.
  Entscheidung des Nutzers (Start von DOS-Programmen aus CMD.EXE):
  **wie NT in derselben Sitzung** – DOS-`.EXE`/`.COM`/`.BAT` und
  `COMMAND.COM` laufen synchron in einer Box (Vollbild, danach zurück zum
  Prompt, Text bleibt stehen, ERRORLEVEL kommt an; Stapeldateien laufen
  der Reihe nach). Eigene Sitzung mit `START`. Der OS/2-Weg (jedes
  DOS-Programm in neuer Sitzung) soll per CONFIG.SYS zuschaltbar werden,
  z. B. `DOSSTART=SESSION`. Später: Box je Sitzung offen halten und
  wiederverwenden wie NTVDM (spart den FreeDOS-Start).
- Testplan DOS 6.0 (bis Phase 2; ab Phase 3 siehe „Testmaschinen 6.0“
  oben): Messmaschine 386SX mit 2 MB in 86Box, sonst gleiche
  Ausstattung wie der SCAT (ET4000, Adaptec, IDE, SB16), damit Ergebnisse
  vergleichbar sind; erste Phase auf 386SX (24 Adressleitungen, 16-MB-Grenze)
  und 386DX in 86Box, danach Umstieg auf 486DX. Build mit `cpu 386` / `-3`,
  damit keine 486-Befehle (`BSWAP`, `XADD`, `CMPXCHG`, `INVD`, `WBINVD`,
  `INVLPG`) hineinrutschen.
- Komprimierung: Ein DoubleSpace-Container hat Schwächen (feste Größe,
  geschätzter freier Platz, ein Fehler trifft das ganze Laufwerk); besser
  ist sie im Dateisystem eingebaut wie bei NTFS (ab NT 3.51, Attribut pro
  Datei, Blöcke von 16 Clustern) bzw. ZFS/btrfs (pro Block, Unkomprimierbares
  bleibt roh) – braucht Extents statt FAT-Kette, schnelles LZ-Verfahren
  für den 386. FAT32 bleibt davon unberührt. Eigenes Dateisystem mit
  Komprimierung: Option für 6.x/7.0, noch nicht entschieden.
  DoubleSpace bleibt im Plan (Entscheidung des Nutzers), vor allem wegen
  der Grundlage: **Laufwerk aus einer Datei** (Loop-Device) im Kernel –
  eine Datei auf einem eingehängten Laufwerk wird als Blockgerät
  eingehängt (auch für Disketten- und Plattenabbilder nützlich). Darauf
  `DBLSPACE.IFS` (Containerdatei `DBLSPACE.nnn`, Format von MS-DOS 6 zum
  Austausch), Werkzeuge `DBLSPACE.EXE` (anlegen, einhängen) und
  `UDBLSPACE.EXE` (CHKDSK). Zeitlich 6.0.
- UEFI (Entscheidung des Nutzers: ab 7.0, mit dem 32-Bit-Kernel): Lader wechselt aus Long Mode in den 32-Bit-
  Protected-Mode. Ersatz für alles, was heute im Real Mode vom BIOS kommt
  (Speicherkarte, Plattenparameter, Zeichensätze, Vorladen von CONFIG.SYS)
  über UEFI-Dienste vor ExitBootServices. Nötig: Konsole im GOP-Framebuffer
  (oberhalb 16 MB, kein B8000h), AHCI/NVMe statt IDE, xHCI/USB-Tastatur
  (oft keine PS/2-Emulation), APIC/HPET, falls PIC/PIT fehlen. 6.0 (Ziel
  P3B-F, hat noch BIOS) braucht UEFI nicht; bis dahin laufen Boards mit
  CSM (bis ~2020) und VMs.
- `COUNTRY=` richtig auswerten (Wunsch des Nutzers, ohne Vorrang – es
  gibt auch andere Länder): heute nimmt der Kernel die Zeile nur an, AT-DOS
  ist fest deutsch, die Box fest `COUNTRY=049,437` (freedos/CONFIG.SYS).
  Ziel: Land aus der CONFIG.SYS für Datum/Zeit/Zahlen/Währung in CMD und
  den Werkzeugen (FAPI-Abfrage wie OS/2 DosGetCtryInfo) und an die Box
  weiterreichen.
- Idee: ext4 als `.IFS`, zuerst nur lesend (Vorbild: GRUB `ext2.c`, liest
  ext2/3/4 inkl. Extents; GPLv3+). Voraussetzung: Adaptec, Partitionstabelle,
  32-/64-Bit-Blocknummern im 16-Bit-Kernel. Schreiben (Journal, Prüfsummen)
  erst viel später, wenn überhaupt.
- Ganz am Ende (Entscheidung des Nutzers, er hat das Programm selbst
  nicht): ein DOOM-Nachbau „v2.02, Built Oct 9 1999“ (nur von mir zum
  Testen besorgt) hängt in der Box bei I_Init – auch mit dem Stand vor
  der virtuellen SB16, also nicht deren Fehler.
