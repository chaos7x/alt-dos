# Eigener DOS-Kern für die DOS-Box (Konzept)

Stand: 21.11.2026, vom Nutzer freigegeben, noch kein Code. Anlass: Wunsch
des Nutzers (21.11.2026), aus der Idee „DOS-Kern wie `DOSKRNL` von OS/2“
(`docs/VDD.md`, Abschnitt „Vorgemerkt“) ein Konzept zu machen – mit der
Kompatibilität von OS/2 oder der heutigen von AT-DOS und der Technik von
NT, zuerst 16 Bit (6.x, Watcom), so geschnitten, dass es in die 32-Bit-
Welt (7.0) übernommen werden kann. Vorgabe: **der heutige Weg über die
Abbilder darf nicht verloren gehen.**

## Heute (6.3)

- `load_img` (kernel.c) startet eine Box aus einem Diskettenabbild:
  Bootsektor nach 0:7C00, INT 13h liest das Abbild (`BOX_READ`). Vorgabe
  `\DOS\MDOS\FREEDOS.IMG` (FreeDOS-Kern + Box-CONFIG.SYS, `SHELL=
  A:\BOXSHELL.COM`, `DEVICE=A:\ATBOX.SYS`), mit `DOS_IMAGE=` in
  DOSBOX.INI ein von MKDOSIMG gebautes MS-DOS/PC-DOS-Abbild (`load_mdos`,
  `box_same`).
- `BOXSHELL.COM` hängt die AT-DOS-Laufwerke als Netzlaufwerke in die CDS
  (Flags C000h), biegt INT 2Fh 11xxh auf INT E0h 10h um, meldet die SDA
  (INT E0h 11h), baut Umgebung/PATH/AUTOEXEC.BAT-Teil, liest DOSBOX.INI je
  Programm, legt SETVER-Tabelle und Geräte ATIN$/ATOUT$/ATERR$ an,
  fängt Laufwerks-IOCTL ab (INT E0h 21h) und parkt die Box nach jedem
  Programm (INT E0h 15h).
- `kernel/redir.c` (KTEXT4) erledigt die Redirector-Aufrufe mit FAPI-
  Funktionen im Task der Box: Pfade, Suche mit Zustand im DTA, SFT-
  Felder, SHARE (Tabellen je Box im Segment `RBX(t)`), MSCDEX, Datum/
  Attribute, Anpassungen für MS-DOS (`ms_dos[t]`).
- Darunter der Monitor (`v86.c`) und die Module (VCOM, VSB, VEMS, VEGA/
  VCGA, VMOUSE über `kernel/vdd.c`): Ports, Interrupts, BIOS, XMS/HMA/
  UMB, EMS, DPMI, VGA.

Kurz: das DOS in der Box ist ein echtes DOS, AT-DOS sieht es nur als
Netzwerk-Client. Das ist sehr kompatibel – die Fehler aus BIOSCMP, Word
5.5, Lotus Works und CheckIt lagen fast alle im Redirector und im BIOS-
Nachbau, kaum im DOS. Es kostet Speicher (konventionell ~627 KB frei),
jeder Dateizugriff läuft zweimal durch eine Dateiverwaltung (DOS, dann
AT-DOS), und die Vorgabe hängt am FreeDOS-Kern.

## Die drei Vorbilder

### OS/2 2.x (MVDM, `DOSKRNL`)

- In der Box läuft kein vollständiges DOS. `\OS2\MDOS\DOSKRNL` ist ein
  angepasster DOS-Kern; laut den öffentlichen Beschreibungen erledigt
  der Protected-Mode-Teil der VDM-Verwaltung einen großen Teil von
  INT 21h direkt mit den Diensten von OS/2 (Dateisysteme, HPFS, Netz),
  nur ein kleiner Teil bleibt in V86. Einzelheiten der Aufteilung sind
  nicht offen (Quellen nicht freigegeben) – nur als Muster verwenden.
- Kompatibilität: gut, aber nicht vollkommen. OS/2 lieferte Korrekturen
  für einzelne Programme (`FFIX.EXE`, `SQ4FIX.COM`, `FIXWP.EXE`) und
  „bestimmte DOS-Version“ (VMB: echtes DOS aus einem Diskettenabbild,
  `FSFILTER.SYS` für die OS/2-Laufwerke) für alles, was nicht lief.
- Stärken: viel freier Speicher, DOS-Einstellungen je Programm, eigene
  Versionsnummer (20.x/30.x), Programme mit wenig Bezug zu DOS-Innereien
  laufen schnell.

### AT-DOS heute

