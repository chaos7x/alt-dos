# Grafiktreiber der Karte für Windows 3.1 in der Box (Plan)

Stand: 25.11.2026. Vorgemerkt vom Nutzer („Festhalten. VGA 17 Farben sind
langweilig“). Es gibt noch keinen Code. Der erste Test (siehe „Befund
25.11.2026“) zeigt: der Treiber der Trio3D/2X läuft nur im erweiterten
Modus – der Plan unten gilt so nur für Treiber, die den Standardmodus
können. Am Lader
(WINLDR, `docs/DOSKERN.md` „Stand Weg B“) arbeitet eine andere Sitzung.

## Ziel

Windows 3.1/3.11 läuft in der Box im Standardmodus auf unserem DPMI
(WINLDR statt DOSX). Heute bekommt es nur VGA.DRV mit 640×480 und 16
Farben. Künftig soll der Windows-Treiber des Kartenherstellers laufen,
zuerst der S3-Treiber für die S3 Trio3D/2X im P3B-F. Der Grundbau
(verlängerte I/O-Bitmap, zusätzliches Sichern) gilt für alle Karten. Je
Chip kommt nur ein Eintrag in eine kleine Tabelle dazu.

## Heute

- Die I/O-Bitmap im TSS deckt nur die Ports 0–3DFh ab (`kern.h:112`,
  `iob[126]`). Jeder Port ab 3E0h führt zu einer Falle, und `port_in`
  liefert dort FFh.
- Rohgrafik der Box: `vga_ports(t, on)` (`v86.c:3001`) gibt 3B0h–3DFh und
  1CEh–1D0h frei. Aufgerufen wird es aus `vga_enter` (`v86.c:3021`) und
  `vga_freeze` (`v86.c:3271`).
- Beim Wegschalten sichert `raw_out` (`vgagfx.c:702`) die
  Standardregister, die DAC und das Bild. In VESA-Modi sichert
  `raw_svout` (`vgagfx.c:507`) über VBE 4F04h und das Bild (in VDD.md als
  „VSVGA.SYS“ geführt).
- DPMI 0800h blendet physischen Speicher ab `mem_top_pg()` ein (LFB,
  MMIO), höchstens 4 Bereiche je Box (`NDPP`).
- Hinten wartet eine Box, die auf die Karte zugreift, bis sie wieder vorn
  ist (`vga_wait`).

Ein Chiptreiber scheitert heute an den Ports. Die 2D-Einheit der älteren
S3 (Trio64, Vision 864/964, 86C80x/928) liegt zum Beispiel bei x2E8h
(42E8h, 4AE8h, 82E8h–BEE8h, 9AE8h, E2E8h …), also weit über 3DFh. Der
Treiber liest dort nur FFh.

Die Trio3D/2X im P3B-F gehört dagegen zur ViRGE-Familie (86Box:
`vid_s3_virge.c`, nicht `vid_s3.c`). Sie hat keine Port-Einheit. Ihre
2D-Einheit erreicht man nur über MMIO, siehe die Tabelle. Für sie zählt
also vor allem, wie MMIO vorn durchgeht und hinten abgefangen wird. Die
lange Bitmap ist dort zweitrangig.

## Befund 25.11.2026: Trio3D/2X verlangt den erweiterten Modus

Getestet in 86Box P3B-F mit `trio3d2x_agp` und dem Treiber S3T3D2X.DRV
2.00.02 (dosdays.co.uk, nur im Scratchpad, nicht im Repository), SYSTEM.INI
wie OEMSETUP.INF für 800×600×256 (`screen-size=800`, `color-format=8`,
`fastmmio=on`). Gegenprobe VGA.DRV auf derselben Karte: Programm-Manager
kommt.

- **In der Box:** WIN kehrt nach etwa 30 s ohne Meldung zu CMD zurück.
  Mitgeschnitten (vorläufige Spur über DMESG): INT 10h 0F00h, 4F14h BL=03h
  und 02h (S3-OEM-Erweiterung, über das Video-BIOS), dann INT 2Fh 1600h,
  INT 10h 0003h und nach 29 s 0012h. Kein Zugriff auf Ports ab 3E0h, kein
  DPMI 0800h, keine VGA-Ports.
- **Unter echtem MS-DOS 6.22 + Windows 3.11 (`WIN /S`, DOSX.EXE) auf
  derselben VM** meldet der Treiber selbst: „To run Windows in Standard
  mode, run the Setup program and select the VGA display driver. The high
  resolution/high color modes will only operate in Windows Enhanced
  mode.“ Der Abbruch in der Box ist also dieselbe Prüfung (INT 2Fh 1600h
  meldet keinen erweiterten Modus) – kein Fehler der Box.
