# Grafiktreiber der Karte für Windows 3.1 in der Box (Plan)

Stand: 24.11.2026. Vorgemerkt vom Nutzer („Festhalten. VGA 17 Farben sind
langweilig“). Das ist nur ein Plan, es gibt noch keinen Code. Am Lader
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

Ein Chiptreiber scheitert heute an den Ports. Die 2D-Einheit der S3 liegt
zum Beispiel bei x2E8h (42E8h, 4AE8h, 82E8h–BEE8h, 9AE8h, E2E8h …), also
weit über 3DFh. Der Treiber liest dort nur FFh.

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
| S3 (Trio64/Trio3D/ViRGE, 86C3xx) | x2E8h-Gruppe der 2D-Einheit (42E8h–E2E8h, jeweils 2 Byte), 3B0h–3DFh wie heute | CR30h–CR6Fh, SR08h–SR1Fh, Hardware-Mauszeiger (Bild + Lage), Einheit leer (9AE8h Bit 9) |
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
- Ob der S3-Treiber im Standardmodus etwas von VxDs erwartet (VDD für
  Enhanced Mode). Das zeigt erst der Test.
