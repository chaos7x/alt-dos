<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Speicherkarte

Stand: 17.11.2026 (`main`, 6.1). Teil 1 beschreibt, wie AT-DOS den
Speicher heute aufteilt (geprüft gegen `kernel/mem.c`, `kernel/kernel.c`
kmain, `kernel/paging.c`, `kernel/v86.c`, `kernel/start.asm`), Teil 2
die Planung für AT-DOS 7.0 (32 Bit, noch kein Code).

Grundsatz: Im Protected Mode tut der Prozessor mit dem ersten MB nichts
Besonderes – es ist gewöhnlicher physischer Speicher, und der Kernel
entscheidet, was damit geschieht. Die Altlasten (IVT, BIOS-Datenbereich,
EBDA, Bildspeicher, ROMs) liegen trotzdem dort, und solange BIOS-Dienste
(Real-Mode-Ausflug) oder DOS-Programme (V86) gebraucht werden, müssen sie
unangetastet bleiben.

## Teil 1: Ist-Stand 6.x

### Physisch (Sicht der Speicherverwaltung)

`mem_init` markiert zuerst alles als belegt und gibt dann nur frei, was
wirklich RAM ist: unten bis zur Größe aus INT 12h (`mem_conv_kb`, höchstens
640 KB – die EBDA liegt damit automatisch außerhalb), oben die RAM-Bereiche
aus INT 15h E820h (Lücken und reservierte Bereiche bleiben belegt; ohne
E820h E801h, zuletzt 88h). Ohne A20 (`a20_ok` = 0) gibt es nichts über 1 MB.
Vergeben wird in 4-KB-Seiten; Seite 0 ist nie frei (0 heißt „kein
Speicher“).

| physisch | Inhalt |
|---|---|
| 00000h – 00FFFh | IVT und BIOS-Datenbereich (bleibt immer belegt) |
| 00600h – 07BFFh | nur beim Start von Platte: Vorladebereich für `CONFIG.SYS` und `DEVICE=`-Dateien (`preload.c`), danach frei |
| 01000h – 05FFFh | nach dem Start frei (Speicherverwaltung) |
| 06000h – 07FFFh | Real-Mode-Ausflug (`rm_int10`: Video-BIOS, APM): Stapel, Register bei 6F00h, rohe GDTR bei 6FD0h, Puffer ab 7000h (VbeInfoBlock, ModeInfo bei 7200h); Bootsektor bei 7C00h während des Starts |
| 08000h – 087FFh | gemeinsamer DMA-Puffer `DMABUF` (2 KB) – Rückfall, wenn ein Task keine eigene Seite bekam (seit Phase 6 hat jeder Task seinen eigenen, s. u.) |
| 08800h – 08BFFh | gemeinsamer FAT-Cache (`FCACHE_LIN`, 1 KB), ebenfalls Rückfall |
| 09000h – 0AFFFh | Belegungs-Bitmap (8 KB, reicht für 256 MB); bei mehr RAM liegt sie im ersten passenden RAM über 1 MB (bis 128 KB, zwei Selektoren) |
| 0B000h – 0FFFFh | nach dem Start frei |
| ab 10000h | Kernel (`ATDOS.SYS`, höchstens 448 KB): Codesegmente KCODE, KCODE2, KCODE3, KCODE4 hintereinander, dahinter DGROUP (Daten, BSS, Kernelstacks), je bis 64 KB |
| Kernelende – INT-12h-Grenze | frei, Speicherverwaltung |
| 80000h – EBDA | nur beim Start von Platte mit ≥ 600 KB konventionell: Vorladebereich (`pre_seg` 8000h, ~127 KB), wird nach `CONFIG.SYS` freigegeben |
| EBDA – 9FFFFh | EBDA des BIOS (Adresse in 40:0Eh), nie vergeben |
| A0000h – BFFFFh | Bildspeicher (VGA, B8000h Text) – nie vergeben |
| C0000h – FFFFFh | Video-BIOS, Options-ROMs, Shadow-RAM, System-BIOS – nie vergeben |
| ab 100000h | freier Speicher laut E820h bis knapp 4 GB (`MEM_MAX_PG` FFFFFh) |

Je Task eine Seitengruppe unter 16 MB (`dmab_pg`, `mem_alloc_low`):
DMABUF 2 KB, FAT-Cache 1 KB, Schreibpuffer `WBUF` 512 Byte bei 3584,
dahinter (ab 4096) der Kontext der Phase 6. ISA-DMA-Puffer (Diskette,
Sound Blaster, Auslagerungsdatei) kommen ebenfalls unter 16 MB
(`kseg_dma`).

