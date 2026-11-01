# alt-dos – AT-DOS 5.0, Meilenstein 6

Alternative Zeitlinie: MT-DOS 4.0 wird fortgeführt, AT-DOS 5.0 läuft im
80286 Protected Mode, segmentiert, mit kooperativem Multitasking. Hardware
ist nur über die Family API (FAPI) erreichbar. „AT“ steht doppelt: für die
Rechnerklasse, die es voraussetzt, und für die Alternative – wie bei DR-DOS
und PC-DOS.

## Was läuft

- Standard-FAT12-Diskette: Der Kernel ist die Datei `ATDOS.SYS` (versteckt,
  System), die wie einst `IO.SYS` als erste Datei zusammenhängend liegt
- Diskettentreiber im Kernel (Controller 765/82077, DMA-Kanal 2, IRQ 6) –
  im Protected Mode gibt es kein BIOS mehr
- IFS-Schicht; FAT12 und FAT16 stecken wie bei OS/2 fest im Kernel, mit
  Unterverzeichnissen, Partitionstabelle und 32-Bit-Sektornummern
- Schreiben auf FAT12/16 (Diskette, IDE, SCSI): anlegen, schreiben,
  löschen, Verzeichnisse anlegen/entfernen, umbenennen und verschieben.
  Daten gehen sofort auf den Datenträger, die FAT am Ende jeder Operation in
  beide Kopien; Zeitstempel aus der CMOS-Uhr. Disketten werden vor jeder
  Operation neu eingelesen (Wechsel), Schreibschutz wird erkannt
- Gerätetreiber als Dateien: `CONFIG.SYS` lädt per `DEVICE=` Ring-0-Treiber,
  die über DevHlp mit dem Kernel sprechen. Dabei: `AHA154X.SYS` (Adaptec
  AHA-1540/1542, SCSI, Busmaster mit Mailboxen; läuft auch mit dem
  kompatiblen BusLogic BT-545C) und `IDE.SYS` (bis zu
  vier IDE-Platten an primärem und sekundärem Kanal, PIO, LBA oder CHS für
  Platten ohne LBA; ATAPI-CD-ROMs als Einheit; dazu MFM/RLL-Platten am
  WD1003 und ESDI am WD1007, Geometrie aus der Tabelle des BIOS) – beide lesen und schreiben,
  CD-ROMs (2048-Byte-Blöcke) nur lesen
- `CONFIG.SYS` wie MS-DOS: `DEVICE=`, `DEVICEHIGH=` (auch mit `/L:`),
  `IFS=`, `FILES=` (Tabelle offener Dateien, 8–255, Vorgabe 20),
  `BUFFERS=` (Sektor-Cache, 1–99 Sektoren, Vorgabe 15; Dateien werden bis
  zu 4 Sektoren am Stück gelesen); `STACKS=`, `FCBS=`, `BREAK=`, `LASTDRIVE=`, `DOS=`, `SHELL=`,
  `COUNTRY=`, `SET=` und die Menübefehle von MS-DOS 6 werden angenommen und
  übergangen, Zeilen mit `;` sind Kommentare
- Nachladbare Dateisysteme: `IFS=` in `CONFIG.SYS`. Erstes Modul ist
  `ISO9660.IFS` für CD-ROMs an IDE (ATAPI) und am Adaptec; jedes CD-Laufwerk
  bekommt einen Buchstaben, ein CD-Wechsel wird bei jedem Zugriff erkannt
- Laufwerke `A:` (Diskette), `C:`, `D:` … für jede FAT-Partition
- Start von Diskette oder von einer FAT16-Partition (IDE oder SCSI), siehe
  „Start von Festplatte“
- Programmlader für das eigene FX-Format. Beim Start läuft der
  Befehlsinterpreter aus `PROTSHELL=` (Vorgabe `COMMAND.EXE /P`), der
  `AUTOEXEC.CMD` abarbeitet. Wie bei OS/2 gehören `.CMD`-Dateien der nativen
  Seite, `AUTOEXEC.BAT` und `SHELL=` bleiben der DOS-Box (ab 6.0) vorbehalten;
  `.BAT` führt COMMAND.EXE bis dahin trotzdem aus
