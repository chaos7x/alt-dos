<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Startcode nach dem Booten freigeben (Init-Segment)

Stand: 25.11.2026 (`main`, 6.3). Treiber (Abschnitt 2.6) und Messwerkzeug
(2.5) und Kernel (Stufen 1–3) gebaut, siehe „Stand Treiber“/„Stand Kernel“ am Ende.
Ursprünglicher Entwurf 17.11.2026 (`main`, 6.1). Anlass: die
Untergrenze 386SX mit 2 MB (CLAUDE.md „Testmaschinen 6.0“) und der Weg
zu 7.0, das auf derselben Maschine laufen soll.

Heute gibt der Kernel nach dem Start schon **Puffer** zurück (1000h–5FFFh,
B000h–FFFFh, Vorladebereich 80000h, CONFIG.SYS-Segment `cfg_sel`, siehe
`docs/SPEICHERKARTE.md`), aber keinen **Code**: `kmain`, `config_sys`,
`pci_init`, `pnp_init`, `mem_init`, `paging_init` usw. bleiben im RAM
liegen, obwohl sie nie wieder laufen. Dasselbe gilt für die Erkennung in
jedem Treiber. Der Vorschlag: was nur beim Start gebraucht wird, kommt in
ein eigenes Segment, das der Kernel nach dem Start abgibt – wie
`__init`/`free_initmem()` bei Linux und wie die INIT-Antwort der Treiber
bei OS/2 1.x (Ende von Code und Daten, der Rest wird verworfen) bzw. die
„Break-Adresse“ der MS-DOS-Gerätetreiber.

## 1. Was heute nur beim Start läuft (gemessen)

Gemessen an `wdis` der Kernelobjekte (Build vom 17.11.2026, d7c2cfe):
Aufrufgraph aus allen `call`/`jmp`, als „nur Start“ zählt eine Funktion,
deren Aufrufer alle schon „nur Start“ sind, ausgehend von `kmain`,
`gdt_build` und `preload` (die beiden laufen im Real Mode aus start.asm);
Funktionen, deren Adresse irgendwo genommen wird (Treibertabellen,
Rückrufe), zählen nicht – außer der Pseudo-Dateisystem-Tabelle von
preload.c, die nur während `CONFIG.SYS` als `Z:` hängt. Das Skript dafür
gehört später als Prüfung ins Repository (Abschnitt 4).

| Teil | Segment | Bytes | Inhalt |
|---|---|---|---|
| kernel.c | KCODE | 6.623 | `kmain`, `config_sys`, `read_cfg`/`next_line`/`cfg_release`, `pnp_start`, `pic_pit_init`, `ldt_init`, `gdt_build`, `blk_cache_init`, `find_boot_vol`, `conv_pending`, `loop_boot`, `cp_load`, `kw`, `num_arg`, `fd_name`, `exec_err` |
| preload.c | KCODE | 2.609 | ganz (Real Mode, Vorladen und Pseudo-IFS `BOOT`) |
| kbd.c | KCODE | 52 | `kbd_init` |
| start.asm | KCODE | ~1.030 | Real-Mode-Teil von `_start` bis `pm_entry` (CPU-, A20-, E820-, VGA-, FPU-, Geometrieabfragen), `rm_geometry`, `rm_read` |
| pnp.c | KTEXT2 | 3.075 | ganz außer den Daten (`pnp[]` bleibt für DosBusInfo) |
| mem.c | KTEXT2 | 1.299 | `mem_init`, `next_range`, `reserve_range` |
| swap.c | KTEXT2 | 1.200 | `swap_boot` |
| paging.c | KTEXT2 | 811 | `paging_init`, `alias_on`, `add_alias` |
| k2misc.c | KTEXT2 | 688 | `list_drives` |
| floppy.c | KTEXT2 | 257 | `fd_init` |
| bus.c | KTEXT2 | 230 | `pci_init` |
| fat.c | KTEXT2 | 129 | `vol_move` |
| **Code** | | **~18.000** | KCODE ~10,3 KB, KTEXT2 ~7,7 KB |
| Texte | DGROUP | ~2.000 | Zeichenketten, die nur diese Funktionen benutzen (kernel.c 1.712, k2misc 231, swap 45, preload 31) |
| BSS | DGROUP | ~300 | preload.c (289), statische Puffer in config_sys |

Zusammen **~20 KB** im Kernel, in ganzen Seiten ~4–5 Seiten (16–20 KB).
Gemessen sind nur Funktionen, die schon heute sauber getrennt sind.
Nicht gezählt, weil sie auch zur Laufzeit gebraucht werden:
`load_driver` (1.180, DEVLOAD), `mount_disk` (579), `cd_drives` (199),
`task_create`, `env_set`, `vio_open`, `kseg_alloc`.

