<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Plan: Grafische Oberfläche und Win16 für AT-DOS

Stand: 15.11.2026, abgestimmt mit dem Nutzer. Nur Plan – noch kein Code.

## Entscheidungen

- **Win16 nativ** ist die Schnittstelle der GUI. Alte Windows-3.x-Programme
  laufen direkt auf AT-DOS, ohne echtes Windows 3.1 (auch nicht in einer
  DOS-Box). Win16-Programme sind 16-Bit-Protected-Mode-Programme im
  NE-Format und passen zum Kernel (LDT je Task, Selektoren, 286-Code).
- **Die ganze Win16-Schicht läuft auch unter AT-DOS 5.x**: 286-Code,
  kooperativ, ohne Paging/V86. Auf 5.x bleibt „Programme bleiben 286“.
- **Eigener Prozess**: Die GUI wird bei Bedarf gestartet (`GUI` aus CMD,
  wie `startx`), läuft in einer eigenen Grafiksitzung, Alt+Esc führt zu
  CMD und den Boxen. Beendet sich die GUI, beendet sie ihre Programme.
  Startet man ein Win16-Programm aus CMD, startet die GUI mit.
- **Kein GUI-Code im Kernel** (DGROUP ist voll). Der Kernel liefert nur
  die Grundlagen (NE-Lader, DLLs, IPC).

## Aufbau

- Wie PM 1.x und Windows 3.x, nicht wie X: kein Protokoll je Zeichen-
  aufruf (auf dem 386 zu teuer). Der GUI-Prozess besitzt Bildschirm,
  Fensterliste und Eingabe; USER und GDI sind DLLs mit gemeinsamen Daten,
  jedes Programm zeichnet unter einer Sperre selbst in den Bildspeicher.
- Grafiktreiber als DLL mit IOPL-Segment im Ring 2 (wie OS/2 1.x) – geht
  auf 286 und 386 gleich, Programme bleiben im Ring 3.
- Beim Sitzungswechsel wird das Bild nicht gesichert, die GUI zeichnet
  alle Fenster neu (480 KB bei 800×600×256 wären auf 2 MB zu viel).

### Kernel-Grundlagen (Stufe 0, für 5.x und 6.0)

1. NE-Lader mit DLLs: gemeinsamer Code, gemeinsame Datensegmente,
   Importe über Ordinalzahl/Namen. Open Watcom linkt NE direkt
   (`format windows`, `format os2`) – auch für die eigenen GUI-Programme.
2. IPC: Semaphoren, gemeinsamer Speicher; Threads später.

### Regeln für die Win16-Schicht (damit Win32 später thunken kann)

- Kern von USER/GDI getrennt von der Win16-API-Schicht (dünne Schicht
  davor, eine Win32-Schicht kommt später daneben).
- Handles sind Indizes in Tabellen (16 Bit), keine Zeiger – später 1:1
  als 32-Bit-Handles.
- Internes MSG mit 32-Bit wParam/lParam, Win16 kürzt beim Weitergeben.
- Fensterprozeduren mit Typkennung (16/32 Bit), wie Wine `winproc.c`.
- Strukturen mit festen Feldbreiten (WORD/DWORD), nie `int`.
- Eigene Treiberschnittstelle (DDI) für Grafik, unabhängig von GDI16,
  auch für ein späteres GDI32.
- Texte intern in der Codepage, W-Aufrufe später per Umwandlung.

## Reihenfolge

1. Kernel: NE-Lader, DLLs, IPC (5.x und 6.0).
2. GDI-Kern mit VGA 640×480×16, Schriften, Maus, Clipping.
3. USER: Fenster, Nachrichten, Fokus, Steuerelemente (Knopf, Eingabe-
   feld, Liste, Bildlaufleiste, Menü, Dialog), Ressourcen.
4. Shell: Programme (Gruppen), Dateien (Baum + Liste), Fensterliste,
   Desktop.
5. Weitere Grafiktreiber (s. u.).

### Zielprogramme (aus der eigenen Windows-Kopie des Nutzers, nie ins Repository)

1. **Notepad** – NE-Lader, KERNEL, USER (Fenster, Menü, Tastenkürzel,
   EDIT, MessageBox), COMMDLG; GDI nur Text in fester Schrift.