- Echtes DOS im Abbild + Redirector + BIOS-/Geräte-Nachbau (wie VMB mit
  FSFILTER, nur als Vorgabe).
- Kompatibilität: so gut wie das DOS im Abbild; Strukturen (SFT, CDS,
  LoL, SDA, MCB) sind echt, weil das DOS sie selbst führt.
- Schwächen: Speicher, doppelte Dateiverwaltung, Fremdkern (FreeDOS) mit
  eigenen Abweichungen (z. B. FCB-Suche, `freedos/patches/fcb-search.patch`),
  Netzlaufwerk-Sicht muss per IOCTL-Haken wieder „lokal“ gemacht werden.

### NT (NTVDM, NTDOS + DEM)

- In der Box läuft ein **echter, aber verkleinerter DOS-Kern** (NTDOS.SYS/
  NTIO.SYS, aus MS-DOS 5.0 abgeleitet). Er führt alle Strukturen selbst
  (PSP, MCB, SFT, CDS, Liste der Listen, SDA), lädt und startet Programme,
  behandelt Strg+C und INT 24h – aber **die eigentliche Dateiarbeit**
  (FAT, Puffer, Platten) ist herausgeschnitten. An ihrer Stelle steht
  eine Falle (BOP, ein ungültiger Befehl `C4 C4 nn`), die den Auftrag an
  das DEM (DOS Emulation Module in NTVDM) gibt; das DEM erledigt ihn mit
  der Win32-Dateischnittstelle.
- Dieselbe Box lief auf x86 in V86 und auf RISC unter einem Emulator –
  der DOS-Teil blieb gleich, nur der Wirt änderte sich.
- Kompatibilität: hoch, weil die Strukturen aus dem MS-DOS-Kern stammen;
  Grenzen bei Programmen, die Platten selbst lesen (INT 25h/26h auf
  NTFS) oder Hardware direkt wollen.
- ReactOS NTVDM macht es umgekehrt: DOS-Kern fast ganz als 32-Bit-C im
  Wirt (`dos32krnl`), in der Box nur Stummel mit BOPs – leichter zu
  schreiben, aber jede Struktur muss im Box-Speicher nachgepflegt werden.

## Vergleich

| | OS/2-Weg (INT 21h im Kernel) | AT-DOS heute (Abbild + Redirector) | NT-Weg (DOS-Kern in der Box + Falle) |
|---|---|---|---|
| DOS-Code in der Box | klein (Rest: MCB, PSP, EXEC) | ganzes DOS (FreeDOS/MS-DOS) | DOS-Kern ohne Dateisystem |
| Strukturen (SFT, CDS, LoL, SDA) | nachgebaut, müssen „echt aussehen“ | echt | echt (vom DOS-Kern geführt) |
| Dateiweg | Monitor -> FAPI | DOS -> INT 2Fh -> BOXSHELL -> INT E0h -> redir.c -> FAPI | DOS-Kern -> Falle -> DEM -> FAPI |
| Kompatibilität | mittel bis gut, Korrekturen je Programm | sehr gut (je Abbild) | sehr gut, wenn die Kernbasis MS-DOS ist |
| freier Speicher | am meisten | ~627 KB | fast wie OS/2 (Kern ~15–20 KB, in die HMA) |
| Aufwand | sehr groß (alles neu, Strukturen nachpflegen) | da | groß, aber zerlegbar; Grundlage (redir.c) da |
| Abhängigkeit von FreeDOS | keine | Vorgabe hängt daran | keine (FreeDOS bleibt als Abbild) |
| Weg nach 7.0 | Kernelteil neu in 32 Bit | Monitor + redir.c neu | nur das DEM neu übersetzen, DOS-Kern bleibt |
| Abbilder (VMB) | zusätzlich nötig | sind der Weg | bleiben als Option, gleicher Lader |

## Empfehlung

**NT-Technik mit der Kompatibilität von heute (MS-DOS 6.22 als Maßstab,
BIOSCMP/doscmp), dazu die Bequemlichkeiten von OS/2.**

Einordnung in die Zeitlinie (Gedanke des Nutzers, 21.11.2026): AT-DOS
steht in der Linie des Multitasking-DOS 4.0 (MT-DOS, 1986). Der eigene
Kern in der Box zeigt, wie es hätte weitergehen können, wenn man dieses
DOS einfach fortentwickelt hätte – statt auf OS/2 oder NT zu setzen,
übernimmt es deren Technik (Boxen wie MVDM, Dateiarbeit wie NTVDM) in
die eigene DOS-Linie.