**Treiber** (gleiche Messung, Wurzel `init()` hinter `case DRV_INIT`):

| Treiber | Code | nur INIT | Anteil |
|---|---|---|---|
| SB16.SYS | 13.635 | 5.878 | 43 % |
| IDE.SYS | 7.019 | 3.435 | 48 % |
| AHCI.SYS | 5.503 | 2.527 | 45 % |
| AHA154X.SYS | 4.666 | 2.250 | 48 % |
| MOUSE.SYS | 3.320 | 1.988 | 59 % |
| AIC7XXX.SYS | 6.056 | 1.870 | 30 % |
| NCR8XX.SYS | 4.514 | 1.636 | 36 % |
| NCR5380.SYS | 4.498 | 1.251 | 27 % |

Eine übliche Ausstattung (IDE, MOUSE, SB16) hätte ~11 KB Startcode, frei
würde davon, was ganze Seiten füllt – grob 8 KB.

**Einordnung:** Insgesamt 25–30 KB von ~1,6 MB nutzbarem RAM auf der
2-MB-Maschine, also knapp 2 % – eine DOS-Box kostet ~660 KB. Der Gewinn
im RAM ist nett, aber nicht der Hauptgrund. Wichtiger ist der **Platz in
den Segmenten**: das erste Codesegment (60.022 von 65.520 Byte) bekäme
~10 KB Luft, KTEXT2 ~7,7 KB, DGROUP (61.892 von 65.536) ~2 KB, wenn die
Texte mitwandern. Das erste Segment und DGROUP sind seit Monaten der
Engpass (CLAUDE.md: „erstes Segment voll“, „DGROUP fast voll“).

## 2. Mechanik in 6.x

### 2.1 Eigene Quelldateien statt Markierung je Funktion

Im 16-Bit-Kernel kann eine Funktion in einem anderen Codesegment nur
`__far` gerufen werden, und sie selbst erreicht nahe Funktionen des
ersten Segments nur über Thunks (`k2.h`/`k2thunk.c`). Eine Markierung je
Funktion (wie `__init` bei Linux) würde jeden Aufruf aus dem Startcode in
einen Thunk verwandeln, ohne dass man es sieht. Deshalb wie bei KTEXT2:
**ganze Dateien** wandern, mit `k2.h` als letztem Include.

- neu `kernel/kinit.c`: `kmain`, `config_sys`, `read_cfg`/`next_line`/
  `cfg_release`, `kw`, `num_arg`, `pnp_start`, `pic_pit_init`,
  `ldt_init`, `gdt_build`, `blk_cache_init`, `find_boot_vol`,
  `conv_pending`, `loop_boot`, `cp_load`, `fd_name`. Die Variablen, die
  `config_sys` setzt (`protshell`, `nbuffers`, `cache_kb`, `lfn_cfg`,
  `dos_high` …), verlieren ihr `static` und stehen in einem gemeinsamen
  Header (`kern.h` oder neu `kinit.h`).
- `preload.c` und `pnp.c` ganz; aus `mem.c`, `paging.c`, `swap.c`,
  `bus.c`, `floppy.c`, `k2misc.c`, `fat.c` die Startfunktionen in je eine
  Datei `*_init.c` (`mem_init.c`, `paging_init.c` …) oder gesammelt in
  `kinit2.c` – je nachdem, wie viele statische Variablen sie mit dem Rest
  der Datei teilen (die werden dann `extern`).
- build.sh: `IMODS="kinit preload pnp mem_init …"` mit `-nt=ITEXT`, wie
  K2MODS.

### 2.2 Lage: das Init-Segment vorn bei 10000h (Vorschlag)

Zwei Möglichkeiten, das Segment im Abbild unterzubringen:

**A – vorn (empfohlen).** Gruppe `CGROUPI` (Segmente ITEXT/ITEXTEND) ganz
vorn in `ATDOS.SYS`, also ab 10000h, auf 4 KB aufgefüllt; dahinter wie
heute CGROUP, CGROUP2–4 und DGROUP. Vorteile:

- Der Real-Mode-Teil von start.asm (`_start` bis `pm_entry`), `preload`,
  `gdt_build`, `rm_read`/`rm_geometry` liegen ohnehin am Anfang und
  laufen mit CS = 1000h – sie rufen sich dort weiter nah. Der Boot-
  sektor (Diskette, `hdboot.asm`, `fat32boot.asm`) springt unverändert
  nach 1000:0000.
