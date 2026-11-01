# alt-dos

Hobby-Betriebssystem aus einer alternativen Zeitlinie: Microsoft führt das
Multitasking-DOS 4.0 (1986) als offizielle DOS-Linie fort, statt auf OS/2 zu
setzen. Spezifikation der ganzen Zeitlinie (DOS 5.0–9.0, FAPI, NT-Linie):
https://claude.ai/code/artifact/43b28dce-1011-46c8-ad1a-e3a2255adff8

Sprache im Projekt: Deutsch (Kommentare, Commits, Doku).

Produktname in der Zeitlinie: **AT-DOS** (AT = Rechnerklasse und Alternative).

## Aktueller Stand: AT-DOS 5.0 (80286, ~1988/89), Meilenstein 7

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
  Datei, zusammenhängend ab Cluster 2, auf 48 KB aufgefüllt) nach 1000:0000;
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

- Jede App ist **ein** 64-KB-Segment: `_TEXT` liegt mit in DGROUP (siehe
  `crt0.asm`), gelinkt als `wlink format raw bin offset=0`.
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
  höchstens 48 KB groß sein (Diskettenbootsektor lädt 96 Sektoren), BSS dahinter nullt `start.asm` selbst.
- Bildschirm nur über Sitzungen (`vio.c`): Zeile 0 Titel, 1–23 Sitzung,
  24 Status (bei 43/50 Zeilen entsprechend tiefer). Programme schreiben in
  den Puffer ihrer Sitzung (Puffer immer 50 Zeilen groß). `VioSetMode(1)`
  (Entscheidung des Nutzers, wie MS-DOS EDIT/OS/2-Vollbild): Sitzung hat
  alle Bildschirmzeilen, Titel- und Statusleiste
  fehlen, solange sie im Vordergrund ist; der normale Inhalt wird in eigenem
  Speicher gesichert und bei `VioSetMode(0)` oder Programmende
  (`vio_task_end` in `task_end`) wiederhergestellt. `ses[].top` (1/0) und
  `rows` statt fester Konstanten benutzen.
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
- Pfade: der Kernel löst sie je Task auf (aktuelles Laufwerk und Verzeichnis)
  und reicht den IFS-Treibern kanonische Pfade `\DIR\DATEI` weiter.
- Sektornummern sind 32 Bit (`unsigned long`); Multiplikation mit der
  Clustergröße per Schieben, keine Watcom-Hilfsroutinen.
- Dateisysteme nur über die IFS-Schnittstelle (`struct ifs_driver`); außer
  `fat.c` kennt kein Kernelteil FAT-Interna. Blockgeräte nie per BIOS ansprechen.
