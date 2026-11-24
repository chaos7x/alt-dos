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

Rolle (Nutzer, 21.11.2026: „Ist das nicht eher ein Compatibility
Mode?“): der Box-Kern ist der **Kompatibilitätsmodus** neben dem
Kernel, vergleichbar mit NTVDM bei NT – kein zweites Betriebssystem und
nicht der Hauptkernel. AT-DOS selbst ist `ATDOS.SYS` mit FAPI und CMD;
`ATDOSCM.SYS` gibt alten DOS-Programmen ihre gewohnte Umgebung und
reicht die eigentliche Arbeit an den Kernel weiter.

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

- Datei `ATDOSCM.SYS` (Entscheidung des Nutzers, 21.11.2026; CM =
  Compatibility Mode – der Name zeigt, dass es nicht der Hauptkernel
  `ATDOS.SYS` ist; keine Microsoft-/IBM-Produktnamen, `DOSKRNL` ist ein
  Dateiname von OS/2). Liegt als einfache Datei in `\DOS\MDOS` und
  wird vom Kernel direkt in die Box geladen, wie NTVDM `NTDOS.SYS` lädt
  (Weg wie `load_com`: Datei lesen, an feste Stelle legen, anspringen –
  ohne Bootsektor, INT 13h und Abbild). Entscheidung des Nutzers,
  21.11.2026: „Die Images sind ja eher gedacht, um FreeDOS, MS-DOS usw.
  zu laden.“ Ein technischer Grund für ein Abbild besteht nicht – es
  hätte nur den heutigen Lader wiederverwendet; die Einstellungen der Box
  (heute Box-CONFIG.SYS im Abbild) kommen aus CONFIG.SYS/DOSBOX.INI des
  Wirts, Shells und Werkzeuge liegen ohnehin in `\DOS\MDOS`.
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
- Neu für den eigenen Kern: INT 25h/26h (siehe unten), Laufwerksinfo
  36h/1Ch/32h aus `fat_geom`; Datenträgerbezeichnung; IOCTL 4400h–440Fh.
- **INT 25h/26h wie MS-DOS 7** (Entscheidung des Nutzers, 21.11.2026,
  „evolutionär“: der Weg, mit dem Microsoft das DOS in die Multitasking-
  Welt von Windows 95 fortgeführt hat – passt zu AT-DOS als fortgeführtem
  DOS; weder frei wie MS-DOS 6 noch abgelehnt wie NT):
  - Lesen (INT 25h, auch CX = FFFFh mit Paket) immer, über `blk_read`
    durch den Platten-Cache – stimmig auch bei verzögert Geschriebenem.
  - Schreiben (INT 26h) nur nach **Laufwerkssperre** 440Dh 4Ah (logisch
    sperren, Stufen 0–3 wie MS-DOS 7; 6Ah logisch freigeben, 4Bh/6Bh
    physisch sperren/freigeben, 6Ch/70h Abfragen; CH 08h oder 48h), sonst
    Fehler „schreibgeschützt“ (0300h) wie dort. Die Sperre entspricht der
    von DosOpenDrive (CHKDSK): abgelehnt (05h), solange Dateien auf dem
    Laufwerk offen sind; beim Entsperren oder Ende des Programms
    `vol_reset` (FAT-/Verzeichnis-Cache weg, neu einhängen).
  - Umgesetzt 22.11.2026: DEM `DEM_DLOCK` (FAPI_OPENDRIVE mit
    DRV_OPEN_LOCK|DRV_OPEN_LBA, Handle je Box und Laufwerk in `struct
    rbx`), `DEM_ABSWRITE`; mit Sperre lesen und schreiben INT 25h/26h und
    7305h über dieses Handle (Platten-Cache, `dasd_rw`). Im Box-Kern
    `dlock_fn` (geschachtelt gezählt, der Kernel sperrt beim ersten und
    gibt beim letzten frei), `dlock_end` in term() gibt Sperren eines
    endenden Programms frei, `abs7305`. 4Bh/6Bh nur bestätigt (ohne
    Wirkung), 6Ch meldet 0, 71h (erster Cluster) seit 22.11.2026 über
    `DEM_FCLUS`. Grenze: ein Laufwerk mit
    Auslagerungsdatei oder LOOP-Abbild (vom Kernel offen) lässt sich nicht
    sperren – bei der Vorgabe also C:. Geprüft: QEMU (FAT16 D:, FAT32 E::
    ohne Sperre 0300h, Sperre bei offener Datei 05h, Muster schreiben/
    zurücklesen, INT 25h auf FAT32 0207h, 7305h lesen/schreiben, Sperre
    beim Programmende frei; fsck sauber) und 86Box 386DX/40 mit
    AHA-154xC (zweite SCSI-Platte; danach COPY dorthin, fsck sauber).
  - LOCK/UNLOCK wie MS-DOS 7 (22.11.2026, Hinweis des Nutzers: nötig z. B.
    für die Installation von NT über eine Startdiskette von Windows 98):
    `apps/lock.asm` -> `\DOS\MDOS\LOCK.COM`/`UNLOCK.COM` (Platte und
    Setup-Diskette 3, `HDMDOS`). `LOCK X:` fragt J/N und ruft 440Dh 084Ah
    mit DX = 4C4Bh – die Sperre gehört dann dem Elternprozess (der Shell)
    und bleibt über Programmenden hinweg, `UNLOCK [X:]` gibt sie frei
    (ohne Laufwerk alle; nicht gesperrt -> 05h). Damit Programme auf einem
    gesperrten Laufwerk weiter Dateien öffnen können, sperrt das DEM mit
    `DRV_OPEN_OWNER` (4): offene Dateien des eigenen Tasks stören nicht,
    `vol[].locked` = Task + 2, `vol_ok` lässt dann den Besitzer durch,
    nach jedem Schreiben über INT 26h/7305h `vol_reset` (Caches weg). In
    QEMU und 86Box 386DX/40 geprüft (Sperre bei eigener offener Datei,
    DIR/COPY danach, fsck sauber). Nicht geprüft: WINNT.EXE selbst
    (INT 13h auf Festplatten gibt es in der Box nicht).

### DEFRAG von MS-DOS 6.22 (Werkzeuge mit Plattenzugriff), 22.11.2026

Frage des Nutzers: laufen DEFRAG & Co. mit `LOCK` unter AT-DOS? Mit
DEFRAG.EXE von den 6.22-Disketten des Nutzers (nur zum Testen) in QEMU
Schritt für Schritt nachgezogen, Spur der INT-21h/2Fh/25h-Aufrufe über
Port E9h:

- „arbeitet nicht in Multitasking-Umgebungen“: DEFRAG fragt DESQview mit
  21h 2B01h (Datum setzen, CX:DX = "DESQ") – der Box-Kern meldete jedes
  Datum als gültig. Jetzt 2Bh/2Dh mit Prüfung wie MS-DOS (AL = FFh).
- DPB (32h/1Fh) war nur grob vorgetäuscht (1 reservierter Sektor, FAT
  256 Sektoren …) – DEFRAG liest FAT und Verzeichnisse per INT 25h an den
  Stellen aus dem DPB. Jetzt für FAT12/16 aus dem echten Bootsektor
  (`read_bs`), ebenso der BPB von 440Dh 0860h.
- 440Dh 0867h/0847h (Zugriffsmerker) fehlten.
- 21h 55h (Kind-PSP anlegen, `$Dup_PDB`) fehlte ganz – DEFRAG lädt sein
  Hauptprogramm so; ohne stand Müll als Speicherende im PSP („Nicht
  genügend Speicher“).
- Das Blockgerät im DPB (DPB+13h) hatte nur `RETF` als Strategie/
  Interrupt; jetzt ein Treiber (entry.asm `blk_strat`/`blk_intr`: Lesen/
  Schreiben über INT 25h/26h der Box, Schreiben also nur mit Sperre,
  Medium unverändert), für Werkzeuge, die den Treiber direkt rufen.
- FASTOPEN-Tabelle wie MS-DOS 6.22 bei DOS-DS:0D30h (aus MSDOS.SYS
  ermittelt; SFT und Bootsektor-Puffer liegen davor im Kopf DOSHDR, damit
  kaum Speicher verloren geht).

Stand: DEFRAG liest FAT und Verzeichnisse und zeigt die Belegungskarte,
bricht dann aber mit „MS-DOS-FASTOPEN-Programm entdeckt“ ab – woran es
das erkennt, ist offen (keine DOS-Aufrufe dazwischen, also ein direkter
Blick in DOS-Strukturen; DEFRAG.EXE ist gepackt). Eine 60-MB-Platte
(über 32 MB) scheiterte vor dem 55h-Fund schon beim FAT-Lesen – erneut zu
prüfen. Achtung für später (EA-Format): DEFRAG verschiebt bei voller
Optimierung Verzeichnisse – die Sätze in `EA DATA. SF` verweisen auf den
Startcluster des Verzeichnisses und würden verwaisen; AT-DOS-DEFRAG
(UFAT) lässt Verzeichnisse stehen. Geprüft (QEMU, 86Box 386DX/40):
BIOSCMP und DOOM-Timedemo unverändert, Sperr-/INT-25h-26h-Test und
LFN-Test wie vorher, DPB gleich dem Bootsektor.

### Lange Dateinamen (INT 21h 71xxh), 22.11.2026

Wunsch des Nutzers (zusammen mit 440Dh 0871h). Der Box-Kern bleibt
innen bei kurzen Pfaden (CDS, SFT, alle alten Aufrufe); `atdoscm/lfn.c`
löst lange Pfade über das DEM auf:

- `DEM_LPATH` (21): Pfad mit langen oder kurzen Teilen -> kurze Form
  (`path`) und lange Form (`path2`); fehlt nur der letzte Teil: 02h und
  dieser Teil unverändert angehängt. `DEM_LFIND` (22): Suche mit langem
  Muster (`*`/`?` ohne Groß/klein, `*.*` = alles, trifft langen oder
  kurzen Namen), Zustand = Eintragsnummer. `DEM_FCLUS` (23): erster
  Cluster (Kernel `kf_hfirst` über ein geöffnetes Handle).
  `DEM_R_LONG` in `rsv`: DEM_OPEN vergleicht den letzten Teil nicht als
  8.3 (sonst träfe „sehrlangername.txt“ eine Datei SEHRLANG.TXT).
  `path`/`path2` jetzt 261 Byte; redir.c gibt sie nur bei LPATH/LFIND
  zurück. Der Kernel gibt der Box sonst nur 8.3 (`task.lfn` 0) – das DEM
  schaltet den Merker je FINDENTRY um und holt so beide Namen.
