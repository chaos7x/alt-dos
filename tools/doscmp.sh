#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Vergleich je DOS-Version (5.x): BIOSCMP laeuft einmal unter dem DOS
# direkt (von dessen Startdiskette in QEMU) und einmal in der Kompatibili-
# taetsbox (RBOX) von AT-DOS,
# deren Abbild MKDOSIMG unter AT-DOS aus derselben Diskette baut
# (DOS_IMAGE=). Die Diskette bringt der Nutzer mit, sie kommt nicht ins
# Repository.
#
#   tools/doscmp.sh <Startdiskette.img> <Name> [Arbeitsverzeichnis]
#
# Ergebnis: <Arbeitsverzeichnis>/<Name>/{ref,box,diff}.txt. Erwartete
# Abweichungen (Box-BIOS, XMS/EMS/SHARE/Maus der Box) stehen in
# docs/BOX-BIOS.md. Braucht build/ (./build.sh), qemu-system-i386,
# mtools, nasm, python3, mkfs.fat.
set -e
export MTOOLS_SKIP_CHECK=1
D="$1"; N="$2"; R=$(cd "$(dirname "$0")/.." && pwd); W=${3:-/tmp/doscmp}/$N
rm -rf "$W"; mkdir -p "$W"
nasm -f bin $R/tools/bioscmp.asm -o "$W/bioscmp.com"
# Bezug: Startdiskette, SETUP & Co. raus, eigene CONFIG/AUTOEXEC
cp "$D" "$W/ref-fd.img"
for f in $(mdir -i "$W/ref-fd.img" -b :: | sed 's|^::/||'); do
  case "${f^^}" in IBMBIO.COM|IBMDOS.COM|IO.SYS|MSDOS.SYS|COMMAND.COM|COUNTRY.SYS) ;; *) mattrib -i "$W/ref-fd.img" -r -s -h "::$f" 2>/dev/null; mdel -i "$W/ref-fd.img" "::$f" 2>/dev/null || mdeltree -i "$W/ref-fd.img" "::$f" 2>/dev/null || true;; esac
done
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
for i in $(seq 1 100); do sleep 1; if mdir -i "$REFP" ::REF.TXT >/dev/null 2>&1; then sleep 3; break; fi; done
kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
mcopy -o -i "$REFP" ::REF.TXT "$W/ref.txt" || echo "kein ref.txt"
# Box: MKDOSIMG aus der Startdiskette (AUTOEXEC.CMD), dann RBOX per
# QEMU-Monitor eingetippt (deutsche Belegung), darin BIOSCMP
cp $R/build/atdos-hd.img "$W/box-hd.img"; P="$W/box-hd.img@@32256"
mcopy -o -i "$P" "$W/bioscmp.com" ::BIOSCMP.COM
printf '@echo off\r\nmkdosimg a: /n:v.img > c:\\mk.txt\r\necho DOS_IMAGE=V.IMG>> c:\\dos\\mdos\\dosbox.ini\r\n' > "$W/autc"
mcopy -o -i "$P" "$W/autc" ::AUTOEXEC.CMD
rm -f "$W/m.s"
timeout 500 qemu-system-i386 -m 16 -display none -no-reboot \
  -monitor unix:"$W/m.s",server,nowait \
  -drive file="$D",if=floppy,format=raw,snapshot=on \
  -drive file="$W/box-hd.img",if=ide,format=raw -boot c &
QP=$!
python3 - "$W/m.s" <<'Q'
import socket, sys, time
time.sleep(60)
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
mp = {':': 'shift-dot', '\\': 'alt_r-minus', '.': 'dot', ' ': 'spc',
      '/': 'shift-7', 'y': 'z', 'z': 'y'}
def typ(t, w):
    for c in t:
        s.send(('sendkey %s\n' % mp.get(c, c)).encode()); time.sleep(0.15)
    s.send(b'sendkey ret\n'); time.sleep(w)
typ('rbox', 40)
typ('c:\\bioscmp c:\\box.txt c:', 60)
typ('', 2)
typ('exit', 15)
typ('shutdown /now', 20)
Q
wait $QP || true
mcopy -o -i "$P" ::MK.TXT "$W/mk.txt" || true
mcopy -o -i "$P" ::BOX.TXT "$W/box.txt" || echo "kein box.txt"
diff "$W/ref.txt" "$W/box.txt" > "$W/diff.txt" && echo "$N: keine Abweichungen" \
  || echo "$N: $(grep -c '^<' "$W/diff.txt") Abweichungen"