1. **DOS-Kern in der Box (V86, Real-Mode-Code)** aus **zwei Quellen**
   (Entscheidung des Nutzers, 21.11.2026: „beides nehmen – MS-DOS 4 hat
   die ganzen, auch undokumentierten Strukturen, FreeDOS ist technisch
   weiter“): **Rumpf und DOS-5/6-Technik nach dem Vorbild des FreeDOS-
   Kernels** – nicht 1:1 übernommen, sondern als Vorlage für eine eigene,
   an AT-DOS angepasste Umsetzung (Nutzer: „Man muss ja nicht den
   FreeDOS-Kern 1:1 übernehmen, da steht aber genügend drin, um es
   perfekt an AT-DOS anzupassen“; C, Watcom wie der Rest), **Strukturen und
   Verhalten nach MS-DOS 4.0** (MIT, `github.com/microsoft/MS-DOS`
   `v4.0/src/DOS`, `INC`). Aufteilung im Abschnitt „Was aus welcher
   Quelle kommt“. Herausgeschnitten: FAT, DPB-Pflege, Puffer, Platten-
   zugriffe; statt dessen eine Falle zum DEM. Das passt zur Zeitlinie:
   Aufbau und Innereien stammen vom DOS 4.0 ab.
2. **DEM im Kernel** = Weiterentwicklung von `redir.c`: dieselbe Arbeit
   (Pfade, Suche, SFT-Felder, SHARE, MSCDEX), aber mit einer eigenen,
   neutralen Auftragsschnittstelle statt INT 2Fh 11xxh.
3. **Abbilder bleiben gleichwertige Option** (siehe unten), der Redirector-
   Zugang über INT 2Fh 11xxh bleibt als zweiter Eingang ins DEM.
4. Von OS/2: DOS-Einstellungen je Programm (DOSBOX.INI gibt es schon),
   eigene Version wählbar, Korrekturen je Programm als INI-Schalter statt
   Hilfsprogrammen.

Warum nicht der OS/2-Weg: alle Strukturen im Kernel nachzubauen und im
Speicher der Box stimmig zu halten ist der größte Aufwand mit dem größten
Risiko – genau dort lagen die Fehler, die heute der Redirector schon mit
echten Strukturen hatte. Der Mehrgewinn an Speicher gegenüber dem NT-Weg
ist klein (der DOS-Kern ohne Dateisystem passt in die HMA).

Warum nicht beim Heute bleiben: Speicher, doppelte Dateiverwaltung,
Fremdkern als Vorgabe. Heute bleibt aber vollständig erhalten.

## Aufbau

### Box-Seite: der DOS-Kern

- Datei `\DOS\MDOS\ATDOSK.SYS` (Name zum Abstimmen – keine Microsoft-/IBM-
  Produktnamen; `DOSKRNL` ist ein Dateiname von OS/2). Erste Stufen:
  geladen wie heute über ein Abbild (`ATDOS.IMG`, eigener Bootsektor wie
  `boot/mdosboot.asm`), damit der Lader unverändert bleibt; später direkt
  vom Kernel geladen (wie `load_com`, ohne Bootsektor und INT 13h).
- Bleibt in der Box: INT 21h-Verteiler, Speicher (48h/49h/4Ah/58h, UMB-
  Kette 5803h), PSP/EXEC/4Ch/31h (MZ-Relocation im Box-Speicher), FCB-
  Logik (29h, Umsetzung FCB <-> Handle wie MS-DOS intern), Strg+C,
  INT 24h-Aufruf (Fehler kommt vom DEM), Ländertabellen (COUNTRY=/CHCP
  der Box aus AT-DOS), Gerätetreiberkette (NUL, CON, AUX, PRN, CLOCK$,
  ATBOX.SYS mit EMMXXXX0/ATIN$/ATOUT$/ATERR$), CONFIG.SYS der Box
  (DEVICE=, FILES=, DOS=HIGH,UMB, SHELL=).
- Strukturen echt im Box-Speicher: LoL (52h), SFT-Kette, CDS je Laufwerk,
  SDA (5D06h/5D0Bh), DPB-Kette als Attrappe (32h/1Fh liefern Werte vom
  DEM: Sektorgröße, Cluster – wie `fat_geom` heute). SFT-Eintrag trägt
  das DEM-Handle (wie heute bei 0Bh), Größe/Position/Datum führt der Kern
  selbst und gleicht beim Lesen/Schreiben mit dem DEM ab – Programme, die
  die SFT lesen, sehen stimmige Werte.
- Laufwerke erscheinen **lokal** (CDS ohne Netz-Bit, wie bei NT) – der
  IOCTL-Haken von BOXSHELL (`a4409`, INT E0h 21h) entfällt dafür. CD-
  Laufwerke behalten die MSCDEX-Form (`\\X.\A.`, C080h).
