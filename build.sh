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
# Bootcode fuer FORMAT/SYS (UFAT.EXE, SYS.EXE) als C-Feld
python3 - $B/boot.bin $B/hdboot.bin > $B/bootcode.h <<'PYEOF'
import sys
for name, path in (("boot_fd", sys.argv[1]), ("boot_hd", sys.argv[2])):
    d = open(path, "rb").read()
    print("static const unsigned char %s[512] = {" % name)
    for i in range(0, 512, 16):
        print("    " + ", ".join("0x%02X" % b for b in d[i:i+16]) + ",")
    print("};")
PYEOF
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
# Code (CGROUP) und Daten (DGROUP) sind getrennte Segmente: als DOS-EXE
# linken, damit Datenoffsets relativ zu DGROUP sind, dann den Kopf abtrennen
wlink option quiet format dos option nodefaultlibs \
      option map order clname CODE clname DATA clname BSS \
      name $B/kernel.exe option map=$B/kernel.map file $KOBJ 2>&1 | grep -v W1014 || true
python3 - $B/kernel.exe $B/kernel.bin $B/kernel.map <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
nrel, hdr = struct.unpack('<HH', d[6:10])
if nrel:
    sys.exit('FEHLER: Kernel enthaelt %d Segment-Relokationen' % nrel)
open(sys.argv[2], 'wb').write(d[hdr * 16:])
g = {}
for l in open(sys.argv[3]):
    f = l.split()
    if len(f) == 3 and f[0] in ('CGROUP', 'DGROUP') and ':' in f[1]:
        s, o = f[1].split(':')
        g[f[0]] = (int(s, 16) * 16 + int(o, 16), int(f[2], 16))
code, data = g['CGROUP'][1], g['DGROUP'][1]
if g['DGROUP'][0] != (code + 15) & ~15:
    sys.exit('FEHLER: DGROUP liegt nicht direkt hinter dem Code')
if code > 65536 or data > 65536:
    sys.exit('FEHLER: Kernel-Code %d oder -Daten %d Bytes ueber 64 KB' % (code, data))
print('  kernel: %d Bytes Datei, Code %d Bytes, Daten %d Bytes mit BSS (je max. 65536)'
      % (len(d) - hdr * 16, code, data))
PYEOF
# der Bootsektor laedt 128 Sektoren; BSS dahinter nullt der Kernel selbst
[ $(stat -c %s $B/kernel.bin) -le 65536 ] || { echo "FEHLER: ATDOS.SYS groesser als 64 KB"; exit 1; }

echo "Anwendungen"
nasm -f obj apps/crt0.asm -o $B/crt0.o
# Werkzeuge mit gemeinsamer Hilfsbibliothek (apps/util.c)
UTILAPPS="attrib move deltree tree label choice sort fc xcopy format sys edit mode mtest"
wcc $CFLAGS -fo=$B/util.o apps/util.c
for t in command keyb a b c mem more find grep less chkdsk ufat $UTILAPPS; do
    U=""
    case " $UTILAPPS " in *" $t "*) U="file $B/util.o" ;; esac
    wcc $CFLAGS -fo=$B/task_$t.o apps/task_$t.c
    wlink $LINK name $B/task_$t.bin option map=$B/task_$t.map \
          file $B/crt0.o file $B/task_$t.o $U 2>&1 | grep -v W1014 || true
done

echo "Programme (FX mit DOS-Stub)"
nasm -f bin tools/stub.asm -o $B/stub.bin
for t in command keyb a b c mem more find grep less chkdsk ufat $UTILAPPS; do
    python3 tools/mkfx.py $B/stub.bin $B/task_$t.bin $B/task_$t.map $B/task_$t.exe
done

echo "Treiber"
nasm -f obj drivers/dcrt0.asm -o $B/dcrt0.o
for t in aha154x ide mouse; do
    wcc $CFLAGS -zu -fo=$B/drv_$t.o drivers/$t.c
    wlink $LINK name $B/drv_$t.bin option map=$B/drv_$t.map \
          file $B/dcrt0.o file $B/drv_$t.o 2>&1 | grep -v W1014 || true
    python3 tools/mkfx.py --driver $B/stub.bin $B/drv_$t.bin $B/drv_$t.map $B/drv_$t.sys