- **Versuch mit vorgetäuschtem erweitertem Modus** (1600h = 0A03h ab dem
  zweiten Aufruf, nur zum Messen, wieder entfernt): der Treiber prüft die
  Modi per 4F14h, fragt INT 2Fh 4010h/4000h, holt mit 1684h BX=000Ah den
  Einsprung des VDD (VDDT3D2X.386) und 1683h, stellt 4F02h BX=8103h ein,
  4F06h, INT 10h 7F00h – und beendet Windows. Er braucht die private
  Schnittstelle seines VxD. Die ließe sich nur nachbauen, wenn man den VxD
  zerlegt; das schließt dieser Plan aus (Lizenz).

Folgen:

- Für die Trio3D/2X führt dieser Weg nicht zum Ziel, solange Windows in
  der Box im Standardmodus läuft. Den erweiterten Modus (WIN386 als
  eigener VMM unter unserem Kernel) gibt es nicht.
- Der Grundbau (lange Bitmap, Tabelle je Chip) lohnt nur für Karten, deren
  Win-3.1-Treiber den Standardmodus kann. Das wird je Karte zuerst unter
  echtem MS-DOS mit `WIN /S` geprüft (billig, eine VM), bevor Code
  entsteht. Kandidaten aus 86Box: ältere S3 (Trio64/Vision 864), Cirrus
  54xx, ET4000/W32, ATI Mach.
- VBESVGA.DRV läuft laut seiner Beschreibung auch im Standardmodus (es
  bessert dafür einen Fehler in DOSX.EXE aus – in der Box ersetzt WINLDR
  DOSX, das ist zu prüfen). Das Binary war hier nicht erreichbar (GitHub-
  Releases über den Proxy gesperrt, Bauen braucht das Win16-DDK); den Test
  macht der Nutzer.

Testweg für die nächste Karte: VM `p3m` im Scratchpad (P3B-F, MS-DOS 6.22
von Diskette, Windows-Verzeichnis aus der AT-DOS-Installation, `WIN /S`
mit HIMEM.SYS aus C:\WINDOWS), dann dieselbe Platte unter AT-DOS (`p3s`).

## Verlängerte I/O-Bitmap (Weg a, gilt für alle Karten)

- Die Bitmap wird je Box auf alle 64K Ports verlängert (8 KB + 1 Byte FFh
  am Ende). Grenze und `iobase` im TSS passen sich an. Das TSS einer Box
  passt dann nicht mehr in DGROUP. Es kommt in eine eigene Seite je Box,
  oder nur die Bitmap kommt dorthin und das TSS bleibt, wenn die CPU-
  Struktur das zulässt (beim Bau prüfen).
- Vorgabe bleibt: alles gesperrt. Freigegeben wird nur in Rohgrafik vorn,
  nach der Chiptabelle.
- Weg b (Durchreichen in `port_in`/`port_out`) wurde verworfen: jede
  Falle kostet zu viel für eine 2D-Einheit mit Tausenden Zugriffen je
  Bild.
- Speicherfenster (MMIO, LFB) laufen schon über DPMI 0800h. Nötig ist dort
  nur, dass hinten nichts auf die Karte durchgeht (bisher: Seiten auf die
  Sicherung oder „nicht vorhanden“, `vga_svmap`).

## Tabelle je Chip

Ein Eintrag je Chip:

- **Kennung:** wie `chip_probe` in vgagfx.c. Zuerst wird im Video-BIOS
  gelesen, dann werden Register geprüft, und geschrieben wird erst nach
  dem Lesen.
- **Freizugebende Ports:** als Liste von Bereichen.
- **Zusätzlicher Sicherungsteil:** was 4F04h nicht oder nicht verlässlich
  abdeckt.
- **„Leerlauf abwarten“:** die 2D-Einheit muss fertig sein, bevor
  gesichert wird.

