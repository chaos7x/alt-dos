<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Plan: Loop-Laufwerke und Plattenkomprimierung (6.1)

Stand: 15.11.2026, Konzept mit dem Nutzer besprochen. Stufe 1 (Loop
lesend) gebaut, siehe CLAUDE.md; der Rest ist Plan. Siehe auch [BIGLOCK.md](BIGLOCK.md).

## Kurzfassung

Zuerst ein **Loop-Laufwerk im Kernel**: eine Datei wird Blockgerät. Erste
Umsetzung über eine beim Einhängen ermittelte Liste ihrer Plattenbereiche
(Extents), die direkt auf Sektoren des Trägerlaufwerks abbildet – ohne
verschachtelte Dateisystemaufrufe. Damit gehen ISO-Dateien (über
ISO9660.IFS, unter AT-DOS selbst ohne MSCDEX), Disketten- und
Plattenabbilder, feste VHDs aus 86Box. Darauf setzt die Komprimierung als
Übersetzer-Treiber **DBLSPACE.SYS** im DriveSpace-6.x-Format auf; das
innere FAT bedient weiter `fat.c`.

## Loop-Laufwerk

Warum nicht einfach per `ifs_read` aus der Datei lesen: ein Loop-Auftrag
käme mitten aus einer IFS-Operation. Dann kollidieren die globalen
Variablen in fat.c (dir_entry-Zeiger in DMABUF, VFAT-Sammler, `ferr`,
`de_lba`, EA-Puffer), DMABUF und FAT-Cache je Task (ein Platz) und die
Regel „höchstens eine Laufwerkssperre je Task“ (abgeben = anderer Task
kommt ins halb geänderte Laufwerk, behalten = Verklemmung). Lösbar nur
mit Wiedereintrittsfestigkeit bzw. Verschachtelungstiefe 2 – siehe
BIGLOCK.md.

So machen es andere (aus dem Gedächtnis): DoubleSpace/DriveSpace liest
den Container über die Sektoren der Trägerplatte (Container zusammen-
hängend); Linux-Swapdateien über `bmap`; Linux-Loop und NT-VHD gehen
durchs Dateisystem (wiedereintrittsfester Kernel + eigener Faden je
Gerät).

**Schnittstelle mit austauschbarem Backend** (Funktionszeiger lesen/
schreiben). LOOP-Befehl, FAPI, `LOOP=` und DBLSPACE.SYS kennen nur
„Blockgerät aus Datei“, nie das Verfahren:

- Backend 1 (6.1): Extents.
- Backend 2 (6.1, nach Verschachtelungstiefe 2): durchs Dateisystem –
  zerstückelte Dateien ohne Grenze, später Netzlaufwerke.
- 7.0: durchs Dateisystem als Regel, Extents nur für die Auslagerungsdatei.

**Extent-Weg:**

1. Einhängen: Datei offen halten (lesend: Schreiber gesperrt; schreibend
   `OPEN_RW` = `in_use`). Löschen, DEFRAG, CONVERT, CHKDSK /F können die
   Kette dann nicht verschieben (sperren nur ohne offene Dateien).
2. Neuer IFS-Aufruf `bmap` (letzter Eintrag in `struct ifs_driver`/
   `ifsmod.h`): Folge (Startsektor, Anzahl). fat.c läuft die Kette einmal
   ab, ISO9660.IFS hat genau einen Bereich.
3. Tabelle im Kernelsegment, 8 Byte je Extent, 64 KB = 8.192 Extents;
   mehr -> „Datei zu stark zerstückelt – DEFRAG oder kopieren“.
4. Suche binär über die aufsummierten Startblöcke, letzter Treffer gemerkt.

**Einbau:**

- `blk[].drv = DRV_LOOP` (-2, wie -1 Diskette), Verzweigung in
  `dev_read`/`blk_write`. Code in KTEXT2 (erstes Segment hat ~1 KB frei).
- Blockgröße: ISO 2048 (dann hängt `cd_drives()` ISO9660.IFS an), sonst
  512; ein 2048er-Block = 4 Trägersektoren, auch über Extent-Grenzen.
- Nur der Träger wird gecacht, das Loop-Gerät nicht.
- Sperren: Loop nimmt nur die Treibersperre des Trägers (`DLB`), nie
  dessen Laufwerkssperre; Reihenfolge Laufwerk(Loop) -> Treiber(Träger).
