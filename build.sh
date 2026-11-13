#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
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

# Build-Nummer fuer VER: Zweig 5.x zaehlt wie Windows 10 (19045.n) nur
# hinter dem Punkt weiter - 240 ist der CI-Lauf von AT-DOS 5.0 (8d7f54f),
# dahinter die Anzahl der Commits seither; lokal mit Zusatz "lokal"
BUILDBASE=240
BUILDN=$(git rev-list --count 8d7f54f..HEAD 2>/dev/null || echo "?")
BUILDNO="$BUILDBASE.$BUILDN"
[ -n "$GITHUB_RUN_NUMBER" ] || BUILDNO="$BUILDNO lokal"
BUILDREV=$(git rev-parse --short HEAD 2>/dev/null || echo unbekannt)
git diff --quiet HEAD 2>/dev/null || BUILDREV="$BUILDREV+"
cat > $B/version.h <<VEREOF
/* erzeugt von build.sh */
#define ATDOS_BUILD "$BUILDNO"
#define ATDOS_REV   "$BUILDREV"
#define ATDOS_DATE  "$(date +%d.%m.%Y)"
VEREOF

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
# Startcode des MBR (ohne Partitionstabelle) fuer FDISK
python3 - $B/mbr.bin > $B/mbrcode.h <<'PYEOF'
import sys
d = open(sys.argv[1], "rb").read()[:446]
print("static const unsigned char mbr_code[446] = {")
for i in range(0, 446, 16):
    print("    " + ", ".join("0x%02X" % b for b in d[i:i+16]) + ",")
print("};")
PYEOF
nasm -f bin tools/atsys.asm -o $B/atsys.com     # braucht hdboot.bin

echo "Tastaturbelegungen"
python3 tools/mkkbl.py $B

echo "Kernel"
nasm -f obj kernel/start.asm -o $B/start.o
KOBJ="$B/start.o"
# KTEXT2: Kernelteile im zweiten Codesegment (KCODE2, siehe kernel/k2.h)
K2MODS="floppy vgagfx rbox fat"
for m in kernel mem floppy fat vio kbd preload vgagfx rbox k2thunk; do
    S=""
    case " $K2MODS " in *" $m "*) S="-nt=KTEXT2 -os" ;; esac   # selten: klein statt schnell
    wcc $CFLAGS $S -fo=$B/$m.o kernel/$m.c
    KOBJ="$KOBJ file $B/$m.o"
done
# Code (CGROUP) und Daten (DGROUP) sind getrennte Segmente: als DOS-EXE
# linken, damit Datenoffsets relativ zu DGROUP sind, dann den Kopf abtrennen
wlink option quiet format dos option nodefaultlibs \
      option map order clname CODE clname DATA clname BSS \
      name $B/kernel.exe option map=$B/kernel.map file $KOBJ 2>&1 | grep -v W1014 || true
python3 - $B/kernel.exe $B/kernel.bin $B/kernel.map <<'PYEOF'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
nrel, hdr, relo = struct.unpack('<H', d[6:8])[0], struct.unpack('<H', d[8:10])[0], \
                  struct.unpack('<H', d[0x18:0x1A])[0]
g = {}
for l in open(sys.argv[3]):
    f = l.split()
    if len(f) == 3 and f[0] in ('CGROUP', 'CGROUP2', 'DGROUP') and ':' in f[1]:
        s, o = f[1].split(':')
        g[f[0]] = (int(s, 16) * 16 + int(o, 16), int(f[2], 16))
code, data = g['CGROUP'][1], g['DGROUP'][1]
c2 = g.get('CGROUP2', ((code + 15) & ~15, 0))
if c2[0] != (code + 15) & ~15:
    sys.exit('FEHLER: CGROUP2 liegt nicht direkt hinter CGROUP')
if g['DGROUP'][0] != (c2[0] + c2[1] + 15) & ~15:
    sys.exit('FEHLER: DGROUP liegt nicht direkt hinter dem Code')
# Fernaufrufe zwischen CGROUP und CGROUP2: der Kernel liegt immer bei
# 10000h, also die Segmentwerte gleich als Selektoren eintragen
# (KCODE 08h, KCODE2 1C8h, siehe kern.h); alles andere ist ein Fehler
img = hdr * 16
for i in range(nrel):
    o, s = struct.unpack('<HH', d[relo + 4 * i:relo + 4 * i + 4])
    at = img + s * 16 + o
    v = struct.unpack('<H', d[at:at + 2])[0]
    if v == 0:
        sel = 0x08
    elif v == c2[0] >> 4:
        sel = 0x1C8
    else:
        sys.exit('FEHLER: Segment-Relokation auf %04Xh (nur KCODE/KCODE2 erlaubt)' % v)
    d[at:at + 2] = struct.pack('<H', sel)
open(sys.argv[2], 'wb').write(d[img:])
if code > 65536 or c2[1] > 65536 or data > 65536:
    sys.exit('FEHLER: Kernel-Code %d/%d oder -Daten %d Bytes ueber 64 KB' % (code, c2[1], data))
