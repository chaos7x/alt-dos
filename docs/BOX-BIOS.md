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
| 14h | Status „Sendepuffer leer“, Senden läuft ab | keine serielle Übertragung (Maus über INT 33h) |
| 15h | 4Fh, 86h, 88h (0), 90h/91h, 2400h–2403h, C0h | C1h (keine EBDA), C2xx (PS/2-Maus), E801h, E820h, 53xx (APM): Fehler wie ein BIOS ohne |
| 16h | 00–02, 05, 09, 0A, 10–12, 0306h | 03 (Rate setzen) ohne Wirkung |
| 17h | 00–02: Zeichen über LPTn des Kernels, Status 90h bzw. 30h ohne Port | Handle bleibt bis zum Ende der Box offen |
| 18h/19h | Box beenden | |
| 1Ah | 00–04, B1xx (PCI lesend) | 05 (Datum setzen) ohne Wirkung |
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
   (Bits 4–6) gegen die anderen Öffnungen der Box -> 20h, Bereichssperren
   110Ah je Datei (Überschneidung/nicht gesperrt 21h, Tabelle voll 24h),
   INT 2Fh 1000h meldet SHARE (BOXSHELL/RBSHELL), 10A0h–10A6h des
   FreeDOS-Kerns für lokale Dateien (nur das Startabbild) „erlaubt“.
   Je Box 12 (6.x) bzw. 16 (5.x) Dateien in der Tabelle, darüber ohne
   Freigabeprüfung wie vorher; 4 bzw. 8 Sperren. Kompatibilitätsmodus gilt
   innerhalb der Box als „nichts verbieten“ (eine Box ist für AT-DOS ein
   Task); zwischen Boxen und FAPI-Programmen gelten die Regeln von AT-DOS.
2. **Box-Laufwerke als Netzlaufwerke** (4408h/4409h/440Dh/6900h): DOS
   meldet sie als „fern“ (4409h Bit 12), Geräte-IOCTL scheitert mit 0Fh.
   Installationsprogramme, die nicht auf Netzlaufwerke wollen, oder
   Werkzeuge, die die Seriennummer per 6900h lesen, scheitern daran.
   Vorschlag: BOXSHELL beantwortet 4408h/4409h/6900h für AT-DOS-Laufwerke
   selbst wie für ein lokales Laufwerk, sobald ein Programm daran hängt.
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