- Box-Kern: 710Dh, 7139h/713Ah/713Bh, 7141h (SI = 1 mit Platzhaltern),
  7143h (BL 0–8; Zugriff/Anlegen = Änderung, Setzen ohne Wirkung außer
  BL 1/3), 7147h, 714Eh/714Fh/71A1h (8 Suchen gleichzeitig, frei beim
  Programmende), 7156h, 7160h (CL 0/1/2), 716Ch/71A9h, 71A0h („FAT“,
  BX 4006h, 255/260), 71A6h, 71A7h (FILETIME ohne Zeitzone, 64-Bit-
  Rechnung in 16-Bit-Stücken), 71A8h; Rest AX = 7100h wie DOS ohne LFN.
  Neue Namen legt der Kernel an wie für FAPI-Programme: VFAT oder EA je
  nach Laufwerk (LONGNAMES=, bei AUTO EA, wenn `EA DATA. SF` im Stamm
  liegt – auch auf FAT32 nach `CONVERT X: /LFN:EA`).
- Alte Aufrufe (3Dh, 4Eh, 4Bh …) mit langen Pfadteilen lösen wie bei
  Windows 95 ebenfalls über `DEM_LPATH` auf (`is_long` in file.c) –
  FreeCOM gibt DIR den per 7160h langen Pfad.
- Geprüft: QEMU (FAT16 C: mit EA-Namen, FAT32 D: mit VFAT und nach
  `CONVERT D: /LFN:EA` mit EA – CHKDSK sauber; Testprogramm
  LT.COM im Scratchpad: Anlegen, Öffnen, Suche, 7160h kurz/lang, CD,
  7147h, Umbenennen, Attribute, erster Cluster, FILETIME gegen Python,
  Löschen mit Platzhaltern, RD), FreeCOM `DIR /LFN`, COPY/REN/TYPE/DEL
  mit langen Namen; mtools sieht die VFAT-Namen, fsck sauber; BIOSCMP
  und DOOM-Timedemo unverändert; 86Box 386DX/40 mit FAT32-Platte am
  AHA-154xC. Grenzen: Befehlszeile der Box bis 127 Zeichen samt Pfad
  von COMMAND.COM (FreeCOM meldet sonst „interner Fehler“); Namen in
  der Codepage, kein Unicode; FreeDOS-/MKDOSIMG-Boxen ohne LFN.
  - FAT32: 7305h (erweitertes absolutes Lesen/Schreiben, wie Windows 95
    OSR2); INT 25h/26h auf FAT32 abgelehnt wie dort.
  - Nur lokale FAT-Laufwerke; NET USE, CD, Loop/DriveSpace-Container ->
    Fehler wie bei Netzlaufwerken unter DOS.
  - Abbilder (FreeDOS, MKDOSIMG) sehen unsere Laufwerke weiter als
    Netzlaufwerke – dort scheitert INT 25h wie heute.
- Ablage: zuerst in KTEXT4 neben `redir.c` (gleiche Stelle). Später
  prüfen, ob das DEM ein Modul wie die VDDs wird (`VDOS.SYS` über
  `DH_VDDREG`, Dienste `VS_FAPI`/`VS_RD`/`VS_WR`) – das hielte den Kernel
  schlank; Voraussetzung ist ein Weg, Kernelpuffer ohne Kopie zu nutzen.

### Abbilder bleiben (Vorgabe des Nutzers)

- Drei Arten Box, je Programm wählbar in DOSBOX.INI:
  1. **eigener Kern** (`ATDOSCM.SYS`),
  2. **FreeDOS-Abbild** (`FREEDOS.IMG`, heutige Vorgabe),
  3. **MKDOSIMG-Abbild** (MS-DOS/PC DOS des Nutzers, z. B. `MSDOS622.IMG`).
- Schlüssel (Entscheidung des Nutzers, 21.11.2026): **ein Schlüssel,
  Wert = Datei in `\DOS\MDOS`** – `DOS_KERNEL=ATDOSCM.SYS`,
  `DOS_KERNEL=FREEDOS.IMG`, `DOS_KERNEL=MSDOS622.IMG`. Die Endung
  entscheidet, wie geladen wird: `.SYS` direkt (`load_cm`), `.IMG` als
  Abbild über Bootsektor/INT 13h (`load_img`). Vorbild OS/2
  `DOS_STARTUP_DRIVE` (ein Schlüssel für eingebauten Kern oder Abbild),
  aber ausdrücklich statt „leer = eingebaut“.
  - `DOS_IMAGE=` bleibt als gleichwertiger alter Name gültig (vorhandene
    INI-Dateien, Hinweis von MKDOSIMG); stehen beide im selben
    Abschnitt, gilt der zuletzt gelesene – wie bei jedem Schlüssel.
  - Gelesen wie heute: erst `[*]`, dann `[NAME.EXT]`, `[COMMAND.COM]`
    für die interaktive Box. Die Vorgabe des Systems ist `[*]` – kein
    Eintrag in der CONFIG.SYS.
  - Ohne Angabe: `ATDOSCM.SYS` (seit Stufe 5, vorher `FREEDOS.IMG`;
    eine Zeile im Kernel, `BOX_DOS`). Fehlt die gewählte Datei, Rückfall auf
    `FREEDOS.IMG` wie heute (`load_mdos`).
  - Geparkte Box nur bei gleichem Wert wiederverwenden (`box_same`
    vergleicht den Dateinamen wie heute).
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
  „lokal“ (4409h) entscheidet das DEM je Laufwerk. Netzabfragen (2Fh
  1100h/B800h, 5E00h, 5F02h) und NetBIOS: Abschnitt „Netz in der Box“.
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
   **Erledigt (21.11.2026):** `include/dem.h` (Vertrag: `struct dem_req`
   mit festen Breiten, lineare Box-Adressen, DOS-Fehlercode; DEM_OPEN mit
   Aktionswunsch wie 6Ch, CLOSE, READ/WRITE, SIZE, LOCK, MKDIR/RMDIR/
   CHDIR, GET/SETATTR, RENAME/DELETE auch mit Platzhaltern, FINDFIRST/
   FINDNEXT mit 21 Byte Suchzustand, SPACE, DINFO), `kernel/dem.c`
   (KTEXT4, `dem_call`; SHARE-Tabellen, Suchplätze, Sperren der Box,
   `dem_xfer` direkt in den Box-Speicher, Kontext `dem_ctx` als vierter
   Block in kernel.c ctx_tab), `kernel/redir.c` nur noch Eingang (SDA/SFT
   <-> dem_req, Aktionscode für MS-DOS, CDS-Prüfung bei RMDIR) plus MSCDEX
   und die umgeleiteten Standard-Handles; `kernel/dembox.h` Zugriff auf den
   Box-Speicher für beide. KTEXT4 +1,3 KB, DGROUP kleiner. Geprüft in QEMU
   alt gegen neu: BIOSCMP (FreeDOS-Box) und doscmp MS-DOS 6.22, PC DOS
   7.0, MS-DOS 5.0 zeilengleich (Suche, SHARE, Sperren, 6Ch, FCB, IOCTL),
   Word 5.5 installiert über alle 10 Disketten (Thesaurus-Diskette mit der
   verschachtelten Suche) und meldet „erfolgreich installiert“. Reine
   Programmlogik – kein 86Box.
2. **Kern minimal**: eigener Rumpf nach FreeDOS-Vorbild ohne Dateisystem
   (Dispatcher, Speicher, PSP/EXEC), Strukturen auf das Layout von MS-DOS 4.0 geprüft,
   Handle-Funktionen 3Ch–46h, 4Eh/4Fh, 47h, 39h–3Bh, 41h, 43h, 56h, 57h
   über das DEM; direkt vom Kernel geladen (`load_cm`, neben load_img); FreeCOM als Shell. Ziel: DIR,
   COPY, HALLO, MEM, Digger – und **DOOM 1.9** (erste Hälfte der Abnahme,
   s. „Abnahme“).
   **Erledigt (21.11.2026):** `atdoscm/` -> `\DOS\MDOS\ATDOSCM.SYS`
   (16 KB Datei, 25 KB im Speicher mit Stapeln und 40 SFT-Einträgen; Real
   Mode, Modell tiny, eigene Umsetzung): `entry.asm` (Start an 0060:0000,
   INT 20h–2Fh, Rahmen des Aufrufers als `struct frame`, zwei Kernstapel
   nach InDOS, Strg+C über `ctrlc_stub` = INT 23h und Wiederholung wie
   DOS), `kern.c` (Verteiler, MCB-Kette mit Strategien 0/1/2, LoL/SDA nach
   MS-DOS 4.0, Datum/Zeit über INT 1Ah, 30h/33h/34h/38h/59h/5Dh/63h/65h/
   66h), `file.c` (SFT/JFT/CDS, Geräte NUL/CON/AUX/PRN/CLOCK$/EMMXXXX0/
   ATIN$/ATOUT$/ATERR$/LPT1/COM1, kanonische Pfade 8.3, 36h/39h–47h/4Eh/
   4Fh/56h/57h/5Ah–5Ch/60h/67h/6Ch/29h, IOCTL 00/01/06/07/08/09/0A/0E),
   `con.c` (CON über INT 16h/10h, Zeileneingabe mit F1/F3/Esc, 01h–0Ch),
   `exec.c` (4B00/01/03 für .COM und .EXE mit Verschiebungen, PSP wie
   PDB.INC, Umgebung mit Programmpfad, Ende 00h/4Ch/31h, INT 22h–24h
   zurück, SETVER-Tabelle LoL+37h und 122Fh). Dateiarbeit nur über das
   DEM: INT E0h 30h (ES:DI = `struct dem_req`, redir.c `redir_cm` kopiert
   herein und zurück), INT E0h 31h meldet bzw. erfragt den eigenen Kern
   (AX = 4D43h „CM“). Kernel: `load_cm` (kernel.c) lädt die Datei ohne
   Abbild nach 0060:0000 (CS = DS = ES = SS = 60h), gewählt über
   DOSBOX.INI `DOS_KERNEL=ATDOSCM.SYS` (wie DOS_IMAGE, auch je Programm);
   Vorgabe bleibt FREEDOS.IMG. `v86_fgbox` zählt eine Box mit eigenem
   Kern als „Box mit DOS“ (Strg+C geht an die Box). BOXSHELL erkennt den
   Kern (E0h 31h) und lässt Redirector, CDS-Abgleich und „anderes DOS“
   weg. Noch nicht: FCB-Dateiarbeit (AL = FFh), UMB/HMA (MEM: 601 KB frei
   statt 627 mit FreeDOS), INT 25h/26h, INT 24h, CONFIG.SYS der Box,
   DOSKEY – Stufen 3/4.
   Geprüft: QEMU (16 MB) – FreeCOM interaktiv, DIR, COPY mit FC /B,
   TYPE | FIND, HALLO (Befehlszeile, ERRORLEVEL 5), FreeDOS-MEM (dabei
   behoben: 5803h BX = 1 muss ohne UMBs mit Fehler 1 scheitern, sonst
   „UMB Kette beschädigt“), COPY CON mit Strg+C, EXIT, Digger spielen,
   **DOOM 1.9 `-timedemo demo1` mit SB16: 2926 Takte** (FreeDOS-Box
   gleiche Maschine 2853 – in QEMU gleichauf). 86Box 386DX/40 4 MB
   (AHA-154x): HALLO, FreeCOM, MEM, Digger. Lehre: die Umgebung des
   ersten Prozesses bekam Besitzer 0 (= frei), der Programmblock legte
   sich darüber und BOXSHELL gab mit seiner Umgebung sich selbst frei.
