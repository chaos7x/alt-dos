#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# COMMAND.COM aus den MS-DOS-4.0-Quellen von Microsoft (MIT) bauen und als
# msdos4/COMMAND4.COM ablegen (siehe msdos4/README.txt). Gebaut wird mit den
# Werkzeugen aus dem Quellbaum (NMAKE, MASM, LINK, BUILDMSG) in dosbox-x.
#   tools/msdos4.sh [Arbeitsverzeichnis]
# Braucht git und dosbox-x.
set -e
R=$(pwd)
D=${1:-/tmp/msdos4-build}
mkdir -p "$D"
[ -d "$D/ms-dos" ] || git clone --depth 1 https://github.com/microsoft/ms-dos "$D/ms-dos"
rm -rf "$D/b"; mkdir -p "$D/b"
cp -r "$D/ms-dos/v4.0/src" "$D/b/src"
(cd "$D/b" && patch -p2 < "$R/msdos4/patches/version.patch")
cat > "$D/b/B.BAT" <<'B'
set COUNTRY=usa-ms
set LIB=d:\src\tools\lib
set INIT=d:\src\tools
set INCLUDE=d:\src\tools\inc
set PATH=d:\src\tools
cd \src\messages
nmake > d:\msg.log
cd \src\cmd\command
nmake > d:\cmd.log
exit
B
printf '[sdl]\noutput=surface\n[dosbox]\nmemsize=16\n[cpu]\ncycles=max\n' > "$D/dx.conf"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy dosbox-x -conf "$D/dx.conf" \
    -nopromptfolder -c "mount d $D/b" -c "d:" -c "B.BAT" > /dev/null 2>&1
cp "$D/b/src/CMD/COMMAND/COMMAND.COM" msdos4/COMMAND4.COM
echo "msdos4/COMMAND4.COM erneuert ($(git -C "$D/ms-dos" rev-parse --short HEAD))"