- `COMMAND.EXE`: Zeileneditor und Befehlsspeicher wie DOSKEY eingebaut
  (Pfeil auf/ab, links/rechts, Pos1/Ende, Einfg/Entf, F3, Esc, Bild auf/ab;
  `DOSKEY /HISTORY` listet die letzten 16 Befehle). **Tab** vervollständigt
  wie bash: erstes Wort aus internen Befehlen und Programmen im PATH, sonst
  Dateien und Verzeichnisse; mehrdeutig ergänzt den gemeinsamen Anfang,
  zweites Tab listet die Möglichkeiten.
  Befehlsinterpreter mit `DIR` (Verzeichnisse zuerst, `/A` alle bzw.
  `/A:HSDRA` mit `-` wie MS-DOS, `/P`, Muster, freier Platz), `CD`,
  Laufwerkswechsel, `TYPE`, `COPY` und `DEL` (auch mit `*` und `?`), `MD`,
  `SET`, `PATH`, `PROMPT` (`$P $G $N $D $T $V $_ …`), Stapeldateien (`.BAT`
  mit `@`, `ECHO ON/OFF`, `ECHO.`, `REM`, `PAUSE`, `GOTO`, `IF [NOT]
  EXIST/==/ERRORLEVEL` – Programme liefern Rückgabewerte, `IF ERRORLEVEL n`
  heißt „n oder größer“ wie bei MS-DOS, `COMMAND /C` reicht den Wert durch –, `CALL`, `SHIFT`, `%0`–`%9`, `%NAME%`), `AUTOEXEC.BAT`
  beim Start (`COMMAND /P`), `COMMAND /C`; Programme werden wie bei
  COMMAND.COM erst im aktuellen Verzeichnis, dann über `PATH` gesucht
  (`.EXE`, `.BAT`; `.COM` wird als MS-DOS-Programm erkannt),
  Umleitung `<`, `>`, `>>` und Pipes `|` (echte Pipes, alle Teile laufen
  gleichzeitig), `RD`, `REN`, `CLS`, `VER`, `ECHO`, `START`, `SHUTDOWN` (`/H` anhalten, `/R`
  neu starten; auch `POWEROFF`, `REBOOT` = `SHUTDOWN /R`: schließt alle Dateien, leert die
  Platten-Caches), `HILFE`, `EXIT`; andere Eingaben
  starten das gleichnamige Programm, auch mit Pfad
- Werkzeuge wie MS-DOS 5/6 (Schalter und Meldungen wie dort): `ATTRIB`
  (`+R -A … /S`), `XCOPY` (`/S /E /A /M /P /Y`, Datum bleibt erhalten),
  `MOVE`, `DELTREE`, `TREE` (`/F /A`), `LABEL`, `CHOICE` (`/C /N /S /T`,
  ERRORLEVEL), `SORT` (`/R /+n`), `FC` (Text mit Resynchronisation wie
  MS-DOS, `/B /C /N /nnnn`); in COMMAND dazu `VOL`, `DATE`, `TIME`.
- `FORMAT X: [/V[:Name]] [/Q] [/S] [/FS:FAT]` wie MS-DOS, gebaut wie OS/2:
  FORMAT startet `U<Dateisystem>.EXE /FORMAT` aus `\DOS\SYSTEM` (für FAT
  `UFAT.EXE`). Disketten (1,44 MB, 1,2 MB, 720 KB, 360 KB) und Partitionen
  (FAT12 bei Typ 01h, sonst FAT16 mit den Clustergrößen von MS-DOS 6); ohne
  `/Q` wird jeder Sektor gelesen und Fehlerhaftes gesperrt (keine
  Low-Level-Formatierung). Das Startlaufwerk wird nicht formatiert.
  Unformatierte Partitionen bekommen trotzdem einen Buchstaben.
- `SYS [Quelle:] Ziel: [/A]` überträgt `ATDOS.SYS`, `COMMAND.EXE`, `\DOS\SYSTEM`,
  `CHKDSK.EXE` und (falls fehlend) `CONFIG.SYS`/`AUTOEXEC.CMD`, dazu den
  Startcode in den Bootsektor; mit `/A` zusätzlich alle Programme aus
  `\DOS` (vollständige Installation). Disketten nur leer (`FORMAT A: /S`). Der
  Diskettenbootsektor meldet „Keine Systemdiskette“, wenn `ATDOS.SYS` fehlt.