- INT 2Fh-Multiplex im Kern (1000h SHARE da, 11xxh für fremde Redirector
  in der Box durchreichen, 12xxh interne Dienste, 15xxh MSCDEX, 1680h
  Leerlauf, 4300h/4310h XMS und 1687h DPMI bleiben im Monitor).
- BOXSHELL wird kleiner: Umgebung, AUTOEXEC.BAT-Teil, DOSBOX.INI je
  Programm, SETVER, Parken bleiben (oder wandern in den Kern-Init);
  Redirector-Haken und CDS-Abgleich entfallen beim eigenen Kern.
- Gemeldete Version (Entscheidung des Nutzers, 21.11.2026): **6.22** über
  INT 21h 30h und 3306h, wie die Box heute – höchste Verträglichkeit.
  SETVER und `DOS_VERSION=` je Programm bleiben.
- COMMAND.COM: siehe „Shells“.

### Shells

Entscheidung des Nutzers (21.11.2026): **mitgeliefert werden FreeCOM und
COMMAND.COM aus MS-DOS 4.0** (MIT, `v4.0/src/CMD/COMMAND`, aus dem
Quelltext gebaut). **COMMAND.COM aus MS-DOS 6.22** ist Kompatibilitätsziel
und wird getestet, aber nicht mitgeliefert (nicht frei) – wer sie nutzen
will, bringt seine eigene Kopie mit (wie bei MKDOSIMG, z. B. über
`SHELL=`/COMSPEC in der Box-CONFIG.SYS oder einen INI-Schlüssel).

| Shell | Versionsprüfung | Folge bei gemeldeter 6.22 |
|---|---|---|
| FreeCOM | keine feste | läuft |
| COMMAND.COM 4.0 (Original) | `CONPROC` in `INIT.ASM` ruft `SYSLOADMSG` -> `$M_VERSION_CHECK` (`INC/MSGSERV.ASM`): `CMP AX,EXPECTED_VERSION` aus `INC/VERSIONA.INC` = genau 4.00 | **scheitert**: „Falsche DOS-Version“, als oberste Shell (eigener Elternteil) Endlosschleife |
| COMMAND.COM 4.0 (eigener Bau) | entfernt | läuft |
| COMMAND.COM 6.22 | will genau 6.22 | läuft |

Entscheidung des Nutzers (21.11.2026: „es hart auf eine Version zu
dongeln ist eh antiquiert … es ist ja nur ein Interpreter“): im eigenen
Bau der 4.0-Shell **fällt die Prüfung der DOS-Version ganz weg**
(`$M_VERSION_CHECK` übersprungen, z. B. über `NOVERCHECKmsg` beim
`MSG_SERVICES`-Aufruf in `INIT.ASM`, als Änderung vermerkt). Sie läuft
dann unter jeder gemeldeten Version, SETVER braucht es dafür nicht.
Bleiben darf die innere Prüfung `$M_COMMAND_VER` (residenter gegen
nachgeladenen transienten Teil derselben Datei) – sie hat mit der
DOS-Version nichts zu tun.

Was COMMAND.COM 6.22 (und DOS-5/6-Programme) gegenüber 4.0 vom Kern
braucht – Ziel von Stufe 4, Liste bei Stufe 0 mit einer Spur der echten
6.22-Shell (welche INT 21h/2Fh sie ruft) bestätigen:

- **UMB**: 58h 02h/03h (UMB-Verkettung abfragen/setzen), 58h 00h/01h mit
  den Strategien 40h/80h, UMB-Kette über die MCB (Z/M an der Grenze
  9FFFh, Besitzer 0008h „SC“), Rückmeldung an XMS 10h/11h (Monitor).
- **LOADHIGH/LH** (intern in COMMAND.COM 5+): braucht genau 5801h/5803h
  und das Verhalten von 4Bh beim Laden in den oberen Speicher; DEVICEHIGH
  in der Box-CONFIG.SYS dasselbe im Kern-Init.
- **DOSKEY**: COMMAND.COM fragt INT 2Fh 4800h (installiert?) und liest
  Zeilen über 4810h, wenn DOSKEY da ist – der Kern muss 48xxh nur
  ungestört an die Kette weiterreichen (DOSKEY hängt sich selbst ein).
- **33h 06h** (wahre Version, BX = 1606h bei 6.22, DL Revision, DH Bit 4
  = DOS in der HMA) – SETVER ändert sie nicht. 33h 05h (Startlaufwerk)
  gibt es seit 4.0.
- **DOS=HIGH**: INT 2Fh 4A01h/4A02h (freie HMA abfragen/belegen) – die
  Shell 5+ legt Teile ihres residenten Teils dort ab.