- Freigegeben wird ein zusammenhängender Block ganzer Seiten im
  konventionellen Speicher direkt hinter den schon freien 1000h–FFFFh –
  zusammen ein Stück von 60 KB + Init-Segment am Stück, gut für
  `mem_alloc_low` und Segmente, die Seiten am Stück brauchen.
- Kein Umkopieren beim Start.

Was sich ändert: `KCODE_LIN` in `kern.h` ist nicht mehr fest 10000h,
sondern `10000h + ISIZE` (Konstante, die build.sh aus der Map in
`build/version.h` o. Ä. schreibt und prüft); build.sh ordnet die
Relokationen neu zu (Segmentwert 0 = KCODEI, `ISIZE >> 4` = KCODE …,
heute `v == 0 -> 08h`); `start.asm` setzt KCODE auf die neue Basis, die
Prüfung „DGROUP direkt hinter dem Code“ bekommt ein Glied mehr. NASM
`obj` kennt `align=4096`; ob wlink das in der DOS-EXE einhält, ist zu
prüfen – sonst füllt start.asm (`ITEXTEND`) per `times` auf und build.sh
bricht ab, wenn die Rechnung nicht stimmt.

**B – hinten.** ITEXT hinter DGROUP. Geht nur, wenn das BSS mit in die
Datei kommt (sonst lägen Init-Code und BSS übereinander) – ~31 KB Nullen
mehr in `ATDOS.SYS`, auf der Diskette ist dafür kein Platz – oder wenn
start.asm das Segment vor dem Nullen des BSS hinter dessen Ende kopiert.
Zwischen `_DATA` und `_BSS` geht es nicht: DGROUP würde dann über 64 KB
reichen. Deshalb A.

### 2.3 Selektor und Aufrufe

- Neuer GDT-Eintrag `KCODEI` (Code, Basis 10000h, Grenze ISIZE − 1).
  `pm_entry` liegt selbst im Init-Segment und springt nach dem Laden der
  GDT nach `KCODEI:pm_entry` (heute `KCODE:pm_entry`); `kmain` ruft es
  danach nah.
- Startcode -> Kernel: über Thunks (`k2.h`), wie KTEXT2 heute. Die
  meisten Aufrufe von `config_sys`/`kmain` gehen ohnehin an Funktionen,
  die schon `__far` sind oder einen Thunk haben (`kf_*`); neu brauchen
  einen Thunk u. a. `load_driver`, `vio_kputs`, `env_set`, `env_find`,
  `kseg_alloc`, `task_create`, `ifs_mount`, `mount_disk`, `cd_drives`,
  `udec`, `scopy`. Jeder Thunk kostet ~6–10 Byte im ersten Segment.
- Kernel -> Startcode: **nie**. Das prüft build.sh (2.5).
- KTEXT2-Teile, die heute Startfunktionen enthalten (`pnp_init` …), sind
  schon `__far`; im Init-Segment bleiben sie `__far`, nur der Segment-
  name ändert sich.

### 2.4 Freigeben

`kmain` endet heute mit `schedule_next()`, das nie zurückkehrt, und der
Startstapel wird zum Stapel des Leerlaufs (CLAUDE.md, Lehre aus
`swap_boot`). Der letzte Schritt von `kmain` wird stattdessen ein
Fernaufruf `kinit_done()` im ersten Codesegment:

1. `gdt[KCODEI_IDX].access = 0` (nicht vorhanden), danach jeder Zugriff
   darauf #NP/#GP -> `fault()` meldet „Aufruf in freigegebenen Start-
   code“ mit CS:IP (eigener Text wie beim #PF), statt still fremden Code
   auszuführen.
2. Seiten frei: `mem_free(0x10, ISIZE >> 12)` – nur ganze Seiten; ist das
   Segment nicht aufgefüllt, bleibt der Rest der letzten Seite liegen.
3. Bei Option A mit Paging zusätzlich die Seiten 10h–(10h + n − 1) in den
   Seitentabellen nicht anfassen: sie sind 1:1 abgebildet wie aller
   Speicher, das bleibt so (die Speicherverwaltung vergibt sie neu).
4. `schedule_next()`.

Der Stapelrahmen von `kmain` bleibt auf dem Startstapel liegen, wird aber
nie wieder benutzt (kein Rücksprung). Wichtig: `kinit_done` darf keine
Rücksprungadresse ins Init-Segment brauchen – es wird nicht gerufen und
kehrt nicht zurück, sondern springt (`jmp far` aus einem kleinen
Assemblerstück, damit der Compiler keinen Rahmen anlegt).

Daten: die ~2 KB Texte und das BSS des Startcodes liegen heute in DGROUP.
Stufe 1 lässt sie dort (DGROUP gewinnt dann nichts). Stufe 2 legt sie in
ein Segment `IDATA` in derselben Gruppe wie ITEXT (vorn, wird mit
freigegeben) und reicht sie als `const char __far *` weiter – dafür
brauchen `vio_kputs` und `kw` eine Fern-Fassung (`vio_kputs_f`). Erst
damit gewinnt DGROUP die ~2 KB.

### 2.5 Schutz beim Bauen

Zur Laufzeit fängt der Deskriptor (2.4) einen Fehler ab, aber erst, wenn
der Pfad läuft – ein seltener Pfad (DEVLOAD, CONVERT beim Start) fiele
erst beim Nutzer auf. Deshalb prüft build.sh wie bei den Relokationen:

- `tools/initcheck.py` liest `wdis` (oder `dmpobj`) aller Kernelobjekte,
  baut den Aufrufgraphen und bricht ab, wenn eine Funktion außerhalb von
  IMODS ein Symbol aus IMODS ruft oder seine Adresse nimmt (Ausnahmen:
  start.asm -> `kmain`, `preload`, `gdt_build`).
- Dasselbe Skript gibt als Vorschlag aus, welche Funktionen heute nur
  beim Start laufen und noch nicht in IMODS liegen (die Messung aus
  Abschnitt 1) – so wächst die Liste mit.
- Funktionszeiger: `pre_driver` (preload.c) ist eine `struct ifs_driver`
  mit Fernzeigern ins Init-Segment. Sie hängt nur als `Z:` während
  `config_sys` und wird danach mit `vol[PRE_DRIVE].used = 0` abgehängt –
  `kinit_done` setzt zusätzlich `vol[PRE_DRIVE].drv = 0`, damit ein
  vergessener Zugriff auf `Z:` als „ungültiges Laufwerk“ endet statt im
  freien Speicher.

### 2.6 Treiber und IFS-Module

Wie OS/2 1.x: die INIT-Antwort sagt, wie viel vom Abbild bleibt.

- `struct drv_req` bekommt ein Rückgabefeld `keep` (Bytes ab Anfang des
  Abbilds, die bleiben; 0 = alles, alte Treiber). Platz: hinter den
  vorhandenen Feldern, die Größe der Anforderung ändert sich nicht, wenn
  ein bisher unbenutztes Feld der INIT-Antwort genommen wird – sonst
  neues Feld am Ende (der Kernel füllt es vor INIT mit 0).
- Treiber: der Startcode kommt in eine eigene Datei bzw. per
  `#pragma code_seg("ITEXT", "ICODE")` hinter alles andere – Treiber
  sind ein einziges Segment (tiny, `format raw`), nahe Aufrufe bleiben
  also erlaubt. Linkreihenfolge `clname CODE clname DATA clname BSS
  clname ICODE`; dabei kommt das BSS mit in die Datei (Treiber haben
  wenig BSS, bei SB16 zu prüfen). `dcrt0.asm` exportiert `_init_start`,
  INIT antwortet `keep = _init_start`.
- Kernel (`load_driver`): nach erfolgreichem INIT Grenze des Code-
  deskriptors auf `keep − 1`, Datendeskriptor ebenso (BSS liegt davor),
  Seiten ab `PAGES(keep)` bis Ende frei. Wer danach in den Startcode
  springt, bekommt #GP im Treiber -> Panik mit Treibername (heute schon
  so bei Grenzverletzungen).
- DEVLOAD lädt immer ein frisches Abbild (auch nach `/U`), also bleibt
  nichts zu beachten. DRV_DEINIT gehört nicht zum Startcode.
- Nur Ganze-Seiten-Gewinn: lohnt bei SB16, IDE, AHCI, MOUSE sofort; ein
  Treiber, dessen Rest in derselben Seite bliebe, gewinnt nichts, schadet
  aber auch nicht.

### 2.7 Reihenfolge

1. `tools/initcheck.py` (nur messen und melden, bricht noch nicht ab).
2. Treiber-INIT mit `keep` (klein, unabhängig vom Kernel, gut in 86Box zu
   prüfen: MEM vorher/nachher, DEVLOAD /U und neu laden).
3. Kernel Stufe 1: kinit.c + preload.c + pnp.c als ITEXT vorn, Code frei;
   initcheck bricht ab.
4. Kernel Stufe 2: Startfunktionen aus mem/paging/swap/bus/floppy/
   k2misc/fat nachziehen, Texte nach IDATA.

Prüfen jeweils: Start von Diskette, von IDE, von FAT32, von NCR8XX und
AHCI (vorgeladene Treiber), `DEVLOAD`, `SETUP`, Umwandeln beim Neustart
(`conv_pending`), isapc 2 MB (MEM vorher/nachher), SHUTDOWN, fsck.