2. **Write** – Schriften (Raster, dann TrueType), Absatzformate,
   Bildlaufleisten, Drucken (erster Druckertreiber), OLE 1 (OLECLI).
3. **Office 4.3** (Word 6, Excel 5, PowerPoint 4) zuletzt: OLE 2 (die
   OLE2-DLLs bringt Office mit), DDE, TrueType, Druckertreiber, VBA/OLE-
   Automation, COMMDLG, SHELL, TOOLHELP, WinHelp.

## Grafiktreiber

| Karte | Maschine (86Box) | Treiber | Version |
|---|---|---|---|
| VGA | alle | 640×480×16, Grundtreiber | 5.x + 6.0 |
| ET4000/W32i (W32p PCI) | SCAT, 386SX (486) | gebankt bis 800×600×256 (evtl. 1024×768), Bank über 3CDh/3CBh, Blitter, Hardware-Zeiger | 5.x + 6.0 |
| Cirrus GD5434 ISA | 386DX | gebankt (GR9), BitBLT über den Speicher, Hardware-Zeiger | 5.x + 6.0 |
| S3 Trio3D (Trio3D/2X AGP) | ab PCI, P3B-F | linearer Bildspeicher, 2D-Engine, Hardware-Zeiger | 6.0 |

Auf 5.x gibt es heute kein VESA: Bankumschaltung geht direkt über die
Chip-Register, der Moduswechsel braucht einen Ausflug in den Real Mode
(Weg der Kompatibilitätsbox, `rbox.c`/`start.asm`: CMOS-Shutdown 0Ah
beim 286, PE löschen beim 386) – nur beim Start und Sitzungswechsel.

## Look

Eigener Entwurf im gemeinsamen CUA-Look von PM 1.x und Windows 3.x:
Größenrahmen mit Eckmarken, Titelleiste blau (aktiv) / weiß (inaktiv)
mit zentriertem Titel, Systemmenü-Knopf links, Minimieren/Maximieren als
Pfeilknöpfe rechts, Menüleiste mit unterstrichenen Buchstaben, MDI-
Gruppenfenster, Fensterliste auf Strg+Esc, minimierte Programme als
Symbole. Bedienung wie damals (CUA). **Eigen** sind alle Symbole,
Bitmaps, die Systemschrift und die Namen („Programme“, „Dateien“,
„Fensterliste“) – nichts von Microsoft oder IBM übernommen.

![Programme und Dateien](gui/entwurf-cua.png)
![Fensterliste](gui/entwurf-fensterliste.png)

### Desktop und Eigene Dateien

- Gibt es `C:\Desktop`, zeigt die GUI dessen Inhalt (Ordner, Dateien,
  Verknüpfungen) als Symbole; Ziehen aus dem Dateimanager legt dort ab.
  Fehlt der Ordner, bleibt der Desktop wie bei 3.x ohne Dateien.
- `C:\Eigene Dateien`: Startordner der Datei-Dialoge.
- Lange Namen: eigene GUI-Programme bekommen den LFN-Merker; Win16-
  Programme sehen wie alle Programme ohne Merker die 8.3-Namen
  (`DESKTOP`, `EIGENE~1`). Offen: langer Name beim Speichern über eine
  Zwischendatei (löschen + umbenennen) erhalten.
- Orte über Variablen, siehe [SYSTEM-DATEN.md](SYSTEM-DATEN.md).

![Desktop mit Dateien](gui/entwurf-desktop.png)

## Wine als Vorlage

- Heutiges Wine: die 16-Bit-Teile sind nur Thunks auf USER32/GDI32
  (32 Bit flach) – für 16 Bit nicht nutzbar.
- Frühes Wine (1993–96) war ein Win16-Emulator: `windows/` ~18.700
  Zeilen, `controls/` ~11.200, `objects/` (GDI) ~10.800 (wine-960616).
  `if1632/*.spec` = Exporttabellen (USER 514, GDI 374, KERNEL 278,
  dazu COMMDLG, SHELL, TOOLHELP, OLE) – wertvoll für den NE-Lader.
  GDI hängt an X11 (~290 Aufrufe) und wird eigener Code; Steuerelemente
  und Fenster-/Nachrichten-/Dialoglogik lassen sich gut übernehmen.
  Alles muss für Watcom 16 Bit umgebaut werden.
