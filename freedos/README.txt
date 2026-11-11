FreeDOS fuer die DOS-Boxen von AT-DOS 6.0
=========================================

KERNEL.SYS, COUNTRY.SYS, COMMAND.COM, MEM.EXE, MEM.DE und fat12com.bin stammen aus dem FreeDOS-Projekt
und stehen unter der GNU General Public License, Version 2 oder spaeter
(siehe COPYING). Sie wurden aus dem Quelltext gebaut, unveraendert:

  KERNEL.SYS    FreeDOS-Kern, https://github.com/FDOS/kernel, Stand d6791ad
                (Submodul country 7f83e04), Open Watcom, 386, FAT32, ohne UPX:
                  make all COMPILER=owlinux XCPU=386 XFAT=32 XUPX=
  fat12com.bin  FAT12-Bootsektor aus demselben Stand (boot/fat12com.bin)
  COUNTRY.SYS   Laenderdaten aus demselben Stand (Submodul country 7f83e04):
                  nasm -o country.sys country.asm
  COMMAND.COM   FreeCOM, https://github.com/FDOS/freecom, Stand d52ba90,
                deutsch:  LNG=german ./build.sh wc
  MEM.EXE       FreeDOS MEM 1.11, https://github.com/FDOS/mem, Stand 2b2c833,
                Open Watcom, 8086, ohne UPX:
                  wcl -bt=dos -ms -0 -oahls -s -j mem.c prf.c kitten.c
  MEM.DE        deutsche Meldungen dazu (nls/mem.de); MEM liest sie ueber
                NLSPATH und LANG=DE, die BOXSHELL in der Box setzt

Neu bauen: tools/freedos.sh (holt die Quellen per git und baut beide).

build.sh legt daraus das Startabbild der DOS-Box an (build/freedos.img,
360 KB, auf dem Ziel \DOS\MDOS\FREEDOS.IMG): FreeDOS-Bootsektor, KERNEL.SYS,
COMMAND.COM, COUNTRY.SYS, BOXSHELL.COM (apps/boxshell.asm) und CONFIG.SYS
aus diesem Ordner (COUNTRY=049,437: deutsches Datum und Zeit). COMMAND.COM, MEM.EXE und MEM.DE liegen in \DOS\MDOS (wie \OS2\MDOS bei OS/2;
der PATH der Box beginnt dort - MEM in der Box ist das von FreeDOS und zeigt
XMS, EMS und UMB der Box, MEM in AT-DOS das eigene):
nach dem Start braucht die Box das Abbild nicht mehr, ihre Laufwerke A:/B:
sind dann die Disketten von AT-DOS. Eine AUTOEXEC.BAT im Stamm des
Startlaufwerks fuehrt die interaktive Box aus.
