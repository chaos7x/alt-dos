# Entwicklungsumgebung für AT-DOS 7.0 (32 Bit)

Stand 15.11.2026. Nur Werkzeuge, noch kein 7.0-Code. Der 16-Bit-Bau
(`./build.sh`, Abschnitt „Bauen und testen“ in CLAUDE.md) bleibt davon
unberührt: das Skript legt nur neue Dateien außerhalb des Repositorys an
(`~/at32`, `~/.atdos32-env`) und nutzt dasselbe `~/watcom`.

Jede Cloud-Sitzung startet frisch, deshalb ein Skript statt einer Liste:

    tools/setup-32.sh            # Grundausstattung, ~3 min
    tools/setup-32.sh --all      # dazu GCC i686-elf und 86Box, ~30 min
    tools/setup-32.sh --check    # nur Rauchtests
    . ~/.atdos32-env             # WATCOM, INCLUDE, PATH

Am Ende laufen Rauchtests in einem Wegwerfverzeichnis: je Compiler ein
Objekt, ein Win32-PE, ein Multiboot-Kern aus GCC, den QEMU startet und
der „AT32“ auf Port E9h schreibt.

## Was und warum

| Werkzeug | Herkunft | Wofür |
|---|---|---|
| Open Watcom 2.0 (`wcc386`, `wlink`) | Snapshot wie CI | 32-Bit-Kernel mit derselben Aufrufkonvention und denselben Headern wie 16 Bit; wlink schreibt PE, LE, raw |
| GCC 14.2 + binutils 2.44, Ziel `i686-elf` | Quelltext (ftp.gnu.org), `--cross` | freistehender 32-Bit-Kernel (OSDev-Standard), Wine-Teile in GCC-C |
| MinGW-w64 (`i686-w64-mingw32-gcc`) | apt | Win32-PE-Testprogramme für den PE-Lader und Win32s/Win32 (GUI-WIN16.md), Wine-DLLs |
| `gcc -m32` | apt (gcc-multilib) | Hilfswerkzeuge und Tests auf dem Entwicklungsrechner |
| NASM, mtools, dosfstools | apt | wie 16 Bit |
| QEMU + OVMF | apt | Schnelltest; OVMF für den späteren UEFI-Start (7.0) |
| Bochs mit Debugger | apt | Einzelschritt, VME/PVI, `port_e9_hack` (siehe CLAUDE.md: `BIOS-bochs-legacy`) |
| gdb-multiarch | apt | an QEMU (`-s -S`) bzw. Bochs-gdbstub |
| grub-mkrescue, xorriso | apt | nur als Werkzeug für Multiboot-Testabbilder; GRUB-Code kommt nicht ins Projekt (GPLv3) |
| 86Box | Quelltext, `--86box` | echte Testmaschinen (86box/*.cfg); ROMs nur für P3B-F, 486SP3, asus386, arb1374 |
| pefile, Pillow | apt | PE-Dateien prüfen, Bildschirmabzüge |

Compiler für den 7.0-Kernel: noch offen (Entscheidung des Nutzers,
15.11.2026: beides einrichten, Wahl beim ersten Kernel-Code). Für
Watcom spricht die gemeinsame Konvention mit dem 16-Bit-Code, für GCC
das Wine-Erbe und der Crosscompiler-Weg ohne Linux-Annahmen.

## Hinweise

- `--cross` baut nur `all-gcc` und `libgcc` (kein libc). Version im Kopf
  des Skripts (`BINUTILS_V`, `GCC_V`).
- 86Box: Aufruf wie in CLAUDE.md („86Box in der Umgebung“), ROM-Pfad
  `$AT32/roms`. Weitere Boards per `git sparse-checkout add`.
- Ablage ändern: `AT32=/pfad tools/setup-32.sh`.