- Weiteres aus DOS 5/6, das Programme nutzen: 4Bh 05h (Ausführungs-
  zustand, Task-Umschalter), 5D0Ah, 6Ch-Feinheiten (seit 4.0 da), 2Fh
  AE00h/AE01h (installierbare Befehle, seit 3.3 – APPEND/DOSKEY),
  2Fh 5500h (Schnittstelle der Shell, zu prüfen).

### Was aus welcher Quelle kommt

Grundsatz: **FreeDOS ist die Vorlage für die Technik, MS-DOS 4.0 die
Messlatte für alles, was Programme sehen.** Wo beide abweichen, gilt
MS-DOS (bzw. 6.22 laut BIOSCMP). Der Kern wird eigens für AT-DOS
geschrieben: was im FreeDOS-Kern für die eigene FAT, die eigenen Puffer
oder für 8086/Diskettenstart da ist, fällt weg; Aufbau, Abläufe und
Sonderfälle werden dort nachgelesen. Einzelne Stücke, die unverändert
passen (z. B. MZ-Relokation, MCB-Kette, NLS-Tabellen), dürfen
übernommen werden – dann mit Herkunftsvermerk.

Spalte „Art“: *Vorlage* = nachgelesen, eigene Umsetzung; *Stücke* =
Vorlage, passende Teile übernommen; *Definitionen* = Layout/Offsets.

| Teil | Quelle | Art |
|---|---|---|
| Rumpf: Start, Init, CONFIG.SYS der Box, Gerätekette, Dispatcher-Gerüst | FreeDOS (`main.c`, `config.c`, `inthndlr.c`, `entry.asm`) | Vorlage – eigener, schlanker Start (CONFIG.SYS kommt vom AT-DOS-Kernel) |
| Speicher: MCB, UMB-Kette 5803h, DOS=HIGH (HMA) | FreeDOS (`memmgr.c`, HMA-Lader) | Stücke |
| PSP, EXEC, 4Ch/31h, MZ-Lader | FreeDOS (`task.c`), Verhalten gegen MS-DOS 4.0 `EXEC.ASM`/`ALLOC.ASM` geprüft | Stücke + Abgleich |
| Handle-/Datei-Funktionen | FreeDOS `dosfns.c` (Schicht über der SFT); `fatfs.c`/`fatdir.c`/`blockio.c` entfallen, statt dessen die Falle zum DEM | Vorlage |
| Netz-/Redirector-Zweig (fremde Redirector in der Box) | FreeDOS `network.c` | Vorlage |
| NLS, COUNTRY, 38h/65h/66h | FreeDOS `nls.c` + COUNTRY.SYS (schon im Abbild) | Stücke |
| Strukturen: LoL (SYSVAR), SFT (SF.INC), CDS (CURDIR.INC), DPB, SDA (Lage aller Felder), MCB-/PSP-Felder | MS-DOS 4.0 `INC/*.INC` | Definitionen (Layout, Offsets) übernommen, als C-Strukturen geschrieben |
| Undokumentiertes: INT 2Fh 12xxh, 5D0Ah/5D0Bh, 1Fh/32h, SDA-Swap (5D06h), Fehlercodes/Klasse/Ort (59h), INT 24h-Ablauf, FCB-Sonderfälle | MS-DOS 4.0 `DOS/*.ASM` (u. a. `DISP.ASM`, `FCB.ASM`, `ERROR.ASM`, `MSCTRLC.ASM`) | Verhalten nachgebaut, kleine Stücke als NASM übernommen |
| Abweichungen von FreeDOS (z. B. FCB-Suche, 59h-Klasse, 5Ah-Länge, Fehler 05h bei Freigabekonflikt) | MS-DOS 4.0 / 6.22-Bezug | gleich richtig umgesetzt |
| Falle, Auftragsblock, Abgleich SFT <-> DEM | eigen | neu |
| COMMAND.COM | FreeCOM und MS-DOS 4.0 `CMD/COMMAND` (MIT) mitgeliefert; 6.22 vom Nutzer | siehe „Shells“ |

Warum FreeDOS als Vorlage für den Rumpf und nicht MS-DOS 4.0: FreeDOS ist C und
schon mit Watcom gebaut (tools/freedos.sh), kennt DOS 5/6 (HMA, UMB,
3306h, 6Ch, FAT32-Aufrufe) und hat den Redirector-Weg, der für unsere
Laufwerke ohnehin genutzt wird. MS-DOS 4.0 ist MASM-5-Assembler ohne
DOS-5-Teile – ganz übernommen hieße umsetzen nach NASM und nachrüsten.
Seine Stärke sind die genauen Strukturen und das Verhalten in den Ecken,
die Programme direkt lesen.

