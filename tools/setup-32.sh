#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Entwicklungsumgebung fuer AT-DOS 7.0 (32 Bit) einrichten.
# Jede Cloud-Sitzung startet frisch – dieses Skript stellt alles in einem
# Lauf wieder her. Beschreibung und Begruendung: docs/BUILD-32.md.
#
#   tools/setup-32.sh                Grundausstattung (apt, Open Watcom)
#   tools/setup-32.sh --cross        dazu GCC/binutils i686-elf (~15 min)
#   tools/setup-32.sh --86box        dazu 86Box aus dem Quelltext (~10 min)
#   tools/setup-32.sh --all          alles
#   tools/setup-32.sh --check        nur pruefen, was da ist (Rauchtests)
#
# Danach:  . ~/.atdos32-env   (setzt WATCOM, INCLUDE, PATH)
# Ablage:  $AT32 (Vorgabe ~/at32): cross/, 86box/, roms/; Open Watcom in
#          ~/watcom wie bei build.sh. Nichts davon kommt ins Repository.
set -e

BINUTILS_V=2.44
GCC_V=14.2.0
TARGET=i686-elf
AT32=${AT32:-$HOME/at32}
ENVF=$HOME/.atdos32-env
JOBS=$(nproc 2>/dev/null || echo 4)
SUDO=; [ "$(id -u)" != 0 ] && SUDO=sudo

DO_BASE=1 DO_CROSS=0 DO_86BOX=0 DO_CHECK=1
for a in "$@"; do
    case $a in
        --cross) DO_CROSS=1 ;;
        --86box) DO_86BOX=1 ;;
        --all)   DO_CROSS=1; DO_86BOX=1 ;;
        --check) DO_BASE=0 ;;
        --no-check) DO_CHECK=0 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unbekannt: $a (--help)"; exit 1 ;;
    esac
done

say() { printf '\n== %s\n' "$*"; }
mkdir -p "$AT32"

# --- 1. Pakete ------------------------------------------------------------
# nasm/mtools/dosfstools/qemu wie fuer 16 Bit (CLAUDE.md); dazu
#  gcc-multilib      gcc -m32 fuer Werkzeuge/Tests auf dem Entwicklungsrechner
#  mingw-w64         i686-w64-mingw32-gcc: Win32-PE-Testprogramme, Wine-Teile
#  bochs*            Bochs mit Debugger (VME/PVI, Haltepunkte, port_e9_hack)
#  ovmf              UEFI fuer QEMU (7.0 soll auch per UEFI starten)
#  grub/xorriso      Multiboot-Abbilder fuer schnelle Kerneltests (nur als
#                    Werkzeug, GRUB-Code kommt nicht ins Projekt – GPLv3)
#  gdb-multiarch     an QEMU (-s -S) und Bochs-gdbstub haengen
#  libgmp/mpfr/mpc   fuer den Bau von GCC
APT_PKGS="build-essential git curl xz-utils bison flex texinfo
 libgmp-dev libmpfr-dev libmpc-dev
 nasm mtools dosfstools qemu-system-x86 qemu-utils ovmf
 gcc-multilib g++-multilib mingw-w64 gdb-multiarch
 bochs bochs-x bochsbios vgabios
 grub-pc-bin grub-common xorriso genisoimage
 xvfb xdotool imagemagick python3 python3-pil python3-pefile"

if [ $DO_BASE = 1 ]; then
    say "apt-Pakete"
    for i in 1 2 3; do
        $SUDO apt-get update -qq &&
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq $APT_PKGS \
            >/dev/null && break
        echo "apt: Versuch $i gescheitert"; sleep 10
    done

    # --- 2. Open Watcom 2.0 (16 und 32 Bit: wcc, wcc386, wlink) -----------
    if [ ! -x "$HOME/watcom/binl64/wcc386" ]; then
        say "Open Watcom 2.0 (Snapshot)"
        T=$(mktemp -d)
        curl -fsSL --retry 3 -o "$T/ow.tar.xz" \
          https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz
        mkdir -p "$HOME/watcom"
        tar -xJf "$T/ow.tar.xz" -C "$HOME/watcom"
        rm -rf "$T"
    fi
fi