- `CHKDSK [X:] [/F] [/V]` wie MS-DOS: prüft FAT, Ketten, Dateigrößen,
  Querverbindungen und verlorene Zuordnungseinheiten; `/F` repariert
  (verlorene Ketten als `FILEnnnn.CHK`, Größen, FAT-Kopien). CHKDSK kennt
  selbst kein Dateisystem: Es fragt den Kernel (`DosQFSInfo`) und startet
  `U<Dateisystem>.EXE` aus dem PATH, für FAT also `UFAT.EXE` – wie
  `UFAT.DLL`/`UHPFS.DLL` bei OS/2. Ein neues Dateisystem bringt sein `.IFS`
  und sein `U….EXE` mit. Für `/F` wird das Laufwerk gesperrt; geht das nicht
  (Dateien offen), merkt CHKDSK es wie Windows NT für den nächsten Start vor
  (`\AUTOCHK.LST`), `COMMAND /P` prüft dann vor `AUTOEXEC.CMD`.
  Querverbindungen behebt `/F` wie ScanDisk: Der gemeinsame Rest wird für
  die zweite Datei in freie Cluster kopiert. ERRORLEVEL wie bei NT: 0 keine
  Fehler, 1 behoben, 3 nicht behoben oder nicht prüfbar. GREP liefert wie
  GNU grep 0/1/2 (Treffer/keiner/Fehler), FIND wie MS-DOS 6 ebenso.
  Wie bei NT und OS/2 merkt sich jede FAT16-Platte, ob sie sauber
  abgemeldet wurde (FAT[1] Bit 15, wie Windows 95/NT; MS-DOS stört es
  nicht): beim ersten Schreiben gelöscht, bei `SHUTDOWN`, `POWEROFF` und
  Strg+Alt+Entf gesetzt. Fehlt es beim Start, prüft `COMMAND /P` die Platte
  mit `CHKDSK /F`. `AUTOCHECK=CD` in der CONFIG.SYS beschränkt das auf
  bestimmte Laufwerke, `AUTOCHECK=` ohne Buchstaben schaltet es ab.
- Sitzungen wie bei OS/2: jedes per `START` gestartete Programm hat einen
  eigenen Bildschirm, **Alt+Esc** wechselt, **Strg+C** bricht ab,
  **Strg+Alt+Entf** schließt alle Dateien, leert die Platten-Caches und
  startet neu; hängt ein Programm, nach spätestens 5 s trotzdem (zweimal
  drücken: sofort)
- Tastaturtreiber (IRQ 1), deutsche Belegung mit Umlauten und AltGr fest
  eingebaut; `KEYB US` oder `KEYB GB` lädt andere Belegungen aus `.KBL`-Dateien
- A20 über den Tastaturcontroller, GDT/IDT, Wechsel per `LMSW`
- PIC auf 20h/28h umgelegt, Systemtakt 100 Hz
- FAPI-Call-Gate (Selektor 0023h) von Ring 3 nach Ring 0
- je Task ein 286-TSS und eine eigene LDT, Ring 3, IOPL 0
- Speicherverwaltung über die vollen 16 MB in 256-Byte-Einheiten,
  Segmente per `DosAllocSeg`/`DosFreeSeg`, Aufräumen bei Task-Ende
- kooperativer Scheduler mit Hardware-Taskwechsel (`JMP` auf TSS)
- Ausnahmen in Anwendungen beenden nur den Task, nicht das System

**MODE CON LINES=25|43|50** stellt die Zeilenzahl der eigenen Sitzung um
(wie OS/2: jede Sitzung hat ihre eigene, beim Wechsel stellt der Kernel die
Karte um). 43 und 50 Zeilen nur mit VGA: 350 bzw. 400 Rasterzeilen mit dem
8x8-Zeichensatz aus dem ROM der Karte; `MODE CON` zeigt den Zustand,
`MODE CO80` geht zurück auf 25. EDIT, LESS, MORE und `DIR /P` nutzen alle
Zeilen.