3. **Strukturen vollständig**: LoL, SFT, CDS, SDA, DPB-Attrappe, FCB-
   Funktionen, 59h, 5Dh/5Eh/5Fh, IOCTL, NLS (38h, 65h, 66h), INT 2Fh
   12xxh, SHARE-Codes. Maßstab: BIOSCMP gleich `docs/bioscmp/msdos622*.txt`.
   **Erster Teil erledigt (21.11.2026):** `tools/bioscmp.sh … cm` lässt
   BIOSCMP in der Box mit ATDOSCM laufen. Der DOS-Teil ist jetzt gleich
   der FreeDOS-Box bzw. MS-DOS 6.22 (`msdos622-share.txt`); übrig sind
   nur Unterschiede der Umgebung (INT 13h A: ohne Startabbild, Version
   6.22, INT 2Fh 1100h/1400h wie MS-DOS ohne Redirector). Dazu:
   FCB-Dateiarbeit (`atdoscm/fcb.c`: 0Fh–17h, 21h–24h, 27h/28h, Suche mit
   dem DEM-Zustand ab FCB+0Ch, 32-Bit-Mul/Div per Inline-Assembler),
   Pfade mit ungültigem Laufwerk -> 03h, Leerzeichen im Namen fallen weg
   („A   .TXT“ = A.TXT), Phantom-B:, 3Ah auf das aktuelle Verzeichnis ->
   10h, 41h mit Platzhaltern -> 03h, 440Ah mit SFT-Flags, 4409h 08C2h,
   440Dh 0860h/0866h und 6900h aus DEM_DINFO (wie BOXSHELL), DPB-Liste ab
   LoL+0 und 1Fh/32h (MS-DOS-4.0-Layout, Werte bei 32h aus DEM_DINFO;
   ohne sie hing FreeDOS-`MEM /D` in einer Endlosschleife über 0:0),
   Blockgerät „ATDOS“ in der Gerätekette. Aus Stufe 4 vorgezogen (Frage
   des Nutzers zu „Free XMS UMBs“ in MSD): **UMBs** – beim Start holt
   ATDOSCM alle UMBs per XMS 10h und hängt sie wie MS-DOS mit DOS=UMB an
   (Systemblock „SC“ bei 9FFFh, Lücken wie der EMS-Rahmen als SC-Blöcke,
   LoL+68h), 5802h/5803h verbinden/trennen, Strategien 40h/80h; BOXSHELL
   zieht sich hoch. QEMU ohne EMS-Rahmen: 68 KB oben, 606 KB unten frei;
   86Box 386DX/40: UMBs bei CD00h und E000h um den Rahmen D000h, 604 KB
   frei (vorher 598/601). Geprüft: BIOSCMP (QEMU), FreeDOS-MEM und
   `MEM /D`, DOOM 1.9 Timedemo (2944 Takte), Digger in QEMU und 86Box.
   **Rest von Stufe 3 erledigt (21.11.2026, nach dem Wechsel der Vorgabe):**
   21h 5Eh/5Fh wie MS-DOS ohne Redirector (5E00h/01h Rechnername, 5Eh
   02h–05h und 5Fh 00h–06h Fehler 01h, 5F07h/08h CDS in Gebrauch, Vorlage
   MACRO.ASM); INT 2Fh 12xxh (`atdoscm/dosint.c`, eigener Eingang `i2f12`
   über denselben Weg wie INT 21h): 00 03 04 05 08 09 0D 0E 0F 11 12 13
   14 16 17 18 1A 1E 20 21 25 26 27 28 29 2B 2C 2D; 1212h/1225h zählen die
   Null mit (MISC2.ASM StrLen). Der Registerrahmen liegt jetzt wie
   SAVE_WORLD (AX zuunterst) – 1218h gibt ihn so heraus. BIOSCMP hat dafür
   einen Abschnitt (nur Dienste, die auch von außen gültig sind – 1216h,
   1220h, 1226h ff. setzen SS = DOS voraus, unter MS-DOS brach der Test
   dort ab); unter MS-DOS 6.22 direkt (`tools/doscmp.sh`, Bezugsteil) und
   in der Box gleich, FreeDOS weicht bei 121Eh ab (Groß/klein).
   Dabei mit MS-DOS-MEM gefunden und behoben (Maßstab: dasselbe MEM unter
   MS-DOS 6.22 als Abbild in derselben Box): (1) Kern lag bei 0060h, MEM
   rechnet „IO“ ab 0070h – jetzt lädt `load_cm` nach 0070h wie IO.SYS;
   (2) LoL+63h ist der Verbindungsmerker, +66h das erste UMB-Segment
   (9FFFh), +68h die letzte Suchstelle (wie FreeDOS kernel.asm) – vorher
   verschoben; (3) bei getrennten UMBs ist wie bei MS-DOS der Block *vor*
   9FFFh das „Z“, der SC-Block bleibt „M“; (4) `mcb_set` las nach „SC“
   weiter im Speicher (Name „SC.:\DOS“); (5) entscheidend: MEM holt das
   DOS-Datensegment über INT 2Fh 1203h und liest dort an festen Stellen –
   die Liste der Listen liegt jetzt wie bei MS-DOS bei DS:0026h (Segment
   DOSHDR vorn in DGROUP). MS-DOS-MEM zeigt danach „MSDOS 14K“ und die UMBs
   als „hohen Speicherbereich“ (vorher 1,2 MB bzw. 202 KB „MSDOS“).
   Geprüft QEMU (BIOSCMP, DOOM 1.9 Timedemo, FreeDOS-MEM, MS-DOS-MEM/FIND
   mit Pipe und Umleitung) und 86Box 386DX/40 (MS-DOS-MEM: 626 KB frei,
   Digger). Bemerkt, nicht vom Kern: MS-DOS-MEM lässt die EMS-Zeilen weg,
   wenn die Ausgabe in eine Datei geht (INT 67h liefert dabei dasselbe).
   **Stufe 4, erster Teil (21.11.2026): INT 24h/25h/26h.** INT 25h wie
   MS-DOS 7: Lesen frei über das DEM (`DEM_ABSREAD`, fat.c `fat_abssec`
   liest Sektor für Sektor relativ zum Bootsektor des Laufwerks nach
   DMABUF), alte Form CX/DX nur bis 32 MB (sonst 0207h), Paketform
   CX = FFFFh; INT 26h meldet Schreibschutz (0300h) – Schreiben bräuchte
   wie unter MS-DOS 7 eine Laufwerkssperre. Beide kehren mit RETF zurück,
   die Flags bleiben auf dem Stapel (`abs_frame` kopiert den IRET-Rahmen
   ein Wort nach unten). INT 24h: meldet das DEM „nicht bereit“,
   Schreibschutz oder allgemeinen Fehler, ruft der Kern wie MS-DOS den
   Handler des Programms – auf dessen Stapel über dem SAVE_WORLD-Rahmen
   (`crit24`, so wie Borlands `hardretn` es erwartet), ErrorMode in der
   SDA gesetzt, AH mit Bereich/Schreiben/erlaubten Antworten, DI
   Fehlercode, BP:SI Gerätekopf. Wiederholen, Ignorieren, Abbrechen
   (Programmende Typ 2) und Fehler (53h) wie MS-DOS; steht INT 24h noch
   auf dem eigenen Eingang, bleibt es beim Fehlercode wie bisher. Im DEM
   dabei: FINDFIRST auf einem leeren Laufwerk meldete „keine Dateien“
   statt „nicht bereit“ (gilt auch für die FreeDOS-Box). Geprüft QEMU:
   FreeCOM `dir a:` ohne Diskette fragt „(A)bbrechen, (I)gnorieren,
   (W)iederholen, (F)ehler?“, Wiederholen/Fehler; ABS.COM liest den
   Bootsektor von C: (Paketform), 0207h/0300h/8002h; ebenso in 86Box
   386DX/40 (AHA-154x).
   **Stufe 4, zweiter Teil (21.11.2026): CONFIG.SYS der Box, DEVICEHIGH,
   LOADHIGH.** Datei `\DOS\MDOS\CONFIG.SYS` (eigene Datei der Box neben
   DOSBOX.INI, ausgeliefert nur mit Kommentaren; was beim Start der ganzen
   Box gilt, bleibt in der CONFIG.SYS von AT-DOS), gelesen vor BOXSHELL
   (`atdoscm/config.c`, Lesepuffer vorübergehend oben im Speicher):
   DEVICE=, DEVICEHIGH=, BUFFERS=, BREAK=; andere Schlüssel still, Zeilen
   ohne `=` -> „Fehler in CONFIG.SYS der Box, Zeile n“. Treiber wie
   SYSINIT von MS-DOS 4.0: Datei als Overlay (4B03h-Weg) an den Anfang
   des größten Blocks, INIT mit höchster Endadresse und Zeiger auf die
   Befehlszeile (InDOS gezählt, damit INT 21h im INIT den zweiten Stapel
   nimmt), Block auf die gemeldete Endadresse kürzen, eigener Besitzer
   und Name (MEM zeigt „ANSI 4.208“ wie MS-DOS), Köpfe hinter NUL in die
   Kette (LoL+22h). DEVICEHIGH nimmt einen UMB, wenn die Datei hineinpasst,
   sonst unten. Ein Treiber mit STDIN/STDOUT ersetzt CON: Ausgaben auf CON
   gehen als WRITE-Anforderung an ihn (ANSI.SYS-Farben gehen). Blocktreiber
   werden abgelehnt (Laufwerke kommen vom DEM). Dabei behoben: INT 21h 30h
   ohne aktuellen PSP las die Version aus dem Interruptvektor 10h (MS-DOS-
   ANSI.SYS lehnte im INIT ab); INIT-Auftrag 20h Byte (DOS 5 schreibt das
   Wort bei 17h). LOADHIGH: EXEC nimmt bei Strategie „oben zuerst“ wie
   MS-DOS den größten oberen Block, wenn das Programm hineinpasst
   (`mem_alloc_prog`) – vorher landete `LH DOSKEY` immer unten.
   Geprüft QEMU (ANSI.SYS von MS-DOS 6.22 mit DEVICEHIGH: Farbe, MEM) und
   86Box 386DX/40 (ANSI oben, FreeCOM `LH DOSKEY` oben, MS-DOS-MEM) und
   DX4-100 mit 16 MB (DOOM 1.9 als erstes Programm, danach MEM: ANSI oben,
   626 KB frei). Die damals noch negative XMS-Zeile „Verwendet“ von
   MS-DOS-MEM ist inzwischen behoben (`docs/BOX-BIOS.md`).
   Dabei gefunden (seit dem ersten Teil): DOOM blieb als **erstes**
   Programm einer frischen Box mit leerem Bildschirm stehen. DOS/4GW sucht
   in der IVT (ab Vektor BDh abwärts) zwei gleiche benachbarte Vektoren
   als freie Plätze und kreist, bis es welche findet. In der Box zeigt
   jeder Vektor auf einen eigenen Stummel; das einzige gleiche Paar waren
   zufällig INT 25h/26h, die mit dem ersten Teil eigene Eingänge bekamen.
   Jetzt stehen 60h–66h wie beim BIOS auf 0000:0000 (`cm_main`). Gefunden
   über `info registers` im QEMU-Monitor (Schleife bei 0750:62F3, DS = 0).
   **Stufe 4, Abschluss (21.11.2026): DOSKEY und COMMAND.COM 4.0.**
   DOSKEY von MS-DOS 6.22 läuft mit deren COMMAND.COM in der Box ohne
   Zutun (INT 2Fh 48xxh hängt DOSKEY selbst ein; Pfeil hoch holt Befehle).
   COMMAND.COM 4.0 aus den MIT-Quellen von Microsoft: `msdos4/COMMAND4.COM`
   (gebaut mit `tools/msdos4.sh` in dosbox-x mit NMAKE/MASM/LINK/BUILDMSG
   aus dem Quellbaum, bytegleich wiederholbar), einzige Änderung
   `msdos4/patches/version.patch` (Versionsprüfung DOS 4.0 oder neuer statt
   genau 4.00). Auf der Platte und auf Setup-Diskette 3 (`HDMDOS` in build.sh –
   Diskette 2 ist voll), in der Box `COMMAND4`. Dafür im Kern: 5702h/5703h
   (erweiterte Attribute) wie die Attrappe in DOS 4.0 `EXTATTR.ASM` – leere
   Liste, CX = 2; 5704h ohne Wirkung (TYPE fragt so die Codepage der Datei
   und brach mit „Extended Error 1“ ab). FCB-Fehler setzen den erweiterten
   Fehler wie MS-DOS (DIR von 4.0 sucht per FCB 11h/12h und erwartet am Ende
   12h, sonst „Path not found“). 5D07h–5D09h (Druckumleitung, ruft COMMAND
   nach jedem Befehl) wie MS-DOS ohne Netz. TAB vor einem CON-Treiber
   (ANSI.SYS) zu Leerzeichen wie DOS selbst (DIR /W zeigte `○`).
   Geprüft in QEMU: DIR, DIR /W, TYPE, COPY, ECHO >, MEM, DOOM 1.9 Timedemo
   unter COMMAND 4.0 (transienter Teil danach heil), EXIT; BIOSCMP
   unverändert 39.
   **Deutsch und Fehlerwege (21.11.2026):** COMMAND.COM 4.0 mit eigener
   deutscher Übersetzung (`msdos4/patches/german.patch`: Meldungsdatei
   `USA-MS.MSG` – COMMON/EXTEND/PARSE/COMMAND – und die Antwortbuchstaben
   A/W/I/F, J/N in `RDATA.ASM`; „Aktuelles Datum: Sa. 21.11.2026“,
   „<DIR>“ (wie im deutschen MS-DOS, Wunsch des Nutzers), „1 Datei(en) kopiert“). Im Kern dafür: 6523h (Ja/Nein, J
   und Y gelten als Ja – DEL /P); INT 2Fh 122Eh (COMMAND meldet dort seine
   Meldungstabellen an und holt sie im INT-24h-Handler wieder – ohne das
   kreiste `SYSGETMSG`); der erweiterte Fehler steht schon vor INT 24h in
   der SDA, 36h setzt ihn bei ungültigem Laufwerk; ein Prozess, der sein
   eigener Elternprozess ist (COMMAND.COM während der Arbeit), wird bei
   „Abbrechen“ nicht beendet, sondern läuft wie bei MS-DOS an seiner
   INT-22h-Adresse weiter (vorher endete die ganze Box). Dabei gefunden:
   `drive_ok` legte die CDS eines beim Start bekannten Laufwerks nie an –
   A: ohne Diskette hatte keinen Pfad, FCB-Suchen liefen als `\…` auf C:
   (DIR A: zeigte den Inhalt von C:). Geprüft in QEMU: DIR A: ohne
   Diskette mit (A)bbrechen/(F)ehler in COMMAND 4.0 und FreeCOM, DEL /P
   mit J, DATE, COPY, DOOM als erstes Programm; BIOSCMP unverändert 39.
   PATH aus der AUTOEXEC.BAT (vom Nutzer gemeldet: COMMAND/COMMAND4 in der
   Box nicht gefunden): ersetzt ihr PATH den der Box ohne `\DOS\MDOS`,
   stellt BOXSHELL `X:\DOS\MDOS` voran (in `\DOS` liegt das MEM von
   AT-DOS, das in der Box nicht läuft; direkt gestartete Programme:
   `ab_mdos`; interaktiv mit /K über `\DOS\MDOS\BOXAUTO.BAT`).
