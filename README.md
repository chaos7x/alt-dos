# alt-dos – DOS 5.0, Kernel-Meilenstein 1

Alternative Zeitlinie: MT-DOS 4.0 wird fortgeführt, DOS 5.0 läuft im
80286 Protected Mode, segmentiert, mit kooperativem Multitasking. Hardware
ist nur über die Family API (FAPI) erreichbar.

## Was läuft

- Bootsektor lädt Kernel + Anwendungen von Diskette nach 1000:0000
- A20 über den Tastaturcontroller, GDT/IDT, Wechsel per `LMSW`
- PIC auf 20h/28h umgelegt, Systemtakt 100 Hz
- FAPI-Call-Gate (Selektor 0023h) von Ring 3 nach Ring 0
- je Task ein 286-TSS, eigenes Code-/Datensegment in Ring 3, IOPL 0
- kooperativer Scheduler mit Hardware-Taskwechsel (`JMP` auf TSS)
- Ausnahmen in Anwendungen beenden nur den Task, nicht das System

Demo-Tasks: **A** zählt und gibt per `DosYield()` ab, **B** schläft zehnmal
mit `DosSleep(1000)` und endet regulär, **C** versucht direkt nach B800:0000
zu schreiben und wird per #GP beendet, **MEM.EXE** zeigt den vom BIOS
gemeldeten Arbeitsspeicher und beendet sich. Einen Programmlader gibt es noch nicht,
alle vier sind fest ins Image gebaut.

## FAPI (Stand Meilenstein 1)

| AX | Funktion        | Argumente                       |
|----|-----------------|---------------------------------|
| 0  | DosGetVersion   | → AX = 0500h                    |
| 1  | VioWrtStr       | BX = String, CX = Zeile<<8\|Spalte |
| 2  | DosYield        | –                               |
| 3  | DosExit         | –                               |
| 4  | DosSleep        | BX = Millisekunden              |
| 5  | DosMemInfo      | BX = `struct fapi_meminfo` (KB) |

## Speicher

| linear           | Inhalt                          |
|------------------|---------------------------------|
| 10000h – 17FFFh  | Kernel (ein 64-KB-Segment)      |
| 18000h – 1FFFFh  | Anwendungen A, B, C, MEM (je 8 KB) |
| B8000h           | VGA-Text (nur Kernel-Selektor)  |

## Bauen

Benötigt NASM und Open Watcom 2.0 (`WATCOM` gesetzt, `wcc`/`wlink` im PATH):

    ./build.sh      # -> build/dos5.img (1,44 MB)

In 86Box als Diskette A: einlegen, beliebige 286-Maschine mit VGA.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