- Neue Hilfe `blk_read_to/blk_write_from(dev, lba, n, phys, sel, off)`
  mit eigenem Zielpuffer statt DMABUF (wie `bc_wb`), auch als DevHlp
  (heute nur `DH_BLKREAD` nach DMABUF).
- Aushängen/SHUTDOWN: Loop vor dem Träger leeren; Träger mit Loop ist
  nicht entlad-/auswerfbar (offene Datei).
- Platz: `NBLK` 8, `blk[]` in DGROUP (voll). Vorschlag NBLK 16, `blk[]`
  mit `blk_nm/blk_chg/blk_dirty` und Loop-Zustand in ein Kernelsegment;
  höchstens 4 Loops.

## Abbilder und Bedienung

| Art | Erkennung | Blockgröße | Schreiben |
| --- | --- | --- | --- |
| `.ISO` | Endung, „CD001“ bei 8000h | 2048 | nein |
| Diskette `.IMG`/`.IMA` | BPB in Sektor 0 | 512 | Stufe 2 |
| Platte `.IMG` | MBR 55AAh ohne BPB (mount_disk je Partition) | 512 | Stufe 2 |
| feste VHD | „conectix“ in den letzten 512 Byte | 512 | Stufe 2 |
| dynamische VHD | „conectix“ am Anfang | – | später (Übersetzer wie DBLSPACE) |

- `LOOP E: C:\ISO\SPIEL.ISO [/RO]`, `LOOP` listet, `LOOP E: /D` hängt
  aus (abgelehnt bei offenen Dateien/aktuellem Verzeichnis).
- FAPI 59h `DosLoop` (ATTACH/DETACH/INFO), mit Kernelsperre;
  LINUX.MAC: `mount`/`umount`.
- CONFIG.SYS `LOOP=E:,C:\ISO\SPIEL.ISO`. `MOUNT=` aus SYSTEM-DATEN.md
  hängt in Ordner und darf später eine Datei als Quelle nehmen.
- Kein Start von einem Loop-Laufwerk.
- Grenzen: Datei bis 4 GB, Träger FAT12/16/32 oder CD; Loop auf Loop ja,
  auf Netzlaufwerk erst mit Backend 2.

**MSCDEX in der DOS-Box** (eigene Stufe, für ISOs unter AT-DOS nicht
nötig): Die Box sieht CDs nur als Redirector-Laufwerke, INT 2Fh 15xxh
beantwortet niemand. DOTT stört das nicht; Spiele mit CD-Prüfung oder
CD-Audio brauchen 1500h, 150Bh, 150Ch, 150Dh, 1508h (Rohlesen über den
Kernel), später 1510h. Gilt für echte und Loop-CDs. CD-Audio bräuchte
BIN/CUE, ISO hat keine Audiospuren.

## Komprimiertes Laufwerk

Format von **DoubleSpace 6.0/6.2 und DriveSpace 6.22** lesen und
schreiben, aufgebaut wie DBLSPACE.BIN: Blockgerät, das unkomprimierte
Sektoren eines inneren FAT-Laufwerks vortäuscht. Eigenes Containerformat
lohnt nicht; eigene Komprimierung gehört ins Dateisystem (je Datei, 7.0).

| | DoubleSpace/DriveSpace 6.x | DriveSpace 3 | eigenes Format |
| --- | --- | --- | --- |
| Austausch | MS-DOS 6.0–6.22, Win 95 | Win 95/98 | keiner |
| Container | bis 512 MB | bis 2 GB | frei |
| Cluster innen | 8 KB | bis 32 KB | frei |
| Verfahren | „DS“/„JM“ (laut dmsdos) | dazu „SQ“ | flate.c/lzh.c |

Lizenz: Format nachbauen = Interoperabilität. Kein Microsoft-Code (MIT-
Freigabe nur 1.25/2.0/4.0; 6.x-Lecks nicht verwenden). dmsdos (Linux,
GPL – Fassung je Datei prüfen) nur als Nachschlagewerk, Herkunft
vermerken. Stac-/Microsoft-Patente von ca. 1990–95 sind abgelaufen.

```
fat.c (inneres FAT12/16)
  |  Blockgerät X:, 512, virtuelle Sektoren
DBLSPACE.SYS – MDFAT, BitFAT, Kompression, Cluster-Cache
  |  blk_read_to/blk_write_from
Loop-Gerät über C:\DBLSPACE.000
  |
Träger C: – IDE/NCR/AIC
```