Programme auf der Diskette: **EDIT** ist ein Bildschirm-Editor wie MS-DOS
EDIT (Vollbild, Menüs mit Alt oder F10, Markieren mit Umschalt,
Umschalt+Entf/Strg+Einfg/Umschalt+Einfg, Strg+Y, Suchen/Weitersuchen (F3)/
Ändern, Strg+S; Text bis knapp 64 KB, Tabulatoren werden zu Leerzeichen;
Dialoge mit Tab und Pfeiltasten; Öffnen mit Dateiliste, Verzeichniswechsel
und Mustern wie `*.TXT`; beim Herunterfahren fragt EDIT, ob geänderter Text
gespeichert werden soll).
Alt+Taste liefert wie beim BIOS Zeichen 0 und den Scancode, Alt allein
`KEY_ALT` (3800h). **LESS** zeigt Text an und blättert vor und
zurück nach dem Vorbild von GNU less (Bild/Pfeile, `g G`, `123g`, `/` `?`
`n N` mit Hervorhebung, `-N` Zeilennummern, `-i`, `h` Hilfe; aus Datei oder
Pipe), **GREP** sucht mit regulären Ausdrücken nach dem
Vorbild von GNU grep (`-i -v -n -c -l -h -H -r -s -E`, `. * + ? ^ $ [..] |`,
Platzhalter in Dateinamen, erkennt Binärdateien), **MORE** blättert seitenweise, **FIND** sucht
Text (`/V /C /N /I`, wie MS-DOS), **MEM** zeigt den Arbeitsspeicher, **TASKA**
zählt und gibt per `DosYield()` ab (schön mit `START TASKA` im Hintergrund),
**TASKB** fordert zehn Segmente zu 64 KB an, prüft sie, gibt die Hälfte frei
und überlässt den Rest dem Kernel, **TASKC** versucht direkt nach B800:0000 zu
schreiben und wird per #GP beendet.

## Programmformat

FAPI-Programme sind `.EXE`-Dateien mit einem MZ-Stub vorn: Unter MS-DOS
gestartet, meldet er nur „Diese FAPI-Anwendung kann nicht im Real Mode
ausgeführt werden.“ Dahinter folgt der FX-Kopf mit der Signatur `CH` – nach dem Vorbild von MZ,
den Initialen von Mark Zbikowski (siehe `include/fx.h`), mit
Größe von Abbild, BSS und Stack sowie dem Einsprung. Der Lader legt für jedes
Programm ein Segment genau dieser Größe an. `tools/mkfx.py` erzeugt die
Dateien aus dem Watcom-Abbild.

## FAPI (Stand Meilenstein 6)

Dateinamen und Pfade nehmen bis 260 Zeichen an (wie bei OS/2), auch wenn FAT
selbst nur 8.3 kennt.