Grenze bei 2 GB: `mem_alloc` liefert nur Seiten unter `MEM_LOW_PG`
(7FC00h = 2 GB − 4 MB), weil Segmente über den Kernel-Alias zeigen.
`mem_alloc_hi` nimmt zuerst Seiten darüber – nur für Speicher, den der
Kernel ausschließlich über Seitenfenster oder Seitentabellen einer Box
anfasst: Box-Seiten (konventionell, HMA, UMB, XMS, DPMI, ROM-Kopien),
EMS-Seiten, Platten-Cache.

### Linear (Sicht von Kernel und Programmen)

Ein Seitenverzeichnis je Adressraum (`paging.c`, CR3 im 386-TSS jedes
Tasks):

| linear | Inhalt |
|---|---|
| 00000000h – 003FFFFFh | im Kernel-Adressraum 1:1; in einer DOS-Box deren eigener Adressraum (s. u.) |
| bis `mem_top_pg()` | 1:1-Abbildung allen RAMs (Seitentabellen aus `mem_alloc`, mindestens das erste MB) |
| 80000000h – FFBFFFFFh | Kernel-Alias `KALIAS`: Verzeichniseinträge 512 + i zeigen auf dieselben Seitentabellen, also physisch 0 bis knapp 2 GB. Alle Speicherdeskriptoren der GDT (und die Basis in `put_desc`) rechnen mit KALIAS + physisch; physische Adressen für DMA bleiben physisch |
| FFC00000h – FFFFFFFFh | Verzeichniseintrag 1023 = Fenster-Seitentabelle `win_pt`: vier Plätze zu 32 Seiten (SCRATCH, SCRATCH2, Cache-Fenster, Rest) für Speicher über `MEM_LOW_PG`, dahinter linearer VESA-Bildspeicher (bis 880 Seiten, PTE mit U), die letzten 16 Seiten Geräteregister (`pg_mmio`, z. B. AHCI, PCD/PWT) |

Weil der Kernel ab 2 GB läuft, gehört der lineare Bereich 0–4 MB (und für
XMS/DPMI weiter bis 1 GB) frei der jeweils laufenden DOS-Box.
FAPI-Programme sehen von alledem nichts: Sie arbeiten mit Selektoren aus
ihrer LDT (Code, Daten, `DosAllocSeg`) und dem FAPI-Gate.

### In jeder DOS-Box (`v86.c`, eigenes Seitenverzeichnis)

Eintrag 0 des Verzeichnisses ist die Seitentabelle der Box, die Einträge
ab 512 übernimmt sie vom Kernel – **ohne U-Bit** (DOS/4G legt flache
4-GB-Selektoren an, Ring 3 käme sonst an den Kernel). Eintrag 1023 (Fenster)
ebenfalls ohne U.

