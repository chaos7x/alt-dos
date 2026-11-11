#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# NASM und mtools fuer die CI (.github/workflows/build.yml): erst apt mit
# drei Versuchen; gehen die Ubuntu-Paketquellen nicht (im November 2026
# loeste der GitHub-Rechner archive.ubuntu.com nicht auf), beide aus dem
# Quelltext bauen (nasm.us, ftp.gnu.org) und nach $HOME/citools legen.
# Mit CI_FORCE_SRC=1 gleich aus dem Quelltext (zum Pruefen).
# DNS: vorher auf Quad9 und Google (9.9.9.9, 8.8.8.8) statt des Azure-DNS
# des Rechners (Wunsch des Nutzers); geht das nicht, bleibt es, wie es ist.
set -e
NASM_V=2.16.03
MTOOLS_V=4.0.48
P=$HOME/citools

have() { command -v nasm >/dev/null && command -v mcopy >/dev/null; }

if [ -n "$GITHUB_ACTIONS" ] && command -v resolvectl >/dev/null; then
    sudo mkdir -p /etc/systemd/resolved.conf.d
    printf '[Resolve]\nDNS=9.9.9.9 8.8.8.8\n' |
        sudo tee /etc/systemd/resolved.conf.d/ci-dns.conf >/dev/null
    sudo systemctl restart systemd-resolved || true
    for i in $(ip -o link show | awk -F': ' '$2 != "lo" {print $2}'); do
        sudo resolvectl dns "$i" 9.9.9.9 8.8.8.8 2>/dev/null || true
        sudo resolvectl domain "$i" '~.' 2>/dev/null || true
    done
    resolvectl status 2>/dev/null | grep -i 'dns server' | head -3 || true
fi

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