done

echo "Dateisysteme (IFS)"
for t in iso9660; do
    wcc $CFLAGS -zu -fo=$B/ifs_$t.o ifs/$t.c
    wlink $LINK name $B/ifs_$t.bin option map=$B/ifs_$t.map \
          file $B/dcrt0.o file $B/ifs_$t.o 2>&1 | grep -v W1014 || true
    python3 tools/mkfx.py --driver $B/stub.bin $B/ifs_$t.bin $B/ifs_$t.map $B/ifs_$t.ifs
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
# auf 64 KB aufgefuellt, damit der Bootsektor nur Nullen als BSS nachlaedt
cp $B/kernel.bin $B/ATDOS.SYS
truncate -s 65536 $B/ATDOS.SYS
mcopy -i $IMG $B/ATDOS.SYS ::ATDOS.SYS
mattrib -i $IMG +r +s +h ::ATDOS.SYS
# alle uebrigen Dateien: Quelle Ziel (Stamm), dann \DOS
FILES="$B/task_command.exe COMMAND.EXE
       CONFIG.SYS CONFIG.SYS  AUTOEXEC.CMD AUTOEXEC.CMD"
DOSFILES="$B/task_keyb.exe KEYB.EXE  $B/task_mem.exe MEM.EXE
       $B/task_more.exe MORE.EXE  $B/task_find.exe FIND.EXE  $B/task_grep.exe GREP.EXE
       $B/task_less.exe LESS.EXE  $B/task_chkdsk.exe CHKDSK.EXE
       $B/task_attrib.exe ATTRIB.EXE  $B/task_move.exe MOVE.EXE  $B/task_deltree.exe DELTREE.EXE
       $B/task_tree.exe TREE.EXE  $B/task_label.exe LABEL.EXE  $B/task_choice.exe CHOICE.EXE
       $B/task_sort.exe SORT.EXE  $B/task_fc.exe FC.EXE  $B/task_xcopy.exe XCOPY.EXE
       $B/task_format.exe FORMAT.EXE  $B/task_sys.exe SYS.EXE
       $B/task_edit.exe EDIT.EXE  $B/task_mode.exe MODE.EXE"
# \DOS\SYSTEM: was das System nachlaedt (Treiber, IFS, Pruefprogramme, Belegungen)
SYSFILES="$B/drv_aha154x.sys AHA154X.SYS  $B/drv_ide.sys IDE.SYS  $B/drv_mouse.sys MOUSE.SYS
       $B/ifs_iso9660.ifs ISO9660.IFS  $B/task_ufat.exe UFAT.EXE
       $B/US.KBL US.KBL  $B/GB.KBL GB.KBL"
DEMOFILES="$B/task_a.exe TASKA.EXE  $B/task_b.exe TASKB.EXE  $B/task_c.exe TASKC.EXE
       $B/task_mtest.exe MAUSTEST.EXE"
copy_files() {   # Ziel-Laufwerk fuer mcopy, z. B. "-i dos5.img ::"
    copy_list "$FILES" ""
    mmd $TARGET "${TO}DOS" "${TO}DOS/SYSTEM" "${TO}DEMO"
    copy_list "$DOSFILES" "DOS/"
    copy_list "$SYSFILES" "DOS/SYSTEM/"
    copy_list "$DEMOFILES" "DEMO/"
}
copy_list() {
    sub=$2
    set -- $1
    while [ $# -gt 0 ]; do
        mcopy -o $TARGET "$1" "$TO$sub$2"
        shift 2
    done
}
TARGET="-i $IMG" TO="::" copy_files
mcopy -i $IMG $B/atsys.com ::ATSYS.COM
mdir -a -i $IMG :: | sed 's/^/  /'
mdir -i $IMG ::DOS | sed 's/^/  /'

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
mdir c:DOS | sed 's/^/  /'
unset MTOOLSRC
rm -f $B/ATDOS.SYS