4. **DOS 5/6-Teil**: DOS=HIGH (Kern in die HMA, 4A01h/4A02h), UMB-Kette
   (58h 02h/03h, LOADHIGH, DEVICEHIGH), 3306h, 2Fh 48xxh für DOSKEY, 6Ch,
   7303h nicht (FAT32 bleibt außen), INT 24h-Weg, Strg+C, CONFIG.SYS der
   Box. Spieleliste: DOOM 1.9, DOTT, Digger, Stunts, Elvira; Anwendungen:
   Word 5.5, Lotus Works, NC, MSD, CheckIt, 7za/FreeDOS-Werkzeuge;
   Shells FreeCOM, COMMAND.COM 4.0 (eigener Bau) und 6.22 (Kopie des
   Nutzers) mit LH und DOSKEY. Am Ende **CheckIt Pro** (zweite Hälfte der
   Abnahme).
5. **Vorgabe wechseln** (Entscheidung des Nutzers), BOXSHELL
   verschlanken; COMMAND.COM aus MS-DOS
   4.0 (gebaut, ohne Versionsprüfung) neben FreeCOM mitliefern.
   **Vorgabe gewechselt (21.11.2026, Entscheidung des Nutzers):** ohne
   `DOS_KERNEL=` startet jede Box mit ATDOSCM.SYS (kernel.c `BOX_DOS`),
   `DOS_KERNEL=FREEDOS.IMG` holt FreeDOS zurück; fehlt die gewählte
   Datei, nimmt die Box FREEDOS.IMG. Dafür aus Stufe 4 vorgezogen, weil
   der eigene Kern sonst 21 KB weniger frei ließ als die FreeDOS-Box
   (599 statt 620 KB): **DOS=HIGH** wie MS-DOS 5 – ATDOSCM wird wie der
   Kernel als DOS-EXE gelinkt (CGROUP/DGROUP getrennt, build.sh nimmt den
   Kopf ab und prüft Relokationen); beim Start holt `boot` (entry.asm)
   per XMS 01h die HMA, kopiert den Code nach FFFF:0010 (Code-Offsets
   beginnen bei 10h, die ersten 16 Byte sind der Sprung für den Start
   unten) und rückt die Daten nach 0060:0000. Unten bleiben Daten und
   die Eingänge (Segment LTEXT: alle Interruptvektoren, Geräteköpfe,
   Länder-RETF, Strg+C); INT 21h ruft den Code fern über `hent`. A20 wie
   FreeDOS: beim Programmstart (4B00h) aus – EXEPACK-Entpacker rechnen mit
   dem Umlauf –, der Eingang von INT 21h prüft FFFF:0090 gegen 0000:0080
   und schaltet bei Umlauf per XMS 05h wieder ein. 3306h DH 10h, INT 2Fh
   4A01h (freie HMA). Ohne HMA (CONFIG.SYS ohne `DOS=HIGH`) bleibt der
   Code unten wie vorher. Gemessen: QEMU 619 KB frei (mit EMS-Rahmen,
   4 KB UMB; FreeDOS-Box 620), 86Box 386DX/40 627 KB (BOXSHELL in der
   UMB). Mit dem Wechsel aufgefallen und behoben: MSCDEX fehlte in der
   Box mit eigenem Kern (BOXSHELL hängt seinen INT-2Fh-Haken dort nicht
   ein) – jetzt beantwortet der INT-2Fh-Eingang von ATDOSCM AH = 15h über
   INT E0h 20h; CD-Laufwerke melden 4409h 1000h (fern wie unter MSCDEX,
   CheckIt nennt sie „Network“), B: ohne eigenes Laufwerk ist bei 440Eh
   das Phantom von A:, leere Disketten liefern 4408h/440Dh 0860h wie
   MS-DOS (1,44 MB), LoL+3Fh BUFFERS = 15. Geprüft in QEMU: BIOSCMP
   (gleich wie mit `cm`, mit `fd` gleich wie vorher), DOOM 1.9 Timedemo
   mit SB (1667 Gametics), Digger, MS-DOS-MEM und -FIND (EXEPACK),
   FreeDOS-MEM („MS-DOS verwendet die HMA“), CD mit MSCDEX (150Bh, DIR,
   TYPE), CheckIt SysInfo (DOS-Details, Laufwerke), Lotus Works startet.
   In 86Box 386DX/40 4 MB: MS-DOS-MEM, Digger, CheckIt SysInfo mit
   Laufwerksliste. DOOM bleibt dort mit 4 MB nach „DPMI memory“ stehen –
   mit FreeDOS genauso (Speicher der Maschine, nicht der Kern). Offen:
   MS-DOS-MEM zeigt bei XMS „Verwendet“ weiter einen falschen Wert (wie
   mit dem MS-DOS-Abbild, `docs/BOX-BIOS.md`); CMD nimmt bei `LW` die
   .EXE vor der .COM (MS-DOS umgekehrt).