- **Lizenz**: bis 24.04.2000 eigene Wine-Lizenz mit Freistellungsklausel
  (vermutlich GPL-unverträglich), 24.04.2000–10.03.2002 X11 (verträglich),
  danach LGPL. Stände von 1996 nur lesen; übernommen wird aus
  wine-20000430 … wine-20020228, Herkunft je Datei vermerken.

## Später: Win32

1. Optional ein eigenes „Win32s“ über Win16 nativ (nur 6.x, nicht 5.x):
   PE-Lader, 32-Bit-Codesegmente in der LDT, Thunks nach USER/GDI.
   Bricht dort „Programme bleiben 286“ – vorher abstimmen.
2. AT-DOS 7.0 mit 32-Bit-Kernel: Win32 als Kern (USER32/GDI32 aus Wine,
   eigener Grafiktreiber statt x11drv), Win16 darüber wie WOW. ReactOS
   nur als Vorbild für die NT-Struktur.

## Nebenbei gewünscht

Werkzeug im Stil von sysedit/regedit für die DOS-Kompatibilität:
zuerst im Textmodus (wie FM/MSD), Oberfläche über `DOSBOX.INI`
(`[*]`, `[NAME.EXT]`, Erklärung und Wertebereich je Schlüssel) und die
Startdateien; keine Registry. Später als Win16-Programm. Noch nicht
zugesagt.

## Offene Entwurfsfragen (Durchsicht 15.11.2026)

Vor Stufe 0 klären, noch nicht entschieden:

1. **Gemeinsamer Adressraum.** Win16 setzt eine LDT für alle Programme
   voraus (GMEM_SHARE, DDE, Zwischenablage, OLE, Datensegmente von
   USER/GDI; Office 4.3 baut darauf). AT-DOS gibt jedem Task eine eigene
   LDT. Bewährter Weg wie WOW (NT) und WIN-OS/2: alle Win16-Programme im
   GUI-Prozess als Tasks mit **einer gemeinsamen LDT**, untereinander
   kooperativ unter einer Sperre (Win16Mutex) – auch für die USER-Logik
   (Nachrichtenschlangen, Fensterliste), nicht nur fürs Zeichnen –,
   gegenüber CMD und Boxen präemptiv. Braucht im Kernel „Tasks mit
   gemeinsamer LDT“ (im Kern Threads) früher als geplant.
2. **5.x hat die große LDT nicht** (512 Einträge nur auf `main`); ein
   Win16-Programm braucht schnell Hunderte Selektoren. Win16 auf 5.x
   heißt: erst die große LDT zurückportieren (eigener Schritt in
   Stufe 0). Auf 6.0 bekommen Rechner unter 3,5 MB nur 64 Einträge
   (`nldt`) – für Win16-Tasks wieder mehr.
3. **Ring 2 für Grafiktreiber** (IOPL-Segment wie OS/2 1.x): Stapel für
   Ring 2 in jedem TSS, IOPL 2 je Task, Code-Selektoren mit DPL 2. Auf
   6.0 verdrängt `irq0` nur Ring 3 – die Prüfung muss „Ring 2 oder 3“
   werden, sonst läuft Treibercode wie Kernelcode ununterbrochen.
4. **Wine als Vorlage**: die Stände mit passender Lizenz (2000–2002)
   sind überwiegend Win32 mit 16-Bit-Thunks. Übernehmbar vor allem die
   Logik von Steuerelementen und Dialogen (umgebaut auf Watcom 16 Bit)
   und die `.spec`-Exporttabellen; sonst mit eigenem Code rechnen.
5. **Platz im Kernel**: NE-Lader nach KTEXT2 (erstes Codesegment ~1 KB
   frei), Tabellen nicht in DGROUP; die GDT-Plätze für Kernelsegmente
   (NKSEG 20) sind fast vergeben – mit Punkt 1 gehören die Segmente der
   DLLs in die gemeinsame LDT.
6. **Sperren**: auch ohne `klock` (BIGLOCK.md, 6.3) bleibt der Win16Mutex
   nötig – er sitzt im GUI-Prozess, nicht im Kernel.
7. Strg+Esc für die Fensterliste ist im Kernel frei (Alt+Esc und
   Strg+Umschalt+Esc belegt).
