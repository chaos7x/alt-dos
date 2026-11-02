FreeDOS fuer die DOS-Boxen von AT-DOS 6.0
=========================================

KERNEL.SYS, COMMAND.COM und fat12com.bin stammen aus dem FreeDOS-Projekt
und stehen unter der GNU General Public License, Version 2 oder spaeter
(siehe COPYING). Sie wurden aus dem Quelltext gebaut, unveraendert:

  KERNEL.SYS    FreeDOS-Kern, https://github.com/FDOS/kernel, Stand d6791ad
                (Submodul country 7f83e04), Open Watcom, 386, FAT32, ohne UPX:
                  make all COMPILER=owlinux XCPU=386 XFAT=32 XUPX=
  fat12com.bin  FAT12-Bootsektor aus demselben Stand (boot/fat12com.bin)
  COMMAND.COM   FreeCOM, https://github.com/FDOS/freecom, Stand d52ba90,
                deutsch:  LNG=german ./build.sh wc

Neu bauen: tools/freedos.sh (holt die Quellen per git und baut beide).

build.sh legt daraus das Startabbild der DOS-Box an (build/freedos.img,
360 KB, auf dem Ziel \DOS\BOX\FREEDOS.IMG): FreeDOS-Bootsektor, KERNEL.SYS,
COMMAND.COM, BOXSHELL.COM (apps/boxshell.asm), CONFIG.SYS und AUTOEXEC.BAT
aus diesem Ordner.