6. **7.0**: DEM als 32-Bit-C neu übersetzt (gleiche `struct dem_req`),
   DOS-Kern in der Box unverändert – wie bei NT x86/RISC.

Prüfstufen wie üblich: QEMU während der Arbeit, 86Box vor dem Commit
(Box = Hardware-Bezug: 386DX/40 4 MB, P3B-F), Proxmox bei Bedarf.

## Netz in der Box (Plan 23.11.2026, Teil 1 umgesetzt)

Anlass (Nutzer): MSD von MS-DOS zeigt in der Box „Network: No Network“,
obwohl NET USE, Domäne und Home-Laufwerk laufen. Die Box soll sich wie
ein DOS mit geladenem Microsoft-Network-Redirector zeigen – AT-DOS ist
eine Fortentwicklung von DOS, Programme der Zeit (MSD, Installations-
programme, Windows 3.1 in der Box) sollen das Netz sehen. Zwei Teile
(Reihenfolge des Nutzers): erst die Abfragen, dann NetBIOS. Nur der
eigene Kern (ATDOSCM.SYS); FreeDOS- und MS-DOS-Abbilder bleiben, wie
sie sind. Stand Teil 1 unten unter „Stand N1“.

Heute: `atdoscm/dosint.c` `net_fn` beantwortet 5E00h/5E01h nur aus einem
eigenen, leeren Feld, 5Eh 02h–05h und 5Fh 00h–06h mit Fehler 01h („kein
Redirector“); 4409h meldet jedes AT-DOS-Laufwerk lokal (08C2h), nur CDs
fern (1000h wie MSCDEX); INT 2Fh 1100h und B8xxh gehen an den Stummel
des Monitors (keine Antwort).

### Teil 1: Redirector-Antworten (Stufe N1)

Box-Seite (ATDOSCM.SYS, Daten über eine neue DEM-Abfrage):

| Aufruf | Antwort |
|---|---|
| INT 2Fh 1100h | AL = FFh (Redirector da) – nur wenn es mindestens ein NET-USE-Laufwerk oder einen Rechnernamen gibt |
| INT 2Fh B800h | Microsoft Network/LAN Manager da, BX = Redirector/Workstation (Bits nach RBIL, beim Bau gegen MSD prüfen) |
| INT 2Fh B809h | Version (wie LAN Manager 2.x bzw. MS Network 3.0 – nach dem, was MSD/Windows erwarten) |
| INT 21h 5E00h | Rechnername (15 Zeichen, mit Leerzeichen aufgefüllt), CH ≠ 0 = gültig, CL = NetBIOS-Nummer |
| INT 21h 5F02h | n-ter Eintrag: DS:SI „X:“, ES:DI UNC (`\\server\freigabe`), BL 04h Laufwerk (später 03h Drucker bei `NET USE LPT1:`), BH Zustand 0 = gültig, CX 0 |
| INT 21h 5F03h/5F04h | Stufe N1b: Verbinden/Trennen über NET USE (siehe unten) |
| INT 21h 4409h, 440Ah | NET-USE-Laufwerke **fern** (Bit 12), alle anderen wie bisher lokal |
| CDS | NET-USE-Laufwerke mit Flag 8000h (Netz) und UNC-Pfad wie unter MS-DOS (`\\server\freigabe\…`, Backslash-Offset hinter der Freigabe) |

Die Dateiarbeit bleibt, wie sie ist (DEM, kein INT 2Fh 11xxh nötig) –
die CDS-Flags sind nur Anzeige; get_fn/Pfadauflösung im Kern muss den
UNC-Pfad der CDS wieder auf den Buchstaben abbilden (wie heute bei den
MSCDEX-CDS `\\X.\A.`).

Wirt-Seite: neue DEM-Abfrage `DEM_NETINFO` (dem.c, keine neuen FAPI-
Aufrufe): Rechnername (TCPIP.SYS, wie HOSTNAME/COMPUTERNAME; ohne
TCPIP leer), Arbeitsgruppe/Domäne (aus `DOMAIN.KEY`, sonst leer) und je
Laufwerk „NET USE ja/nein + UNC“ aus `ufs.c` (`UFS_INFO` liefert
`fapi_ufsreg.name`). Abfrage beim Start des Kerns und vor jedem
Programmstart (wie `cds_sync` bei BOXSHELL – NET USE kann sich während
einer offenen Box ändern).

Stufe N1b, Verbinden aus der Box: 5F03h (Laufwerk + UNC + Kennwort)
startet über das DEM einen NET USE (`SMBCLI --serve` als losgelöster
Task wie NET.EXE, Kerberos-Ticket aus LOGON, sonst Kennwort aus dem
Aufruf), 5F04h trennt. Damit kann der Datei-Manager von Windows 3.1
Laufwerke verbinden. Erst nach N1, eigener Schritt (startet Tasks aus
der Box – Rechte und Fehlerwege klären).

Vorgehen:
1. **Messen, was gefragt wird**: Spur der INT-2Fh/21h/2Ah/5Ch-Aufrufe
   über Port E9h (wie bei CheckIt), während MSD 2.11 (Kopie des Nutzers)
   die Netz-Seite zeigt und Windows 3.1 startet – unter MS-DOS 6.22 mit
   und ohne Netzclient (MS Network Client 3.0) als Bezug. Erst danach
   die Bits/Versionen der Tabelle festlegen.
2. DEM_NETINFO, Box-Antworten, 4409h/CDS für NET-USE-Laufwerke.
3. BIOSCMP um die Netzaufrufe erweitern (Bezug: MS-DOS 6.22 mit MS
   Network Client 3.0, falls der Nutzer ihn hat).

Testziele Teil 1: MSD 2.11 „Network“ zeigt Microsoft Network/LAN
Manager, Rechnername; Windows 3.1 in der Box mit `network.drv=msnet.drv`
(„Microsoft Network oder 100 % kompatibel“): Datei-Manager zeigt die
NET-USE-Laufwerke mit Netzsymbol und UNC, Systemsteuerung „Netzwerk“;
Gegenprobe ohne NET USE: „No network“ wie bisher; BIOSCMP sonst
unverändert. Prüfstufen: QEMU (Testserver wie bei SMB), dann 86Box
P3B-F am LAN des Nutzers (Samba-AD).

#### Stand N1 (23.11.2026)

Umgesetzt, Abweichungen vom Plan in Klammern:

- Wirt: `DEM_NETINFO` (24, `include/dem.h`, `kernel/dem.c` `d_netinfo`).
  Laufwerk FFFFh: `pos` = Maske der NET-USE-Laufwerke, `flags` = Maske
  LPT1–3, `act` 1 = Netz da (NET USE oder Rechnername), `path` =
  Rechnername (`COMPUTERNAME`, sonst Name aus NET_IFINFO), `path2` =
  Domäne (`USERDOMAIN`, sonst NET_IFINFO). Laufwerk d bzw. 80h + n:
  UNC in `path` (`ufs_unc`, kernel/ufs.c), sonst Fehler 0Fh. redir.c
  gibt dafür den ganzen Auftrag zurück.
- Box (`atdoscm/dosint.c`): `net_sync` fragt bei jedem Programmstart
  (EXEC) einmal ab und merkt Zustand/Masken – die Antworten auf INT 2Fh
  rufen das DEM nicht (wiedereintrittsfest). 2Fh 1100h -> AL = FFh,
  B800h -> AL = 01h, BX = 0008h (Redirector), B809h -> AX = 0300h; ohne
  Netz alles wie vorher (keine Antwort). entry.asm leitet diese drei an
  den C-Teil von INT 2Fh 12xxh.
- 21h 5E00h: Rechnername groß, mit Leerzeichen auf 15 Zeichen, CX =
  0101h; ein per 5E01h gesetzter Name geht vor. 5E02h–05h mit Netz
  „erledigt“ (5E03h CX = 0). 5F00h BH = 1, 5F01h ok, 5F02h/5F05h zählen
  erst die NET-USE-Laufwerke, dann LPT1–3 (BL 04h bzw. 03h, BH 0, CX 0,
  5F05h BP 0), danach Fehler 12h. Ohne Netz 5Eh/5Fh wie vorher.
