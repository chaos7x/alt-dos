FreeDOS fuer die Kompatibilitaetsbox von AT-DOS 5.1
===================================================

KERNEL.SYS, COUNTRY.SYS, COMMAND.COM, MEM.EXE, MEM.DE, CTMOUSE.COM und
fat12com.bin stammen aus dem
FreeDOS-Projekt und stehen unter der GNU General Public License, Version 2
oder spaeter (siehe COPYING). Sie wurden aus dem Quelltext gebaut,
unveraendert:

  KERNEL.SYS    FreeDOS-Kern, https://github.com/FDOS/kernel, Stand d6791ad
                (Submodul country 7f83e04), Open Watcom, 8086 (die Box laeuft
                auch auf dem 286), FAT32, ohne UPX:
                  make all COMPILER=owlinux XCPU=86 XFAT=32 XUPX=
  fat12com.bin  FAT12-Bootsektor aus demselben Stand (boot/fat12com.bin)
  COUNTRY.SYS   Laenderdaten aus demselben Stand: nasm -o country.sys country.asm
  COMMAND.COM   FreeCOM, https://github.com/FDOS/freecom, Stand d52ba90,
                deutsch (Open Watcom erzeugt 8086-Code):
                  LNG=german ./build.sh wc
  MEM.EXE       FreeDOS MEM 1.11, https://github.com/FDOS/mem, Stand 2b2c833,
                Open Watcom, 8086, ohne UPX:
                  wcl -bt=dos -ms -0 -oahls -s -j mem.c prf.c kitten.c
  CTMOUSE.COM   CuteMouse 2.1b4 (Maustreiber), https://github.com/FDOS/mouse,
                Stand dd26852, deutsche Meldungen (ctm-de.msg), assembliert
                mit JWasm (github.com/Baron-von-Riedesel/JWasm 77ccfcd) und
                gelinkt mit wlink format dos com; drei Stellen angepasst,
                weil das heutige JWasm strenger ist (Strukturen mit <>, ein
                Feld aus 3 Strukturen, say mit Tabellenzeiger) - inhaltlich
                unveraendert. Neu bauen: tools/ctmouse.sh
  MEM.DE        deutsche Meldungen dazu (nls/mem.de); MEM liest sie ueber
                NLSPATH und LANG=DE, die RBSHELL in der Box setzt

Neu bauen: tools/freedos.sh (holt die Quellen per git und baut beide).

build.sh legt daraus das Startabbild der Box an (build/freedos.img, 360 KB,
auf dem Ziel \DOS\MDOS\FREEDOS.IMG): FreeDOS-Bootsektor, KERNEL.SYS,
COMMAND.COM, COUNTRY.SYS, RBSHELL.COM (apps/rbshell.asm) und CONFIG.SYS
aus diesem Ordner. RBOX ohne Argument startet es. COMMAND.COM, MEM.EXE
und MEM.DE liegen zusaetzlich in \DOS\MDOS: dort startet RBSHELL FreeCOM,
und der PATH der Box beginnt mit \DOS\MDOS - MEM in der Box ist das von
FreeDOS (wie \OS2\MDOS bei OS/2), MEM in AT-DOS das eigene. CTMOUSE.COM
laedt RBSHELL beim Start der Box, wenn AT-DOS eine Maus hat (mit /P bzw.
/S1, /S2 wie MOUSE.SYS): solange die Box vorn ist, gehoert ihr die Maus.

cp/CPnnn.FNT  Zeichensaetze fuer CODEPAGE=/CHCP von AT-DOS (850, 852, 857,
              858, 860, 863, 865, 866, 737): 8x16 und 8x8 aus den CPI-
              Dateien des FreeDOS-Pakets cpidos 3.0a (Henrique Peron,
              GNU GPL Version 2; gitlab.com/FreeDOS/base/cpidos, CPI/*.CPX,
              UPX-gepackt), dazu die Unicode-Tabelle 80h-FFh aus den
              Python-Codecs. Neu erzeugen: CPX mit `upx -d` entpacken, dann
              tools/mkcp.py <Ordner mit *.cpi> freedos/cp