### Falle (BOP-Gegenstück)

- Gibt es schon: die Stummel `ARPL n` im F000-Segment (V86: #UD -> Monitor,
  ein Trap je Dienst, wie die BIOS-Haken von Windows 386) und INT E0h.
  Neu ein Dienst „DEM“ (z. B. INT E0h 30h oder ein eigener ARPL-Stummel),
  ein Aufruf je DOS-Dateifunktion.
- Auftrag als **Block im Box-Speicher** (`struct dem_req`, feste Feld-
  breiten, Wort/Doppelwort, nie `int`): Funktionsnummer, SFT-Eintrag als
  lineare Box-Adresse, Puffer als lineare Box-Adresse + Länge, Pfad(e),
  Ergebnis (DOS-Fehlercode, Klasse/Ort für 59h). Keine Segmentregister
  im Vertrag – das ist der Punkt für 7.0.
- Große READ/WRITE gehen direkt zwischen Datei und Box-Speicher (heute
  `xfer` über `lwin`), ohne Zwischenpuffer im DOS-Kern.

### Wirt-Seite: das DEM

- Aus `redir.c` herauslösen: `op()` arbeitet heute direkt auf SDA-
  Offsets und INT-2Fh-Nummern. Neuer Kern: Funktionen je Auftrag
  (`dem_open`, `dem_read`, `dem_find`, …) auf `struct dem_req`; zwei
  dünne Eingänge davor: **Redirector** (INT 2Fh 11xxh, für FreeDOS und
  MKDOSIMG-Abbilder, Verhalten wie heute) und **eigener Kern** (Falle).
- Was das DEM schon kann und beide nutzen: Pfadauflösung über FAPI im
  Task der Box (SUBST/JOIN/Sperren/Cache), Suche mit Zustand im DTA,
  SHARE (`RBX(t)`), MSCDEX, Datum/Attribut beim Schließen (`kf_hmeta`),
  NET USE-Laufwerke (sind AT-DOS-Laufwerke über `kernel/ufs.c`), lange
  Namen nur als 8.3 (Box ohne LFN-Merker).
- Neu für den eigenen Kern: INT 25h/26h (absolut lesen/schreiben) über
  DosOpenDrive mit Sperre wie CHKDSK – oder abgelehnt wie bei NT (zum
  Abstimmen); Laufwerksinfo 36h/1Ch/32h aus `fat_geom`; Datenträger-
  bezeichnung; IOCTL 4400h–440Fh.
- Ablage: zuerst in KTEXT4 neben `redir.c` (gleiche Stelle). Später
  prüfen, ob das DEM ein Modul wie die VDDs wird (`VDOS.SYS` über
  `DH_VDDREG`, Dienste `VS_FAPI`/`VS_RD`/`VS_WR`) – das hielte den Kernel
  schlank; Voraussetzung ist ein Weg, Kernelpuffer ohne Kopie zu nutzen.

### Abbilder bleiben (Vorgabe des Nutzers)

- Drei Arten Box, je Programm wählbar in DOSBOX.INI (Schlüssel zum
  Abstimmen, z. B. `DOS_KERNEL=ATDOS|FREEDOS|IMAGE` neben dem heutigen
  `DOS_IMAGE=`):
  1. **eigener Kern** (`ATDOSK.SYS`),
  2. **FreeDOS-Abbild** (`FREEDOS.IMG`, heutige Vorgabe),
  3. **MKDOSIMG-Abbild** (MS-DOS/PC DOS des Nutzers, `DOS_IMAGE=`).
- Alle drei teilen Monitor, Module (VDDs), BIOS-Nachbau, XMS/EMS/DPMI,
  BOXSHELL-Dienste und das DEM; 2 und 3 nur über den Redirector-Eingang.
  `load_img`, `load_mdos`, `box_same`, MKDOSIMG, `doscmp.sh` bleiben.
- Die Vorgabe wechselt erst, wenn der eigene Kern in BIOSCMP/doscmp mit
  MS-DOS 6.22 gleichzieht und die Spieleliste läuft – Entscheidung des
  Nutzers. FreeDOS bleibt danach im Lieferumfang (Abbild auf Diskette 2).
- 5.x (286, Kompatibilitätsbox mit Ausflügen in den Real Mode): bleibt
  bei den Abbildern – jede Falle wäre dort ein CPU-Reset-Ausflug.

### Die Module (VDDs) und das Netz