| AX | Funktion        | Argumente                       |
|----|-----------------|---------------------------------|
| 0  | DosGetVersion   | → AX = 0500h                    |
| 1  | VioWrtStr       | BX = String, CX = Zeile<<8\|Spalte |
| 2  | DosYield        | –                               |
| 3  | DosExit         | BX = Rückgabewert 0–255 (ERRORLEVEL); `main` ohne DosExit liefert 0 |
| 4  | DosSleep        | BX = Millisekunden              |
| 5  | DosMemInfo      | BX = `struct fapi_meminfo` (KB) |
| 6  | DosAllocSeg     | BX = Größe, 0 = 64 KB → AX = Selektor |
| 7  | DosFreeSeg      | BX = Selektor                   |
| 8  | KbdCharIn       | → AX = Zeichen \| Scancode<<8   |
| 9  | KbdPeek         | wie 8, wartet nicht, 0 = nichts |
| 10 | VioWrtTTY       | BX = Text, CX = Länge           |
| 11 | VioCls          | –                               |
| 12 | DosExecPgm      | BX = Befehlszeile, CX = 0 warten / 1 eigene Sitzung |
| 13 | DosGetArgs      | BX = Puffer (81 Bytes)          |
| 14 | DosOpen         | BX = Pfad → Handle              |
| 15 | DosRead         | BX = Handle, CX = Puffer, DX = Länge → Bytes |
| 16 | DosClose        | BX = Handle                     |
| 17 | DosFindEntry    | BX = `struct fapi_dirent`, CX = Nummer, DX = Verzeichnis (0 = aktuelles) |
| 18 | KbdSetLayout    | BX = `.KBL`-Daten (278 Bytes), 0 = eingebaute |
| 19 | KbdGetLayout    | BX = Puffer (4 Bytes) für die Kennung |
| 20 | DosChDir        | BX = Pfad                       |
| 21 | DosQCurDir      | BX = Puffer (130 Bytes) → z. B. `C:\DOS` |
| 22 | DosSelectDisk   | BX = Laufwerk (0 = A:)          |
| 23 | DosQCurDisk     | → AX = Laufwerk                 |
| 24 | DosCreate       | BX = Pfad → Handle (legt an oder kürzt auf 0) |
| 25 | DosWrite        | BX = Handle, CX = Puffer, DX = Länge → Bytes |
| 26 | DosDelete       | BX = Pfad                       |
| 27 | DosMkDir        | BX = Pfad                       |
| 28 | DosRmDir        | BX = Pfad (muss leer sein)      |
| 29 | DosMove         | BX = alter, CX = neuer Pfad (gleiches Laufwerk) |
| 30 | DosFreeSpace    | BX = Laufwerk, CX = Puffer (4 Bytes) → freie Bytes |
| 31 | DosShutdown     | BX = 0 anhalten, 1 Neustart (kehrt nicht zurück) |
| 32 | DosSetSigHandler | BX = Weiche (0 = abmelden), CX = `SIG_BREAK` \| `SIG_SHUTDOWN` – in C: `DosSetSigHandler(fn, maske)` |
| 33 | DosSigReturn    | Handler fertig (macht die Weiche in `fapi.h`) |
| 34 | DosGetEnv       | BX = Name, CX = Puffer, DX = Länge → Länge des Werts |
| 35 | DosSetEnv       | BX = `NAME=WERT` (leerer Wert löscht) |
| 36 | DosEnvEntry     | BX = Nummer, CX = Puffer, DX = Länge → n-ter Eintrag |
| 37 | DosGetDateTime  | BX = Puffer (4 Bytes): Datum, Uhrzeit im FAT-Format |
| 38 | DosDup2         | BX = Handle (FFFFh = Konsole), CX = Standard-Handle 0–2 |
| 39 | DosDup          | BX = Handle → neues Handle auf dasselbe |
| 40 | DosPipe         | BX = Puffer (2 Worte) → Lese-, Schreib-Handle |
| 41 | VioWrtStrAtt    | wie VioWrtStr, DX = Farbattribut |
| 42 | DosQHandType    | BX = Handle → 0 Datei, 1 Pipe, 2 Konsole, 3 Laufwerk |
| 43 | DosOpenDrive    | BX = Laufwerk, CX = 0 lesen / 1 exklusiv (Sperre) → Handle; Sektoren mit DosRead/DosWrite |
| 44 | DosSeek         | BX = Handle, CX = Zeiger auf Position (4 Bytes) |
| 47 | DosSetFInfo     | BX = Pfad, CX = struct fapi_finfo: Attribute und/oder Datum/Uhrzeit setzen |
| 48 | DosSetDateTime  | BX = Datum, Uhrzeit (FAT-Format) → Echtzeituhr stellen |
| 49 | DosLabel        | BX = Laufwerk, CX = Ausgabe (Bezeichnung, Seriennummer), DX = neue Bezeichnung oder 0 |
| 46 | DosQChildRC     | → Rückgabewert des zuletzt beendeten Kindes (255 = Ausnahme oder Strg+C) |
| 45 | DosQFSInfo      | BX = Laufwerk, CX = Puffer: Dateisystem-Name, Sektoren, Sektorgröße, Flags |
| 50 | VioSetMode      | BX = 1 Vollbild (alle Zeilen ohne Titel- und Statusleiste), 0 normal → Zeilenzahl; der normale Inhalt wird gesichert und am Ende wiederhergestellt |
| 51 | VioSetCurPos    | BX = Zeile << 8 \| Spalte (Zeile FFh = Cursor aus) |
| 52 | KbdShift        | → Umschalttasten jetzt: 1 Umschalt, 2 Strg, 4 Alt, 8 AltGr |
| 53 | VioSetLines     | BX = Bildschirmzeilen der eigenen Sitzung: 25, 43, 50 (nur VGA), 0 = fragen → Zeilen; löscht den Bildschirm, nicht im Vollbild |
| 54 | VioGetRows      | → Zeilen, die das Programm beschreiben kann (normal Bildschirm − 2, Vollbild alle) |
| 55 | DosShutdownHold | nach SIG_SHUTDOWN: Herunterfahren aufhalten, weil das Programm den Benutzer fragt; Sitzung kommt nach vorn, der Kernel wartet bis zum Programmende (sonst 5 s) |
| 56 | MouOpen         | → Anzahl Maustasten (0 = keine Maus); Ereignisse für die eigene Sitzung an, Zeiger zunächst aus |
| 57 | MouReadEvent    | BX = Puffer (Tasten, Zeile, Spalte), CX = 1 warten → 0 Ereignis, 1 keins; Zeile/Spalte als Textzelle der Sitzung, Zeile FFh = Titel-/Statusleiste |
| 58 | MouShowPtr      | BX = 1 Mauszeiger zeigen (invertierte Zelle), 0 verbergen |