| Chip | Ports | zusätzlich sichern |
|---|---|---|
| S3 alt (Trio64/Vision 864/964, 86C80x/928) | x2E8h-Gruppe der 2D-Einheit (42E8h–E2E8h, jeweils 2 Byte), dazu die x148h-Gruppe (8148h–A548h, 4148h, 4948h) und 46E8h, 3B0h–3DFh wie heute | CR30h–CR6Fh, SR08h–SR1Fh, Hardware-Mauszeiger (Bild + Lage), Einheit leer (9AE8h Bit 9) |
| S3 ViRGE/Trio3D/2X (86C325/375/362/368) | 3B0h–3DFh wie heute, dazu 3A0h–3AFh (nur bei Misc-Ausgang Bit 0 = 0) und E8h/E9h (E2h/E3h bei CR6F Bit 1, aus bei CR6F Bit 2). 2D-Einheit nur MMIO: alt bei A0000h (64 KB, B8000h mit CR53 Bit 5), an über CR53 Bit 4; neu bei LFB + 16 MB (64 KB), an über CR53 Bit 3 – kommt über DPMI 0800h | CR30h–CR6Fh (darin CR53, CR6F), SR08h–SR1Fh, Streams-Einheit, Mauszeiger, FIFO leer vor dem Sichern |
| Cirrus 54xx (später) | meist nur 3C4h/3CEh/3D4h (schon frei), BitBLT über GRxx | SR06h–SR1Fh, GR09h–GR3Fh, CR19h–CR1Fh |
| Tseng ET4000/W32 (später) | 3BFh, 3CBh, 3CDh, 3D8h, 217Ah/217Bh (W32) | Segmentregister, ATC 16h, CRTC 32h–3Fh, W32-Beschleuniger über MMIO |
| ATI Mach (später) | x2EEh-Gruppe, 1CEh/1CFh | erweiterte Register über 1CEh |
| Matrox (später) | MMIO über BAR (DPMI 0800h) | CRTCEXT, Beschleuniger-Zustand |

Die späteren Zeilen sind nur Anhaltspunkte. Ports und Register werden beim
Bau aus Datenbuch und Treiberquellen genau festgelegt.

Karten, die Windows nur über VBE und den LFB betreibt, brauchen keinen
Eintrag (Treiber wie VBESVGA.DRV, siehe unten). Für sie reicht der
heutige Stand mit 4F04h und dem Bild.

## Sichern beim Wegschalten

Ablauf in `raw_out` und `raw_in`, wenn ein Chip-Eintrag gilt:

1. Warten, bis die 2D-Einheit leer ist (mit Frist).
2. Den heutigen Weg ausführen (4F04h bzw. Standardregister, DAC, Bild,
   bei LFB das ganze Bild).
3. Die erweiterten Register aus dem Eintrag in den Puffer schreiben,
   hinter RAW_STATE.
4. Zurück in umgekehrter Folge: Modus, Bild, 4F04h, dann die erweiterten
   Register, zuletzt der Mauszeiger.

Hinten bleibt die Box eingefroren (`vga_wait`) wie bei DOOM in Mode Y.
Eine 2D-Einheit hinten nachzubilden lohnt sich nicht.

## 86Box als Vorlage und Testweg je Chip

Alle Chips der Tabelle bildet 86Box nach (Hinweis des Nutzers). Für jeden
Eintrag gilt daher:

- **Vorlage:** die Emulationsquellen in `src/video/vid_*.c`, also
  `vid_s3.c`, `vid_s3_virge.c`, `vid_cl54xx.c`, `vid_et4000w32.c`,
  `vid_ati_mach64.c` und `vid_mga.c`. Daraus kommen die Ports und
  Register für die Tabelle, zusammen mit den Datenbüchern. Sie zeigen,
  welche Ports der Chip dekodiert und welche Register den Zustand
  tragen.
- **Testweg:** 86Box mit genau dieser Karte und dem Windows-Treiber des
  Herstellers.

Hinweise aus dem Git-Log von 86Box (24.11.2026 durchgesehen). Sie
zeigen, wo die Windows-3.1-Treiber der Hersteller empfindlich sind:

- **S3:** c4038f48f (FIFO neu, damit sie nicht hängt; behebt Bildfehler
  mit den Win-3.1-Treibern für 86x/96x), e37f477eb (Doppelwortbetrieb
  über CR14 Bit 6 statt CR31 Bit 3, Win 3.1 und Duke3D), c0891644f
  (SVGA/MMIO unter Win 3.1/NT), 68a5dbd65 (BT485-Farbtiefe für die
  Vision964-Treiber), 52317cd23 (CR39 entsperrt auch das ID-Register,
  NT-3.1-Treiber).
- **Trio3D/2X und ViRGE:** aa0e21c41 (2X-Modi), 22ba8b32c (8-Bit-DAC),
  9bcf3c0c9 (Gammakorrektur), 47ee1c4ea (Streams-Einheit), c9c903f6b
  (Interrupt beim senkrechten Rücklauf). Einen Test mit dem
  Win-3.1-Treiber der Trio3D nennt das Log nicht – das bleibt unser
  Test.
- **Cirrus:** 80a0c4878 (Speicherfenster, die Adressbits übergehen;
  behebt Hänger des Win-3.1x-Treibers), 0a8c142c3 und 7701174b3
  (Fenster mit Bytetausch, Schrift-Cache des Win-3.1-Treibers).
