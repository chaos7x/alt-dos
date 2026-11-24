# F5-Notstart und BASEDEV= (Konzept)

Stand: 24.11.2026, vom Nutzer gewünscht („ok. mach das“). Nur Plan,
noch kein Code. Umsetzung erst nach Freigabe des Nutzers.

## Ziel

Wie F5 bei MS-DOS 6 („MS-DOS wird ohne Ihre Konfigurationsdateien
gestartet“): ein Start, der fast nichts aus der Konfiguration nimmt und
deshalb auch dann hochkommt, wenn CONFIG.SYS, ein Treiber, AUTOEXEC.CMD
oder DOSBOX.INI kaputt sind. Danach kann man die Dateien mit EDIT
reparieren.

Im Notstart fällt weg:

- `DEVICE=`, `DEVICEHIGH=`, `IFS=` (keine Treiber, keine IFS-Module –
  CD und Netz fehlen also)
- `LOOP=` (keine Abbilder) und die Auslagerungsdatei (`SWAPPATH=` wird
  übergangen, SWAPPER.DAT bleibt zu)
- `RUN=`
- `PROTSHELL=`: es startet immer `X:\CMD.EXE` des Startlaufwerks,
  ohne AUTOEXEC.CMD
- alle anderen Zeilen (`SET`, `CACHE=`, `COUNTRY=`, `CODEPAGE=`,
  `LONGNAMES=`, `DOS=`, `RMSIZE=` …) – es gelten die Vorgaben des Kernels
- ISA-PnP (`pnp_init` läuft nicht; weniger Hardware, die stören kann)
- die DOS-Box nimmt ATDOSCM.SYS mit Vorgaben: DOSBOX.INI,
  `\DOS\MDOS\CONFIG.SYS` und die AUTOEXEC.BAT der Box werden nicht
  gelesen

Was bleibt: `BASEDEV=`-Zeilen (Treiber für das Startlaufwerk, s. u.),
der Kernel mit FAT, Diskette, Tastatur und Bildschirm.

Ein Hinweis zeigt, dass es ein Notstart ist:

- Startmeldung „F5: Notstart – ohne CONFIG.SYS und AUTOEXEC.CMD“
- in der Statuszeile „Notstart“ (statt der Laufzeit oder davor)
- Variable `NOTSTART=1` in der Systemumgebung (Stapeldateien und Werkzeuge
  können darauf reagieren)

## Das Henne-Ei-Problem

Der Plattentreiber für das Startlaufwerk (IDE.SYS, AHA154X.SYS,
NCR8XX.SYS, AIC7XXX.SYS, AHCI.SYS, VIOBLK.SYS …) steht in der CONFIG.SYS.
Ohne ihn findet der Kernel die Startpartition nicht und hält heute mit
„Startpartition nicht gefunden – Treiber in CONFIG.SYS?“ an
(kernel.c `kmain` nach `config_sys`). Ein Notstart, der alle `DEVICE=`
weglässt, käme also nie bis zur Eingabe.

Lösung in zwei Teilen:

1. **`BASEDEV=`** wie bei OS/2: eine eigene Zeilenart für die Treiber,
   die das System zum Starten braucht. Der Notstart lädt sie immer.
2. **INT 13h als Rückfall**: scheitert ein BASEDEV-Treiber, fehlt er oder
   findet er die Startplatte nicht, liest und schreibt der Kernel das
   Startlaufwerk über das BIOS.

## Bestand im Code (geprüft 24.11.2026, main 569f363)

- **F8** gibt es: `start.asm` liest vor `preload` die schon getippten
  BIOS-Tasten (Scancode 42h -> `boot_f8 = 1`), `kernel.c f8_wait` gibt
  vor `config_sys` eine Sekunde, `cfg_ask` fragt jede Zeile, `step_arg`
  hängt `/STEP` an eine CMD-PROTSHELL. **F5 gibt es nicht.**
- **INT 13h zur Laufzeit gibt es nicht.** INT 13h benutzt nur der Real
  Mode vor dem Kernel: Bootsektoren, `preload.c` über `rm_read`
  (start.asm; CHS zuerst, 42h nur hinter Zylinder 1023 – wegen des
  Award-BIOS des P3B-F), dazu Geometrieabfragen (`bios_geo_*`,
  `rm_geometry`). In den DOS-Boxen ist INT 13h nachgebildet (nur das
  Startabbild, v86.c), mit der echten Platte hat das nichts zu tun.