print('  kernel: %d Bytes Datei, Code %d + %d Bytes, Daten %d Bytes mit BSS (je max. 65536), %d Fernaufrufe'
      % (len(d) - img, code, c2[1], data, nrel))
PYEOF
# der Diskettenbootsektor laedt 160 Sektoren; BSS dahinter nullt der Kernel selbst
[ $(stat -c %s $B/kernel.bin) -le 458752 ] || { echo "FEHLER: ATDOS.SYS groesser als 448 KB"; exit 1; }

echo "Anwendungen"
nasm -f obj apps/crt0.asm -o $B/crt0.o
# Programme (FX-Version 2): Code und Daten getrennt, als DOS-EXE gelinkt
# wie der Kernel (gruppenrelative Offsets), mkfx.py nimmt den Kopf ab
ALINK="option quiet format dos option nodefaultlibs \
      option map order clname CODE clname BEGDATA clname DATA clname BSS"
# Werkzeuge mit gemeinsamer Hilfsbibliothek (apps/util.c)
UTILAPPS="attrib move deltree tree label choice sort fc xcopy format sys edit mode mtest grafik fputest fdisk diskcopy subst join ansi fm diskcomp comp replace expand compress cut head tail wc uniq tr play mixer undel scandisk msd print defrag taskman rbox convert"
wcc $CFLAGS -fo=$B/util.o apps/util.c
for t in cmd keyb a b c mem more find grep less chkdsk ufat $UTILAPPS; do
    U=""
    case " $UTILAPPS " in *" $t "*) U="file $B/util.o" ;; esac
    wcc $CFLAGS -fo=$B/task_$t.o apps/task_$t.c
    wlink $ALINK name $B/task_$t.bin option map=$B/task_$t.map \
          file $B/crt0.o file $B/task_$t.o $U 2>&1 | grep -v W1014 || true
done

echo "DOS-Stub mit FAPI-Uebersetzung (stub/)"
nasm -f obj stub/scrt.asm -o $B/scrt.o
wcc -q -ms -0 -s -zl -zls -zp1 -d0 -os -wx -we -fo=$B/fapidos.o stub/fapidos.c
wlink $LINK name $B/dstub.bin option map=$B/dstub.map \
      file $B/scrt.o file $B/fapidos.o 2>&1 | grep -v W1014 || true

echo "Programme (FX mit DOS-Stub)"
nasm -f bin tools/stub.asm -o $B/stub.bin
# Programme, die auch unter MS-DOS laufen (gebunden wie mit BIND bei OS/2:
# der Stub uebersetzt die FAPI in DOS-/BIOS-Aufrufe, die .EXE wird groesser)
BINDAPPS="more find grep less sort fc comp cut head tail wc uniq tr expand compress edit fm tree attrib grafik mtest fputest"
# Programme, die lange Namen sauber verarbeiten (FX_FLAG_LONGNAMES); alle
# anderen bekommen bei der Dateisuche nur 8.3-Namen (wie OS/2 1.2)
LFNAPPS="cmd attrib tree deltree grep print xcopy move join edit fm comp replace expand compress cut head tail wc uniq tr"
for t in cmd keyb a b c mem more find grep less chkdsk ufat $UTILAPPS; do
    L=""
    case " $LFNAPPS " in *" $t "*) L="--lfn" ;; esac
    case " $BINDAPPS " in *" $t "*) L="$L --bind=$B/dstub.bin,$B/dstub.map" ;; esac
    python3 tools/mkfx.py $L $B/stub.bin $B/task_$t.bin $B/task_$t.map $B/task_$t.exe
done

echo "Treiber"
nasm -f obj drivers/dcrt0.asm -o $B/dcrt0.o
for t in aha154x ide mouse ncr5380 sb16; do
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
# der Bootsektor laedt so viele Sektoren, wie sie gross ist (BSS nullt
# start.asm selbst)
cp $B/kernel.bin $B/ATDOS.SYS
mcopy -i $IMG $B/ATDOS.SYS ::ATDOS.SYS
mattrib -i $IMG +r +s +h ::ATDOS.SYS
# alle uebrigen Dateien: Quelle Ziel (Stamm), dann \DOS
FILES="$B/task_cmd.exe CMD.EXE
       CONFIG.SYS CONFIG.SYS  AUTOEXEC.CMD AUTOEXEC.CMD"