- VCOM, VSB, VEMS, VEGA/VCGA, VMOUSE arbeiten auf Ports/Interrupts unter
  dem DOS – sie merken vom Wechsel nichts. Zwei Berührungspunkte:
  VEMS braucht das Gerät `EMMXXXX0` (bleibt in ATBOX.SYS, die Gerätekette
  des Kerns muss es tragen), und `VDD_EV_PROG` (Programmstart, heute aus
  INT E0h 00h von BOXSHELL) kommt beim eigenen Kern direkt aus EXEC.
- SHARE, Sperren, NET USE: im DEM, für alle drei Arten gleich. „Fern“ oder
  „lokal“ (4409h) entscheidet das DEM je Laufwerk.
- Programme aus der Box heraus: EXEC im eigenen Kern kann ein FX-Programm
  (FAPI, nicht gebunden) erkennen und es als nativen Task starten lassen
  (wie NT Win32-Programme aus COMMAND.COM) – Option, eigene Stufe.

## Stufen

0. **Messen** (vor jedem Code): freier konventioneller Speicher und Zeit
   je Dateizugriff (COPY großer/vieler kleiner Dateien, DIR über 800
   Einträge, Word 5.5 laden) in drei Fällen: FreeDOS direkt, FreeDOS-Box,
   MS-DOS-6.22-Box. Ergebnis entscheidet, ob es weitergeht.
1. **DEM herauslösen**: `struct dem_req`, Funktionen je Auftrag, Redirector
   als Eingang. Keine Änderung im Verhalten – prüfbar mit den Abbildern
   (BIOSCMP, doscmp MS-DOS 5.0/6.22/PC DOS 2000, Word 5.5, Lotus Works).
2. **Kern minimal**: eigener Rumpf nach FreeDOS-Vorbild ohne Dateisystem
   (Dispatcher, Speicher, PSP/EXEC), Strukturen auf das Layout von MS-DOS 4.0 geprüft,
   Handle-Funktionen 3Ch–46h, 4Eh/4Fh, 47h, 39h–3Bh, 41h, 43h, 56h, 57h
   über das DEM; geladen über `ATDOS.IMG`; FreeCOM als Shell. Ziel: DIR,
   COPY, HALLO, MEM, Digger.
3. **Strukturen vollständig**: LoL, SFT, CDS, SDA, DPB-Attrappe, FCB-
   Funktionen, 59h, 5Dh/5Eh/5Fh, IOCTL, NLS (38h, 65h, 66h), INT 2Fh
   12xxh, SHARE-Codes. Maßstab: BIOSCMP gleich `docs/bioscmp/msdos622*.txt`.
4. **DOS 5/6-Teil**: DOS=HIGH (Kern in die HMA, 4A01h/4A02h), UMB-Kette
   (58h 02h/03h, LOADHIGH, DEVICEHIGH), 3306h, 2Fh 48xxh für DOSKEY, 6Ch,
   7303h nicht (FAT32 bleibt außen), INT 24h-Weg, Strg+C, CONFIG.SYS der
   Box. Spieleliste: DOOM 1.9, DOTT, Digger, Stunts, Elvira; Anwendungen:
   Word 5.5, Lotus Works, NC, MSD, CheckIt, 7za/FreeDOS-Werkzeuge;
   Shells FreeCOM, COMMAND.COM 4.0 (eigener Bau) und 6.22 (Kopie des
   Nutzers) mit LH und DOSKEY.
5. **Vorgabe wechseln** (Entscheidung des Nutzers), Kern direkt laden
   statt über ein Abbild, BOXSHELL verschlanken; COMMAND.COM aus MS-DOS
   4.0 (gebaut, ohne Versionsprüfung) neben FreeCOM mitliefern.
6. **7.0**: DEM als 32-Bit-C neu übersetzt (gleiche `struct dem_req`),
   DOS-Kern in der Box unverändert – wie bei NT x86/RISC.

Prüfstufen wie üblich: QEMU während der Arbeit, 86Box vor dem Commit
(Box = Hardware-Bezug: 386DX/40 4 MB, P3B-F), Proxmox bei Bedarf.

## Regeln für den Schnitt nach 7.0

- `struct dem_req` und die SFT-Erweiterung mit festen Breiten, als
  gemeinsamer Header (wie `fapi_abi.h`), keine Watcom-Eigenheiten im
  Vertrag.
- Box-Adressen im Vertrag linear (Segment · 16 + Offset), nicht als
  Segment:Offset – der Wirt bildet sie auf sein Fenster ab (heute `lwin`/
  `RWIN_SEL(t)`, in 7.0 flach).
- DEM greift nur über FAPI-artige Dienste auf Dateien zu (heute
  `kf_box_fapi`), nie auf FAT-Interna – dann bleibt es in 7.0 gleich.
