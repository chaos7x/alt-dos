# alt-dos

Hobby-Betriebssystem aus einer alternativen Zeitlinie: Microsoft führt das
Multitasking-DOS 4.0 (1986) als offizielle DOS-Linie fort, statt auf OS/2 zu
setzen. Spezifikation der ganzen Zeitlinie (DOS 5.0–9.0, FAPI, NT-Linie):
https://claude.ai/code/artifact/43b28dce-1011-46c8-ad1a-e3a2255adff8

Sprache im Projekt: Deutsch (Kommentare, Commits, Doku).

Lizenz: vorläufig MIT (`LICENSE`, vom Nutzer bewusst so gesetzt), endgültig
noch nicht entschieden; das Projekt ist nicht öffentlich. Entscheidung
(Nutzer) vor Beginn von DOS 6.0 – evtl. Wechsel auf GPL, solange er der
einzige Urheber ist; dann dürfte Code aus FreeDOS, 86Box, Linux, GRUB
übernommen werden (Herkunft je Übernahme vermerken). Fremdes DOS in der
V86-Box (FreeDOS, MS-DOS) geht unter jeder Lizenz. Bis dahin keinen
fremden Code übernehmen (GPL/LGPL wie 86Box, Wine,
ReactOS, GNU nur lesen) – so bleiben alle Wege offen. Größere fremde
Teile (z. B. Wine-Bibliotheken für Win32) nur als abgegrenzte Teilsysteme
neben dem Kernel denken.

Produktname in der Zeitlinie: **AT-DOS** (AT = Rechnerklasse und Alternative).

## Aktueller Stand: AT-DOS 5.0 (80286, ~1988/89), Meilenstein 8

README aktuell halten (Wunsch des Nutzers): jede Neuerung gleich im
README („Was läuft“, FAPI-Tabelle, Meilenstein in der Überschrift)
nachtragen, im selben Commit.

Meilensteine (Zählung des Nutzers): 7 = Werkzeuge wie MS-DOS 5/6 und
Treiber; 8 = lange Namen komplett (Stufen 1–4, EDIT), Platten-Cache im
Kernel, ANSI, JOIN, CD auf andere Laufwerke, Stapelsprache komplett
(MS-DOS 6.22 + OS/2 CMD), Dateimanager FM, DISKCOMP/COMP/REPLACE/EXPAND.
Plan (Entscheidung des Nutzers): Ton fertig (FM/OPL, MPU-401) -> 5.0
fertig (taggen, Zweig `5.x` für Fehlerbehebungen) -> 6.0 auf `main`;
asynchrone Plattenzugriffe in 6.0 bauen (Sperren je Gerät statt `klock`,
ohne 386-Abhängigkeit) und danach nach 5.x zurückübertragen. 5.1
(Kompatibilitätsbox) zurückgestellt – die echten DOS-Boxen in 6.0 gehen vor.

- **Alles 16 Bit.** Kein 32-Bit-Code, kein Flat Memory – das kommt erst mit DOS 7.0.
- **Nur 286-Befehle.** NASM-Dateien beginnen mit `cpu 286`, Watcom baut mit `-2`.
  Keine 386-Präfixe (66h/67h), kein Paging, kein V86.
- **Protected Mode, segmentiert**, Adressraum bis 16 MB.
- **Kooperatives Multitasking.** Kein präemptiver Scheduler vor DOS 6.0.
- **FAPI ist der einzige Weg zur Hardware.** Anwendungen laufen in Ring 3 mit
  IOPL 0 und haben keine Selektoren auf Hardware-Speicher. Nur der Kernel
  spricht Hardware an. Nie eine Abkürzung an der FAPI vorbei einbauen.
- **IFS** (Installable File System) ist Teil der FAPI-Architektur. FAT12/16
  steckt wie bei OS/2 fest im Kernel; weitere Dateisysteme (ISO 9660, das
  DOS-6.0-Dateisystem) kommen als nachladbare `.IFS`-Treiber.

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
  `COMMAND.EXE`, `CONFIG.SYS`, `AUTOEXEC.CMD` (+ `ATSYS.COM` auf der
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
  `mem.c` (physische Speicherverwaltung, Bitmap in 256-Byte-Einheiten),
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
- `apps/task_command.c` – `COMMAND.EXE`, der Befehlsinterpreter, mit
  Programmsuche über `PATH`, Stapelverarbeitung (Datei je Stufe in einem
  `DosAllocSeg`-Segment, bis 4 Stufen) und `AUTOEXEC.BAT`. Umgebung je Task
  im Kernel (`env_*` in kernel.c, ein Kernelsegment für alle Tasks plus
  Systemumgebung)
- `AUTOEXEC.CMD` – Startdatei des nativen Befehlsinterpreters (`PROTSHELL=`
  in CONFIG.SYS, Vorgabe `COMMAND.EXE /P`). Trennung wie OS/2: `.CMD` und
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
  `wcc -nt=KTEXT2` übersetzt (build.sh `K2MODS`, heute `floppy`, `vgagfx`).
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
  Stand: Code 48 + 8 KB (vorher 56 KB in einem Segment). `hdboot.asm` folgt der FAT-Kette, ATSYS/SYS
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
  COMMAND /P macht DosMkDir(%TEMP%). Bisher nutzt kein AT-DOS-Werkzeug
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
  `COMMAND /P` löscht die Datei und ruft `CHKDSK X: /F /AUTO`.
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

    ./build.sh          # NASM + Open Watcom -> build/dos5.img

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

Schnelltest headless in QEMU (`qemu-system-i386 -fda build/dos5.img`), der
echte Test ist eine 286-Maschine in 86Box. QEMU emuliert keinen echten 286
und prüft Segmentgrenzen nicht – ein Zugriff über die Grenze läuft in QEMU
durch und endet auf dem SCAT mit #GP. Deshalb bei jedem Zugriff über ein
Fenster (`SCRATCH`, `SCRATCH2`, Anwendungssegmente) die Grenze selbst prüfen.

CI baut bei jedem Push und veröffentlicht `dos5.img` als Release `latest`.

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
  Offen: Stufe 3 FM (OPL3 an 388h über FAPI), Stufe 4 MPU-401, ISA-PnP
  (SB16 PnP/AWE32 PnP, nach der Intel/Microsoft-Spezifikation), AWE32-
  Wavetable (6.0). SBEMU/VSBHDA (GPL) nur als Nachschlagewerk für die
  virtuelle SB der DOS-Boxen in 6.0.
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
  32 KB); PIIX4-IDE im Kompatibilitätsmodus wie heute, dazu Busmaster-DMA
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
- Speicher in DOS 6.0: Erkennung per INT 15h E820h (Speicherkarte mit
  Lücken), E801h als Rückfall, 88h zuletzt (nur 16 Bit KB, max. 64 MB).
  Ziel: auch VMs mit 128 MB und mehr (Nutzer hat DOS/Windows-VMs mit
  128 MB) richtig erkennen. Bitmap dann in 4-KB-Seiten (Paging) statt
  256-Byte-Einheiten, sonst 64 KB Bitmap für 128 MB.
- Testplan DOS 6.0: Messmaschine 386SX mit 2 MB in 86Box, sonst gleiche
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
- Idee: ext4 als `.IFS`, zuerst nur lesend (Vorbild: GRUB `ext2.c`, liest
  ext2/3/4 inkl. Extents; GPLv3+). Voraussetzung: Adaptec, Partitionstabelle,
  32-/64-Bit-Blocknummern im 16-Bit-Kernel. Schreiben (Journal, Prüfsummen)
  erst viel später, wenn überhaupt.