## 3. Bezug zu `codeswap.c` und zum Auslagern

`kernel/codeswap.c` verwirft Code von **Programmen**: LDT-Eintrag 0
nicht vorhanden, beim nächsten Zugriff #NP, `code_np` lädt ihn aus der
EXE nach. Das Init-Segment ist der einfachere Fall: verworfen wird
einmal und für immer, ein #NP darauf ist ein Fehler, kein Nachladen.

Der gleiche Weg reicht aber weiter (Ausblick, nicht Teil dieses
Vorschlags): ein Segment **KTEXTP** für seltenen Kernelcode, der zur
Laufzeit gebraucht wird – Treiberlader, DEVLOAD/Entladen, CONVERT-Pfade,
FORMAT/FDISK-Hilfen, MSD-Abfragen –, das wie Programmcode verworfen und
bei #NP aus `ATDOS.SYS` nachgeladen wird (Stelle in der Datei beim Start
gemerkt, Prüfsumme wie `code_note`). Das ist der „teilweise auslagerbare
Kernel“ wie bei NT. Schwierigkeit gegenüber Programmen: der #NP kommt im
Ring 0, oft mit gehaltener Kernelsperre, und das Nachladen braucht selbst
Dateisystem und Treiber – die dürfen also nie in KTEXTP liegen, und
`code_np` müsste den Fall „Kernel“ ohne neuen Sperrversuch können. Erst
angehen, wenn Stufe 1/2 hier laufen.

Speicherkarte: nach Stufe 1 steht in `docs/SPEICHERKARTE.md` statt „ab
10000h Kernel“: 10000h – 10000h + ISIZE Init-Segment (nach dem Start
frei), dahinter KCODE … DGROUP.

## 4. AT-DOS 7.0 (32 Bit)

In 7.0 ist der Kernel flach, nahe und ferne Aufrufe gibt es nicht mehr –
dort geht die Markierung je Funktion wie bei Linux:

- Makros `KINIT` und `KINITDATA` (`__attribute__((section(".init.text")))`
  bei GCC i686-elf, `#pragma code_seg("INIT_TEXT")`/`data_seg` bei Open
  Watcom 32), im Linkerskript `.init.text`/`.init.data` seitengenau ans
  Ende des Kernels, Symbole `__init_start`/`__init_end`; `free_initmem()`
  am Ende des Starts setzt die Seiten in der Seitentabelle auf nicht
  vorhanden (Seitenfehler statt fremdem Code) und gibt sie der
  Seitenverwaltung zurück. Kein eigener Selektor nötig.
- Treiber (PE/LE-Module in 7.0) bekommen einen Abschnitt `INIT`, den der
  Lader nach dem Einsprung verwirft – wie `INIT` bei NT-Treibern.

**Rückportierbarkeit:** Damit Startcode zwischen 7.0 und 6.x hin und her
wandern kann, gilt schon in 6.x die Regel „Startcode steht in eigenen
Dateien“ (`kinit.c`, `*_init.c`). In 6.x heißt das `-nt=ITEXT` + Thunks,
in 7.0 nimmt das Linkerskript dieselben Objekte nach `.init.text` (dann
braucht es nicht einmal `KINIT` an jeder Funktion). `KINIT` als leeres
Makro in 6.x schadet nicht und dokumentiert die Absicht; `initcheck.py`
arbeitet in beiden Linien auf dem Aufrufgraphen und lässt sich für 7.0
auf `objdump -d` umstellen. Die INIT-Antwort `keep` der Treiber ist in 7.0
überflüssig (Abschnitt statt Grenze), schadet aber nicht, wenn 16-Bit-
Treiber unter 7.0 weiterlaufen sollen.

## Stand Treiber (25.11.2026)

Gebaut wie 2.6, mit zwei Abweichungen:

- **Rückgabe in `buf_off`** statt eines neuen Feldes: bei INIT ist es
  ungenutzt, der Kernel setzt es vorher auf 0. `dcrt0.asm` (`drv_entry`)
  trägt nach jedem INIT `_init_start` ein – den Anfang von ITEXT, das als
  Klasse `ICODE` hinter dem BSS liegt (build.sh `order … clname BSS
  clname ICODE`, ITEXT in DGROUP). Startfunktionen stehen in den Treibern
  zwischen `#pragma code_seg("ITEXT", "ICODE")` und `#pragma
  code_seg("_TEXT", "CODE")`. Nahe Aufrufe bleiben (ein Segment).