- **ATI:** 4cf8f629a (Mach64: kleines Fenster nur bei CONFIG_CNTL
  Bit 2, behebt den VGA-Treiber von Win 3.1), 524fd30c0 und d17d13e3b
  (Mach8/32: Ports 4AE8h, 5AEEh, 46EEh, 22E8h, 4AEEh; Win-3.1-Treiber
  der ATI Ultra).
- **Matrox:** 48718eb16 (harte Hänger mit DynaView 3D unter Win 3.x
  außer bei 16 Bit), 46840b239 (Wechsel der Auflösung unter Win 3.1).
- **Tseng:** 7b4a6a338 (ET4000AX, Prüfung des 8-Bit-Modus behebt den
  Treiber von Windows 3.0).

Lizenz, geprüft am 24.11.2026 an `master`: das README von 86Box nennt
„GNU General Public License, version 2 or later“, und `COPYING` enthält
den Text der GPL v2. Die Dateien oben tragen nur Copyright-Zeilen und
keinen eigenen Lizenzvermerk. Das passt zu AT-DOS (GPL-2.0-or-later).
Für den Plan genügt Lesen: Register und Ports sind Fakten über die
Hardware. Wird doch Code übernommen, kommt die Herkunft (Datei, 86Box,
GPL-2.0-or-later) in den Kopf der Datei, wie in CLAUDE.md verlangt.
Vorher wird der Kopf der Datei erneut geprüft. Dabei gilt die Warnung
aus CLAUDE.md: der Emulator ist nachsichtiger als echte Hardware, die
Datenbücher bleiben die erste Quelle.

## Prüfung

- 86Box P3B-F mit S3 Trio3D/2X (86C368). Treiber „Win 3.1 v2.00.02“
  (Engineering Release 1999, dosdays.co.uk). Für den 86C365 gibt es
  v2.00.18. Der Nutzer besorgt ihn und installiert ihn selbst, er kommt
  nicht ins Repository.
- Die Zugriffe des Treibers werden zuerst mit der Spur über Port E9h bzw.
  WIN-DMESG mitgeschnitten: welche Ports, welches MMIO. Dann wird die
  Liste im Eintrag festgelegt.
- Geprüft werden:
  - Start in 800×600/1024×768 mit 256 und mehr Farben;
  - Fenster ziehen (BitBLT);
  - Mauszeiger;
  - Wechsel mit Alt+Esc zu CMD und zurück, auch mitten im Zeichnen;
  - Ende von Windows, danach ein sauberer Textmodus.
- Gegenprobe: VGA.DRV unverändert, DOOM, VBETEST/BANKT/SVBG.

## Rückfall: VBESVGA.DRV

VBESVGA.DRV (github.com/PluMGMK/vbesvga.drv) ist ein allgemeiner VBE-
Treiber für Windows 3.1 mit 8–32 Bit, der auch im Standardmodus läuft. Er
stammt vom DDK-Beispiel V7VGA ab und hat keine Lizenz. Deshalb liefern wir
ihn nicht mit und übernehmen keinen Code daraus. Der Nutzer kann ihn
selbst installieren. Er braucht nur VBE und den LFB, also nichts aus der
Tabelle.

## Lizenz

Code nur als Vorlage, und nur mit passender Lizenz:

- **xf86-video-s3virge (MIT):** Nachschlagewerk für Register und die
  2D-Einheit der S3.
- **Linux `s3fb`:** GPLv2-only, also nur lesen.
- **Treiber von Microsoft und S3:** nicht zerlegen. Geleakten Code nicht
  verwenden.

## Aufwand (Schätzung)

| Teil | Aufwand |
|---|---|
| Bitmap verlängern (TSS-Lage, Grenze, Freigabe nach Tabelle) | 1–2 Tage |
| S3-Eintrag (Spur, Ports, Sicherung, Leerlauf) | 2–3 Tage |
| Prüfung in 86Box | 1 Tag |
| je weiterer Chip | 1–2 Tage |

## Offen

- TSS mit langer Bitmap: Lage im Speicher, Kosten von 8 KB je Box auf dem
  386 mit 4 MB. Möglich wäre auch, die lange Bitmap nur für Boxen mit
  Chip-Eintrag anzulegen.
- Hardware-Mauszeiger der S3, solange CMD vorn ist: muss aus sein.
- Trio3D hinten: das alte MMIO-Fenster bei A0000h ist im Rohbetrieb
  heute schon die Karte (`vga_map`). Hinten muss es mit CR53 Bit 4 als
  MMIO gelten, nicht als Bild – sonst schreibt die Box in die Sicherung
  statt zu warten. Dasselbe gilt für das neue Fenster hinter dem LFB.
- Ob der S3-Treiber im Standardmodus etwas von VxDs erwartet (VDD für
  Enhanced Mode). Das zeigt erst der Test.
