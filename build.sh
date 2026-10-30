#!/bin/sh
# Baut build/dos5.img (1,44-MB-Diskette): Bootsektor + Kernel + 3 Anwendungen
# Benoetigt: nasm, Open Watcom 2.0 (WATCOM gesetzt, wcc/wlink im PATH)
set -e
cd "$(dirname "$0")"
B=build
mkdir -p $B

CFLAGS="-q -ms -2 -s -zl -zls -zp1 -d0 -ox -wx -we"
LINK="option quiet format raw bin option offset=0 option nodefaultlibs \
      option map order clname CODE clname DATA clname BSS"

# Ende von DGROUP (inkl. BSS) aus der Map-Datei
group_end() {
    python3 -c "import sys
e=0
for l in open(sys.argv[1]):
    f=l.split()
    if len(f)>=5 and f[1]=='BSS' and f[2]=='DGROUP': e=int(f[3],16)+int(f[4],16)
print(e)" "$1"
}

check_size() {   # Datei, Map, Maximum, Name
    end=$(group_end "$2")
    [ "$end" -le "$3" ] || { echo "FEHLER: $4 belegt $end Bytes, erlaubt $3"; exit 1; }
    echo "  $4: $(stat -c %s "$1") Bytes Datei, $end Bytes mit BSS (max. $3)"
}

echo "Bootsektoren"
nasm -f bin boot/boot.asm -o $B/boot.bin
nasm -f bin boot/hdboot.asm -o $B/hdboot.bin
nasm -f bin boot/mbr.asm -o $B/mbr.bin
nasm -f bin tools/atsys.asm -o $B/atsys.com     # braucht hdboot.bin

echo "Tastaturbelegungen"
python3 tools/mkkbl.py $B

echo "Kernel"
nasm -f obj kernel/start.asm -o $B/start.o
KOBJ="$B/start.o"
for m in kernel mem floppy fat vio kbd preload; do
    wcc $CFLAGS -fo=$B/$m.o kernel/$m.c
    KOBJ="$KOBJ file $B/$m.o"
done
wlink $LINK name $B/kernel.bin option map=$B/kernel.map \
      file $KOBJ 2>&1 | grep -v W1014 || true
check_size $B/kernel.bin $B/kernel.map 65536 kernel
# der Bootsektor laedt 64 Sektoren; BSS dahinter nullt der Kernel selbst
[ $(stat -c %s $B/kernel.bin) -le 32768 ] || { echo "FEHLER: ATDOS.SYS groesser als 32 KB"; exit 1; }

echo "Anwendungen"
nasm -f obj apps/crt0.asm -o $B/crt0.o
for t in command keyb a b c mem; do
    wcc $CFLAGS -fo=$B/task_$t.o apps/task_$t.c
    wlink $LINK name $B/task_$t.bin option map=$B/task_$t.map \
          file $B/crt0.o file $B/task_$t.o 2>&1 | grep -v W1014 || true
done

echo "Programme (FX mit DOS-Stub)"
nasm -f bin tools/stub.asm -o $B/stub.bin
for t in command keyb a b c mem; do
    python3 tools/mkfx.py $B/stub.bin $B/task_$t.bin $B/task_$t.map $B/task_$t.exe
done

echo "Treiber"
nasm -f obj drivers/dcrt0.asm -o $B/dcrt0.o
for t in aha154x ide; do
    wcc $CFLAGS -zu -fo=$B/drv_$t.o drivers/$t.c
    wlink $LINK name $B/drv_$t.bin option map=$B/drv_$t.map \
          file $B/dcrt0.o file $B/drv_$t.o 2>&1 | grep -v W1014 || true
    python3 tools/mkfx.py --driver $B/stub.bin $B/drv_$t.bin $B/drv_$t.map $B/drv_$t.sys
done

echo "Diskette (FAT12)"
IMG=$B/dos5.img
export MTOOLS_SKIP_CHECK=1
truncate -s 0 $IMG
truncate -s 1474560 $IMG
dd if=$B/boot.bin of=$IMG conv=notrunc status=none
# beide FATs initialisieren: Medienbyte F0h, Cluster 1 = Ende
for fat in 1 10; do
    printf '\360\377\377' | dd of=$IMG bs=1 seek=$((fat * 512)) conv=notrunc status=none
done
# ATDOS.SYS als erste Datei, damit sie zusammenhaengend ab Cluster 2 liegt;
# auf 32 KB aufgefuellt, damit der Bootsektor nur Nullen als BSS nachlaedt
cp $B/kernel.bin $B/ATDOS.SYS
truncate -s 32768 $B/ATDOS.SYS
mcopy -i $IMG $B/ATDOS.SYS ::ATDOS.SYS
mattrib -i $IMG +r +s +h ::ATDOS.SYS
# alle uebrigen Dateien: Quelle Ziel
FILES="$B/task_command.exe COMMAND.EXE  $B/task_keyb.exe KEYB.EXE
       $B/US.KBL US.KBL  $B/GB.KBL GB.KBL  $B/task_a.exe TASKA.EXE
       $B/task_b.exe TASKB.EXE  $B/task_c.exe TASKC.EXE  $B/task_mem.exe MEM.EXE
       STARTUP.CMD STARTUP.CMD  CONFIG.SYS CONFIG.SYS
       $B/drv_aha154x.sys AHA154X.SYS  $B/drv_ide.sys IDE.SYS"
copy_files() {   # Ziel-Laufwerk fuer mcopy, z. B. "-i dos5.img ::"
    set -- $FILES
    while [ $# -gt 0 ]; do
        mcopy -o $TARGET "$1" "$TO$2"
        shift 2
    done
}
TARGET="-i $IMG" TO="::" copy_files
mcopy -i $IMG $B/atsys.com ::ATSYS.COM
mdir -a -i $IMG :: | sed 's/^/  /'

echo "Festplatte (atdos-hd.img, 130/16/63 = 63 MB, FAT16)"
HD=$B/atdos-hd.img
rm -f $HD
truncate -s $((130 * 16 * 63 * 512)) $HD
printf 'drive c: file="%s" partition=1\nmtools_skip_check=1\n' "$PWD/$HD" > $B/mtoolsrc
export MTOOLSRC=$B/mtoolsrc
mpartition -I -B $B/mbr.bin c: 2>/dev/null
mpartition -c -t 130 -h 16 -s 63 -a c: 2>/dev/null
mformat -H 63 -v AT-DOS c:
python3 tools/sysboot.py $HD $((63 * 512)) $B/hdboot.bin
mcopy $B/ATDOS.SYS c:ATDOS.SYS
mattrib +r +s +h c:ATDOS.SYS
TARGET="" TO="c:" copy_files
mdir -a c: | sed 's/^/  /'
unset MTOOLSRC
rm -f $B/ATDOS.SYS
