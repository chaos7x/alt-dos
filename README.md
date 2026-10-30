# alt-dos – DOS 5.0, Kernel-Meilenstein 3

Alternative Zeitlinie: MT-DOS 4.0 wird fortgeführt, DOS 5.0 läuft im
80286 Protected Mode, segmentiert, mit kooperativem Multitasking. Hardware
ist nur über die Family API (FAPI) erreichbar.

## Was läuft

- Standard-FAT12-Diskette: Der Kernel ist die Datei `KERNEL.SYS` (versteckt,
  System), die wie einst `IO.SYS` als erste Datei zusammenhängend liegt
- Diskettentreiber im Kernel (Controller 765/82077, DMA-Kanal 2, IRQ 6) –
  im Protected Mode gibt es kein BIOS mehr
- IFS-Schicht mit FAT12 als erstem Dateisystemtreiber
- Programmlader: `STARTUP.CMD` nennt bis zu vier Programme, die als Dateien
  geladen werden
- A20 über den Tastaturcontroller, GDT/IDT, Wechsel per `LMSW`
- PIC auf 20h/28h umgelegt, Systemtakt 100 Hz
- FAPI-Call-Gate (Selektor 0023h) von Ring 3 nach Ring 0
- je Task ein 286-TSS und eine eigene LDT, Ring 3, IOPL 0
- Speicherverwaltung über die vollen 16 MB in 256-Byte-Einheiten,
  Segmente per `DosAllocSeg`/`DosFreeSeg`, Aufräumen bei Task-Ende
- kooperativer Scheduler mit Hardware-Taskwechsel (`JMP` auf TSS)
- Ausnahmen in Anwendungen beenden nur den Task, nicht das System

Demo-Tasks: **A** zählt und gibt per `DosYield()` ab, **B** fordert zehn
Segmente zu 64 KB an, prüft sie, gibt die Hälfte frei und endet – den Rest
räumt der Kernel auf, **C** versucht direkt nach B800:0000
zu schreiben und wird per #GP beendet, **MEM.EXE** zeigt den Arbeitsspeicher laufend
an. Alle vier liegen als `TASKA.EXE` … `MEM.EXE` auf der Diskette.

Die Programme sind noch reine Binärabbilder (Code und Daten in einem
Segment, Einsprung bei 0), kein MZ- oder NE-Format.

## FAPI (Stand Meilenstein 3)

| AX | Funktion        | Argumente                       |
|----|-----------------|---------------------------------|
| 0  | DosGetVersion   | → AX = 0500h                    |
| 1  | VioWrtStr       | BX = String, CX = Zeile<<8\|Spalte |
| 2  | DosYield        | –                               |
| 3  | DosExit         | –                               |
| 4  | DosSleep        | BX = Millisekunden              |
| 5  | DosMemInfo      | BX = `struct fapi_meminfo` (KB) |
| 6  | DosAllocSeg     | BX = Größe, 0 = 64 KB → AX = Selektor |
| 7  | DosFreeSeg      | BX = Selektor                   |

## Speicher

| linear           | Inhalt                          |
|------------------|---------------------------------|
| 08000h – 081FFh  | DMA-Puffer für Laufwerke        |
| 10000h – 17FFFh  | Kernel (ein 64-KB-Segment)      |
| 18000h – 9FFFFh  | frei, Speicherverwaltung        |
| A0000h – FFFFFh  | Systembereich (Video, ROMs)     |
| ab 100000h       | frei, Speicherverwaltung        |

## Bauen

Benötigt NASM, mtools und Open Watcom 2.0 (`WATCOM` gesetzt, `wcc`/`wlink`
im PATH):

    ./build.sh      # -> build/dos5.img (1,44 MB, FAT12)

Das Image ist eine normale FAT12-Diskette und lässt sich unter MS-DOS, mit
mtools oder unter jedem anderen System lesen. `KERNEL.SYS` ist versteckt
(`DIR /A` zeigt sie) und muss die erste Datei bleiben; beim Austauschen
also nicht einfach überschreiben, sondern das Image neu bauen.

In 86Box als Diskette A: einlegen, beliebige 286-Maschine mit VGA.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