- **Real-Mode-Ausflug gibt es**: `rm_int10` (start.asm, KTEXT2) für
  Video-BIOS und APM (`rm_call` in vesa.c, `intno` 10h/15h). Er maskiert
  den PIC ganz, schaltet Paging/PE aus, Register bei 6F00h, Puffer ab
  7000h. Für INT 13h reicht das so nicht (s. u.).
- **Plattentreiber** lädt `config_sys` aus `DEVICE=`-Zeilen über
  `load_driver(v, 0)`, danach `mount_disk` für jedes neue Blockgerät.
  Vorher hat `preload` (Real Mode) CONFIG.SYS und alle `DEVICE=`/`IFS=`-
  Dateien per BIOS nach 80000h (bzw. 0600h) gelesen, das Pseudo-
  Dateisystem `BOOT` stellt sie als Z: bereit (`PRE_DRIVE`). Danach sucht
  `find_boot_vol` die Startpartition (Startsektor + Seriennummer),
  sonst Panik. Blockgeräte haben `blk[].drv` = Treibernummer bzw. -1
  Diskette, -2 Loop, -3 Netz.
- Die mitgelieferte CONFIG.SYS lädt alle Plattentreiber mit `DEVICE=`
  (AHA154X, IDE, NCR5380, NCR8XX, AIC7XXX, AHCI, REM VIOBLK); SETUP
  (`write_cfg`) setzt nicht geladene auf `REM`.

## BASEDEV=

Syntax wie `DEVICE=`: `BASEDEV=IDE.SYS /DMA`. Suche wie `load_driver`
(reiner Name: `\DOS\SYSTEM`, dann `\DOS`).

- **Normaler Start**: alle `BASEDEV=`-Zeilen kommen in einem ersten
  Durchgang vor allen anderen Zeilen dran (wie OS/2, unabhängig von ihrer
  Stelle in der Datei), danach der Rest der CONFIG.SYS wie heute. Damit
  steht das Startlaufwerk fest, bevor andere Treiber laden.
- **Notstart**: nur dieser erste Durchgang, mit den Argumenten aus der
  Zeile.
- **F8**: fragt auch `BASEDEV=`-Zeilen. F5 und F8 zusammen = Notstart mit
  Frage je BASEDEV-Zeile – so kommt man an einem Treiber vorbei, der
  beim INIT hängt (dagegen hilft der Rückfall nicht).
- Gedacht für Blockgeräte-Treiber. Andere Treiber nimmt der Kernel auch
  an (wie OS/2), die Doku rät davon ab.
- `preload.c` liest auch `BASEDEV=`-Dateien vor (Wortvergleich wie bei
  `DEVICE`/`IFS`). Im Notstart (F5 ist vor `preload` bekannt) nur
  CONFIG.SYS und die BASEDEV-Dateien – der Vorladebereich (16 Dateien,
  ~127 KB) kann dann nicht überlaufen.
- Alte CONFIG.SYS mit Plattentreibern unter `DEVICE=` laufen normal wie
  bisher; im Notstart nimmt der Kernel dort den INT-13h-Rückfall.
- Mitgelieferte CONFIG.SYS: die Plattentreiber werden `BASEDEV=`
  (VIOBLK als `REM BASEDEV=`), Erklärung in einer REM-Zeile. SETUP
  (`write_cfg`, Hardware-Erkennung `detect`) behandelt `BASEDEV=` wie
  `DEVICE=`. SYS und CONFIG.NEW unverändert.
- `DEVLOAD` zur Laufzeit bleibt bei `DEVICE`-Art; ein geladener
  BASEDEV-Treiber lässt sich wie heute nicht entladen, solange das
  Startlaufwerk an ihm hängt.

## INT 13h als Rückfall

Wann: nach dem BASEDEV-Durchgang (Notstart) bzw. nach der ganzen
CONFIG.SYS (normaler Start), wenn `find_boot_vol` nichts findet und von
Festplatte gestartet wurde. Statt der Panik legt der Kernel ein
Blockgerät `DRV_BIOS` (-4, unit = BIOS-Laufwerk, Vorgabe `boot_drive`)
an, ruft `mount_disk` und sucht die Startpartition erneut. Erst wenn auch
das scheitert, Panik wie heute. Meldung: „Startlaufwerk über das BIOS
(INT 13h) – langsam, Treiber in BASEDEV= prüfen“. Gilt in beiden
Startarten – auch ein normaler Start mit kaputter Treiberzeile kommt so
noch hoch.