DOSFILES="$B/task_keyb.exe KEYB.EXE  $B/task_mem.exe MEM.EXE
       $B/task_more.exe MORE.EXE  $B/task_find.exe FIND.EXE  $B/task_grep.exe GREP.EXE
       $B/task_less.exe LESS.EXE  $B/task_chkdsk.exe CHKDSK.EXE
       $B/task_attrib.exe ATTRIB.EXE  $B/task_move.exe MOVE.EXE  $B/task_deltree.exe DELTREE.EXE
       $B/task_tree.exe TREE.EXE  $B/task_label.exe LABEL.EXE  $B/task_choice.exe CHOICE.EXE
       $B/task_sort.exe SORT.EXE  $B/task_fc.exe FC.EXE  $B/task_xcopy.exe XCOPY.EXE
       $B/task_format.exe FORMAT.EXE  $B/task_sys.exe SYS.EXE  $B/task_convert.exe CONVERT.EXE
       $B/task_edit.exe EDIT.EXE  $B/task_mode.exe MODE.EXE  $B/task_fdisk.exe FDISK.EXE
       $B/task_diskcopy.exe DISKCOPY.EXE  $B/task_subst.exe SUBST.EXE  $B/task_join.exe JOIN.EXE  $B/task_ansi.exe ANSI.EXE  $B/task_fm.exe FM.EXE  $B/task_diskcomp.exe DISKCOMP.EXE  $B/task_comp.exe COMP.EXE  $B/task_replace.exe REPLACE.EXE  $B/task_expand.exe EXPAND.EXE  $B/task_compress.exe COMPRESS.EXE  $B/task_cut.exe CUT.EXE  $B/task_head.exe HEAD.EXE  $B/task_tail.exe TAIL.EXE  $B/task_wc.exe WC.EXE  $B/task_uniq.exe UNIQ.EXE  $B/task_tr.exe TR.EXE  $B/task_play.exe PLAY.EXE  $B/task_mixer.exe MIXER.EXE
       $B/task_undel.exe UNDELETE.EXE  $B/task_scandisk.exe SCANDISK.EXE
       $B/task_msd.exe MSD.EXE  $B/task_print.exe PRINT.EXE  $B/task_defrag.exe DEFRAG.EXE  $B/task_taskman.exe TASKMAN.EXE  $B/task_rbox.exe RBOX.EXE"
# \DOS\SYSTEM: was das System nachlaedt (Treiber, IFS, Pruefprogramme, Belegungen)
SYSFILES="$B/drv_aha154x.sys AHA154X.SYS  $B/drv_ide.sys IDE.SYS  $B/drv_mouse.sys MOUSE.SYS  $B/drv_ncr5380.sys NCR5380.SYS  $B/drv_sb16.sys SB16.SYS
       $B/ifs_iso9660.ifs ISO9660.IFS  $B/task_ufat.exe UFAT.EXE
       $B/US.KBL US.KBL  $B/GB.KBL GB.KBL"
python3 tools/mkaudio.py $B                # Klangbeispiele fuer PLAY
nasm -f bin apps/rbtest.asm -o $B/rbtest.com  # Test der Kompatibilitaetsbox (5.1)
nasm -f bin apps/rbgfx.asm -o $B/rbgfx.com
nasm -f bin apps/rbfpu.asm -o $B/rbfpu.com
nasm -f bin apps/rbtime.asm -o $B/rbtime.com
DEMOFILES="$B/task_a.exe TASKA.EXE  $B/task_b.exe TASKB.EXE  $B/task_c.exe TASKC.EXE
       $B/task_mtest.exe MAUSTEST.EXE  $B/task_grafik.exe GRAFIK.EXE  $B/task_fputest.exe FPUTEST.EXE
       $B/KLANG.WAV KLANG.WAV  $B/ODE.MID ODE.MID  $B/ODE.IMF ODE.IMF  $B/rbtest.com RBTEST.COM  $B/rbgfx.com RBGFX.COM  $B/rbfpu.com RBFPU.COM  $B/rbtime.com RBTIME.COM"
# Startabbild der Kompatibilitaetsbox (5.1): 360-KB-Diskette mit FreeDOS-
# Bootsektor, KERNEL.SYS zuerst (8086-Bau), FreeCOM, RBSHELL
echo "Box: Startabbild freedos.img"
nasm -f bin apps/rbshell.asm -o $B/rbshell.com
nasm -f bin apps/rbready.asm -o $B/rbready.com
nasm -f bin apps/rbhma.asm -o $B/rbhma.sys
rm -f $B/freedos.img
mformat -i $B/freedos.img -C -f 360 -B freedos/fat12com.bin -v FREEDOS ::
for f in KERNEL.SYS COMMAND.COM CONFIG.SYS COUNTRY.SYS; do
    mcopy -i $B/freedos.img freedos/$f ::$f
done
mcopy -i $B/freedos.img $B/rbshell.com ::RBSHELL.COM
mcopy -i $B/freedos.img $B/rbhma.sys ::RBHMA.SYS
BOXFILES="$B/freedos.img FREEDOS.IMG  $B/rbready.com RBREADY.COM  freedos/COMMAND.COM COMMAND.COM  freedos/MEM.EXE MEM.EXE  freedos/MEM.DE MEM.DE  freedos/CTMOUSE.COM CTMOUSE.COM  freedos/README.txt README.TXT  freedos/COPYING COPYING"
copy_files() {   # Ziel-Laufwerk fuer mcopy, z. B. "-i dos5.img ::"
    copy_list "$FILES" ""
    mmd $TARGET "${TO}DOS" "${TO}DOS/SYSTEM" "${TO}DEMO"
    copy_list "$DOSFILES" "DOS/"
    copy_list "$SYSFILES" "DOS/SYSTEM/"
    mmd $TARGET "${TO}DOS/MDOS"
    copy_list "$BOXFILES" "DOS/MDOS/"
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