# --- 3. GCC-Crosscompiler i686-elf ---------------------------------------
# Freistehend (ohne libc), wie im OSDev-Wiki empfohlen: der Host-gcc mit
# -m32 kennt Linux-Annahmen (Stack-Schutz, PIE, glibc-Header).
if [ $DO_CROSS = 1 ] && [ ! -x "$AT32/cross/bin/$TARGET-gcc" ]; then
    say "binutils $BINUTILS_V und GCC $GCC_V fuer $TARGET"
    S=$AT32/src; mkdir -p "$S"
    get() {
        [ -f "$S/$1" ] || curl -fsSL --retry 3 -o "$S/$1" "$2" ||
            curl -fsSL --retry 3 -o "$S/$1" "${2/ftp.gnu.org\/gnu/ftpmirror.gnu.org}"
    }
    get binutils-$BINUTILS_V.tar.xz https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_V.tar.xz
    get gcc-$GCC_V.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-$GCC_V/gcc-$GCC_V.tar.xz
    (cd "$S" && tar -xJf binutils-$BINUTILS_V.tar.xz && tar -xJf gcc-$GCC_V.tar.xz)
    mkdir -p "$S/b-bin" "$S/b-gcc"
    (cd "$S/b-bin" && ../binutils-$BINUTILS_V/configure --target=$TARGET \
        --prefix="$AT32/cross" --with-sysroot --disable-nls --disable-werror \
        >/dev/null && make -s -j"$JOBS" >/dev/null && make -s install >/dev/null)
    (cd "$S/b-gcc" && PATH=$AT32/cross/bin:$PATH ../gcc-$GCC_V/configure \
        --target=$TARGET --prefix="$AT32/cross" --disable-nls \
        --enable-languages=c,c++ --without-headers >/dev/null &&
     PATH=$AT32/cross/bin:$PATH make -s -j"$JOBS" all-gcc all-target-libgcc >/dev/null &&
     make -s install-gcc install-target-libgcc >/dev/null)
    rm -rf "$S"
fi

# --- 4. 86Box aus dem Quelltext (wie CLAUDE.md, „86Box in der Umgebung“) --
if [ $DO_86BOX = 1 ] && [ ! -x "$AT32/86box/build/src/86Box" ]; then
    say "86Box (ohne Qt) und ROMs"
    $SUDO apt-get install -y -qq cmake ninja-build pkg-config libsdl2-dev \
        libfreetype-dev libpng-dev libopenal-dev libslirp-dev libsndfile1-dev \
        libevdev-dev libxkbcommon-dev libzstd-dev >/dev/null
    [ -d "$AT32/86box" ] ||
        git clone -q --depth 1 https://github.com/86Box/86Box.git "$AT32/86box"
    (cd "$AT32/86box" && cmake -S . -B build -G Ninja -DQT=OFF -DSDL2=ON \
        -DRTMIDI=OFF -DFLUIDSYNTH=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null &&
     ninja -C build >/dev/null)
    if [ ! -d "$AT32/roms" ]; then
        # nur die Boards/Karten der Testmaschinen (86box/*.cfg) – sonst >1 GB
        git clone -q --depth 1 --filter=blob:none --sparse \
            https://github.com/86Box/roms.git "$AT32/roms"
        (cd "$AT32/roms" && git sparse-checkout set machines/p3bf \
            machines/486sp3 machines/asus386 machines/arb1374 sound/creative \
            video/s3 video/tseng video/cirrus floppy hdd scsi)
    fi
fi

# --- 5. Umgebung ----------------------------------------------------------
cat > "$ENVF" <<EOF
# erzeugt von tools/setup-32.sh – mit '. $ENVF' laden
export WATCOM=$HOME/watcom
export INCLUDE=\$WATCOM/h
export AT32=$AT32
export PATH=$AT32/cross/bin:$AT32/86box/build/src:\$WATCOM/binl64:\$PATH
EOF
. "$ENVF"