- Der Box-Kern kennt nur Falle + Auftragsblock, kein Wissen über den
  Wirt (16 oder 32 Bit, V86 oder Emulator).

## Quellen und Lizenzen

- **MS-DOS 4.0** (MIT, 2024 freigegeben): Code und Definitionen dürfen
  übernommen werden; MIT verlangt nur, den Urheber- und Lizenzhinweis
  mitzuführen. Quelltext ist MASM 5 – Umsetzung nach NASM ist eigene
  Arbeit (Makros, `ASSUME`, Segmentgruppen). `v4.0-ozzie` (Multitasking-
  Beta) nur als Hintergrund – liegt als Disketten/Scans vor.
- **FreeDOS-Kernel** (GPL-2.0-or-later, C, mit Watcom übersetzbar): Vorlage
  für die eigene Umsetzung, einzelne Stücke dürfen übernommen werden
  (Entscheidung des Nutzers). Vor jeder Übernahme den Kopf der Datei
  prüfen (fast alle „version 2 or later“).

**Folgen der GPL für die FreeDOS-Teile:**

- Eigener Code, nach dem Lesen des FreeDOS-Kerns geschrieben, ist Code
  des Projekts (GPL-2.0-or-later wie alles). Übernommene oder eng
  nachgebaute Stücke machen den Box-Kern zum abgeleiteten Werk – das ist
  **dieselbe Lizenz**, ändert also nichts. Eine strenge Trennung
  („Clean Room“) ist deshalb nicht nötig; wichtig ist nur der
  Herkunftsvermerk je übernommenem Stück.
- MS-DOS-4.0-Teile im selben Kern sind mit der GPL verträglich (MIT ist
  freizügiger). Im fertigen Kern gilt für das Ganze die GPL; die MIT-
  Hinweise von Microsoft bleiben in den betroffenen Dateien stehen. Wer
  später nur MIT-Code haben wollte, müsste die FreeDOS-Teile ersetzen –
  das ist kein Ziel (Grundsatz des Projekts: Ableger bleiben offen).
- Pflichten: Urheberhinweise von FreeDOS (Pat Villani und Mitwirkende)
  und Microsoft in jeder Datei behalten, je Übernahme Herkunft vermerken
  (Datei, Projekt, Lizenz, Stand/Commit – wie `freedos/README.txt`),
  geänderte Dateien als geändert kennzeichnen (GPLv2 §2a), Quelltext
  mit jedem ausgelieferten Abbild/Release zugänglich (liegt im Repo).
- Grenze: GPLv3-Code (fdpp, GRUB) bleibt draußen – er würde das Ganze zu
  v3 machen; GPLv2-only-Code (Linux) nicht mit v3 mischen (gilt wie
  bisher, CLAUDE.md).
- Der übrige Kernel (DEM, Monitor) bleibt eigener Code; die Grenze ist
  die Falle mit `struct dem_req` – in 7.0 übernimmt man nur das DEM, der
  Box-Kern bleibt eine eigene, getrennt ausgelieferte Datei.
- **FreeCOM** (GPL-2.0-or-later) und **COMMAND.COM aus MS-DOS 4.0**
  (MIT): beide mitgeliefert. COMMAND.COM 6.22 ist nicht frei – nur als
  Kopie des Nutzers, nie im Repository oder in Abbildern.
- **ReactOS NTVDM** (GPL-2.0-or-later) und **DOSEMU2** (GPL): nur lesen.
  **fdpp** (FreeDOS-Kern als Wirtsprogramm in DOSEMU2) ist GPLv3 – nicht
  übernehmen (machte das Ganze zu v3).
- OS/2- und NT-Quellen: nicht freigegeben, Lecks nicht verwenden; nur
  öffentlich beschriebenes Verhalten.

## Offene Fragen an den Nutzer

1. ~~Grundlage des Box-Kerns~~ – entschieden: beide (FreeDOS-Rumpf,
   MS-DOS-4.0-Strukturen und -Verhalten); FreeDOS als Vorlage, nicht
   1:1. Passt die Aufteilung in der
   Tabelle „Was aus welcher Quelle kommt“?
2. Name der Datei (`ATDOSK.SYS`?) und des INI-Schlüssels.
3. ~~Gemeldete Version~~ – entschieden: 6.22; die 4.0-Shell im eigenen
   Bau ohne Versionsprüfung.
4. INT 25h/26h auf AT-DOS-Laufwerke: erlauben (mit Sperre wie CHKDSK) oder
   wie NT ablehnen?
5. Stufe 0 zuerst messen und danach entscheiden – einverstanden?