| linear in der Box | Inhalt |
|---|---|
| 00000h – RMSIZE | eigene Seiten (`RMSIZE=` 256–640 KB, Vorgabe 640), physisch beliebig verstreut, beim Anlegen genullt; Seite 0 bekommt eine IVT auf die Stummel bei F000h und eine Kopie des BIOS-Datenbereichs |
| RMSIZE – 9FFFFh | bei kleinerem RMSIZE nicht abgebildet |
| A0000h – BFFFFh | Bildschirm: eine Ersatzseite für den ganzen Bereich; B8000h/B9000h zeigen direkt auf den Sitzungspuffer (Änderungen über das Dirty-Bit, kein Trap). Im Grafikbetrieb (`vga_enter`) die echte VGA; im Hintergrund der Schatten (`vga_map`, `vga_plmap`, `vga_svmap`) |
| C0000h – EFFFFh | ROMs 1:1, nur lesbar (`PTE_ROM`); Schreiben gibt der Box eine eigene Kopie (`rom_cow`). Freie Lücken (`umb_scan`, 2-KB-Schritte ab C0000h, 55AA-ROMs ausgelassen; bei SeaBIOS E0000h–EFFFFh nicht) werden EMS-Rahmen (`ems_pick_frame`, erste 16 freie Seiten ab C8000h, auf 16 KB ausgerichtet; `EMS_FRAME_LOCATION=NONE` lässt ihn weg) und UMBs (DOS=HIGH,UMB) |
| F0000h – F0FFFh | eigene „BIOS“-Seite der Box: Dienst-Stummel `ARPL n; IRET` je Vektor (ARPL ist in V86 #UD -> Monitor), Einsprünge für XMS (0500h), DPMI (0510h …), Diskettenparameter (0800h), XMS-Handle-Tabelle (0900h), Mauszustand (0B00h), DPMI-Standardvektoren (0C00h) |
| F1000h – FFFFFh | System-BIOS 1:1, nur lesbar |
| 100000h – 10FFFFh | HMA: bei A20 aus Umlauf auf 0–64 KB, bei A20 an eigene Seiten (`hma_pg`) – A20 je Box |
| 110000h – 3FFFFFFh | XMS-Blöcke der Box (bis 64 MB, `XMS_TOP`), Seiten mit `PTE_OWN` |
| 4000000h – 3FFFFFFFh | DPMI-Speicher (0501h, 64 MB – 1 GB, `dmem_gap`), dazu physische Einblendungen über DPMI 0800h (VESA-LFB, nur Seiten ab `mem_top_pg()`) |

EMS-Seiten (je 4 zusammenhängende 4-KB-Seiten) werden im Rahmen über die
PTEs eingeblendet. Ausgelagerte Box-Seiten (`swap.c`) haben P = 0 und
`PTE_SWP`; nie ausgelagert werden Seiten 0–Fh, A0–C7h, F0h–10Fh und
Seitentabellen.

### 5.x (286, ohne Paging)

Auf dem 286 gibt es keine Seitentabellen, nur die physische Aufteilung:
IVT/BIOS-Daten, DMABUF bei 8000h, Bitmap bei 9000h, Kernel ab 10000h,
frei bis 640 KB, A0000h–FFFFFh nie vergeben, darüber erweiterter Speicher.
286-Deskriptoren haben 24 Bit Basis – mehr als 16 MB gibt es dort nicht.
Programme laufen in Segmenten, die Kompatibilitätsbox (5.1) belegt
0–640 KB selbst.

## Teil 2: Planung 7.0 (32 Bit)

Noch kein Code; Was sich aus 6.x bewährt hat,
bleibt, damit es sich zurückportieren lässt.

- **Speicherkarte nur aus E820h** (davor E801h/88h), nichts hart kodieren.
  Wie heute: alles belegt, nur gemeldetes RAM freigeben, Typen 3/4 (ACPI)
  merken – die ACPI-Tabellen werden zum Ausschalten gebraucht. Mit UEFI
  (ab 7.0 geplant) kommt die Karte aus `GetMemoryMap` vor
  `ExitBootServices`.
- **Erstes MB = reserviert, DOS-Kompatibilität.** Der Kernel vergibt
  daraus nichts für eigene Zwecke außer festen, kleinen Stellen
  (Real-Mode-Ausflug, Bootsektor, ggf. ISA-DMA). Gründe: IVT, BDA und EBDA
  bleiben gültig, solange BIOS-Dienste per V86 oder Real-Mode-Ausflug
  laufen; V86-Boxen bekommen ihre 640 KB ohnehin aus eigenen Seiten
  irgendwo im RAM. Freie Lücken der UMA (A0000h–FFFFFh) nutzen nur die
  Boxen als UMB/EMS-Rahmen wie heute.
- **Kernel ab 1 MB** (physisch 100000h), statt ab 10000h. Damit fällt die
  Grenze 448 KB für `ATDOS.SYS` weg, und der Kernel liegt nicht mehr im
  Bereich, den Boxen und BIOS brauchen. Der Lader muss dafür A20 an haben
  und die Datei über 1 MB legen (Unreal Mode, Protected-Mode-Kopie oder
  INT 15h 87h).
- **A20**: ohne A20 läuft der Speicher ab 1 MB auf 0 um (jedes ungerade
  MB spiegelt das gerade darunter). Einschalten wie heute (Tastatur-
  controller, dazu Port 92h „Fast A20“ und INT 15h 2401h als weitere Wege)
  und danach prüfen (Vergleich 0:0500 mit FFFF:0510). A20 je Box bleibt
  Sache der Seitentabellen (HMA-Umlauf), nicht der echten Leitung.
- **Linear**: Kernel im oberen Teil (wie heute der Alias ab 2 GB, ggf.
  3 GB wie Linux), unten bleibt Platz für Prozesse und Boxen. Speicher
  über der direkten Abbildung weiter über Fenster wie `pg_win`; PAE erst
  bei Bedarf.
- **Bildspeicher**: Textausgabe im PM weiter direkt nach B8000h (über die
  Abbildung), Grafik über den linearen Bildspeicher (VBE bzw. GOP bei
  UEFI, dort gibt es kein B8000h).

Offene Punkte: Lage der Kernel-Abbildung (2 oder 3 GB), PAE, ob 7.0 den
Real-Mode-Ausflug behält oder Video-BIOS/APM nur noch per V86 ruft.
