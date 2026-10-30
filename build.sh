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

echo "Bootsektor"
nasm -f bin boot/boot.asm -o $B/boot.bin

echo "Kernel"
nasm -f obj kernel/start.asm -o $B/start.o
KOBJ="$B/start.o"
for m in kernel mem floppy fat12; do
    wcc $CFLAGS -fo=$B/$m.o kernel/$m.c
    KOBJ="$KOBJ file $B/$m.o"
done
wlink $LINK name $B/kernel.bin option map=$B/kernel.map \
      file $KOBJ 2>&1 | grep -v W1014 || true
check_size $B/kernel.bin $B/kernel.map 32768 kernel

echo "Anwendungen"
nasm -f obj apps/crt0.asm -o $B/crt0.o
for t in a b c mem; do
    wcc $CFLAGS -fo=$B/task_$t.o apps/task_$t.c
    wlink $LINK name $B/task_$t.bin option map=$B/task_$t.map \
          file $B/crt0.o file $B/task_$t.o 2>&1 | grep -v W1014 || true
    check_size $B/task_$t.bin $B/task_$t.map 6144 task_$t   # Rest = Stack
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
# KERNEL.SYS als erste Datei, damit sie zusammenhaengend ab Cluster 2 liegt;
# auf 32 KB aufgefuellt, damit der Bootsektor nur Nullen als BSS nachlaedt
cp $B/kernel.bin $B/KERNEL.SYS
truncate -s 32768 $B/KERNEL.SYS
mcopy -i $IMG $B/KERNEL.SYS ::KERNEL.SYS
mattrib -i $IMG +r +s +h ::KERNEL.SYS
mcopy -i $IMG $B/task_a.bin ::TASKA.EXE
mcopy -i $IMG $B/task_b.bin ::TASKB.EXE
mcopy -i $IMG $B/task_c.bin ::TASKC.EXE
mcopy -i $IMG $B/task_mem.bin ::MEM.EXE
mcopy -i $IMG STARTUP.CMD ::STARTUP.CMD
rm -f $B/KERNEL.SYS
mdir -a -i $IMG :: | sed 's/^/  /'
