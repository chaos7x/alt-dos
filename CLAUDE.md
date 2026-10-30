# alt-dos

Hobby-Betriebssystem aus einer alternativen Zeitlinie: Microsoft führt das
Multitasking-DOS 4.0 (1986) als offizielle DOS-Linie fort, statt auf OS/2 zu
setzen. Spezifikation der ganzen Zeitlinie (DOS 5.0–9.0, FAPI, NT-Linie):
https://claude.ai/code/artifact/43b28dce-1011-46c8-ad1a-e3a2255adff8

Sprache im Projekt: Deutsch (Kommentare, Commits, Doku).

## Aktueller Stand: DOS 5.0 (80286, ~1988/89)

- **Alles 16 Bit.** Kein 32-Bit-Code, kein Flat Memory – das kommt erst mit DOS 7.0.
- **Nur 286-Befehle.** NASM-Dateien beginnen mit `cpu 286`, Watcom baut mit `-2`.
  Keine 386-Präfixe (66h/67h), kein Paging, kein V86.
- **Protected Mode, segmentiert**, Adressraum bis 16 MB.
- **Kooperatives Multitasking.** Kein präemptiver Scheduler vor DOS 6.0.
- **FAPI ist der einzige Weg zur Hardware.** Anwendungen laufen in Ring 3 mit
  IOPL 0 und haben keine Selektoren auf Hardware-Speicher. Nur der Kernel
  spricht Hardware an. Nie eine Abkürzung an der FAPI vorbei einbauen.
- **IFS** (Installable File System) ist Teil der FAPI-Architektur; Dateisysteme
  sind ladbare Treiber, kein fest eingebautes FAT.

Was zu einer späteren Version gehört (präemptiv, Paging/Swap, V86, Journaling-FS,
Win32s), nicht vorziehen, sondern nachfragen.

## Aufbau

- `boot/` – Bootsektor, lädt das Image nach 1000:0000
- `kernel/` – `start.asm` (Einstieg, Gate, ISRs, CPU-Hilfsroutinen), `kernel.c`
- `apps/` – Ring-3-Anwendungen, `crt0.asm`, `fapi.h`
- `include/fapi_abi.h` – FAPI-Funktionsnummern, gemeinsam für Kernel und Apps

## Konventionen

- Kernel und jede App sind je **ein** 64-KB-Segment: `_TEXT` liegt mit in
  DGROUP (siehe Segmentkopf in `start.asm`/`crt0.asm`), gelinkt als
  `wlink format raw bin offset=0`.
- Watcom-Registerkonvention: Argumente in AX, DX, BX, CX; Assembler-Routinen
  erhalten alle Register außer dem Rückgabewert (AX bzw. DX:AX).
- Kein C-Laufzeitsystem (`-zl`): keine Bibliotheksaufrufe, keine
  32-Bit-Division/-Multiplikation (würde `__U4D` etc. ziehen), keine
  Struct-Zuweisungen.
- FAPI-Aufruf: `call far 0023:0000`, AX = Funktion, BX/CX = Argumente.
  Neue FAPI-Funktionen immer in `include/fapi_abi.h` und im README eintragen.
- Bildschirmtexte in CP437: Umlaute als Escape (`\x84` ä, `\x94` ö, `\x81` ü,
  `\xE1` ß), auf folgende Hex-Ziffern achten.

## Bauen und testen

    ./build.sh          # NASM + Open Watcom -> build/dos5.img

Referenzmaschine in 86Box: 286er mit SCAT-Chipsatz (C&T 82C235), 16 MB RAM,
Tseng ET4000/W32i, Diskette A: 3,5" 1,44 MB. Das SCAT-BIOS meldet 640 KB +
15232 KB, es fehlen 128 KB zu 16 MB.

Schnelltest headless in QEMU (`qemu-system-i386 -fda build/dos5.img`), der
echte Test ist eine 286-Maschine in 86Box. QEMU emuliert keinen echten 286,
deshalb 286-Kompatibilität zusätzlich am Code prüfen.

CI baut bei jedem Push und veröffentlicht `dos5.img` als Release `latest`.