- **BSS nicht in der Datei**: hinter dem BSS stehender Code zwingt wlink,
  die BSS-Nullen mitzuschreiben (die Disketten liefen voll, VEMS.SYS 29
  statt 6 KB). `mkfx.py --driver` schneidet sie heraus und setzt
  `FX_FLAG_GAP` (8): bei `entry` (bei Version 1 ungenutzt) fehlen
  `bss_size` Nullbytes, `load_image` setzt sie beim Laden wieder ein.

Kernel `load_driver`: nach erfolgreichem INIT mit `buf_off` < Segment-
größe beide Deskriptoren auf `buf_off − 1`, Seiten ab `PAGES(buf_off)`
frei; `drv_unload` gibt über die Datengrenze genau den Rest frei.
Code-Deskriptor jetzt über das ganze Segment (mit BSS) – vorher Abbild.

`tools/initcheck.py` (wdis, Aufrufgraph, Größe aus dem Abstand der
Marken): ohne `--seg` Vorschlag, mit `--seg ITEXT` bricht build.sh ab,
wenn Laufzeitcode etwas in ITEXT ruft, dessen Adresse nimmt oder dort
etwas liegt, das auch zur Laufzeit läuft. Eingerahmt wurden genau die
Funktionen, die initcheck meldet und die in der `.c`-Datei des Treibers
stehen (aus gemeinsamen Headern wie drvlib.h/awe.h nichts).

Gewinn in ganzen Seiten (vorher → nachher): IDE 3 → 2, SB16 5 → 4,
AHA154X 2 → 1, AIC7XXX 3 → 2, MOUSE 2 → 1, NCR5380 2 → 1, TCPIP 13 →
12; bei AHCI, NCR8XX, NE2000, den Netzkarten und Box-Modulen reicht der
Startcode für keine ganze Seite (schadet nicht). MEM (QEMU, 16 MB,
fester Cache): „BIOS, Boot, Puffer“ 415 → 410 KB nur mit MOUSE.SYS
umgestellt, `DEVLOAD /U` + neu laden ohne Leck.

## Stand Kernel, Stufe 1 (25.11.2026)