Ablauf eines Auftrags (`bios_rw`, neues Modul `kernel/biosdisk.c` in
KTEXT4, Ausflug in start.asm KTEXT2 neben `rm_int10`):

- `blk_read`/`blk_read_n`/`blk_write` verzweigen bei `DRV_BIOS` vor dem
  Treiberaufruf (wie bei `DRV_LOOP`); der Platten-Cache gilt wie für
  andere Platten.
- Puffer unter 1 MB: Prallpuffer bei 7000h (2 KB = 4 Sektoren; 6000h–
  7FFFh bleibt laut kmain für Real-Mode-Aufrufe reserviert), danach über
  SCRATCH in den DMABUF des Tasks bzw. aus ihm. Größere Aufträge in
  Stücken.
- Adressierung wie `rm_read`: CHS, solange der Sektor in 1024 Zylinder
  der BIOS-Geometrie passt, sonst 42h/43h (wenn 41h die Erweiterungen
  meldet). Drei Versuche mit Reset (00h) dazwischen. `rm_read` dafür
  so umbauen, dass beide (preload und Ausflug) denselben Code nehmen.
- Schreiben (03h/43h) ist erlaubt – ohne Schreiben könnte man die
  CONFIG.SYS im Notstart nicht reparieren. `blk_flush` bei SHUTDOWN
  geht denselben Weg.

Was der Ausflug anders machen muss als `rm_int10`:

- **Interrupts**: Die Plattenroutinen des BIOS warten auf IRQ 14/15
  (INT 76h setzt 0040:008Eh) und messen Fristen über den Takt
  (0040:006Ch); Option-ROMs von SCSI-Karten warten teils auf ihren IRQ.
  Mit ganz maskiertem PIC liefen sie in die Frist. Daher während des
  Ausflugs: PIC auf die BIOS-Basen 08h/70h umprogrammieren, die Maske
  vom Start (in start.asm vor dem Umprogrammieren merken) freigeben,
  STI; zurück: CLI, Kernelbasen und alte Maske. Die IVT bei 0000:0000
  muss dafür die Vektoren des BIOS enthalten (heute unberührt – die
  Boxen haben eigene Kopien; beim Bau prüfen).
- **Zeit**: Takte, die während des Ausflugs auflaufen, zählt das BIOS in
  006Ch – den Unterschied in Kerneltakte umrechnen (·5,49) und
  nachtragen, sonst geht die Uhr nach.
- **Tastatur**: eine Taste während des Ausflugs landet im BIOS-Puffer
  und geht verloren (Notbetrieb, hinnehmbar).
- **Sperren**: der Ausflug läuft unter der Kernel- und einer eigenen
  Gerätesperre, nie aus einem Interrupt; währenddessen steht das ganze
  System (wie beim Moduswechsel über das Video-BIOS). Andere Treiber
  mit laufendem Busmaster-DMA stört das nicht.
- **Nicht auf dem BIOS-Gerät**: Auslagerungsdatei (`kf_swap_rw` geht
  direkt an den Treiber) und `LOOP=`-Abbilder – wie bei einem nicht
  sauberen Startlaufwerk bleiben beide zu, auch im normalen Start.
  `pd_prepare`/`pn_disk` (PANIC.DAT) ebenso nicht.
- Nur das Startlaufwerk (eine BIOS-Platte). Weitere BIOS-Platten (80h–
  83h) wären einfach, aber dort droht der Zugriff von zwei Seiten, wenn
  ein Treiber dieselbe Platte hat.

## DOS-Box im Notstart

- kernel.c `load_dos`/`load_cm`: `DOS_KERNEL=`/`DOS_IMAGE=` aus
  DOSBOX.INI nicht lesen (immer `BOX_DOS` = ATDOSCM.SYS), `ini_noframe`
  übergehen (EMS-Rahmen wie Vorgabe).