# --- 6. Rauchtests --------------------------------------------------------
# Nur in einem Wegwerfverzeichnis: je Compiler ein kleines Objekt, ein
# Win32-PE und ein Multiboot-Kern, der in QEMU „AT32“ auf Port E9h schreibt.
[ $DO_CHECK = 1 ] || exit 0
say "Pruefen"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
ok=0 bad=0
chk() { if eval "$2" >"$T/log" 2>&1; then echo "  ok    $1"; ok=$((ok+1));
        else echo "  FEHLT $1"; sed 's/^/        /' "$T/log" | tail -3; bad=$((bad+1)); fi; }

cat > "$T/k.c" <<'EOF'
static inline void outb(unsigned short p, unsigned char v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
void kmain(void) { const char *s = "AT32\n"; while (*s) outb(0xE9, *s++);
  outb(0xF4, 0); for (;;) __asm__ volatile("hlt"); }
EOF
cat > "$T/mb.asm" <<'EOF'
bits 32
global start
extern kmain
section .multiboot
align 4
dd 0x1BADB002, 0, -0x1BADB002
section .text
start: mov esp, stk + 4096
       call kmain
section .bss
stk: resb 4096
EOF
cat > "$T/w.c" <<'EOF'
int __cdecl add(int a, int b) { return a + b; }
EOF
cat > "$T/pe.c" <<'EOF'
#include <windows.h>
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int n)
{ MessageBoxA(0, "AT-DOS 7.0", "Test", MB_OK); return 0; }
EOF

chk "nasm $(nasm -v | cut -d' ' -f3)"           "nasm -f elf32 $T/mb.asm -o $T/mb.o"
chk "mtools / mkfs.fat"                          "mformat -V && command -v mkfs.fat"
chk "Open Watcom wcc (16 Bit)"                   "wcc -q -zq -fo=$T/w16.o $T/w.c"
chk "Open Watcom wcc386 (32 Bit)"                "wcc386 -q -zq -fo=$T/w32.o $T/w.c"
chk "Open Watcom wlink"                          "command -v wlink"
chk "gcc -m32 (Host)"                            "gcc -m32 -c -ffreestanding -fno-pie -fno-stack-protector $T/k.c -o $T/kh.o"
chk "MinGW i686 PE"                              "i686-w64-mingw32-gcc -mwindows $T/pe.c -o $T/t.exe && python3 -c \"import pefile; p=pefile.PE('$T/t.exe'); assert p.FILE_HEADER.Machine==0x14c\""
if command -v $TARGET-gcc >/dev/null; then
    chk "$TARGET-gcc $($TARGET-gcc -dumpversion)" "$TARGET-gcc -c -ffreestanding -O2 $T/k.c -o $T/k.o"
    chk "$TARGET-ld Multiboot-Kern"              "$TARGET-gcc -T /dev/stdin -nostdlib -ffreestanding $T/mb.o $T/k.o -lgcc -o $T/k.elf <<<'ENTRY(start) SECTIONS { . = 1M; .text : { *(.multiboot) *(.text*) } .data : { *(.rodata*) *(.data*) } .bss : { *(.bss*) } }'"
    chk "QEMU startet Multiboot-Kern (Port E9h)" "timeout 20 qemu-system-i386 -display none -kernel $T/k.elf -debugcon file:$T/e9 -device isa-debug-exit,iobase=0xf4 ; grep -q AT32 $T/e9"
else
    echo "  --    $TARGET-gcc nicht eingerichtet (--cross)"
fi
chk "QEMU $(qemu-system-i386 --version | head -1 | cut -d' ' -f4)" "command -v qemu-system-i386"
chk "OVMF (UEFI fuer QEMU)"                      "ls /usr/share/OVMF/OVMF_CODE*.fd"
chk "Bochs mit Debugger"                         "bochs --help 2>&1 | grep -qi debugger || dpkg -s bochs | grep -q Version"
chk "gdb-multiarch"                              "command -v gdb-multiarch"
chk "grub-mkrescue / xorriso"                    "command -v grub-mkrescue && command -v xorriso"
if [ -x "$AT32/86box/build/src/86Box" ]; then
    chk "86Box"                                   "test -x $AT32/86box/build/src/86Box"
else
    echo "  --    86Box nicht eingerichtet (--86box)"
fi
echo; echo "$ok ok, $bad fehlen.  Umgebung: . $ENVF"
[ $bad = 0 ]