Abweichend von 2.2 (Variante A, alles verschieben) liegt der Startcode in
einer **fünften Codegruppe `CGROUPI` (Segment ITEXT) zwischen KTEXT4 und
DGROUP**: keine Basis und kein fester Selektor ändert sich, DGROUP rückt
nur nach (start.asm rechnet `codei_end` mit). Selektor `KCODEI` 328h
(`KCODEI_IDX` = KCODE3_IDX + 6, NGDT eins mehr), build.sh setzt Fern-
aufrufe dorthin ein und prüft die Lage. Freigegeben werden nur die
ganzen Seiten im Segment (die Ränder teilen sich Seiten mit KTEXT4 und
DGROUP) – `kinit_done` (k2misc.c, selbst in ITEXT) gibt sie frei und
macht danach den Deskriptor ungültig; kmain ruft es direkt vor
`sched_start`. Lehre: erst `mem_free`, dann den Deskriptor – das RETF
von mem_free lud 0328h neu (#GP, Fehlercode 0328h).

In ITEXT stehen Funktionen aus KTEXT2/KTEXT4, je Funktion per `#pragma
code_seg("ITEXT", "CODE")` und zurück auf das Segment der Datei: pnp.c
ganz außer `pnp_info` (DosBusInfo), cfgboot.c ganz, aus panic.c die
Startprüfung (`pn_boot`, `pd_prepare` …), `procfs_boot`, `fd_motor_off`,
`kinit_done` – 6,5 KB; KTEXT2 hat dadurch 3,2 KB, KTEXT4 2,6 KB mehr
Platz. Nicht verschoben, weil sie nah Laufzeit-Hilfen ihrer Datei rufen:
`mem_init` (`release_range`), `paging_init` (`put32`), `fd_init`
(`select`, `recalibrate`), `pci_init` (`scan_bus`), `vol_move`,
`list_drives` (`scopy`), `bios_attach` (`i13`) – dafür müssten die Hilfen
fern werden. Das erste Codesegment (kmain, config_sys … ~8 KB) braucht
Thunks für jeden nahen Aufruf: Stufe 2.

`initcheck.py` prüft beim Bau den ganzen Kernel (Wurzeln kmain,
gdt_build, preload): nur Startcode ruft ITEXT, niemand nimmt eine
Adresse dorthin, und jeder Aufruf über die Grenze von ITEXT ist fern
(Opcode 9Ah/EAh). Gleichnamige statische Funktionen verschiedener
Dateien hält es auseinander (`name@datei`). Wichtig: was der Compiler
einsetzt (inline), steht nicht in der Liste – ganze Dateien sind
sicherer als einzelne Funktionen (pnp.c `wr`, `rd`, `key`, `hx`).

Geprüft: QEMU (Start, F5, F8, MSD mit PnP-Liste, DEVLOAD, Box, MEM) und
86Box 486SP3C mit AWE32 PnP (ISA-PnP findet alle vier logischen Geräte,
SB16, MSD, fsck sauber).

## Stand Kernel, Stufe 2 (25.11.2026)

Der Startcode des ersten Segments liegt jetzt ebenfalls in ITEXT: `kmain`,
`config_sys`, `run_boot`, `cp_load`, `conv_pending`, `read_cfg`/
`next_line`/`cfg_release`, `find_boot_vol`, `cfg_nth`, `kw`, `num_arg`,
`fd_name`, `exec_err`, `ldt_init`, `pic_pit_init`, `udec`,
`blk_cache_init` – erstes Codesegment **65.423 → 59.164 Byte** (6,2 KB
frei), ITEXT 12,6 KB. Statt eigener Datei (2.1) stehen sie am Ende von
kernel.c hinter `#pragma code_seg("ITEXT", "CODE")` – so bleiben alle
statischen Variablen sichtbar. Davor: Thunks `kt_*` (fern, im ersten
Segment) für die 31 Laufzeitfunktionen, die der Startcode nah ruft, und
`#define name kt_name`, die die Aufrufe darauf umlenken.

- kernel.c wird mit `-oe=0` übersetzt (build.sh): sonst setzt `-ox` die
  Thunks ein und es entstehen wieder nahe Direktaufrufe über die Grenze
  (`#pragma inline_depth(0)` half nicht – Watcom erzeugt den Code erst
  am Dateiende). Das erste Segment wurde dadurch sogar etwas kleiner.
- `kmain` ist `__far`, start.asm ruft `call KCODEI:kmain_`.
- Ende des Starts über `kt_boot_end` im ersten Segment: erst
  `kinit_done`, dann `sched_start` – kinit_done darf nicht nach kmain
  zurückkehren, das liegt selbst in ITEXT.
- Beim Real-Mode-Teil (preload, gdt_build) bleibt es: er läuft mit
  CS = 1000h vor dem Umschalten.

Geprüft: QEMU (Start von Platte und Diskette/SETUP, F5, F8, PING,
MSD, DEVLOAD, XCOPY auf NCR8XX, Box, MEM, fsck sauber) und 86Box
386DX/40 mit AHA-154x und NE2000 (Start, XCOPY gleich, fsck sauber).

## Stand Kernel, Stufe 3 (25.11.2026)

- Startfunktionen aus KTEXT2 ebenfalls in ITEXT, je am Ende ihrer Datei
  hinter `#pragma code_seg("ITEXT", "CODE")`, davor ferne Thunks wie in
  kernel.c (Dateien mit Thunks mit `-oe=0`, build.sh): mem.c
  (`reserve_range`, `next_range`, `mem_init`; Thunk `release_range`),
  paging.c (`add_alias`, `alias_on`, `paging_init`, `paging_tables`;
  `put32`), floppy.c (`fd_init`; `ctl_reset`, `recalibrate`, `select`,
  `done`), bus.c (`scan_bus`, `pci_init`), k2misc.c (`list_drives` mit
  `scopy`/`udec`/`fd_name`), biosdisk.c (`bios_attach`; `i13`).
  `fd_motor_off` bleibt in KTEXT2 – `fd_tick` ruft es zur Laufzeit (in
  Stufe 1 hatte -ox die Kopie eingesetzt und das verdeckt; initcheck fand
  es). `vol_move` (fat.c) bleibt: Thunks und `-oe=0` kosteten im heißen
  Pfad von fat.c mehr, als ITEXT gewinnt. ITEXT jetzt 17,1 KB.
- MEM und MSD zählen das freigegebene Start-Segment nicht mehr zum Kernel
  (`kinit_pg` = freigegebene Seiten, k2misc.c `kinit_done`).
- Texte: sechste Gruppe CGROUPD (Segment IDATA, Klasse FAR_DATA) direkt
  hinter CGROUPI, Selektor KIDATA 330h (KCODE3_IDX + 7, NGDT + 1),
  `kidata_size` aus start.asm; kinit_done gibt ITEXT und IDATA zusammen
  frei. Texte der `vio_kputs`-Aufrufe im Startcode stehen als
  `static const char __based(__segname("IDATA")) ks_n[]` (kernel.c;
  k2misc.c `km_n`, cfgboot.c `kc_n`), ausgegeben über `ks_i` & Co. in
  32-Byte-Stücken über den Stapel (Startstapel knapp). DGROUP 62.118 ->
  61.468 Byte, IDATA 683 Byte. Lehre: `cond ? ks_1 : ks_2` ergibt bei
  Watcom einen `__based`-Zeiger, der beim Übergang zu `__far` das
  Segment aus DS nimmt (gelesen wurde DGROUP – „tet neu.)“ statt
  „■ DEVICE=“); beide Zweige auf `const char __far *` casten.
