<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Plan: Segmente auslagern auf dem 286 (5.4)

Stand: 15.11.2026, Baustein vom Nutzer freigegeben (nur Doku, noch kein
Code). Geplant für AT-DOS 5.4 neben DEVLOAD, Zweig `5.x`; die Regeln von
5.x gelten (286-Befehle, kooperativ). Siehe auch
[LOOP-DBLSPACE.md](LOOP-DBLSPACE.md) (Extent-Weg) und
[BIGLOCK.md](BIGLOCK.md).

## Kurzfassung

Speicher wird knapp -> Segmente eines wartenden Programms kommen aus dem
Speicher, ihr Deskriptor bekommt P = 0. Greift das Programm wieder darauf
zu, löst der 286 „Segment fehlt“ (#NP, Ausnahme 11; beim Stapelsegment
#SS, Ausnahme 12) aus, der Kernel holt das Segment zurück und der Befehl
läuft neu an – der 286 kann das ohne Hilfe, der Fehlercode nennt den
Selektor. Heute (5.x und 6.0) wird nichts ausgelagert; 6.0 hat Paging,
lagert aber auch nicht aus (DPMI 0702h/0703h ohne Wirkung).

Vorbilder: OS/2 1.x (`SWAPPATH=`, `MEMMAN=SWAP,MOVE`, `SWAPPER.DAT`) und
Windows 3.x im Standardmodus (Code-Segmente verwerfbar, aus der EXE neu
geladen; LRU über die Segmente). Nur das Verhalten als Vorlage, kein Code.

## Bausteine

1. **Handler für #NP/#SS** (start.asm, Ausnahme 11/12): aus Ring 3 ->
   `seg_in(sel)` (Speicher holen, Inhalt laden, Basis + P = 1 eintragen,
   Zugriff merken), dann IRET – der Befehl wiederholt sich. Fehlt der
   Speicher auch nach dem Auslagern anderer: Programm beenden wie heute
   bei einer Ausnahme („zu wenig Speicher“). Aus Ring 0 darf #NP nicht
   vorkommen (siehe Regeln).
2. **Verwerfbare Code-Segmente zuerst** (Stufe 1, braucht keine
   Auslagerungsdatei): FX-Programme haben getrennten Code (CGROUP) ohne
   Segment-Relokationen (`mkfx.py` bricht sonst ab) – der Code steht
   byte­gleich in der EXE. Verwerfen = Speicher frei, P = 0; zurück =
   neu aus der Datei ab `image_off`. Die EXE bleibt dafür offen (Handle
   im Kernel, `in_use` – Überschreiben/Löschen während des Laufs wird
   abgelehnt, wie bei OS/2). Geteilter Code (`path_key`, ein Code für
   alle Instanzen): in allen LDTs, die ihn benutzen, zugleich P = 0.
3. **Auslagerungsdatei über den Extent-Weg** (Stufe 2, Daten): DGROUP
   und DosAllocSeg-Segmente sind beschrieben, sie müssen in eine Datei.
   Wie Loop-Backend 1: Datei beim Start anlegen und offen halten
   (`SWAPPATH=C:\DOS\SYSTEM 512` – Verzeichnis, Mindestfrei in KB wie
   OS/2; Datei `SWAPPER.DAT`, feste Größe `SWAPSIZE=`), Extent-Liste
   einmal über `ifs_bmap` (fat.c, seit Loop Stufe 1) in die Stücktabelle
   wie bei kernel/loop.c, danach Lesen/Schreiben direkt per Sektor über
   den Plattentreiber, am Platten-Cache vorbei (ausgelagerte Daten
   gehören nicht in den Cache, der Platz soll ja frei werden) – kein
   Dateisystemaufruf im Seitenfehler
   (verschachtelte IFS-Operation, siehe LOOP-DBLSPACE.md). Belegung in
   Plätzen zu 1 KB (Bitmap im Kernelsegment), ein Segment belegt
   aufeinanderfolgende Plätze, wenn möglich. Diskette und Wechselplatte
   als Ort ablehnen.
4. **Einfache Auswahl** (Stufe 1 und 2): Zugriffsbit des Deskriptors
   (Byte 5 Bit 0, setzt die CPU beim Laden des Selektors). Uhr-Verfahren:
   reihum Segmente ansehen, Bit gesetzt -> löschen und weiter, Bit leer
   -> Kandidat. Vorrang: verwerfbarer Code vor Daten, Tasks im Hinter-
   grund bzw. wartend (Tastatur, EXEC) vor dem Vordergrund, große vor
   kleinen. Angestoßen von `mem_get`, wenn sonst kein Speicher da ist
   (wie heute das Freigeben geparkter Boxen und `mem_reclaim` des
   Caches – Cache zuerst, dann Code, dann Daten).

## Regeln

- **Nie ausgelagert:** Kernel, Treiber, IFS-Module, Kernelsegmente
  (`kseg_alloc`), DMA-Puffer, Sitzungspuffer und deren Aliase
  (`D_ALIAS`, VioGetBuf/GetPhysBuf), Segmente eines Tasks, der gerade im
  Kernel steht (`in_op`), die Kompatibilitätsbox (5.1/5.2, 0–640 KB
  gehören der Box).
- **Kernel fasst Programmspeicher nur, wenn er da ist:** überall, wo der
  Kernel über `desc_base` oder SCRATCH in ein Programmsegment greift
  (FAPI-Puffer, far_copy, seg_free, DosReallocSeg, Signale, Rahmen),
  vorher `seg_in`. Einfachste Form: `fapi_dispatch` holt beim Eintritt
  DS/ES/SS des Aufrufers und jeden Selektor eines Zeigerarguments herein
  und lagert für Tasks in einer Operation nichts aus. Lehre aus 6.0:
  sonst #NP mitten im Kernel mit gehaltener Sperre.
- Auslagern und Zurückholen laufen unter der Kernelsperre; die Platten-
  aufträge gehen in den Speicher des Segments selbst (`buf_phys`), nicht
  über DMABUF.
- Alles abschaltbar: ohne `SWAPPATH=` keine Auslagerungsdatei,
  `MEMMAN=NOSWAP` auch kein Verwerfen von Code (wie OS/2).

## Stufen

0. Voraussetzung: `ifs_bmap` und die Stücktabelle aus 6.1 (Loop
   Stufe 1, kernel/loop.c) nach `5.x` zurückportieren.
1. #NP/#SS-Handler, `seg_in`, verwerfbare Code-Segmente, Auswahl über das
   Zugriffsbit. Test in QEMU mit 1 MB: mehrere große Programme
   gleichzeitig (EDIT, FM, GREP -r), Wechsel zwischen den Sitzungen;
   danach auf dem SCAT (286, echte Segmentprüfung).
2. Auslagerungsdatei für Daten, `SWAPPATH=`, MEM zeigt „ausgelagert“.
   Test: Strom weg mit ausgelagerten Segmenten (Datei wird beim Start
   neu angelegt, Dateisystem bleibt stimmig), fsck sauber.
3. Später, wenn nötig: Segmente verschieben gegen Zerstückelung
   (`MEMMAN=MOVE`, Basis im Deskriptor umtragen – Selektoren bleiben),
   Auslagerungsdatei wachsen lassen.

## 6.x und 7.0

Auf dem 386 (6.x) könnte derselbe Weg mitlaufen (Segmente liegen in
Seiten); Auslagern je Seite gehört zu 7.0 (BIGLOCK.md: „Extents nur für
die Auslagerungsdatei“). Die Auslagerungsdatei und ihr Extent-Weg sind
in allen drei Fällen dieselben.

## Offene Punkte

- Wie groß darf ein verwerfbarer Code werden, ohne dass das Neuladen auf
  dem 286 stört (64 KB von der Platte ≈ Bruchteil einer Sekunde)?
- Geteilter Code und mehrere Instanzen: Zugriffsbit aus allen LDTs
  zusammenführen.
- Gebundene Programme unter MS-DOS (Stub) betrifft das nicht.
