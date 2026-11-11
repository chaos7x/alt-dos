#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# CuteMouse 2.1b4 (FreeDOS, GPL) fuer die Kompatibilitaetsbox bauen ->
# freedos/CTMOUSE.COM (deutsche Meldungen). Braucht git, gcc, Open Watcom
# (wlink, WATCOM gesetzt). JWasm wird mitgebaut.
set -e
D=${1:-/tmp/ctm-build}
mkdir -p "$D"
[ -d "$D/JWasm" ] || git clone --depth 1 https://github.com/Baron-von-Riedesel/JWasm "$D/JWasm"
[ -x "$D/JWasm/build/GccUnixR/jwasm" ] || (cd "$D/JWasm" && make -f GccUnix.mak)
[ -d "$D/mouse" ] || git clone --depth 1 https://github.com/FDOS/mouse "$D/mouse"
rm -rf "$D/b" && cp -r "$D/mouse" "$D/b" && cd "$D/b"
# Linux unterscheidet Gross/klein: Namen wie in den include-Zeilen
(cd asmlib && ln -s bios BIOS && ln -s dos DOS &&
 ln -s mcb.def dos/MCB.def && ln -s psp.def dos/PSP.def &&
 ln -s uart.def hard/UART.def && ln -s pic8259a.def hard/PIC8259A.def)
# heutiges JWasm ist strenger als das von 2010: drei Stellen
sed -i -E 's/^([A-Za-z_0-9]+[ \t]+)(POINT|HWLIST|KBDFLAGS|VIDEOCONTROL|VIDEOSWITCHES|VGAOPTIONS|KBDSTATUS|BUTTLASTSTATE)([ \t]+)\?/\1\2\3<>/' ctmouse.asm asmlib/bios/area0.def
sed -i 's/BUTTLASTSTATE <>,?,?/BUTTLASTSTATE 3 dup (<>)/' ctmouse.asm
sed -i 's/^\t\tsay\tS_mousetype\[si\]/\t\tmov\tdi,word ptr S_mousetype[si]\n\t\tcall\tsayASCIIZ/' ctmouse.asm
cp ctm-de.msg ctmouse.msg
"$D/JWasm/build/GccUnixR/jwasm" -mt -Fo=ctmouse.obj ctmouse.asm
wlink option quiet format dos com file ctmouse.obj name CTMOUSE.COM
cd - >/dev/null
cp "$D/b/CTMOUSE.COM" freedos/CTMOUSE.COM
echo "freedos/CTMOUSE.COM erneuert: mouse $(git -C "$D/mouse" rev-parse --short HEAD), JWasm $(git -C "$D/JWasm" rev-parse --short HEAD)"