- 4409h: NET-USE-Laufwerke 1000h (fern), CDS-Flag 8000h zusätzlich
  (C000h), andere lokal (`net_drive`). (Der CDS-Pfad bleibt `X:\` statt
  UNC – der Kern löst über den Buchstaben auf; MSD und Windows nehmen
  die UNC aus 5F02h. 440Ah meldet weiter lokal – offen.)

Geprüft mit einem eigenen Testprogramm (alle obigen Aufrufe, 4409h
für C:–Z:, CDS-Flags) gegen einen Samba-Testserver (Freigabe und
Drucker per NET USE): QEMU (RTL8139) und 86Box 386DX/40 mit NE2000
(SLiRP) – Antworten wie erwartet; COPY/DIR/TYPE auf das NET-USE-
Laufwerk aus der Box. Ohne Netz gibt das Testprogramm dasselbe aus wie
der Stand davor, BIOSCMP unverändert. Offen: MSD 2.11 und Windows 3.1
(msnet.drv, Datei-Manager) mit den Disketten des Nutzers, N1b
(5F03h/5F04h), 440Ah, Teil 2.

### Teil 2: NetBIOS (Stufe N2)

INT 5Ch (NCB, Netzwerkkontrollblock) wie der NetBIOS-Teil des MS
Network Client, darunter NetBIOS über TCP/IP nach RFC 1001/1002 –
Gegenstellen sind die Samba-AD-DCs des Nutzers (dort prüfen, ob
`nmbd`/NBT an ist; Samba-AD bedient Port 137/138 selbst, WINS nur
eingeschränkt).

- Wirt: NBT-Teil im Kernel neben TCPIP.SYS (eigenes Modul `NETBIOS.SYS`
  über die Sockets des Stapels, oder Ring-3-Dienst wie SMBCLI – beim Bau
  abwägen, `docs/NETZWERK.md`): Namensdienst UDP 137 (Namen eintragen/
  prüfen/löschen per Rundruf, Abfrage per Rundruf oder WINS = DC),
  Datagramme UDP 138, Sitzungen TCP 139 (Session Request/Positive
  Response, Keepalive). Namen je Box (bis 16), Sitzungen je Box (bis 8).
- Box: INT 5Ch in ATDOSCM -> Falle -> DEM-Auftrag mit dem NCB. Befehle
  RESET, ADAPTER STATUS, ADD (GROUP) NAME, DELETE NAME, FIND NAME, CALL,
  LISTEN, SEND/RECEIVE (auch CHAIN SEND, RECEIVE ANY), HANG UP, SESSION
  STATUS, SEND/RECEIVE (BROADCAST) DATAGRAM, CANCEL; Warten (Bit 7 aus)
  und ohne Warten (Bit 7, POST-Routine). POST wie der Maus-Rückruf
  (VMOUSE `VS_CBARM`/`VDD_CBQ`): der Kernel stellt die Routine zu, wenn
  der Auftrag fertig ist, auch aus dem Takt.
- Installationsprüfung wie üblich: ungültiger Befehl 7Fh -> Rückgabe
  03h; INT 2Ah 0000h (Netz da) – erst mit N2.

Testziele Teil 2: MSD „NetBIOS present“; NBTSTAT-artiger Test (ADAPTER
STATUS an den DC, FIND NAME); WfW 3.11 in der Box mit NetBIOS (Arbeits-
gruppe/Netzwerk durchsuchen, Freigaben der DCs sehen); ein NetBIOS-
Programm der Zeit (z. B. NET VIEW des MS Network Client) gegen die DCs.

Entschieden (Nutzer, 23.11.2026): Plan freigegeben; NET-USE-Laufwerke
gelten in der Box als „fern“ (MS-DOS-getreu, DEFRAG/CHKDSK lehnen sie
dann wie unter MS-DOS ab), alle anderen Laufwerke bleiben lokal.
Windows 3.1 (mit MSD) hat der Nutzer zum Testen (nur zum Testen, nicht
ins Repository); für Teil 2 fehlt noch ein NetBIOS-Programm der Zeit
(MS Network Client 3.0 oder WfW 3.11).

## Stufe 0: Messung (21.11.2026)

Werkzeuge: `tools/dosbench.asm` (`\DEMO\DOSBENCH`, reine INT-21h-Aufrufe,
Zeit über den BIOS-Takt 46Ch), `tools/dosstruc.asm` (`\DEMO\DOSSTRUC`,
Strukturen), `tools/dosbench.sh` (QEMU: legt eine 128-MB-FAT16-Testplatte
mit 800 Dateien in `MANY\` und DOOM 1.9 Shareware an, misst FreeDOS
direkt, MS-DOS 6.22 direkt – Startdiskette des Nutzers –, FreeDOS-Box und
MS-DOS-Box (MKDOSIMG) auf derselben Platte). 86Box: dieselben Abbilder,
in der Box ist die Testplatte D: (IDE, zweite Platte). Direkt ohne
HIMEM/SMARTDRV (wie die Startdisketten), die Box mit dem Kernel-Cache.

**86Box, Am486DX4-100 (ohne VME, Interpreter), 16 MB, IDE 5400 U/min**
– Werte in BIOS-Takten (55 ms), je ein Durchgang:

| | FreeDOS direkt | MS-DOS direkt | FreeDOS-Box | MS-DOS-Box |
|---|---|---|---|---|
| größtes Programm | 496 KB¹ | 571 KB¹ | 626 KB | 620 KB |
| 4 MB schreiben (32 KB) | 19 | 18 | 84–134 | 133 |
| 4 MB lesen (32 KB) | 14 | 14 | 99–125 | 99 |
| 1 MB lesen (2.048 × 512 Byte) | 6 | 6 | 38 | 37 |
| 2.000 × Seek + 64 Byte | 628 | 371 | 281–351 | 349 |
| 200 Dateien anlegen (4 KB) | 129 | 117 | 346–374 | 348 |
| 200 × öffnen/lesen/schließen | 9 | 4 | 168–183 | 169 |
| 1.000 × Attribut (4300h) | 4 | 2 | 32–34 | 34 |
| 3 × Suche über 800 Einträge | 8 | 4 | 51–52 | 49 |
| 200 Dateien löschen | 95 | 92 | 15 | 15 |
| DOOM `-timedemo demo1` (realtics) | 1.136 | 1.133 | 1.145–1.147 | 1.147 |
| DOOM laden (Takte, geschätzt²) | ~75 | ~42 | ~164 | ~156 |

¹ direkt ohne HIMEM, DOS unten; mit DOS=HIGH wären es ~615–620 KB.
² Gesamtzeit um DOOM minus realtics · 18,2/35.

**86Box, P3B-F mit Pentium II 233 (VME/PVI, Dynarec), 64 MB**:

| | FreeDOS direkt | MS-DOS direkt | FreeDOS-Box | MS-DOS-Box |
|---|---|---|---|---|
| 1 MB lesen (2.048 × 512 Byte) | 3 | 2 | 5 | 3 |
| 200 × öffnen/lesen/schließen | 3 | 2 | 28 | 32 |
| 1.000 × Attribut | 1 | 0 | 6 | 5 |
| 3 × Suche über 800 Einträge | 2 | 1 | 7 | 8 |
| 200 Dateien anlegen | 120 | 112 | 60 | 65 |
| 200 Dateien löschen | 91 | 90 | 3 | 2 |
| DOOM-Timedemo (realtics) | 508 | 505 | 500 | 502 |
| DOOM laden (Takte, geschätzt) | ~45 | ~30 | ~54 | ~67 |

Große Übertragungen und Seek schwanken in der Box stark (Kernel-Cache,
verzögertes Schreiben: zweiter FreeDOS-Lauf 84 statt 134 Takte) – dort
nur die Richtung werten.

**Fallen je Aufruf** (Zählbau, nicht im Repository: Zähler in
`v86_exc`/`dpmi_trap`/`redir`, Ausgabe über Port E9h beim Parken; QEMU,
ohne VME): ein DOSBENCH-Lauf in der FreeDOS-Box = 171.184 Fallen bei
rund 9.800 Redirector-Aufrufen, also **~17 Fallen je DOS-Dateiaufruf** –
INT 46.734 (INT 2Fh 11xxh aus dem DOS in BOXSHELL, INT E0h in den
Kernel, INT 2Ah/28h), CLI 36.502, STI 35.592, IRET 24.812, PUSHF 13.832,
POPF 13.690. MS-DOS-Box 154.870. Redirector-Aufrufe nach Art: READ 3.364,
FINDNEXT 2.400, „Namen prüfen“ (23h) 1.626, GETATTR 1.003, CLOSE 412,
WRITE 328, OPEN/CREATE/DELETE je ~200. DOOM-Timedemo mit Laden: 62.000
V86- und 76.000 PM-Fallen, davon nur 972 Lese-Aufrufe über den
Redirector – DOOM kostet die Box fast nur im PM-Monitor (Takt, PIC,
CLI/STI), nicht in der Dateiverwaltung.

**Ergebnis**

1. **Speicher ist kein Grund**: die Box hat schon 620–626 KB, mehr als
   jedes der beiden DOS direkt ohne HIMEM. Ein Kern in der HMA gewinnt
   nur noch wenige KB.
2. **Spiele nicht**: DOOM läuft in der Box gleich schnell (±1 %), nur das
   Laden dauert 1,5–4-mal so lange (WAD über den Redirector, DOS/4GW-Start).
3. **Kleine Dateiaufrufe sind der Kostenpunkt**: öffnen/lesen/schließen
   20–40-mal so teuer wie direkt (DX4), mit VME noch ~10-mal; Attribut,
   Suche, 512-Byte-Lesen 5–10-mal. Genau das, was Compiler, Make,
   Datenbanken (dBase, Clipper), Word mit vielen kleinen Dateien tun. Die
   Ursache ist gezählt: ~17 Fallen je Aufruf über DOS -> INT 2Fh ->
   BOXSHELL -> INT E0h -> redir.c. Der eigene Kern bringt das auf eine
   Falle je Aufruf (Auftragsblock) – auf 386/486 ohne VME der größte
   Hebel, auch mit VME noch deutlich.
4. **Große Übertragungen** sind in der Box ~7-mal langsamer (DX4), obwohl
   es nur 128 Aufrufe sind und `xfer` direkt in den Speicher der Box
   schreibt – das liegt im Kernel (FAT, Cache, Fenster), nicht im Weg
   über das DOS, und betrifft vermutlich auch FAPI-Programme. Eigener
   Punkt, unabhängig vom Box-Kern (mit nativem Lesen gegenprüfen).
5. **Wo die Box gewinnt**: Anlegen, Löschen, verstreutes Lesen –
   dort hilft der Kernel-Cache (direkt ohne SMARTDRV).

Empfehlung: weiter mit **Stufe 1** (DEM herauslösen). Sie verbessert
für alle Abbilder nichts an der Zahl der Fallen, schafft aber die
Schnittstelle, über die Stufe 2 den Aufruf auf eine Falle bringt. Punkt 4
als eigene Aufgabe im Kernel. Strukturen von MS-DOS 6.22, FreeDOS und
beiden Boxen: `docs/dosstruc/` (Maßstab für Stufe 3).

### Abnahme: DOOM und CheckIt Pro

Vorgabe des Nutzers (21.11.2026): die neue Schicht muss **DOOM** und
**CheckIt Pro** zum Laufen bringen – die beiden ziehen an entgegengesetzten
Enden. Maßstab ist jeweils dasselbe Programm unter MS-DOS 6.22 direkt und
in der MS-DOS-Box auf derselben 86Box-Maschine (DX4-100 und 386DX/40 mit
4 MB; P3B-F für VME).

**DOOM 1.9 (DOS/4GW)** prüft, ob der Kern als Unterlage für ein
DPMI-Programm trägt – wenig DOS, aber das Wenige muss genau stimmen:
- EXEC eines großen MZ mit angebundenem Extender: DOS/4GW findet seine
  eigene EXE über den Namen hinter der Umgebung im PSP und liest sie per
  Seek – Umgebung, Programmname und Speicherblöcke (48h/4Ah) wie DOS.
- INT 21h aus dem PM kommt über die Spiegelung des Monitors im Real Mode
  an (DPMI 0300h, Raw Switch) – der Kern muss wiedereintrittsfest genug
  sein, wie DOS es ist (InDOS, kritische Abschnitte, INT 28h).
- WAD laden: große READ mit Seek dazwischen über das DEM – heute ~9 s
  statt 2–4 s Ladezeit (DX4, Stufe 0); Ziel: höchstens wie direkt.
- Bleibt unverändert im Monitor/in den Modulen: DPMI, Takt/PIC, SB/AdLib/
  MPU (VSB.SYS), VGA – DOOM ist damit der Beleg, dass der Wechsel des
  DOS diese Schicht nicht stört.
- Messung: `-timedemo demo1` (realtics wie heute ±1 %) und Ladezeit
  (DOSBENCH /T davor und danach), mit Ton (SB16) und ohne.

**CheckIt Pro 1.11** prüft das Gegenteil – das DOS von innen. Es liest,
was heute das echte DOS im Abbild liefert und der Kern dann selbst
führen muss (Funde der Box-Tests: `docs/BOX-BIOS.md`):
- DOS-Details: Version/wahre Version, COMSPEC, Umgebung, BREAK/VERIFY,
  FILES/BUFFERS/LASTDRIVE aus der Liste der Listen, Gerätekette (NUL,
  CON, CLOCK$, EMMXXXX0, ATIN$/ATOUT$), Länderdaten.
- Speicherkarte: MCB-Kette mit Besitzern und Namen, UMB-Verkettung, HMA
  (3306h, 4A01h), XMS/EMS über den Monitor.
- Laufwerke: DPB (32h/1Fh), 36h, IOCTL 4408h/4409h/440Dh, Bezeichnung,
  CD über MSCDEX 15xxh (als „Network“ wie unter MS-DOS) – mit dem eigenen
  Kern erscheinen die AT-DOS-Laufwerke **lokal** wie unter MS-DOS.
- Plattentests über INT 25h (lesend, Regel wie MS-DOS 7) – heute meldet
  CheckIt in der Box „nicht vorhanden“, mit dem eigenen Kern wird der
  Lesetest möglich; Schreibtests nur mit Laufwerkssperre.
- Bildschirmseiten von CheckIt (DOS Details, Memory, Drives) werden je
  Seite gegen MS-DOS 6.22 auf derselben Maschine verglichen; erlaubte
  Unterschiede wie heute in `docs/BOX-BIOS.md` (86Box-Koprozessor, Uhr).

Beide zusammen sind die Bedingung für Stufe 5 (Vorgabe wechseln),
zusätzlich zu BIOSCMP/doscmp und `docs/dosstruc/`.

## Undokumentiertes: wie wir es treffen

Hinweis des Nutzers (21.11.2026): das eigentliche Problem waren immer
die undokumentierten Teile des MS-DOS-Kerns. Programme der Zeit lesen
sie direkt, und jede Abweichung fällt erst bei einem bestimmten Programm
auf (wie bei Word 5.5, Lotus Works und CheckIt in der Box).

Wer was liest (Auswahl, zum Abhaken in Stufe 3/4):

| Nutzer | Was | Wo |
|---|---|---|
| Windows 3.x erweitert, Task-Umschalter (DOSSHELL) | SDA tauschen (5D06h/5D0Bh, Liste der Bereiche), INT 2Fh 1605h/1606h, 4B05h | SDA, LoL |
| TSRs (Popups, Druckspooler) | InDOS- und Fehler-Merker (34h, SDA), INT 28h, aktueller PSP (SDA, 50h/51h/62h) | SDA |
| Speicherverwalter, MEM, MSD | MCB-Kette ab LoL-2, UMB-Verkettung, LoL-Felder (erster MCB, DPB, SFT, CDS, Puffer, LASTDRIVE, Geräteköpfe) | LoL, MCB |
| Netz-Redirector, SHARE, MSCDEX | INT 2Fh 11xxh/12xxh, CDS-Flags, SFT-Felder, SDA-Pfade (`FN1`/`FN2`) | CDS, SFT, SDA |
| Plattenwerkzeuge (CHKDSK, Norton, PC Tools) | DPB (32h/1Fh), INT 25h/26h, 440Dh | DPB |
| Programme mit eigener Dateiverwaltung | SFT direkt (Größe, Position, Modus), JFT im PSP (+18h, +32h/+34h) | SFT, PSP |
| DOSKEY, APPEND, installierbare Befehle | INT 2Fh 4800h/AE00h/AE01h, B7xxh | Multiplex |

Drei Quellen, in dieser Reihenfolge:

1. **MS-DOS 4.0-Quelltext** – für 4.0 nicht mehr undokumentiert, sondern
   die Wahrheit: Layout aus `INC/*.INC` (SYSVAR, SF, CURDIR, DPB, DOSSYM,
   SDA in `DOSDATA`/`MS_DATA`), Verhalten aus `DOS/*.ASM`.
2. **Echtes MS-DOS 6.22 in der Box messen** – was sich von 4.0 bis 6.22
   geändert hat (SDA-Felder, LoL-Erweiterungen, UMB, HMA), wird nicht
   geraten, sondern abgelesen: MKDOSIMG-Box mit der 6.22 des Nutzers,
   ein eigenes Werkzeug (Teil von BIOSCMP oder eigenes `DOSSTRUC.COM`)
   gibt LoL, SFT-Kopf und -Einträge, CDS, DPB, SDA-Länge und Lage der
   bekannten SDA-Felder, MCB-Kette und PSP-Felder als Text aus. Diese
   Ausgabe (nur Werte, kein Microsoft-Code) kommt wie `docs/bioscmp/
   msdos622.txt` ins Repository und ist der Maßstab für den eigenen
   Kern. Dieselbe Ausgabe unter FreeDOS zeigt gleich, wo FreeDOS als
   Vorlage abweicht.
3. **Öffentliche Beschreibungen** (Ralf Browns Interrupt-Liste,
   „Undocumented DOS“) – nur zum Nachschlagen, welche Programme welches
   Feld nutzen und welche Versionen es wie belegen.

Regel: ein Feld, das in der 6.22-Messung steht, hat im eigenen Kern
denselben Ort und dieselbe Bedeutung – auch wenn der Kern es selbst nicht
braucht (dann mit dem Wert, den 6.22 dort hat). Wo 4.0-Quelle und
6.22-Messung abweichen, gilt die Messung.

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

## Windows 3.1 in der Box (vorgemerkt, 23.11.2026)

Vorgemerkt vom Nutzer, noch kein Code (Grund: Windows dient nur als
Prüffall für die Netzerkennung, die eigene Oberfläche kommt später als
Win16-Schicht). Ausgangslage, gemessen mit der Aufzeichnung in `v86.c`
(`wtrace`, DMESG-Zeilen „WIN >nn …“, ba1bb7f/0630bf1 – bleibt vorerst
drin): Windows 3.11 (ohne WfW) meldet in SETUP „andere Protected-Mode-
Software“. DOSX (Standardmodus) findet über 1687h unser DPMI 0.90,
fragt dann aber VCPI (INT 67h DE00h) ab, bekommt 84h und bricht ab.
WIN386 (erweiterter Modus) schickt 2Fh 1605h, fragt ebenfalls DE00h und
meldet sich mit 1606h wieder ab. EMS 5900h fehlt (A4h).

Nachmessung (23.11.2026, Windows 3.11 deutsch von der ISO des Nutzers,
nur zum Testen; QEMU 16 MB, eigener Box-Kern): SETUP-DOS-Teil läuft
(Express, kopiert nach C:\WINDOWS), der Wechsel in den Grafikteil
startet DOSX und bricht ab wie in 86Box. `C:\WINDOWS\SYSTEM\DOSX`
direkt: 2Fh 1605h (kein Widerspruch), 67h 4600h -> 0040h (EMS 4.0, also
V86 unter einem Speichermanager), 67h DE00h -> 84h (kein VCPI), 2Fh
1606h, Meldung. **DPMI (1687h) fragt DOSX gar nicht ab** – der
Standardmodus kennt nur VCPI oder den Real Mode. WIN.COM baut SETUP erst
im Grafikteil, nach dem DOS-Teil fehlt es.

Weg A, ein VCPI-Dienst, scheidet aus: der Client übernimmt mit eigener
GDT/IDT/CR3 die Maschine im Ring 0, die Box wäre nicht mehr geschützt.

### Stand Weg B (23.11.2026): Windows 3.11 startet im Standardmodus

`C:\DOS\MDOS\WINLDR.COM SYSTEM\KRNL286.EXE` (aus `C:\WINDOWS`)
startet Windows 3.11 deutsch in der Box: KRNL286, SYSTEM/KEYBOARD/
VGA.DRV, GDI, USER, Schriften und SETUP.EXE laden, der Grafikteil
zeigt sein erstes Fenster (SETUP meldet „nicht vollständig
eingerichtet“, weil es ohne den DOS-Teil gestartet wurde). Geprüft in
QEMU (16 und 64 MB) und 86Box P3B-F (Pentium II, ET4000/W32p, 64 MB);
DOOM 1.9 Timedemo mit Ton danach unverändert.

- **WINLDR.COM** (`apps/winldr.asm`, `\DOS\MDOS`): fängt 2Fh 1687h
  ab, ruft den Moduswechsel des Wirts selbst und hängt sich im PM in
  INT 21h, 10h und 15h ein; der Codeselektor des Wirts zeigt danach auf
  den Kern (KRNL rechnet mit ihm), WINLDR selbst nimmt einen eigenen.
  Übersetzt wie DOSX: Namen über DS:DX/DS:SI (39h–3Dh, 41h, 43h, 4Eh,
  5Ah, 5Bh, 56h, 60h, 6Ch), 3Fh/40h in 4-KB-Stücken über einen eigenen
  Selektor auf die lineare Adresse (KRNL liest über die Grenze seines
  Segments hinaus – fiel nur in 86Box auf), DTA (1Ah/2Fh/4Eh/4Fh), 47h,
  38h, 25h/35h über DPMI 0204h/0205h, PSP-Selektor ↔ Segment (50h, 51h,
  62h, 26h, 55h), 34h/52h ES -> Selektor, 48h/49h/4Ah über 0100h–0102h,
  09h; INT 10h 1002h/1009h/1012h/1017h/1Bh/13h/1100h/1110h (Palette –
  ohne sie blieb das Bild schwarz), INT 15h C0h (ES:BX der Konfigura-
  tionstabelle, COMM.DRV). Alles andere geht unverändert an den Wirt.
- **DPMI-Wirt** (`kernel/v86.c`): Erweiterung „MS-DOS“ (2Fh 168Ah im
  PM, Einsprung `DP_MSDOS`, AX = 0100h LDT-Alias): die LDT ist bei
  7FFF0000h nur lesbar eingeblendet, `al_write` bildet jeden Schreib-
  zugriff nach (MOV, MOV imm, OR/AND/XOR, XCHG, STOS/MOVS auch mit REP)
  und prüft den Eintrag – Code/Daten bekommen DPL 3, Systemdeskriptoren
  (Tore) werden „nicht vorhanden“, so kommt kein Programm an Ring 0.
  Mit dem Alias wächst die LDT auf 8192 Einträge (16 Seiten am Stück,
  Belegung in eigener Seite, `ldt_grow`), KRNL braucht über 512.
  0002h liefert für dasselbe Segment denselben Selektor, 0004h/0005h
  (Selektor sperren) ohne Wirkung. Selektor 40h auch aus dem Speicher
  (LES/LDS/LSS/LFS/LGS/MOV Sreg, `sel40m`). INT 1Dh–1Fh/41h/43h/44h/46h
  im PM ohne Handler nur IRET (INT 41h AX=004Fh fragt nach einem
  Debugger – weitergereicht sprang die Box in die Plattentabelle).
  2Fh 1687h geht durch die Kette, wenn ein Programm INT 2Fh seit dem
  Start umgebogen hat (`v2f0`), sonst wie bisher der Monitor direkt.
- **Box-Kern** (`atdoscm/exec.c`): minalloc = maxalloc = 0 lädt oben in
  den Block wie MS-DOS (KRNL286.EXE rechnet damit).

### Stand Weg B (24.11.2026): SETUP von CD läuft über DOSX → WINLDR

Das normale SETUP von der CD läuft jetzt durch DOS-Teil, Namens-
abfrage, Kopieren aller Dateien und REGEDIT; Windows beendet sich
danach sauber und die Box kehrt zu CMD zurück. Geprüft in QEMU (32 MB,
frische Platte, `D:` + `SETUP`, Express) und 86Box P3B-F (Start über
`C:\WINDOWS\SYSTEM\DOSX.EXE`); DOOM 1.9 Timedemo unverändert.

- **Box-Kern** (`atdoscm/exec.c`): 4B00h auf `…\DOSX.EXE` startet statt
  dessen `X:\DOS\MDOS\WINLDR.COM` (Startlaufwerk) mit `KRNL286.EXE` aus
  dem Verzeichnis von DOSX und der alten Befehlszeile dahinter.
- **Box-Kern** (`atdoscm/file.c`): Bit 7 des Öffnungsmodus („nicht
  vererben“) blieb nicht im SFT-Eintrag stehen (`mode & 7Fh`). Windows
  öffnet seine Module und Schriften so; jeder neue Task-PSP (55h) erbte
  sie alle, die JFT (20) war voll und das Öffnen von REG.DAT scheiterte
  mit 04h. Jetzt bleibt das Bit im Modus und steht wie bei MS-DOS als
  `sf_no_inherit` (1000h) in den Flags.
- **WINLDR**: INT 25h/26h aus dem PM wie DOSX (Paketform über den
  Puffer unter 1 MB, Rückkehr mit RETF, die Flags bleiben auf dem
  Stapel – CPWIN386.CPL stürzte sonst mit #GP ab); 2Fh 4310h im PM
  liefert einen eigenen XMS-Einsprung, der über DPMI 0301h an den
  XMS-Treiber geht (0Bh mit umgerechneten Adressen bei Handle 0) – SETUP
  ruft den XMS-Treiber direkt aus dem PM; 56h und 60h nahmen nach dem
  Umkopieren des ersten Namens ein verbrauchtes DI (das Umbenennen
  TEMP.000 → Ziel scheiterte mit 05h); INT 21h 00h im PM wird 4C00h;
  vor einem 4Ch aus dem PM stellt WINLDR den PSP des Kerns wieder ein
  (Windows lässt den einer Task stehen, DOS beendete sonst diese und
  die Box stürzte danach in WINLDR ab).
- **Kernel**: `FILES=` höchstens 219 (ein Kernelsegment; 255 lief über).

Offen: Nach REGEDIT beendet sich Windows mit Code 0, statt Drucker,
Gruppen und den Abschlussdialog zu zeigen – die schon angelegte
WIN.COM bleibt unter ihrem Zwischennamen liegen (Ursache noch unklar,
der Windows-Teil macht danach keinen Dateiaufruf mehr). SETUP schrieb
in die CONFIG.SYS des Startlaufwerks (HIMEM, SMARTDRV, `DOS=HIGH,UMB`) –
in der Box ist das die CONFIG.SYS von AT-DOS. Seit 24.11.2026 (Ent-
scheidung des Nutzers) lenkt der Box-Kern `X:\CONFIG.SYS` des Start-
laufwerks auf `X:\DOS\MDOS\CONFIG.WIN` um, die niemand auswertet (wie
CONFIG.NT bei NT; `cfg_redirect` in atdoscm/file.c, auch für die LFN-
Aufrufe – FreeCOM-COPY nimmt 716Ch, das leerte vorher die echte Datei).
Geprüft in QEMU: COPY/TYPE/DEL aus der Box treffen CONFIG.WIN, die
CONFIG.SYS von AT-DOS bleibt bytegleich. Nur der eigene Box-Kern; mit
`DOS_KERNEL=FREEDOS.IMG` gilt es nicht. AUTOEXEC.BAT bleibt die der Box
(BOXSHELL übernimmt daraus nur PATH/SET/PROMPT).
Weiter: Maus/Tastatur im Windows-Betrieb, KRNL386, EMS 5900h,
DOS-Programme aus Windows (Standardmodus: DSWAP), Netz.

Weg B (Plan, wie WIN-OS/2): ein eigener Lader statt DOSX.EXE, geliefert
in `\DOS\MDOS`. Der Box-Kern ersetzt beim Programmstart (INT 21h
4B00h) den Aufruf von `DOSX.EXE` durch den Lader – CD und Windows-
Installation bleiben unverändert (grafisches SETUP, `WIN`, `WIN /S`).
Der Lader startet KRNL286/KRNL386 als Programm auf unserem DPMI-Wirt
und übernimmt, was DOSX dem Windows-Kern bietet: Übersetzung der DOS-
Aufrufe aus dem Protected Mode (Puffer über DS:DX u. a. unter 1 MB
umkopieren) und die übrigen DOSX-Dienste; fehlende DPMI-Funktionen im
Wirt ergänzen, EMS 5900h nachziehen. Nur Standardmodus – WIN386 bleibt
außen vor. Geschätzt 3–6 Tage, Risiko: welche Dienste KRNL zusätzlich
erwartet.

## Offene Fragen an den Nutzer

1. ~~Grundlage des Box-Kerns~~ – entschieden: beide (FreeDOS-Rumpf,
   MS-DOS-4.0-Strukturen und -Verhalten); FreeDOS als Vorlage, nicht
   1:1; Aufteilung der Tabelle „Was aus welcher Quelle kommt“ vom
   Nutzer bestätigt (21.11.2026), Undokumentiertes siehe eigener
   Abschnitt.
2. ~~Name der Datei~~ – entschieden: `ATDOSCM.SYS` in `\DOS\MDOS`,
   direkt geladen (kein Abbild); INI-Schlüssel `DOS_KERNEL=Datei`
   (`.SYS` direkt, `.IMG` Abbild), `DOS_IMAGE=` als alter Name.
3. ~~Gemeldete Version~~ – entschieden: 6.22; die 4.0-Shell im eigenen
   Bau ohne Versionsprüfung.
4. ~~INT 25h/26h~~ – entschieden: wie MS-DOS 7 (Lesen frei, Schreiben
   nur mit Laufwerkssperre 440Dh 4Ah, FAT32 über 7305h).
5. ~~Stufe 0~~ – gemessen (Abschnitt „Stufe 0: Messung“); Entscheidung
   über Stufe 1 beim Nutzer.
6. ~~Netz in der Box: NET-USE-Laufwerke „fern“ melden?~~ – entschieden
   (Nutzer, 23.11.2026): ja, nur die NET-USE-Laufwerke (4409h Bit 12,
   CDS 8000h); alle anderen AT-DOS-Laufwerke bleiben lokal.
