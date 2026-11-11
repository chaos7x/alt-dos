#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# NASM und mtools fuer die CI (.github/workflows/build.yml): erst apt mit
# drei Versuchen; gehen die Ubuntu-Paketquellen nicht (im November 2026
# loeste der GitHub-Rechner archive.ubuntu.com nicht auf), beide aus dem
# Quelltext bauen (nasm.us, ftp.gnu.org) und nach $HOME/citools legen.
# Mit CI_FORCE_SRC=1 gleich aus dem Quelltext (zum Pruefen).
set -e
NASM_V=2.16.03
MTOOLS_V=4.0.48
P=$HOME/citools

have() { command -v nasm >/dev/null && command -v mcopy >/dev/null; }

if [ -z "$CI_FORCE_SRC" ]; then
    for i in 1 2 3; do
        if sudo apt-get update && sudo apt-get install -y nasm mtools; then
            have && exit 0
        fi
        echo "apt: Versuch $i gescheitert"
        sleep 20
    done
fi

echo "NASM $NASM_V und mtools $MTOOLS_V aus dem Quelltext"
W=$(mktemp -d)
get() {                         # get Datei URL...
    f=$1; shift
    for u in "$@"; do
        curl -fsSL --retry 3 --retry-delay 5 -o "$W/$f" "$u" && return 0
    done
    return 1
}
get nasm.tar.xz "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_V/nasm-$NASM_V.tar.xz"
get mtools.tar.gz "https://ftp.gnu.org/gnu/mtools/mtools-$MTOOLS_V.tar.gz" \
                  "https://ftpmirror.gnu.org/mtools/mtools-$MTOOLS_V.tar.gz"
mkdir -p "$P"
(cd "$W" && tar -xJf nasm.tar.xz && cd nasm-$NASM_V &&
 ./configure -q --prefix="$P" && make -s -j4 && make -s install)
(cd "$W" && tar -xzf mtools.tar.gz && cd mtools-$MTOOLS_V &&
 ./configure -q --prefix="$P" --disable-floppyd && make -s -j4 && make -s install)
[ -n "$GITHUB_PATH" ] && echo "$P/bin" >> "$GITHUB_PATH"
"$P/bin/nasm" -v
"$P/bin/mcopy" --version | head -1