- Rest der Texte ebenfalls in IDATA (25.11.2026): `scopy(x, "…", n)` ->
  `fscopy`, `panic("…")` -> `pn_i` (kopiert nach `fi_buf`), `kw()` nimmt
  das Schlüsselwort fern, die Liste stiller CONFIG.SYS-Wörter ist eine
  Zeichenfolge mit 0 als Trenner (`ks_quiet`), LONGNAMES=/AUTOCHECK= aus
  `ks_lfn`, `exec_err`/`fd_name` liefern Fernzeiger; dazu die Texte im
  Startcode von k2misc.c (`list_drives`, scopy fern), cfgboot.c,
  biosdisk.c und panic.c (`pn_cat` fern). DGROUP 61.468 -> 59.926 Byte,
  IDATA 2.221 Byte. In DGROUP bleiben nur Kleinigkeiten: veränderbare
  `static char`-Felder (Dateinamen, die der Code beschreibt), Namen für
  env_find/read_cfg/vio_open, die drei Suchverzeichnisse von RUN= und
  `"NOTSTART=1"` – zusammen unter 100 Byte.

Geprüft: QEMU (Start von Platte und Diskette, Startmeldungen über DMESG,
F5, MEM „Kernel 290K“, XCOPY \DOS, `command /c ver`, CHKDSK, DIR A:
ohne Diskette; nach dem Rest der Texte: unbekanntes und stilles Wort in
der CONFIG.SYS, AUTOCHECK=, LONGNAMES=VFAT, Systemumgebung, Panik „Kein
Befehlsinterpreter.“ und PANIC.LOG beim nächsten Start) und 86Box
386DX/40 mit AHA-154x und NE2000.

## Stand: Seitengrenzen und KTEXTP (25.11.2026)

**Seitengrenze hinter den Starttexten.** kinit_done gibt nur ganze Seiten
frei; vorn teilt sich ITEXT eine Seite mit KTEXT4, hinten IDATA eine mit
DGROUP. Gemessen vorher: 19,3 KB Start-Segment, frei 12 KB (vorn 3.520,
hinten 3.965 Byte verschenkt). Statt `align=4096` (gilt für den Fall
„vorn bei 10000h“, der nicht gebaut wurde) füllt build.sh nach dem ersten
Binden das Ende von IDATA bis zur Seitengrenze auf (`IDATA_PAD`, start.asm
neu übersetzt, zweites Binden), aber nur, wenn der verschenkte Rest größer
ist als das Auffüllen – dann wird mehr frei, als ATDOS.SYS wächst
(DGROUP rückt um das Auffüllen nach hinten). Vorn aufzufüllen lohnt nicht:
der gewonnenen Seite steht fast dieselbe Menge Auffüllung gegenüber.

**KTEXTP bewertet.** Verwerfbarer Laufzeitcode, der bei #NP aus ATDOS.SYS
nachgeladen würde, bringt wenig: gemessen je Objekt (Codegröße) sind die
seltenen Teile entweder im Plattenweg (loop.c 12 KB, dem.c 9,7 KB, ufs.c
4 KB, swap.c 5 KB, codeswap.c 4 KB – das Nachladen bräuchte sie selbst,
oft unter gehaltener Sperre), jederzeit über FAPI erreichbar (vesa.c
4,4 KB, net.c, vdd.c – DEVLOAD kann später laden) oder klein (panic.c
0,7 KB). Übrig bleibt nur **PROCFS** (6,2 KB), das ab Werk aus ist und nur
über `SET PROCFS=` beim Start eingeschaltet wird. Dafür reicht der
einfache Weg ohne Nachladen: procfs.c wird mit `-nt=PTEXT` übersetzt,
PTEXT steht vorn in CGROUPI (vor ITEXT, Rahmen = Gruppe, `kptext_size`
aus start.asm). Ist PROCFS aus (`pfs_drive < 0`), gibt kinit_done PTEXT
mit dem Startcode frei; ist es an, beginnt das Freigeben hinter PTEXT und
KCODEI bleibt gültig (Texte per `MK_FP(KCODEI, …)`). Ein KTEXTP mit
Nachladen wird nicht gebaut.

Ergebnis (QEMU, 16 MB): MEM „Kernel“ 290K -> 279K (PROCFS aus), 287K mit
PROCFS; PROCFS geprüft mit DIR, TYPE VERSION/FILES/LOCKS, FIND über TASKS.

## Offene Punkte

- keine.
