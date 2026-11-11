#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# FreeDOS-Kern und FreeCOM aus dem Quelltext bauen und nach freedos/ legen
# (siehe freedos/README.txt). Braucht git, nasm, Open Watcom (WATCOM gesetzt).
set -e
D=${1:-/tmp/fdos-build}
mkdir -p "$D"
[ -d "$D/kernel" ] || git clone --depth 1 https://github.com/FDOS/kernel "$D/kernel"
[ -d "$D/freecom" ] || git clone --depth 1 https://github.com/FDOS/freecom "$D/freecom"
[ -d "$D/mem" ] || git clone --depth 1 https://github.com/FDOS/mem "$D/mem"
(cd "$D/kernel" && git submodule update --init --depth 1 &&
 rm -f kernel/exeflat.rsp && make all COMPILER=owlinux XCPU=86 XFAT=32 XUPX=)
(cd "$D/freecom" && LNG=german ./build.sh wc)
cp "$D/kernel/bin/kernel.sys" freedos/KERNEL.SYS
cp "$D/kernel/boot/fat12com.bin" freedos/fat12com.bin
(cd "$D/kernel/country" && nasm -o country.sys country.asm)
cp "$D/kernel/country/country.sys" freedos/COUNTRY.SYS
cp "$D/freecom/command.com" freedos/COMMAND.COM
(cd "$D/mem/source" && wcl -q -bt=dos -ms -0 -oahls -s -j mem.c prf.c kitten.c -fe=MEM.EXE)
cp "$D/mem/source/MEM.EXE" freedos/MEM.EXE
cp "$D/mem/nls/mem.de" freedos/MEM.DE
echo "freedos/ erneuert: KERNEL.SYS $(git -C "$D/kernel" rev-parse --short HEAD), COMMAND.COM $(git -C "$D/freecom" rev-parse --short HEAD), MEM.EXE $(git -C "$D/mem" rev-parse --short HEAD)"
