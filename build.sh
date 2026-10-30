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
wcc $CFLAGS -fo=$B/kernel.o kernel/kernel.c
wlink $LINK name $B/kernel.bin option map=$B/kernel.map \
      file $B/start.o file $B/kernel.o 2>&1 | grep -v W1014 || true
check_size $B/kernel.bin $B/kernel.map 32768 kernel

echo "Anwendungen"
nasm -f obj apps/crt0.asm -o $B/crt0.o
for t in a b c; do
    wcc $CFLAGS -fo=$B/task_$t.o apps/task_$t.c
    wlink $LINK name $B/task_$t.bin option map=$B/task_$t.map \
          file $B/crt0.o file $B/task_$t.o 2>&1 | grep -v W1014 || true
    check_size $B/task_$t.bin $B/task_$t.map 6144 task_$t   # Rest = Stack
done

echo "Image"
pad() { cp "$1" "$2"; truncate -s "$3" "$2"; }
pad $B/kernel.bin $B/kernel.pad 32768
for t in a b c; do pad $B/task_$t.bin $B/task_$t.pad 8192; done
cat $B/boot.bin $B/kernel.pad $B/task_a.pad $B/task_b.pad $B/task_c.pad > $B/dos5.img
truncate -s 1474560 $B/dos5.img
rm -f $B/*.pad
echo "  $B/dos5.img fertig"
