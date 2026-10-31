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
  `preload()` sucht erst im Stamm, dann im Verzeichnis `\DOS`.
- Ablage auf den Abbildern: Stamm nur `ATDOS.SYS`, `COMMAND.EXE`,
  `CONFIG.SYS`, `AUTOEXEC.CMD` (+ `ATSYS.COM` auf der Diskette), alles andere
  in `\DOS` (`build.sh`: `FILES` bzw. `DOSFILES`). `load_driver()` und
  `KEYB` fallen bei reinem Dateinamen auf `\DOS` zurück; Standard-PATH
  `X:\DOS;X:\`. `ATSYS` kopiert `\DOS` mit.
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
- `apps/` – Ring-3-Anwendungen, `crt0.asm`, `fapi.h`
- `tools/` – `stub.asm` (DOS-Stub), `mkfx.py` (Abbild + Map -> FX-`.EXE`),
  `mkkbl.py` (alle Tastaturbelegungen: `.KBL`-Dateien und `kbd_gr.h` für den
  Kernel – Belegungen nur dort ändern)
- `include/fx.h` – Programmformat: MZ-Stub, `e_lfanew` -> FX-Kopf
- `include/fapi_abi.h` – FAPI-Funktionsnummern, gemeinsam für Kernel und Apps

## Konventionen

- Kernel und jede App sind je **ein** 64-KB-Segment: `_TEXT` liegt mit in
  DGROUP (siehe Segmentkopf in `start.asm`/`crt0.asm`), gelinkt als
  `wlink format raw bin offset=0`.
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
  SS = DS voraus). DGROUP liegt bei ~49 KB von 64 KB.
- Was mit der Konfiguration wächst, gehört nicht nach DGROUP, sondern in
  eigene Kernelsegmente: `kseg_alloc(bytes)` -> Selektor (GDT-Plätze
  `KSEG_IDX`, `NKSEG`), Zugriff über `__far`-Zeiger. Beispiel: Dateitabelle
  `files` (Größe `FILES=`). `KSEG_IDX(0)` ist fest die Belegungs-Bitmap der
  Speicherverwaltung bei linear 9000h–AFFFh (`MEMMAP_SEL`).
  `struct ifs_file` wird überall als `__far`-Zeiger übergeben.
- Der Kernel ist ein 64-KB-Segment ab 10000h; `ATDOS.SYS` (Datei) darf
  höchstens 48 KB groß sein (Diskettenbootsektor lädt 96 Sektoren), BSS dahinter nullt `start.asm` selbst.
- Bildschirm nur über Sitzungen (`vio.c`): Zeile 0 Titel, 1–23 Sitzung,
  24 Status. Programme schreiben in den Puffer ihrer Sitzung.
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

## Geplant (noch nicht umgesetzt)

- `ERRORLEVEL`: Programme liefern noch keinen Rückgabewert (`DosExit` ohne
  Code); `IF ERRORLEVEL n` ist nur für n = 0 wahr. Mit Rückgabewerten
  nachziehen (DosExit(code), DosExecPgm liefert ihn).
- Nächster Schritt: die Werkzeuge, die MS-DOS hatte, als FAPI-Programme
  (z. B. FORMAT, CHKDSK, XCOPY, ATTRIB, LABEL, MORE, FIND, SORT, EDIT, DISKCOPY).
  Grundsatz: alles, was zur Kompatibilität gehört, wie bei MS-DOS (Namen,
  Schalter mit `/`, Meldungen, Verhalten). GNU-Vorbild nur für Werkzeuge, die
  MS-DOS nicht hatte. Wichtigste Werkzeuge für den Nutzer: `LESS` und `GREP`. `MORE` bleibt aus
  Kompatibilität (einfach, nur vorwärts, wie MS-DOS); `FIND` ebenso neben `GREP`.
  Vorbild für das Verhalten sind die GNU-Werkzeuge, besonders `less`:
  vor- und zurückblättern, Suche mit `/` und `n`, Sprung an Anfang/Ende,
  Zeilennummern – statt des einfachen MORE von MS-DOS. Nur Verhalten und
  Bedienung übernehmen, keinen GNU-Code (Projekt ist MIT, GNU ist GPL).
  `DIR /P` blättert seitenweise.
- `EDIT` gehört zu den wichtigsten Werkzeugen (neben `LESS`, `GREP`):
  Vollbild-Editor im Stil von MS-DOS EDIT, nutzt `SIG_SHUTDOWN`, um vor dem
  Herunterfahren nach dem Speichern zu fragen.
- Maus: ein Treiber `MOUSE.SYS`, der wie MOUSE.COM von Microsoft/Logitech
  selbst sucht und nimmt, was da ist: PS/2 (Hilfsport des Tastatur-
  controllers, IRQ 12), Busmaus (InPort/Logitech, 23Ch, ID-Register lesen),
  seriell (COM1/COM2: UART erst lesend prüfen, dann DTR/RTS umschalten, die
  Maus antwortet mit 'M'). Optional per Argument festlegen
  (`DEVICE=MOUSE.SYS /COM2`). Braucht Treiber mit Interrupt (DevHlp SetIRQ).
  FAPI wie das Mou-API von OS/2: Ereignisse je Sitzung (nur Vordergrund),
  Zeiger als invertierte Textzelle. Erster Nutzer: EDIT. Test zuerst mit
  serieller Microsoft-Maus in 86Box.
- EDIT im Stil von MS-DOS EDIT/QBasic (Textmodus, Farben, Menüleiste mit
  Alt, Dialoge, CUA-Tasten), dazu Statuszeile, Suchen/Ersetzen. Braucht
  FAPI: Zellen mit Attribut schreiben, Cursor setzen, Sondertasten.
- CHKDSK: Querverbindungen beheben (Kopie der gemeinsamen Cluster, wie
  ScanDisk), Datei-Fragmentierung anzeigen; FORMAT, LABEL, DISKCOPY über
  dieselbe DosOpenDrive-Schnittstelle, FORMAT ebenfalls über
  `U<Dateisystem>.EXE` (wie OS/2: `UFAT`/`UHPFS` machen CHKDSK und FORMAT).
- Meldung „Laufwerk nicht bereit“ statt „nicht gefunden“, wenn keine CD
  drin ist
- Joliet (lange Namen auf CD), wenn lange Namen im FAPI ankommen
- Platten-Cache wie SMARTDRV (Ausbau des Sektor-Caches): größer als ein
  Segment, verzögertes Schreiben mit Zeitgrenze,
  Leeren bei `SHUTDOWN`, Strg+Alt+Entf und Laufwerkssperre; Disketten nur
  Lese-Cache oder sofort schreiben (Wechsel). Heute: Daten sofort, nur FAT
  im 2-Sektor-Cache.
- `COPY` mit Verketten (`A+B`), `XCOPY`, `ATTRIB`, Umleitung `>`
- `ATSYS`: CHS-Anfang der Partition im MBR gegen die BIOS-Geometrie prüfen
  und warnen (siehe Falle oben); eigener MBR mit LBA-Umrechnung statt CHS
  aus der Tabelle
- Treiber mit Interrupt statt Abfrage (DevHlp SetIRQ)
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
- Lange Namen auf FAT (falls vor DOS 6.0 gewünscht): IBM/OS/2-Modell, also
  `.LONGNAME` als erweitertes Attribut in `EA DATA. SF`, nicht VFAT. Grund:
  MS-DOS-Werkzeuge (CHKDSK, ScanDisk) sehen nur eine normale Datei, während
  sie VFAT-Einträge als Fehler „reparieren“. Dazu ein Prüfprogramm, das
  verwaiste EAs aufräumt.
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
  Vorarbeit schon in 5.0: Kernel aus dem einen 64-KB-Segment holen (Tabellen,
  Stacks, Puffer in eigene Segmente), wie der 16-Bit-Kernel von OS/2 1.x.
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
- Idee: ext4 als `.IFS`, zuerst nur lesend (Vorbild: GRUB `ext2.c`, liest
  ext2/3/4 inkl. Extents; GPLv3+). Voraussetzung: Adaptec, Partitionstabelle,
  32-/64-Bit-Blocknummern im 16-Bit-Kernel. Schreiben (Journal, Prüfsummen)
  erst viel später, wenn überhaupt.