- Sektor-Cache (`BUFFERS=`, kernel.c `bc_*`): ein Kernelsegment mit
  n × 512 Byte, nur 512er-Geräte, write-through. `blk_read_n()` liest bei
  Fehltreffern bis zu 4 Sektoren am Stück (DMABUF 2 KB) – `fat_read()`
  bis Clusterende. Vorausgelesenes ist „Strom“ und wird zuerst verdrängt
  (FAT/Verzeichnisse bleiben). Treiber melden in `drv_req.count` beim INIT,
  wie viele Blöcke ein READ kann (0 = 1); IDE und Adaptec: 16. Diskette:
  `blk_invalidate()` wirft ihre Einträge bei jedem `revalidate` raus.
  Messung `grep -r` über 120 Dateien in QEMU: 3095 -> 1173 IDE-Befehle.
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
  Disketten werden vor jeder Operation neu eingehängt (`revalidate`).
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
Interpreter, kein Dynarec), 287XL, 16 MB RAM, vier Everex EV-159 als EMS
(zusammen 11 MB, Ports 208h/218h, 258h/268h, 2A8h/2B8h, 2E8h; von DOS 5.0
nicht genutzt, erst für V86-Sitzungen in DOS 6.0 interessant), NE2000,
Tseng ET4000/W32i, Diskette A: 3,5" 1,44 MB, SCSI Adaptec AHA-154xC
(Port 334h, IRQ 11, DMA 6, BIOS C800h), Sound Blaster 16 (220h, MPU-401 330h,
IRQ 5, DMA 1/5, DSP 4.05). Das SCAT-BIOS meldet 640 KB +
15232 KB erweitert, dazu 384 KB Shadow-RAM für System- und Video-BIOS; es
fehlen 128 KB zu 16 MB. Am Adaptec: SCSI-ID 0 Festplatte (BIOS-Laufwerk C:),
SCSI-ID 6 CD-ROM.

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
  Maus, horizontale Bildlaufleiste. Erledigt: Öffnen-Dialog mit Dateiliste
  (`open_dialog`, Verzeichnisse zuerst, `..`, Muster, Laufwerk `D:`),
  Nachfrage bei SIG_SHUTDOWN: Signal setzt nur `shut_req`, `getkey()`
  (statt KbdCharIn überall in EDIT) ruft `shutdown_now()` beim nächsten
  Warten; bei Änderungen `DosShutdownHold` (FAPI 37h: Sitzung nach vorn,
  `end_programs` wartet ohne Frist, solange ein Task `hold` hat; zweites
  Strg+Alt+Entf während `going_down` startet sofort neu). Danach nächster
  Schritt mit dem Nutzer absprechen. FORMAT und SYS sind da. Später: UNDELETE, MSD,
  HELP, DISKCOPY, FDISK, DEFRAG, SCANDISK, PRINT/MODE, SUBST.
  Schon da: CHKDSK, XCOPY, ATTRIB, LABEL, MORE, FIND, SORT, FC, TREE,
  MOVE, DELTREE, CHOICE; VOL/DATE/TIME in COMMAND.
  Grundsatz: alles, was zur Kompatibilität gehört, wie bei MS-DOS (Namen,
  Schalter mit `/`, Meldungen, Verhalten). GNU-Vorbild nur für Werkzeuge, die
  MS-DOS nicht hatte. Wichtigste Werkzeuge für den Nutzer: `LESS` und `GREP`. `MORE` bleibt aus
  Kompatibilität (einfach, nur vorwärts, wie MS-DOS); `FIND` ebenso neben `GREP`.
  Vorbild für das Verhalten sind die GNU-Werkzeuge, besonders `less`:
  vor- und zurückblättern, Suche mit `/` und `n`, Sprung an Anfang/Ende,
  Zeilennummern – statt des einfachen MORE von MS-DOS. Nur Verhalten und
  Bedienung übernehmen, keinen GNU-Code (Projekt ist MIT, GNU ist GPL).
  `DIR /P` blättert seitenweise.
- `EDIT` gehört zu den wichtigsten Werkzeugen (neben `LESS`, `GREP`).
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
- Meldung „Laufwerk nicht bereit“ statt „nicht gefunden“, wenn keine CD
  drin ist
- CHKDSK auf CD-ROM (Entscheidung des Nutzers, festgehalten): heute „CHKDSK
  kann CD-ROM-Laufwerke nicht prüfen“, solange es kein U*.EXE gibt. Später
  denkbar: Lesbarkeitsprüfung (Oberflächentest) für CDs, und ein Dateisystem
  für beschreibbare Medien bringt sein eigenes U*.EXE mit – CHKDSK nimmt
  es dann automatisch (Reihenfolge in task_chkdsk.c: erst U*.EXE suchen,
  Meldung nur, wenn keins da ist).
- Lange Namen auf CD (Wunsch des Nutzers): `ISO9660.IFS` um Joliet
  (Zusatz-Deskriptor mit UCS-2-Namen, Windows 95), ISO 9660:1999 (Level 4,
  bis 207 Zeichen) und evtl. Rock Ridge erweitern; Namen über das FAPI mit
  bis 260 Zeichen, 8.3 nur als Rückfall. Kopieren auf FAT braucht dann die
  `.LONGNAME`-EAs (siehe unten), sonst 8.3-Ersatznamen. UDF (DVD, ab 1996)
  als eigenes `UDF.IFS` – zeitlich DOS 6.0/7.0.
