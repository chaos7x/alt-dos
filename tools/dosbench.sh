#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Stufe 0 von docs/DOSKERN.md: dieselben Messungen (DOSBENCH.COM, DOOM
# -timedemo) und DOSSTRUC.COM in vier Faellen auf derselben Testplatte:
#   FD  FreeDOS direkt (Startdiskette aus build/freedos.img)
#   MD  MS-DOS direkt (Startdiskette des Nutzers)
#   FB  FreeDOS-Box unter AT-DOS
#   MB  MS-DOS-Box unter AT-DOS (MKDOSIMG aus derselben Diskette)
#
#   tools/dosbench.sh <MS-DOS-Startdiskette.img> <DOOM-Verzeichnis> [Arbeitsverz.]
#
# DOOM-Verzeichnis: DOOM.EXE + DOOM1.WAD (z. B. Shareware 1.9). Diskette
# und DOOM bringt der Nutzer mit, sie kommen nicht ins Repository.
# REP=n (1-9): Tests mit vielen kleinen Aufrufen n-mal (QEMU: Taktaufloesung).
# Ergebnis: <Arbeitsverz.>/{R,D,S}-{FD,MD,FB,MB}.TXT (S = DOSSTRUC). NODIRECT=1 nur die
# Boxen, QX= weitere QEMU-Schalter fuer AT-DOS (z. B. -debugcon file:x).
# Braucht build/,
# qemu-system-i386, mtools, nasm, python3, mkfs.fat.
set -e
export MTOOLS_SKIP_CHECK=1
MS="$1"; DOOM="$2"; R=$(cd "$(dirname "$0")/.." && pwd); W=${3:-/tmp/dosbench}
MEM=${MEM:-32}; REP=${REP:-1}
mkdir -p "$W"; rm -f "$W"/*.TXT
nasm -f bin $R/tools/dosbench.asm -o "$W/dosbench.com"
nasm -f bin $R/tools/dosstruc.asm -o "$W/dosstruc.com"

# Testplatte: 128 MB FAT16, MANY\ mit 800 Dateien, DOOM\, RUN.BAT
mkt() {
  rm -f "$W/t.img"; truncate -s 128M "$W/t.img"
  python3 -c '
import struct, sys
f = open(sys.argv[1], "r+b"); n = 128 * 2048 - 2048
e = struct.pack("<B3sB3sII", 0, b"\xff\xff\xff", 6, b"\xff\xff\xff", 2048, n)
f.seek(446); f.write(e); f.seek(510); f.write(b"\x55\xaa")' "$W/t.img"
  mkfs.fat -F 16 -n BENCH --offset 2048 -h 2048 "$W/t.img" > /dev/null
  T="$W/t.img@@1048576"
  mcopy -i "$T" "$W/dosbench.com" "$W/dosstruc.com" ::
  mmd -i "$T" ::MANY ::DOOM
  rm -rf "$W/many"; mkdir -p "$W/many"
  for i in $(seq 1 800); do printf 'x' > "$W/many/M$(printf %04d $i).TXT"; done
  mcopy -i "$T" "$W"/many/* ::MANY/
  mcopy -i "$T" "$DOOM"/DOOM.EXE "$DOOM"/DOOM1.WAD ::DOOM/ 2>/dev/null || \
    mcopy -i "$T" "$DOOM"/doom.exe "$DOOM"/doom1.wad ::DOOM/
  printf '@echo off\r\n%%1\\dosstruc > %%1\\S-%%2.TXT\r\n%%1\\dosbench %%1\\ %%3 > %%1\\R-%%2.TXT\r\n%%1\r\ncd \\DOOM\r\n%%1\\dosbench /T > %%1\\D-%%2.TXT\r\ndoom -timedemo demo1 -nosound >> %%1\\D-%%2.TXT\r\n%%1\\dosbench /T >> %%1\\D-%%2.TXT\r\ncd \\\r\necho fertig > %%1\\X-%%2.TXT\r\n' > "$W/run.bat"
  mcopy -i "$T" "$W/run.bat" ::RUN.BAT
}
# wartet, bis X-<n>.TXT auf der Testplatte steht
waitx() {
  for i in $(seq 1 ${2:-600}); do sleep 1
    if mdir -i "$W/t.img@@1048576" ::X-$1.TXT >/dev/null 2>&1; then sleep 2; return 0; fi
  done; return 1
}
fetch() {
  for n in "$@"; do for k in R D S; do
    mcopy -o -i "$W/t.img@@1048576" ::$k-$n.TXT "$W/$k-$n.TXT" 2>/dev/null || echo "kein $k-$n.TXT"
  done; done
}
CFG='FILES=30\r\nBUFFERS=20\r\nLASTDRIVE=Z\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n'
direct() {      # $1 Name, $2 Diskette
  mkt
  cp "$2" "$W/fd.img"
  printf "$CFG" > "$W/cfg"; mcopy -o -i "$W/fd.img" "$W/cfg" ::CONFIG.SYS
  printf '@echo off\r\nc:\\run.bat c: %s %s\r\n' "$1" "$REP" > "$W/aut"
  mcopy -o -i "$W/fd.img" "$W/aut" ::AUTOEXEC.BAT
  qemu-system-i386 -m $MEM -display none -no-reboot \
    -drive file="$W/fd.img",if=floppy,format=raw \
    -drive file="$W/t.img",if=ide,format=raw -boot a &
  QP=$!; waitx $1 || echo "$1: Frist"; kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
  fetch $1
}
if [ -z "$NODIRECT" ]; then
# FreeDOS direkt: Abbild der Box mit eigener CONFIG.SYS (ohne BOXSHELL)
cp $R/build/freedos.img "$W/fdos.img"; mdel -i "$W/fdos.img" ::BOXSHELL.COM ::ATBOX.SYS
direct FD "$W/fdos.img"
# MS-DOS direkt: Startdiskette ohne Extras
cp "$MS" "$W/ms.img"; mdel -i "$W/ms.img" ::DRVSPACE.BIN ::MSCDEX.EXE 2>/dev/null || true
direct MD "$W/ms.img"
fi
# beide Boxen unter AT-DOS: C: = atdos-hd.img, D: = Testplatte
mkt
cp $R/build/atdos-hd.img "$W/at.img"; P="$W/at.img@@32256"
printf '@echo off\r\nmkdosimg a: /n:ms.img > c:\\mk.txt\r\ncommand /c d:\\run.bat d: FB %s\r\necho DOS_IMAGE=MS.IMG>> c:\\dos\\mdos\\dosbox.ini\r\ncommand /c d:\\run.bat d: MB %s\r\nshutdown /now\r\n' "$REP" "$REP" > "$W/autc"
mcopy -o -i "$P" "$W/autc" ::AUTOEXEC.CMD
qemu-system-i386 -m $MEM -display none -no-reboot \
  -drive file="$MS",if=floppy,format=raw,snapshot=on \
  -drive file="$W/at.img",if=ide,index=0,format=raw \
  -drive file="$W/t.img",if=ide,index=1,format=raw -boot c $QX &
QP=$!; waitx MB 900 || echo "MB: Frist"; sleep 5; kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
mcopy -o -i "$P" ::MK.TXT "$W/mk.txt" 2>/dev/null || true
fetch FB MB
for n in FD MD FB MB; do echo "== $n"; cat "$W/R-$n.TXT" "$W/D-$n.TXT" 2>/dev/null | tr -d '\r'; done
