# BIOS und DOS in der Box – Bestandsliste und Vergleichstest

Stand 18.11.2026. Anlass: Lotus Works 3.0 und Word 5.5 scheiterten an
Stellen, an denen die Box anders antwortete als echtes DOS/BIOS. Diese
Liste hält fest, was die Box selbst beantwortet, was ans echte BIOS geht
und wo sie bewusst anders ist – damit Programme der Zeit nicht wieder
über dieselben Dinge stolpern.

## Vergleichstest BIOSCMP

`tools/bioscmp.asm` -> `\DEMO\BIOSCMP.COM` (nur auf der Platte), 8086-Code:
ruft rund 180 BIOS- und DOS-Dienste so auf, wie Programme der Zeit es
tun, und schreibt Register, Flags und Puffer zeilenweise in eine Datei
(`BIOSCMP Ausgabe [Laufwerk:]`; auf dem Laufwerk wird `\BCT` angelegt
und wieder entfernt). Werte, die sich auch zwischen zwei echten Rechnern
unterscheiden (Uhrzeit, Segmente, Speichergröße), gibt es nicht aus.

`tools/bioscmp.sh [Verzeichnis] [rbox]` lässt es in QEMU zweimal laufen:
unter FreeDOS direkt (Bezug: echtes BIOS, FreeDOS auf eigener FAT16) und
in der Box (6.x: DOS-Box über `command /c`; 5.x mit `rbox`: Kompatibili-
tätsbox, eingetippt über den QEMU-Monitor), danach `diff`. In 86Box von
Hand: Platte mit einem MBR, der den Startsektor der Diskette lädt
(BIOS startet sonst von C:), FreeDOS-Diskette mit `a:\bioscmp c:\ref.txt
c:` in AUTOEXEC.BAT; danach normal starten, in der Box dasselbe.

Unter echtem MS-DOS 6.22 lässt sich BIOSCMP genauso starten – das wäre
der dritte Bezug (FreeDOS selbst weicht an Stellen von MS-DOS ab).

Geprüft (18.11.2026): QEMU (SeaBIOS) 6.x und 5.x, 86Box 386DX/40
(AMI-BIOS, Cirrus GD5434) 6.x, 86Box SCAT 286 (5.x, Kompatibilitätsbox im
echten Real Mode).

## Bezug MS-DOS 6.22 (vom Nutzer, 18.11.2026)

`docs/bioscmp/msdos622.txt` (ohne SHARE) und `msdos622-share.txt` (mit
SHARE), MS-DOS 6.22 ohne EMM386 in 86Box (P3B-F), Testlaufwerk C: FAT16.
Für einen Vergleich mit der Box nur den DOS-Teil ab Zeile 67 nehmen
(BIOS und Maschine sind andere). Was MS-DOS anders macht als FreeDOS
(und damit als die Box):

| Fall | MS-DOS 6.22 | FreeDOS/Box | Folge |
|---|---|---|---|
| Freigabekonflikt beim Öffnen (SHARE) | 05h | Box gab 20h | **übernommen**: 05h |
| FCB-Suche 11h mit normalem FCB | findet keine versteckten Dateien | FreeDOS fand sie (fcbfns.c: `wAttr = D_ALL`) | **behoben** im FreeDOS-Kern der Boxen (`freedos/patches/fcb-search.patch`) |
| 59h nach „Pfad fehlt“ | Klasse 08h, Aktion 03h, Ort 02h | 0/0/0 | FreeDOS, unkritisch |
| 5Ah Zwischendatei | 9 Zeichen Name | 8 | unkritisch |
| 3300h BREAK | aus | an (Box-CONFIG) | Einstellung |
| 4408h/4409h/440Dh/6900h/440Ah | lokales Laufwerk | Box war Netzlaufwerk | **übernommen**: lokal |
| 2F 1400h NLSFUNC | nicht geladen | FreeDOS eingebaut | unkritisch |

Mit SHARE stimmt die Box sonst überein: Größe über zwei Handles, Sperren
(0/21h), INT 2Fh 1000h = FFh. Bei MS-DOS ruft ein Konflikt im Kompati-
bilitätsmodus INT 24h auf („Unzulässige SHARE-Operation …“) – die Box
nicht.

## Gefunden und behoben

Redirector (6.x `kernel/redir.c`, 5.x `apps/rbox_redir.c`):