- Platten-Cache wie SMARTDRV (Ausbau des Sektor-Caches): größer als ein
  Segment, verzögertes Schreiben mit Zeitgrenze,
  Leeren bei `SHUTDOWN`, Strg+Alt+Entf und Laufwerkssperre; Disketten nur
  Lese-Cache oder sofort schreiben (Wechsel). Heute: Daten sofort, nur FAT
  im 2-Sektor-Cache.
- `COPY` mit Verketten (`A+B`); COPY sollte wie XCOPY Datum/Uhrzeit erhalten
- `ATSYS`: CHS-Anfang der Partition im MBR gegen die BIOS-Geometrie prüfen
  und warnen (siehe Falle oben); eigener MBR mit LBA-Umrechnung statt CHS
  aus der Tabelle
- Treiber mit Interrupt: Grundlage und IDE erledigt (siehe Konventionen).
  `AHA154X.SYS` mit IRQ auf dem SCAT bestätigt (Anzeige „IRQ 11, DMA 6“,
  Start von der Adaptec-Platte über den Interrupt, 3× zwei XCOPY
  gleichzeitig über IDE/Adaptec/CD ohne Fehler). IRQ aus Befehl 0Bh
  (Byte 2: Bit 0–3 IRQ 9–12, Bit 5/6 IRQ 14/15), Routine quittiert bei
  INTV mit IRST (sonst bleibt die ISA-Leitung oben) und weckt; fertig ist,
  wenn `mb[1].status` gesetzt ist (`wait_mbox`). In 86Box (`scsi_x54x.c`)
  löst START kein HACC aus, MBIF kommt nach HACC erst nach IRST.
  Offen: Diskette (kernel floppy.c) ebenfalls schlafen lassen statt
  HLT-Schleife, danach `MOUSE.SYS`. `has_opt()` und `hex()` in drvlib.h.
- Weitere SCSI-Treiber, Ziel NCR-Familie (in 86Box vorhanden): zeitlich
  passend für 5.0 der ISA-NCR 53C400 (z. B. Trantor T130B, PIO); die
  PCI-Chips 53C810/825/875 (SCRIPTS-Prozessor, PCI-Konfiguration mit
  32-Bit-Portzugriffen) gehören zu 6.0 auf dem 386/486. Die 53C8xx sind
  die Standard-SCSI-Controller des Nutzers auf PCI-Maschinen: für 6.0 der
  wichtigste neue Plattentreiber (`NCR8XX.SYS`, mit Interrupt).
- Laufwerkszugriffe asynchron statt synchron (andere Tasks laufen weiter)
- Idee „AT-DOS 5.1“: Kompatibilitätsbox wie bei OS/2 1.x – eine einzelne
  Real-Mode-DOS-Sitzung unter 640 KB (z. B. MS-DOS 6 von der SCSI-Platte),
  Rückweg in den Real Mode per CPU-Reset mit Shutdown-Code im CMOS, Box im
  Hintergrund eingefroren. Erweiterung der Zeitlinie (Spezifikation sieht
  Altkompatibilität erst mit DOS 6.0/V86 vor).
- Codesegmente zwischen Instanzen desselben Programms teilen (FX-Kopf muss
  Code und Daten getrennt beschreiben)
- Optional: DOS-Extender als Stub, damit FAPI-Programme auch unter MS-DOS laufen
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
- FPU: Zustand des 287 pro Task sichern (MSW.TS/MP setzen, Taskwechsel per #NM)
- Zweite Referenzmaschine: 286 mit 10–12 MHz für zeitgemäße Geschwindigkeit
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
  Verhalten zu verstehen, nie Code übernehmen (AT-DOS ist MIT). Datenblätter
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
- Idee: ext4 als `.IFS`, zuerst nur lesend (Vorbild: GRUB `ext2.c`, liest
  ext2/3/4 inkl. Extents; GPLv3+). Voraussetzung: Adaptec, Partitionstabelle,
  32-/64-Bit-Blocknummern im 16-Bit-Kernel. Schreiben (Journal, Prüfsummen)
  erst viel später, wenn überhaupt.
