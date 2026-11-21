#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Vergleichstest der DOS-Box: BIOSCMP.COM laeuft einmal unter FreeDOS
# direkt in QEMU (Bezug: echtes BIOS, FreeDOS auf eigener FAT) und einmal
# in der DOS-Box von AT-DOS (nachgebildetes BIOS, Laufwerke ueber den
# Redirector), auf derselben QEMU-Maschine.  Danach diff der beiden
# Ergebnisdateien.
#
#   tools/bioscmp.sh [Arbeitsverzeichnis] [rbox|fd|cm]
#
# Die Box laeuft mit der Vorgabe (seit DOSKERN Stufe 5 der eigene Kern
# ATDOSCM.SYS; "cm" setzt ihn ausdruecklich), mit "fd" mit dem
# FreeDOS-Abbild (DOSBOX.INI DOS_KERNEL=FREEDOS.IMG, docs/DOSKERN.md).
#
# Mit "rbox" (Zweig 5.x): Box ist die Kompatibilitaetsbox (RBOX, echter
# Real Mode); BIOSCMP wird dort ueber den QEMU-Monitor eingetippt
# (deutsche Belegung).  Der Bezug nimmt dann das FreeDOS des Zweigs.
#
# Braucht build/ (./build.sh vorher), qemu-system-i386, mtools, python3,
# mkfs.fat.  Erwartete Abweichungen stehen in docs/BOX-BIOS.md.

set -e
cd "$(dirname "$0")/.."
B=build
W=${1:-/tmp/bioscmp}
MODE=${2:-box}
mkdir -p "$W"
nasm -f bin tools/bioscmp.asm -o "$W/bioscmp.com"

# --- Bezug: FreeDOS-Startdiskette + eigene FAT16-Platte
rm -f "$W/ref-fd.img" "$W/ref-hd.img"
mformat -i "$W/ref-fd.img" -C -f 1440 -B freedos/fat12com.bin -v BCREF ::
mcopy -i "$W/ref-fd.img" freedos/KERNEL.SYS ::KERNEL.SYS
mcopy -i "$W/ref-fd.img" freedos/COMMAND.COM ::COMMAND.COM
mcopy -i "$W/ref-fd.img" "$W/bioscmp.com" ::BIOSCMP.COM
printf 'FILES=30\r\nLASTDRIVE=Z\r\nSHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > "$W/cfg"
printf '@echo off\r\nc:\r\na:\\bioscmp c:\\ref.txt c:\r\nctty nul\r\n' > "$W/aut"
mcopy -i "$W/ref-fd.img" "$W/cfg" ::CONFIG.SYS
mcopy -i "$W/ref-fd.img" "$W/aut" ::AUTOEXEC.BAT
truncate -s 64M "$W/ref-hd.img"
python3 -c '
import struct, sys
f = open(sys.argv[1], "r+b"); n = 64 * 2048 - 2048
e = struct.pack("<B3sB3sII", 0x80, b"\xff\xff\xff", 6, b"\xff\xff\xff", 2048, n)
f.seek(446); f.write(e); f.seek(510); f.write(b"\x55\xaa")' "$W/ref-hd.img"
mkfs.fat -F 16 -n BCTEST --offset 2048 -h 2048 "$W/ref-hd.img" > /dev/null
REFP="$W/ref-hd.img@@1048576"

timeout 120 qemu-system-i386 -m 16 -display none -no-reboot \
    -drive file="$W/ref-fd.img",if=floppy,format=raw \
    -drive file="$W/ref-hd.img",if=ide,format=raw -boot a &
QP=$!
for i in $(seq 1 100); do
    sleep 1
    if mdir -i "$REFP" ::REF.TXT > /dev/null 2>&1; then sleep 3; break; fi
done
kill $QP 2> /dev/null || true
wait $QP 2> /dev/null || true
mcopy -o -i "$REFP" ::REF.TXT "$W/ref.txt"

# --- DOS-Box von AT-DOS: Platte mit BIOSCMP, AUTOEXEC ruft es in der Box
cp $B/atdos-hd.img "$W/box-hd.img"
P="$W/box-hd.img@@32256"
mcopy -o -i "$P" "$W/bioscmp.com" ::BIOSCMP.COM
if [ "$MODE" = cm -o "$MODE" = fd ]; then   # DOS der Box ausdruecklich
    K=ATDOSCM.SYS; [ "$MODE" = fd ] && K=FREEDOS.IMG
    mtype -i "$P" ::DOS/MDOS/DOSBOX.INI | sed "s/^\[\*\]\r\?\$/[*]\r\nDOS_KERNEL=$K\r/" > "$W/ini"
    mcopy -o -i "$P" "$W/ini" ::DOS/MDOS/DOSBOX.INI
fi
if [ "$MODE" = rbox ]; then
    rm -f "$W/m.s"
    timeout 400 qemu-system-i386 -m 16 -display none -no-reboot \
        -monitor unix:"$W/m.s",server,nowait \
        -drive file="$W/ref-fd.img",if=floppy,format=raw,snapshot=on \
        -drive file="$W/box-hd.img",if=ide,format=raw -boot c &
    QP=$!
    python3 - "$W/m.s" <<'Q'
import socket, sys, time
time.sleep(30)
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
mp = {':': 'shift-dot', '\\': 'alt_r-minus', '.': 'dot', ' ': 'spc',
      '/': 'shift-7', 'y': 'z', 'z': 'y'}
def typ(t, w):
    for c in t:
        s.send(('sendkey %s\n' % mp.get(c, c)).encode()); time.sleep(0.15)
    s.send(b'sendkey ret\n'); time.sleep(w)
typ('rbox', 30)
typ('c:\\bioscmp c:\\box.txt c:', 60)
typ('', 2)
typ('exit', 15)
typ('shutdown /now', 20)
Q
    wait $QP || true
    mcopy -o -i "$P" ::BOX.TXT "$W/box.txt"
    diff "$W/ref.txt" "$W/box.txt" > "$W/diff.txt" && echo "keine Abweichungen" \
        || echo "$(grep -c '^<' "$W/diff.txt") Abweichungen, siehe $W/diff.txt"
    exit 0
fi
printf '@echo off\r\ncommand /c c:\\bioscmp.com c:\\box.txt c:\r\nshutdown /now\r\n' > "$W/autc"
mcopy -o -i "$P" "$W/autc" ::AUTOEXEC.CMD
timeout 300 qemu-system-i386 -m 16 -display none -no-reboot \
    -drive file="$W/ref-fd.img",if=floppy,format=raw,snapshot=on \
    -drive file="$W/box-hd.img",if=ide,format=raw -boot c || true
mcopy -o -i "$P" ::BOX.TXT "$W/box.txt"

diff "$W/ref.txt" "$W/box.txt" > "$W/diff.txt" && echo "keine Abweichungen" \
    || echo "$(grep -c '^<' "$W/diff.txt") Abweichungen, siehe $W/diff.txt"