Handles 0, 1, 2 sind Standardeingabe, -ausgabe und -fehler (Vorgabe:
Konsole), eigene Dateien beginnen bei 3. Gestartete Programme erben die
Standard-Handles; `DosCreate` mit CX = 1 hängt an (`>>`). `DosExecPgm` mit
CX = 2 startet in derselben Sitzung im Hintergrund (für Pipes). Pipes
puffern im Kernel 2 KB; der Schreiber wartet bei vollem, der Leser bei
leerem Puffer, ein Leser ohne Schreiber bekommt das Dateiende.

Signale wie bei OS/2: Ein Programm kann Strg+C (`SIG_BREAK`) und das
Herunterfahren (`SIG_SHUTDOWN`) abfangen. Der Kernel stellt sie beim
nächsten Rücksprung aus der FAPI zu; wartende Aufrufe wie `KbdCharIn`
kehren dafür vorzeitig zurück. `SHUTDOWN` gibt solchen Programmen bis zu
5 s zum Aufräumen und beendet dann alle Programme regulär. TASKA zeigt es.

Jedes Programm hat eine eigene Umgebung (1 KB) und erbt sie von dem
Programm, das es startet. Die Systemumgebung enthält `COMSPEC`, `PATH`
(`C:\DOS;C:\` mit dem Startlaufwerk) und alles, was die `CONFIG.SYS` mit `SET` festlegt.

Fehler sind Werte ab FFF0h: FFFFh nicht gefunden/unzulässig, FFFEh
schreibgeschützt, FFFDh voll, FFFCh existiert schon, FFFBh Zugriff
verweigert (Attribut, Verzeichnis nicht leer), FFFAh in Benutzung (offene
Datei, aktuelles Verzeichnis eines Programms).

## Gerätetreiber

Dateisystem-Module (`IFS=`) sind gleich aufgebaut, melden bei der
Initialisierung ihre Medienklassen und werden danach mit `struct ifs_req`
aufgerufen (`include/ifsmod.h`); Blöcke lesen sie über DevHlp `DH_BLKREAD`.

Befehle: `DRV_INIT`, `DRV_READ`, `DRV_WRITE` (je ein Block über den
DMA-Puffer) und `DRV_FLUSH` (Schreib-Cache der Platten leeren: IDE
`FLUSH CACHE`, SCSI `SYNCHRONIZE CACHE`).

Treiber sind FX-Dateien mit gesetztem Treiber-Flag und laufen in Ring 0 mit
eigenem Code- und Datensegment. Der Kernel ruft ihre Strategieroutine mit
einer Anforderung (`struct drv_req`) auf, Treiber erreichen den Kernel über
DevHlp – angelehnt an die Treiber von OS/2 1.x. Schnittstelle:
`include/drv.h`. Die Hardware-Erkennung liest immer erst: Der Adaptec-Treiber
erkennt die Karte am Bitmuster der Register, bevor er etwas schreibt, und
übergeht so etwa den MPU-401 einer Sound Blaster auf Port 330h.

DevHlp-Funktionen: `DH_PUTS` (Text ins Startprotokoll), `DH_TICKS`
(Systemtakt), `DH_DELAY` (warten), `DH_BLKREAD` (Block eines anderen
Geräts lesen, für IFS-Module), `DH_BIOSDISK` (Plattentabelle des BIOS hinter
INT 41h/46h, vom Kernel beim Start im Real Mode gesichert), `DH_SETIRQ`/
`DH_UNSETIRQ` (IRQ belegen: der Kernel ruft die Routine des Treibers mit
dessen DS und schickt danach das EOI), `DH_PROCBLOCK`/`DH_PROCRUN` (Task
schläft im Treiber bis zum Ereignis oder zur Frist, andere Tasks laufen
weiter; die Interruptroutine weckt ihn).

Treiber mit Interrupt: Während ein Task in einem Treiber schläft, hält er
die Kernelsperre – Dateisystem, Sektor-Cache, DMA-Puffer und Laufwerke
gehören in der Zeit ihm. Andere Programme rechnen weiter und dürfen
Bildschirm, Tastatur, Umgebung, Konsole und Pipes benutzen; ein
Dateizugriff wartet, bis die Sperre frei ist. `IDE.SYS` benutzt IRQ 14/15
(Abschalten mit `DEVICE=IDE.SYS /NOIRQ`); bleibt der Interrupt aus, obwohl
das Gerät fertig ist, fragt der Kanal nach drei Fehltreffern wieder ab.
`MOUSE.SYS` sucht eine PS/2-Maus (IRQ 12), dann eine serielle (Microsoft,
Logitech mit dritter Taste) an COM1/COM2 (`/PS2`, `/COM1`, `/COM2` legt fest) und meldet ihre Bewegungen
dem Kernel; Programme lesen sie über MouOpen/MouReadEvent, der Zeiger ist
eine invertierte Textzelle (`\DEMO\MAUSTEST.EXE` zeigt die Ereignisse).
`AHA154X.SYS` liest den IRQ aus der Konfiguration der Karte (Befehl 0Bh,
Anzeige beim Start) und schläft, bis die Karte die Eingangs-Mailbox
beschrieben hat; ebenfalls mit Rückfall und `/NOIRQ`. Beide Treiber melden
gescheiterte Versuche mit Status (IDE) bzw. Host-/Zielstatus (SCSI) in
Sitzung 1.

`IDE.SYS` und ältere Controller: Der WD1003 (MFM/RLL) hat die Register der
IDE-Platten, kennt aber kein IDENTIFY und bricht es ab. Dann nimmt der
Treiber die Geometrie aus der BIOS-Tabelle (nur primärer Kanal, Platte
80h/81h), stellt sie mit INITIALIZE DRIVE PARAMETERS (91h) ein und prüft
sie, indem er den ersten und den letzten Sektor der ersten Spur liest. Der
WD1007 (ESDI) liefert IDENTIFY mit der Rohgeometrie; das BIOS stellt oft
etwas weniger ein. Bei Platten ohne LBA und ohne gültige aktuelle Geometrie
(IDENTIFY-Wort 53) gilt deshalb die des BIOS, wenn sie in die Platte passt.

## Speicher

| linear           | Inhalt                          |
|------------------|---------------------------------|
| 08000h – 087FFh  | DMA-Puffer für Laufwerke (2 KB, ein CD-Block) |
| ab 10000h        | Kernel: Code (bis 64 KB), direkt dahinter Daten samt Stacks (bis 64 KB) |
| Kernelende – 9FFFFh | frei, Speicherverwaltung     |
| A0000h – FFFFFh  | Systembereich (Video, ROMs)     |
| ab 100000h       | frei, Speicherverwaltung        |

## Bauen

Benötigt NASM, mtools und Open Watcom 2.0 (`WATCOM` gesetzt, `wcc`/`wlink`
im PATH):

    ./build.sh      # -> build/dos5.img (1,44 MB, FAT12) und build/atdos-hd.img

`CONFIG.SYS` auf der Diskette lädt beide Plattentreiber; was keine Hardware
findet, wird wieder entladen.

Im Stammverzeichnis liegen nur `ATDOS.SYS`, `COMMAND.EXE`, `CONFIG.SYS`,
`AUTOEXEC.CMD` (und auf der Diskette `ATSYS.COM`). Wie bei OS/2 (`\OS2`,
`\OS2\BOOT`, `\OS2\DLL`) getrennt:

- `\DOS` – was man aufruft: Werkzeuge wie `LESS`, `GREP`, `XCOPY`, `CHKDSK`
- `\DOS\SYSTEM` – was das System nachlädt: Treiber (`.SYS`), Dateisysteme
  (`.IFS`), Prüfprogramme für CHKDSK (`UFAT.EXE`), Tastaturbelegungen (`.KBL`)
- `\DEMO` – die Testprogramme `TASKA`–`TASKC`

Treiber in `DEVICE=`/`IFS=` ohne Pfad sucht der Kernel im Stamm, in
`\DOS\SYSTEM` und (ältere Installationen) in `\DOS`; `KEYB` sucht die
`.KBL` genauso, CHKDSK sein `U*.EXE` zuerst fest in `\DOS\SYSTEM`.

Das Image ist eine normale FAT12-Diskette und lässt sich unter MS-DOS, mit
mtools oder unter jedem anderen System lesen. `ATDOS.SYS` ist versteckt
(`DIR /A` zeigt sie) und muss die erste Datei bleiben; beim Austauschen
also nicht einfach überschreiben, sondern das Image neu bauen.

In 86Box als Diskette A: einlegen, beliebige 286-Maschine mit VGA.

## Start von Festplatte

Der Bootsektor der Partition (`boot/hdboot.asm`) sucht `ATDOS.SYS` im
Stammverzeichnis und folgt der FAT-Kette – die Datei darf also zerstückelt
liegen. Gelesen wird per BIOS (CHS), deshalb muss `ATDOS.SYS` unterhalb von
Zylinder 1024 liegen.

Die Plattentreiber stehen in `CONFIG.SYS` auf eben der Platte, die sie erst
lesbar machen. Wie bei OS/2 liest der Kernel deshalb noch im Real Mode über
das BIOS `CONFIG.SYS` und alle `DEVICE=`-Dateien vor (höchstens 30 KB, aus dem
Stammverzeichnis, `\DOS\SYSTEM` oder `\DOS`). Danach erkennt er die Startpartition an Startsektor
und Seriennummer; sie wird immer `C:` (die übrigen Laufwerke rücken in ihrer Reihenfolge nach) und das erste Laufwerk der Programme. `A:` wird dann
erst beim ersten Zugriff eingehängt.

Zwei Wege auf die Platte:

- **Fertige Platte:** `build/atdos-hd.img` (im Release als
  `atdos-hd.zip`), 63 MB, Geometrie 130 Zylinder / 16 Köpfe / 63 Sektoren.
  In 86Box als IDE-Platte einhängen und im BIOS-Setup als Typ „User“ mit
  dieser Geometrie eintragen.
- **Vorhandene FAT16-Partition:** Unter MS-DOS von der AT-DOS-Diskette
  `ATSYS D:` aufrufen. Es kopiert alle Dateien der Diskette (auch die
  versteckte `ATDOS.SYS`) ins Stammverzeichnis, dazu `\DOS`, `\DOS\SYSTEM` und `\DEMO`, und ersetzt
  den Code im Bootsektor; der BPB bleibt. Laufwerke mit `IO.SYS`, `MSDOS.SYS` oder
  `IBMBIO.COM` lehnt es ab. Danach muss die Partition aktiv sein (FDISK) und
  die Platte die erste Festplatte des BIOS.

Achtung: `ATSYS` überschreibt eine vorhandene `CONFIG.SYS` im Stammverzeichnis.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