- Gewöhnlicher Treiber (DEVICE= oder DEVLOAD). Sektor -> Cluster über
  den inneren BPB, MDFAT-Eintrag (Lage, Längen, Merker), entpacken in
  einen Cluster-Puffer.
- Schreiben: Cluster lesen, ändern, packen, Platz in der BitFAT, MDFAT
  umhängen, alten Platz frei – in dieser Reihenfolge. Unpackbares roh.
- Cluster-Cache 4–8 Cluster im eigenen Kernelsegment (DH_KSEG),
  verzögert; DRV_FLUSH/SHUTDOWN leeren.
- Freier Platz: geschätzt wie MS-DOS; `DBLSPACE /INFO` zeigt den Heap.
- Werkzeuge: `DBLSPACE /MOUNT /UNMOUNT /INFO /CREATE`, später
  `/COMPRESS`; `UDBLSPACE.EXE` (MDFAT gegen BitFAT/FAT), inneres FAT
  über UFAT.
- Buchstabe: neues Laufwerk; Tausch C:/H: erst, wenn Start davon geklärt.
- DOS-Box sieht es über den Redirector.

Formatdetails aus dem Gedächtnis – vor dem Bau an der dmsdos-Doku und an
einem echten MS-DOS-6.22-Container in 86Box prüfen.

## Stufen

1. Loop lesend – **erledigt** (15.11.2026): `bmap` als `ifs_bmap` in fat.c
   (ISO braucht keinen Aufruf), DRV_LOOP, LOOP.EXE, FAPI 59h; `blk[]`
   blieb in DGROUP (NBLK 12), Platz kam aus `list_drives` -> KTEXT2.
   Offene Punkte 1, 3 und 4 unten sind damit erledigt, 2 nur teilweise
   (Medienwechsel am Träger nicht erkannt).
2. Loop schreibend + Platten/VHD, `blk_read_to/blk_write_from`, `LOOP=`.
3. MSCDEX in der DOS-Box.
4. DBLSPACE.SYS lesend.
5. DBLSPACE.SYS schreibend, Cluster-Cache, UDBLSPACE.EXE.
6. `/CREATE`, evtl. `/COMPRESS`, DriveSpace 3 lesen, Start von C:.

Parallel (BIGLOCK.md, 6.1): fat.c-Zustand je Operation, dann Tiefe 2 und
Backend 2.

Offen: Name `LOOP` oder `MOUNT`; 6.22-Container zum Testen beim Nutzer?

## Offene Punkte (Durchsicht 15.11.2026)

1. **Gleicher Stand wie der Platten-Cache**: das Loop-Gerät liest und
   schreibt über den Cache des Trägers (`blk_read`/`blk_write` bzw. die
   neuen `_to/_from`-Hilfen mit Cache-Abfrage), nie direkt am Treiber –
   eine eben kopierte Datei liegt wegen des verzögerten Schreibens oft
   noch nicht auf der Platte.
2. **Wechselplatte/Diskette als Träger**: bei Medienwechsel meldet das
   Loop-Gerät „nicht bereit“ und verwirft die Extent-Liste. Disketten
   gehen über den Spurpuffer und geben die Kernelsperre nie ab – ein
   Loop auf A: geht, hält aber das System auf, solange es arbeitet.
3. **`blk[]` ins Kernelsegment**: 63 Zugriffe in kernel.c, ferne
   Zugriffe kosten zusammen grob 200–300 Byte im ersten Codesegment
   (~1 KB frei). Vorher den seltenen Teil der Blockgeräte-Schicht
   (mount_disk, cd_drives, Partitionen, Laden/Entladen) nach KTEXT2.
4. **Letzter Extent** endet an der Dateigröße, nicht am Clusterende;
   Zugriffe dahinter lehnt das Loop-Gerät ab (sonst Fremddaten).
5. **DoubleSpace-Format** vor Stufe 4 an einem echten 6.22-Container in
   86Box prüfen; danach Container und Inhalt mit ScanDisk von MS-DOS
   6.22 gegenprüfen, nicht nur mit UDBLSPACE.
6. Container liegt versteckt/System/schreibgeschützt im Stamm –
   `OPEN_RW` darf dafür nicht am Attribut scheitern (eigener Merker
   beim Einhängen).