| Fall | vorher | jetzt (wie DOS) |
|---|---|---|
| Datum/Uhrzeit per 5701h, dann Schließen | ging verloren (Datum des Schließens) | bleibt (SFT-Merker 4000h -> beim Schließen eingetragen) |
| Anlegen mit Attribut (3Ch/5Bh/6Ch, CX) | Attribut fiel weg | bleibt (versteckt/System/R) |
| 5Bh auf vorhandene Datei | überschrieb sie | Fehler 50h |
| Öffnen/Löschen/Attribut, Verzeichnis fehlt | 02h | 03h |
| FINDFIRST, Verzeichnis fehlt | 12h | 03h |
| MKDIR auf Vorhandenes, RENAME auf Vorhandenes | 50h | 05h |
| RMDIR des aktuellen Verzeichnisses | 05h/20h | 10h (CDS über SDA+282h) |

Dazu schon vorher an diesem Tag: Suchzustand im DTA (verschachtelte
Suchen), FINDFIRST ohne Treffer 12h, Namen als 8+3-Felder, Clusterzahl
bei 36h – siehe CLAUDE.md (Lotus Works, Word 5.5).

BIOS der DOS-Box (6.x `kernel/v86.c`):

| Dienst | vorher | jetzt |
|---|---|---|
| INT 1Eh (Diskettenparameter) | zeigte auf Stummel-Code | F000:0800 |
| Tabelle bei F000:0800 | ein Byte verschoben (Sektorgröße fehlte) | AF 02 25 02 12 1B FF 6C F6 0F 08 |
| INT 1Dh/1Fh/43h | Stummel | Zeiger des echten BIOS |
| INT 10h 1007h/1008h/1015h/1017h/101Ah | fehlten | Textpalette, Attribut-, Randregister |
| INT 10h 1Ch (Zustand sichern) | fehlte | BIOS-Daten (128 Byte) |
| INT 10h 12h BL=10h | CL fehlte | CL = 9 (Schalter) |
| INT 10h 1Bh, statische Tabelle | Byte 8/9 vertauscht | wie die echten Video-BIOSe (02 08) |
| INT 13h 01h | Fehler | letzter Status (BDA 441h) |
| INT 14h Status | fehlte (Register unverändert) | „Sendepuffer leer“, Senden läuft ab |
| INT 17h | Ausdrucke gingen verloren | Zeichen über das LPT-Gerät des Kernels (`lpt_out`), Status 90h |
| INT 15h C0h | fehlte | Tabelle F000:0890 (AT, Modell FCh) |
| INT 15h 90h/91h | CF = 1 | CF = 0 |
| INT 15h 2400h–2403h | fehlten | A20 wie über XMS |
| INT 16h 09h/0Ah/0306h | fehlten | Fähigkeiten 30h, MF2-Kennung, Rate |

## CheckIt Pro 1.11 in der Box (19.11.2026)

Diagnoseprogramm von 1993 (vom Nutzer, nur zum Testen, nicht im
Repository), prüft die Hardware über Ports, CMOS und Privilegbefehle –
gut, um die Nachbildung auszureizen. Bezug: FreeDOS direkt auf derselben
Maschine (QEMU, 86Box 386DX/40). Behoben:

| Fund | vorher | jetzt |
|---|---|---|
| Diskette ohne 55AAh im Bootsektor (Kopierprogramm „DUPLICAT“) | Laufwerk leer (fat.c verlangte 55AAh) | wie MS-DOS: Sprungbefehl EBh/E9h genügt (beide Linien) |
| `MOV EAX,CR0` in V86 (Prozessorerkennung) | #GP, Box abgebrochen | MOV r32,CR0/CR2/CR3/DRn liefert wie EMM386 Werte (CR0 80000011h), Schreiben wirkt nicht |
| INVD/WBINVD/CLTS (Cache-Test) | #GP | ohne Wirkung |
| Schreiben auf F000:0000 (RAM-Probe im oberen Speicher) | #PF, Box abgebrochen | verpufft wie beim ROM (`rom_skip`: Befehl decodiert und übersprungen; Stummelseite bleibt heil) |
| Parallelport-Register 378h–37Ah | FFh („0P“) | Datenlatch, Status DFh, Steuerregister; Strobe druckt über LPTn des Kernels |
| COM-Register (8250) | FFh („1 Baud“) | Teiler/LCR/MCR/IER/SCR behalten den Wert, Loopback, Senden über COMn; INT 14h 01h sendet |
| CMOS schreiben (Weckregister 01h/03h/05h, Status B, ab 0Eh) | verworfen („Clock: None“, „DIP Switches“, „XT“) | Kopie je Box, Uhr läuft weiter |
| INT 1Ah 06h/07h (Weckzeit) | fehlte („Alarm Function FAILED“) | Weckzeit je Box, INT 4Ah zur Zeit |
| zweiter PIC (A0h/A1h) | FFh („IRQ controller #2 failed“) | Maske A1h, ICW-Folgen an 20h/A0h verstellen keine Masken |
| 0417h/0418h bei jeder Zustellung neu geschrieben | Speichertest: „Bad Bits“ dort | nur noch bei Änderung des Tastaturzustands |
| geparkte Box zählt ihren Takt nicht | DOS-Zeit 17 s hinter der Uhr | nach langer Pause 46Ch aus der Echtzeituhr neu |

Zweiter Durchgang (19.11.2026, Nachfrage des Nutzers: auch die Menüs
Memory, DOS, Windows, Performance; dazu sein MS-DOS-6.22-Startabbild als
Bezug auf derselben 86Box-386 – als 1,2-MB-Kopie, weil das CMOS der VM
A: als 5¼" führt und IO.SYS sonst hängt):

| Fund | MS-DOS 6.22 | Box vorher | jetzt |
|---|---|---|---|
| COMSPEC (DOS Details, Umgebung) | A:\COMMAND.COM | fehlte (BOXSHELL startet Programme direkt) | X:\DOS\MDOS\COMMAND.COM |
| INT 15h C0h, ID-Bytes | FC, 01 (IBM AT 339) | FC, 00 (feste Tabelle) | Modell/Untermodell/Revision aus der Tabelle des BIOS (F000:E6F5), ohne EBDA- und 4Fh-Merkmal |
| LPT bidirektional | Yes | No | Steuerbit 5 = Eingabe: Daten lesen FFh |
| COM nach dem Start | 2400 Baud | 9600 | 2400, 8N1 |
| Ctrl-Break | Off | On | BREAK=OFF in der CONFIG.SYS der Box |
| MSCDEX 1501h | – | Nullzeiger als Treiberkopf | Kopf „MSCD001“ (C800h) bei F000:0AE0 |

Dritter Durchgang (19.11.2026): zweite Diskette des Nutzers mit ASPI4DOS/
ASPICD/MSCDEX (CONFIG.SYS: `DEVICE=A:\ASPI4DOS.SYS /P334` – der Adaptec
der VM liegt auf 334h –, `DEVICE=A:\ASPICD.SYS /D:ASPICD0`; AUTOEXEC.BAT:
`MSCDEX /D:ASPICD0 /L:E`; COMMAND.COM der Diskette war nicht 6.22, auf der
Kopie ersetzt), im CMOS der VM A: auf 1,44 MB und das Koprozessor-Bit
gesetzt. Prüfprogramm (CDS, IOCTL, 1500h/150Bh/1501h, 36h) unter MS-DOS
und in der Box:

| | MSCDEX 2.23 | Box vorher | jetzt |
|---|---|---|---|
| CDS-Pfad / Flags / Offset | `\\E.\A.` / C080h / 7 | `E:\` / C000h / 2 | wie MSCDEX (BOXSHELL `cds_sync` fragt den Kernel per 150Bh) |
| 4408h / 4409h | Fehler 1 / 1000h | gleich | gleich |
| 1501h | Kopf „ASPICD0“, Laufwerk 05, 1 Einheit | Nullzeiger | Kopf „MSCD001“, Laufwerk, Einheiten |
| 36h (frei) | 0 | Platz der Platte (Laufwerk falsch gelesen) | 0, Größe aus dem PVD |

Die CheckIt-Laufwerksliste zeigt das CD auch unter MS-DOS als „Network“
– der vermeintliche Fehler ist das Originalverhalten. Redirector: Namen
`\\X.\A.\…` werden in `get_fn` zu `X:\…`, 0Ch nimmt den Laufwerksbuch-
staben aus Stelle 2 des CDS-Pfads. Koprozessor: auch mit gesetztem CMOS-
Bit FAILED unter MS-DOS (das AMI-BIOS findet den 387 von 86Box selbst
nicht) – 86Box. Geprüft in QEMU und 86Box 386DX/40 (CD am Adaptec: CD
wechseln, Programm starten, TYPE, COPY, DIR), BIOSCMP unverändert.

Unverändert und gleich wie unter MS-DOS: Koprozessor FAILED in 86Box
(also 86Box), Uhrabstand ~2 s, doppelte Listeneinträge nach Bild↓ (Eigenheit
von CheckIt). Das CD-ROM als „Network“ ist wie unter MS-DOS (siehe dritter
Durchgang). NumLock (vorher in der Box immer aus) behoben: der Kernel
übernimmt den Startzustand des BIOS (0040:0017 Bit 5), die Num-Taste schaltet,
die Box sieht NumLock/Feststelltaste in 0417h. Performance: CPU-Benchmark in
86Box 11.467 Einheiten (32,5 × XT); in QEMU läuft er nie durch (FreeDOS
direkt hängt dort, die Box beendet SYSINFO) – QEMU-Eigenheit. Platten-
Benchmark: „nicht vorhanden“ (keine BIOS-Platten), Video läuft.

Ergebnis danach: SYSINFO (alle Seiten), Systemplatine (CPU, PIC, Uhr,
Weckzeit, DMA), serieller und Parallelport, Drucker (Ausgabe kommt an),
Speicher (konventionell, EMS, Video), Video bestehen in QEMU und 86Box.
Erwartet/übersprungen: „Protected Mode“ (in V86 nicht möglich),
erweiterter Speicher (gehört XMS, wie unter HIMEM), Festplatte (keine
BIOS-Platten in der Box), Diskette 0 = 360-KB-Startabbild. Nicht der Box
zuzurechnen: „Interrupt Bug“ nur in QEMU (TCG lässt nach `POP SS` mit TF
den Einzelschritt sofort zu; in 86Box bestanden), „Numerical Processor
FAILED“ nur in 86Box (387-Nachbildung; der Test hängt dort unter FreeDOS
direkt sogar), FreeDOS direkt bricht ohne `STACKS=` mit „Stack overflow“
ab – die Box nicht (Stapel des Monitors).

## MS-DOS als DOS der Box (19.11.2026)

Wunsch des Nutzers (wie „OS/2 für Windows“): die eigene MS-DOS-Kopie statt
FreeDOS in der Box. `MKDOSIMG` (apps/task_mkdosimg.c) baut das Startabbild,
`DOS_IMAGE=` in DOSBOX.INI wählt es (kernel.c `ini_noframe` liest es mit,
`load_mdos`; eine geparkte Box wird nur wiederverwendet, wenn sie dasselbe
Abbild hat – `box_same`). BOXSHELL fragt das DOS (INT 21h 30h, BH = FDh
FreeDOS) und meldet es mit der SDA (INT E0h 11h, BL); INT E0h 22h liefert
den Namen des Abbilds (Unterverzeichnis der Werkzeuge im PATH).
Abweichungen von FreeDOS, die der Redirector (redir.c, `ms_dos[t]`) jetzt
beachtet – gefunden mit MS-DOS 6.22 des Nutzers (Disketten und ein
vollständiges C:\DOS; nur zum Testen, nicht im Repository):

- 1105h (CHDIR) und andere bekommen Pfade mit `\` am Ende (XCOPY: „Pfad
  nicht gefunden“) – `get_fn` schneidet ihn ab.
- REN mit Platzhaltern geht an den Redirector (FreeDOS löst sie selbst
  auf) – `ren_wild` nach den Regeln von MS-DOS.
- FCB-Dateien: DOS setzt sf_mode Bit 15 vor dem Öffnen (bleibt stehen),
  prüft danach sf_PID (SFT+31h), das es für Netzdateien nicht selbst
  einträgt.
- 6Ch: der Aktionscode gehört in CX des Aufrufers (MS-DOS liest ihn vom
  Stapel: user_SP SDA+264h, user_SS SDA+266h, CX bei +4).
- Die Shell (SHELL=BOXSHELL) bekommt unter MS-DOS keine Umgebung (PSP:2Ch
  = 0); BOXSHELL baut sie selbst und setzt COMSPEC/PATH.
- XMS: CMOS 30h/31h, XMS 08h und die 4309h-Tabelle melden während eines
  Programms dieselbe Größe (Merker `xms_snap` beim ersten CMOS-Lesen), auch
  wenn der Platten-Cache derweil Speicher abgibt.

Offen: MEM von MS-DOS zeigt auf knappen Maschinen (86Box 386DX/40, 4 MB)
bei „Erweiterung (XMS)“ eine Gesamtgröße unter der freien (Verwendet
„4.194.2“). Kernelseitig stimmen CMOS, LoL+45h, XMS 08h und die Tabelle
überein; die Gesamtgröße von MEM ändert sich mit dem oberen Speicher (mit
EMS-Rahmen 1.904 K bei 76 K UMB, ohne 1.776 K bei 140 K UMB), nicht mit
CMOS. Mit 16 MB und in QEMU mit 4 MB stimmt die Anzeige. Nur Anzeige –
Programme bekommen ihren XMS-Speicher.

## Bestand: BIOS der DOS-Box (6.x)

Im Textmodus beantwortet die Box die Video-Aufrufe selbst; schaltet ein
Programm auf Grafik (oder VESA), geht jedes INT 10h ans echte Video-BIOS
(Rohbetrieb, siehe CLAUDE.md „Virtuelle VGA“).

| INT | nachgebildet | Bemerkung |
|---|---|---|
| 08h/1Ch | Takt, 46Ch, virtueller PIT | schneller Takt für Spiele |
| 09h | Tastatur, Puffer 41Eh, Port 60h | INT 15h/4Fh ruft der Stummel nicht |
| 10h | 00–03, 05–0A, 0E, 0F, 1003, 1007, 1008, 1015, 1017, 101A, 11 (30), 12 (10), 13, 1A, 1B, 1C; 4Fxx über das Video-BIOS | 04 (Lichtgriffel), 0B, 0C/0D (Punkte, nur Grafik), 1010/1012 im Text ohne Wirkung |
| 11h, 12h | aus dem BDA | ein Diskettenlaufwerk, 640 KB bzw. RMSIZE |
| 13h | Laufwerk 0 = Startabbild (nur lesen) | keine Festplatten, B: fehlt |
| 14h | Status „Sendepuffer leer“, 01h sendet über COMn | kein Empfang (Maus über INT 33h) |
| 15h | 4Fh, 86h, 88h (0), 90h/91h, 2400h–2403h, C0h | C1h (keine EBDA), C2xx (PS/2-Maus), E801h, E820h, 53xx (APM): Fehler wie ein BIOS ohne |
| 16h | 00–02, 05, 09, 0A, 10–12, 0306h | 03 (Rate setzen) ohne Wirkung |
| 17h | 00–02: Zeichen über LPTn des Kernels, Status 90h bzw. 30h ohne Port | Handle bleibt bis zum Ende der Box offen |
| 18h/19h | Box beenden | |
| 1Ah | 00–04, 06/07 (Weckzeit, INT 4Ah), B1xx (PCI lesend) | 05 (Datum setzen) ohne Wirkung |
| 1Eh | Diskettenparameter F000:0800 | |
| 33h | Maus (Kernel) | |
| 67h | EMS 4.0 (Kernel) | |
| 70h–77h | über die Stummel | |

Bewusste Abweichungen vom echten BIOS (im Vergleich erwartet): keine
Festplatten (75h = 0, INT 13h 80h Fehler), A: ist das 360-KB-Startabbild,
INT 15h 88h = 0 (XMS statt erweitertem Speicher wie unter HIMEM), Maus,
EMS und XMS sind da, INT 2Fh 1100h meldet den Redirector.

Kompatibilitätsbox (5.x): echtes BIOS im Real Mode. Eigen sind nur
INT 13h (Startabbild), INT 15h (88h/87h/89h/E801h/E820h abgelehnt, 4Fh
für Alt+Esc und die Tastaturbelegung von RBSHELL – ein direkter Aufruf
mit einem Tastencode legt wie KEYB das Zeichen in den Puffer) und INT 8.

## Bestand: Redirector (INT 2Fh 11xxh)

| Unterfunktion | 6.x/5.x |
|---|---|
| 01 RMDIR, 03 MKDIR, 05 CHDIR | ja |
| 06 CLOSE (mit Datum/Attribut), 07 COMMIT | ja |
| 08 READ, 09 WRITE (0 Byte = Größe setzen) | ja |
| 0A LOCK/UNLOCK (BL), SHARE | ja, je Datei der Box |
| 0C freier Platz (echte Clusterzahl) | ja |
| 0E/0F Attribut setzen/holen | ja |
| 11 RENAME, 13 DELETE (Platzhalter) | ja |
| 16 OPEN, 17 CREATE (auch 5Bh), 2E erweitert (6Ch) | ja |
| 1B/1C FINDFIRST/NEXT (Zustand im DTA) | ja |
| 21 SEEK vom Ende | ja |
| 1D/22 Prozessende | „erledigt“ |
| 23/25/2D (Qualify, Spool, Extended Attributes) | nicht – DOS macht es selbst |

## Offene Punkte – Entscheidung des Nutzers

1. ~~Dieselbe Datei zweimal zum Schreiben öffnen~~ – erledigt mit SHARE
   im Redirector (18.11.2026, Wunsch des Nutzers „SHARE als Stub“): öffnet
   die Box eine Datei noch einmal, teilen sich beide SFTs den Handle von
   AT-DOS (Stelle je SFT, Größe nur einmal – DOS ohne SHARE meldet dort die
   alte Größe, BIOSCMP „SHARE Größe“: Bezug 200, Box 250). Freigabemodi
   (Bits 4–6) gegen die anderen Öffnungen der Box -> 05h (wie MS-DOS), Bereichssperren
   110Ah je Datei (Überschneidung/nicht gesperrt 21h, Tabelle voll 24h),
   INT 2Fh 1000h meldet SHARE (BOXSHELL/RBSHELL), 10A0h–10A6h des
   FreeDOS-Kerns für lokale Dateien (nur das Startabbild) „erlaubt“.
   Je Box 12 (6.x) bzw. 16 (5.x) Dateien in der Tabelle, darüber ohne
   Freigabeprüfung wie vorher; 4 bzw. 8 Sperren. Kompatibilitätsmodus gilt
   innerhalb der Box als „nichts verbieten“ (eine Box ist für AT-DOS ein
   Task); zwischen Boxen und FAPI-Programmen gelten die Regeln von AT-DOS.
2. ~~Box-Laufwerke als Netzlaufwerke~~ – erledigt (18.11.2026,
   Entscheidung des Nutzers „LW als lokal melden“): BOXSHELL/RBSHELL
   fangen INT 21h 4408h, 4409h, 440Ah (Handle), 440Dh 0860h/0866h/0846h,
   440Eh/440Fh und 6900h/6901h für AT-DOS-Laufwerke ab, bevor FreeDOS sie
   als Netzlaufwerk ablehnt; die Werte (fest/Wechsel, Aufteilung,
   Seriennummer, Bezeichnung, FAT12/16/32, Medienbyte) holt der Kernel
   (6.x INT E0h 21h -> redir.c `redir_dinfo`, 5.x INT E0h 18h -> RBOX
   `dinfo`). 4409h meldet 08C2h wie MS-DOS, 440Ah löscht Bit 15, Setzen
   der Seriennummer (0846h/6901h) 05h. CD-ROMs und leere Laufwerke bleiben
   Netzlaufwerke (MSCDEX-Art). INT 2Fh 1100h meldet den Redirector weiter
   (ohne ihn ginge nichts).
3. ~~Drucken aus der Box~~ – erledigt (18.11.2026): INT 17h 00h geht
   über das LPT-Gerät des Kernels (FAPI OPEN/WRITE auf `LPTn` im Task
   der Box). Geprüft in QEMU (`-parallel file:`) und 86Box (386DX/40,
   „Generic Text Printer“ an LPT1: `echo … > prn` und `copy datei prn`
   aus der Box landen in `printer\*.txt` des VM-Ordners). Zum Testen ohne
   Drucker in 86Box: LPT1 = Generic Text Printer (Textdateien), Generic
   ESC/P 2 Dot-Matrix (Seiten als Bilder) oder Generic PostScript
   Printer (PDF, braucht Ghostscript auf dem Rechner). Die Kompatibili-
   tätsbox (5.x) druckt über das echte BIOS direkt auf den Port.

Platz: `redir.c` liegt seitdem in KTEXT4 (ruft nur ferne Funktionen),
KTEXT3 (v86.c) hat wieder rund 6,8 KB frei.