- BOXSHELL fragt den Notstart ab (neue Unterfunktion von INT E0h oder
  ein Bit in einer vorhandenen Antwort): dann kein `ini_apply`
  (DOSBOX.INI je Programm) und kein `ab_merge` (AUTOEXEC.BAT), interaktiv
  ohne `/K X:\AUTOEXEC.BAT`.
- ATDOSCM.SYS (`atdoscm/config.c`) liest `\DOS\MDOS\CONFIG.SYS` nicht.

## CMD im Notstart

- Der Kernel startet `X:\CMD.EXE /P /SAFE` (Name offen: `/SAFE` oder
  `/N`). CMD führt dann AUTOEXEC.CMD nicht aus, macht TEMP an, meldet den
  Notstart und nennt die Wege: `EDIT C:\CONFIG.SYS`, Neustart ohne F5.
- `autochk`/AUTOCHK.LST: im Notstart nicht ausführen, nur melden (eine
  Umwandlung oder Prüfung soll nicht ausgerechnet im Notstart laufen);
  nicht sauberes Startlaufwerk -> Hinweis „CHKDSK C: /F“.
- Fehlt CMD.EXE im Stamm: Panik mit klarer Meldung (wie heute bei
  fehlender Shell).

## Taste F5

- start.asm: in der Schleife bei `.f8` auch Scancode 3Fh -> neues Byte
  `boot_f5` (oder `boot_f8` Bit 1). `f8_wait` zeigt „F5: Notstart, F8:
  Startdateien schrittweise“ und nimmt beide.
- `preload` (Real Mode) kennt `boot_f5` schon und liest nur CONFIG.SYS
  und BASEDEV-Dateien.
- Start von Diskette: F5 wirkt genauso (Diskette 1 hat eine eigene
  CONFIG.SYS); den BIOS-Rückfall braucht es dort nicht.

## Platz

- Das erste Kernel-Codesegment ist fast voll (laut CLAUDE.md um 65.450
  von 65.520 Byte). `config_sys`, `f8_wait`, `cfg_ask` und die neue
  BASEDEV-/F5-Logik gehören vorher nach KTEXT4 (Thunks wie bei
  k2thunk.c) – passt zum Plan, Startcode abzugeben
  (`docs/INIT-SEGMENT.md`). Im ersten Segment bleibt nur der Aufruf.
- DGROUP: ein Byte `boot_f5`, `DRV_BIOS` ohne Tabelle (unit reicht),
  Geometrie gibt es schon (`bios_geo_*`).

## Prüfung (wenn gebaut)

- QEMU: F5 per `sendkey f5` direkt nach dem Start; CONFIG.SYS mit
  absichtlich kaputter Zeile, fehlender PROTSHELL, RUN= auf ein
  hängendes Programm – Notstart kommt bis CMD; DOS-Box mit kaputter
  DOSBOX.INI.
- Rückfall: Start von IDE ohne IDE.SYS (Zeile gelöscht) und mit einem
  Treiber, der kein Gerät findet; XCOPY, CHKDSK, SHUTDOWN, fsck sauber,
  Inhalte gleich; CHS-Grenze wie bei rm_read mit Geometrie 1/2.
- **86Box Pflicht** (BIOS-Aufrufe, Interrupts): 386DX/40 mit AHA-154x
  (Option-ROM, IRQ 11), P3B-F mit Award-BIOS (IDE und AHA-2940 mit
  Karten-BIOS an), SCAT erst nach Rückport.
- Uhr nach vielen Ausflügen (Uptime gegen Wanduhr).

## 5.x

Entschieden (Nutzer, 24.11.2026): F5 und `BASEDEV=` kommen auch nach
5.x, der INT-13h-Rückfall nicht. Grund: der 286 kommt aus dem
Protected Mode nur über den CPU-Reset zurück in den Real Mode (wie die
Kompatibilitätsbox) – je Sektor zu langsam. Fehlt auf 5.x das
Startlaufwerk nach den BASEDEV=-Treibern, bleibt es bei der Meldung
„Startpartition nicht gefunden“ wie heute.

## Offene Punkte

- Name des CMD-Schalters (`/SAFE` oder `/N`).
- Rückfall auch im normalen Start ohne Rückfrage, oder nur mit Meldung
  und Taste? (Vorschlag: ohne Rückfrage, mit Meldung.)
- Soll `NOTSTART=1` auch in der DOS-Box stehen?
