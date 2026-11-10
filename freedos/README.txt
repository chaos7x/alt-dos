FreeDOS fuer die Kompatibilitaetsbox von AT-DOS 5.1
===================================================

KERNEL.SYS, COUNTRY.SYS, COMMAND.COM und fat12com.bin stammen aus dem
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

Neu bauen: tools/freedos.sh (holt die Quellen per git und baut beide).

build.sh legt daraus das Startabbild der Box an (build/freedos.img, 360 KB,
auf dem Ziel \DOS\BOX\FREEDOS.IMG): FreeDOS-Bootsektor, KERNEL.SYS,
COMMAND.COM, COUNTRY.SYS, RBSHELL.COM (apps/rbshell.asm) und CONFIG.SYS
aus diesem Ordner. RBOX ohne Argument startet es.
