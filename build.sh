#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Baut build/atdos-fd.img (1,44-MB-Diskette): Bootsektor + Kernel + 3 Anwendungen
# Benoetigt: nasm, Open Watcom 2.0 (WATCOM gesetzt, wcc/wlink im PATH)
set -e
cd "$(dirname "$0")"
B=build
mkdir -p $B

# Programme: 286-Befehle (-2), laufen so auch unter AT-DOS 5.x;
# Kernel, Treiber, IFS: 386 (-3) ab AT-DOS 6.0
CFLAGS="-q -ms -2 -s -zl -zls -zp1 -d0 -ox -wx -we"
KCFLAGS="-q -ms -3 -s -zl -zls -zp1 -d0 -ox -wx -we"
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

# Build-Nummer fuer VER (Anzahl der Commits, Kurz-Hash, Datum)
# in der CI die Lauf-Nummer von GitHub (wie auf der Actions-Seite und im
# Release), lokal die Anzahl der Commits mit Zusatz "lokal"
if [ -n "$GITHUB_RUN_NUMBER" ]; then
    BUILDNO=$GITHUB_RUN_NUMBER
else
    BUILDNO="$(git rev-list --count HEAD 2>/dev/null || echo 0) lokal"
fi
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
nasm -f bin boot/fat32boot.asm -o $B/fat32boot.bin
nasm -f bin boot/mdosboot.asm -o $B/mdosboot.bin    # MKDOSIMG
# Bootcode fuer FORMAT/SYS (UFAT.EXE, SYS.EXE) als C-Feld
python3 - $B/boot.bin $B/hdboot.bin $B/fat32boot.bin $B/mdosboot.bin > $B/bootcode.h <<'PYEOF'
import sys
for name, path, at in (("boot_fd", sys.argv[1], 0), ("boot_hd", sys.argv[2], 0),
                       ("boot_32", sys.argv[3], 0), ("boot_32b", sys.argv[3], 512),
                       ("boot_md", sys.argv[4], 0)):
    d = open(path, "rb").read()[at:at + 512]   # FAT32: zweite Haelfte = Sektor 2
    if name == "boot_fd":
        print("#ifndef BOOTMD                  /* MKDOSIMG nur boot_md */")
    if name == "boot_32":
        print("#endif")
        print("#ifdef BOOT32                   /* nur SYS braucht FAT32 */")
    if name == "boot_md":
        print("#ifdef BOOTMD                   /* nur MKDOSIMG */")
    print("static const unsigned char %s[512] = {" % name)
    for i in range(0, 512, 16):
        print("    " + ", ".join("0x%02X" % b for b in d[i:i+16]) + ",")
    print("};")
    if name in ("boot_32b", "boot_md"):
        print("#endif")
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
K2MODS="floppy vgagfx bus pnp mem paging k2misc vesa fat swap blkmnt codeswap"
# KTEXT3: DOS-Boxen im dritten Codesegment (KCODE3)
K3MODS="v86"
# KTEXT4: viertes Codesegment (KCODE4), Erreichbarkeit wie KTEXT2 (k2.h)
K4MODS="loop vio net redir dem vdd ufs panic"
for m in kernel mem floppy fat vio kbd preload vgagfx bus pnp paging v86 redir dem k2misc vesa loop swap blkmnt codeswap net vdd ufs panic k2thunk; do
    S=""
    case " $K2MODS " in *" $m "*) S="-nt=KTEXT2" ;; esac
    case " $K3MODS " in *" $m "*) S="-nt=KTEXT3" ;; esac
    case " $K4MODS " in *" $m "*) S="-nt=KTEXT4" ;; esac
    wcc $KCFLAGS $S -fo=$B/$m.o kernel/$m.c
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
    if len(f) == 3 and f[0] in ('CGROUP', 'CGROUP2', 'CGROUP3', 'CGROUP4', 'DGROUP') and ':' in f[1]:
        s, o = f[1].split(':')
        g[f[0]] = (int(s, 16) * 16 + int(o, 16), int(f[2], 16))
code, data = g['CGROUP'][1], g['DGROUP'][1]
c2 = g.get('CGROUP2', ((code + 15) & ~15, 0))
if c2[0] != (code + 15) & ~15:
    sys.exit('FEHLER: CGROUP2 liegt nicht direkt hinter CGROUP')
c3 = g.get('CGROUP3', ((c2[0] + c2[1] + 15) & ~15, 0))
if c3[0] != (c2[0] + c2[1] + 15) & ~15:
    sys.exit('FEHLER: CGROUP3 liegt nicht direkt hinter CGROUP2')
c4 = g.get('CGROUP4', ((c3[0] + c3[1] + 15) & ~15, 0))
if c4[0] != (c3[0] + c3[1] + 15) & ~15:
    sys.exit('FEHLER: CGROUP4 liegt nicht direkt hinter CGROUP3')
if g['DGROUP'][0] != (c4[0] + c4[1] + 15) & ~15:
    sys.exit('FEHLER: DGROUP liegt nicht direkt hinter dem Code')
# Fernaufrufe zwischen CGROUP und CGROUP2: der Kernel liegt immer bei
# 10000h, also die Segmentwerte gleich als Selektoren eintragen
# (KCODE 08h, KCODE2 2A8h, KCODE3 2F8h, KCODE4 320h, siehe kern.h); alles andere ist ein Fehler
img = hdr * 16
for i in range(nrel):
    o, s = struct.unpack('<HH', d[relo + 4 * i:relo + 4 * i + 4])
    at = img + s * 16 + o
    v = struct.unpack('<H', d[at:at + 2])[0]
    if v == 0:
        sel = 0x08
    elif v == c2[0] >> 4:
        sel = 0x2A8
    elif v == c3[0] >> 4:
        sel = 0x2F8
    elif v == c4[0] >> 4 and c4[1]:
        sel = 0x320
    else:
        sys.exit('FEHLER: Segment-Relokation auf %04Xh (nur KCODE/KCODE2/KCODE3/KCODE4 erlaubt)' % v)
    d[at:at + 2] = struct.pack('<H', sel)
open(sys.argv[2], 'wb').write(d[img:])
if code > 65520:
    sys.exit('FEHLER: erstes Kernel-Codesegment %d Bytes ueber 65520 (start.asm rundet auf 16)' % code)
if code > 65536 or c2[1] > 65536 or c3[1] > 65536 or c4[1] > 65536 or data > 65536:
    sys.exit('FEHLER: Kernel-Code %d/%d/%d/%d oder -Daten %d Bytes ueber 64 KB' % (code, c2[1], c3[1], c4[1], data))
print('  kernel: %d Bytes Datei, Code %d + %d + %d + %d Bytes, Daten %d Bytes mit BSS (je max. 65536), %d Fernaufrufe'
      % (len(d) - img, code, c2[1], c3[1], c4[1], data, nrel))
PYEOF
# der Diskettenbootsektor laedt 160 Sektoren; BSS dahinter nullt der Kernel selbst
# der Diskettenbootsektor laedt nach Dateigroesse ab 1000:0000; Platz bis
# 80000h (darueber Real-Mode-Reste, ab 9FC00h das EBDA)
[ $(stat -c %s $B/kernel.bin) -le 458752 ] || { echo "FEHLER: ATDOS.SYS groesser als 448 KB"; exit 1; }

echo "Anwendungen"
nasm -f obj apps/crt0.asm -o $B/crt0.o
# Programme (FX-Version 2): Code und Daten getrennt, als DOS-EXE gelinkt
# wie der Kernel (gruppenrelative Offsets), mkfx.py nimmt den Kopf ab
ALINK="option quiet format dos option nodefaultlibs \
      option map order clname CODE clname BEGDATA clname DATA clname BSS"
# Werkzeuge mit gemeinsamer Hilfsbibliothek (apps/util.c)
ARCAPPS="gzip gunzip tar zip unzip"
LZHAPPS="unarj unzoo unlha lha arj zoo"        # ARJ/ZOO/LHA (lzh.c, lzhenc.c)
UTILAPPS="$ARCAPPS $LZHAPPS 7zdec 7za attrib move deltree tree label choice sort fc xcopy format sys edit mode mtest grafik fputest busy joytest fdisk diskcopy subst join ansi fm diskcomp comp replace expand compress cut head tail wc uniq tr cat strings md5sum sha1sum hexdump xxd dmesg fragtest play mixer undel scandisk msd print at backup restore defrag setver vesa vdemo taskman convert aweload eject devload loop packdisk setup ping ipconfig nslookup dig nc netstat firewall mkdosimg wget ftp telnet sntp smbcli net kinit klist kdestroy ssh sftp domjoin logon hostname net1"
wcc $CFLAGS -fo=$B/util.o apps/util.c
wcc $CFLAGS -fo=$B/flate.o apps/flate.c      # Deflate, CRC-32 (Packprogramme)
wcc $CFLAGS -fo=$B/arcutil.o apps/arcutil.c
wcc $CFLAGS -fo=$B/tz.o apps/tz.c
wcc $CFLAGS -fo=$B/crypto.o apps/crypto.c      # SHA-256, AES-GCM, X25519 (TLS)
wcc $CFLAGS -fo=$B/tls.o apps/tls.c            # TLS-1.2-Client (WGET)
wcc $CFLAGS -fo=$B/pki.o apps/pki.c            # X.509, RSA, ECDSA
wcc $CFLAGS -fo=$B/sha512.o apps/sha512.c      # SHA-384/512 (pki, ed25519, SMB 3.1.1)
wcc $CFLAGS -fo=$B/smb3c.o apps/smb3c.c        # AES-CMAC/CCM, KDF (SMB 3)
wcc $CFLAGS -fo=$B/rnd.o apps/rnd.c            # Zufall (FAPI_RANDOM gehasht)
wcc $CFLAGS -fo=$B/ed25519.o apps/ed25519.c    # Ed25519-Pruefung (SSH)
wcc $CFLAGS -fo=$B/ssh.o apps/ssh.c            # SSH-2-Client (SSH, SFTP)
wcc $CFLAGS -fo=$B/krbcrypt.o apps/krbcrypt.c  # SHA-1, PBKDF2, AES-CTS (Kerberos)
wcc $CFLAGS -fo=$B/krb5.o apps/krb5.c          # Kerberos v5 (KINIT, SMBCLI)
wcc $CFLAGS -fo=$B/ldapc.o apps/ldapc.c        # LDAP-Client (DOMJOIN, LOGON)
wcc $CFLAGS -fo=$B/lzh.o apps/lzh.c          # LH5-Familie, lh1, ARJ, ZOO-LZW
wcc $CFLAGS -fo=$B/lzhenc.o apps/lzhenc.c    # Packer LH5-Familie (LHA, ARJ)
wcc $CFLAGS -fo=$B/lzma.o apps/lzma.c        # LZMA/LZMA2, Delta (7ZDEC)
wcc $CFLAGS -fo=$B/bcj.o apps/bcj.c          # BCJ x86 (7ZDEC, 7ZA)
wcc $CFLAGS -fo=$B/lzmaenc.o apps/lzmaenc.c  # LZMA-Packer (7ZA)
for t in cmd keyb a b c mem more find grep less chkdsk ufat ufat32 $UTILAPPS; do
    U=""
    case " $UTILAPPS " in *" $t "*) U="file $B/util.o" ;; esac
    case " $ARCAPPS " in *" $t "*) U="$U file $B/flate.o file $B/arcutil.o" ;; esac
    case " $LZHAPPS " in *" $t "*) U="$U file $B/lzh.o file $B/arcutil.o" ;; esac
    case " lha arj zoo " in *" $t "*) U="$U file $B/lzhenc.o" ;; esac
    [ $t = 7zdec ] && U="$U file $B/lzma.o file $B/bcj.o file $B/arcutil.o"   # 7z (lzma.c)
    [ $t = sntp ] && U="$U file $B/tz.o file $B/arcutil.o"
    [ $t = net ] && U="$U file $B/tz.o file $B/arcutil.o"
    [ $t = net1 ] && U="$U file $B/tz.o"
    [ $t = wget ] && U="$U file $B/tls.o file $B/pki.o file $B/sha512.o file $B/rnd.o file $B/crypto.o"
    case " ssh sftp " in *" $t "*) U="$U file $B/ssh.o file $B/ed25519.o file $B/pki.o file $B/sha512.o file $B/rnd.o file $B/crypto.o" ;; esac
    [ $t = smbcli ] && U="$U file $B/smb3c.o file $B/sha512.o file $B/krb5.o file $B/krbcrypt.o file $B/rnd.o file $B/crypto.o file $B/arcutil.o"
    case " domjoin logon net1 " in *" $t "*) U="$U file $B/ldapc.o" ;; esac
    case " kinit klist kdestroy domjoin logon net1 " in *" $t "*) U="$U file $B/krb5.o file $B/krbcrypt.o file $B/rnd.o file $B/crypto.o file $B/arcutil.o" ;; esac
    [ $t = 7za ] && U="$U file $B/lzmaenc.o file $B/bcj.o file $B/arcutil.o"  # 7z-Packer
    wcc $CFLAGS -fo=$B/task_$t.o apps/task_$t.c
    wlink $ALINK name $B/task_$t.bin option map=$B/task_$t.map \
          file $B/crt0.o file $B/task_$t.o $U 2>&1 | grep -v W1014 || true
done

echo "DOS-Stub mit FAPI-Uebersetzung (stub/)"
nasm -f obj stub/scrt.asm -o $B/scrt.o
wcc -q -ms -0 -s -zl -zls -zp1 -d0 -os -wx -we -fo=$B/fapidos.o stub/fapidos.c
wlink $LINK name $B/dstub.bin option map=$B/dstub.map \
      file $B/scrt.o file $B/fapidos.o 2>&1 | grep -v W1014 || true

echo "Eigener Box-Kern (atdoscm/)"
nasm -f obj atdoscm/entry.asm -o $B/cmentry.o
for f in kern file fcb con exec dosint config lfn; do
    wcc -q -ms -0 -s -zl -zls -zp1 -d0 -os -wx -we -fo=$B/cm_$f.o atdoscm/$f.c
done
# Code und Daten getrennt (DOS=HIGH: Code in die HMA), als DOS-EXE gelinkt
# wie der Kernel; Kopf ab, keine Segment-Relokationen, DGROUP direkt hinter
# dem Code (entry.asm rechnet so)
wlink option quiet format dos option nodefaultlibs \
      option map order clname CODE clname DATA clname BSS \
      name $B/atdoscm.exe option map=$B/atdoscm.map file $B/cmentry.o \
      file $B/cm_kern.o file $B/cm_file.o file $B/cm_fcb.o file $B/cm_con.o file $B/cm_exec.o file $B/cm_dosint.o file $B/cm_config.o file $B/cm_lfn.o \
      2>&1 | grep -v W1014 || true
python3 - $B/atdoscm.exe $B/atdoscm.sys $B/atdoscm.map <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
nrel, hdr = struct.unpack('<HH', d[6:10])
if nrel:
    sys.exit('FEHLER: ATDOSCM.SYS hat %d Segment-Relokationen' % nrel)
g = {}
for l in open(sys.argv[3]):
    f = l.split()
    if len(f) == 3 and f[0] in ('CGROUP', 'DGROUP') and ':' in f[1]:
        s, o = f[1].split(':')
        g[f[0]] = (int(s, 16) * 16 + int(o, 16), int(f[2], 16))
code, data = g['CGROUP'][1], g['DGROUP'][1]
if g['CGROUP'][0] != 0 or g['DGROUP'][0] != (code + 15) & ~15:
    sys.exit('FEHLER: ATDOSCM.SYS: DGROUP nicht direkt hinter dem Code')
if code > 65520 - 16 or data > 65520:
    sys.exit('FEHLER: ATDOSCM.SYS: Code %d oder Daten %d zu gross' % (code, data))
open(sys.argv[2], 'wb').write(d[hdr * 16:])
print('  atdoscm: Code %d Bytes (HMA), Daten %d Bytes mit BSS' % (code, data))
PYEOF

echo "Programme (FX mit DOS-Stub)"
nasm -f bin tools/stub.asm -o $B/stub.bin
nasm -f bin apps/hallo.asm -o $B/hallo.com   # Test der DOS-Boxen (V86)
nasm -f bin apps/boxtest.asm -o $B/boxtest.com
nasm -f bin tools/bioscmp.asm -o $B/bioscmp.com          # Vergleichstest (docs/BOX-BIOS.md)
nasm -f bin tools/dosbench.asm -o $B/dosbench.com        # Messung Stufe 0 (docs/DOSKERN.md)
nasm -f bin tools/dosstruc.asm -o $B/dosstruc.com
nasm -f bin apps/bgtest.asm -o $B/bgtest.com
nasm -f bin apps/bankt.asm -o $B/bankt.com
nasm -f bin apps/svbg.asm -o $B/svbg.com
nasm -f bin apps/vbetest.asm -o $B/vbetest.com
nasm -f bin apps/opltest.asm -o $B/opltest.com
# Programme, die auch unter MS-DOS laufen (gebunden wie mit BIND bei OS/2:
# der Stub uebersetzt die FAPI in DOS-/BIOS-Aufrufe, die .EXE wird groesser)
BINDAPPS="$ARCAPPS $LZHAPPS 7zdec 7za setver hostname dig nslookup more find grep less sort fc comp cut head tail wc uniq tr cat strings md5sum sha1sum hexdump xxd expand compress edit fm tree attrib grafik xcopy move deltree choice replace label msd"
# Programme, die lange Namen sauber verarbeiten (FX_FLAG_LONGNAMES); alle
# anderen bekommen bei der Dateisuche nur 8.3-Namen (wie OS/2 1.2)
LFNAPPS="$ARCAPPS $LZHAPPS 7zdec 7za cmd attrib tree deltree grep print backup restore xcopy move join edit fm comp replace expand compress cut head tail wc uniq tr cat strings md5sum sha1sum hexdump xxd packdisk wget ftp smbcli sftp"
for t in cmd keyb a b c mem more find grep less chkdsk ufat ufat32 $UTILAPPS; do
    L=""
    case " $LFNAPPS " in *" $t "*) L="--lfn" ;; esac
    case " $BINDAPPS " in *" $t "*) L="$L --bind=$B/dstub.bin,$B/dstub.map" ;; esac
    python3 tools/mkfx.py $L $B/stub.bin $B/task_$t.bin $B/task_$t.map $B/task_$t.exe
done

echo "Treiber"
nasm -f obj drivers/dcrt0.asm -o $B/dcrt0.o
for t in aha154x ahci aic7xxx ide mouse ncr5380 ncr8xx rtl8139 pcnet e1000 sb16 vcom vsb vems vega vmouse vioblk vionet; do
    wcc $KCFLAGS -zu -fo=$B/drv_$t.o drivers/$t.c
    wlink $LINK name $B/drv_$t.bin option map=$B/drv_$t.map \
          file $B/dcrt0.o file $B/drv_$t.o 2>&1 | grep -v W1014 || true
    python3 tools/mkfx.py --driver $B/stub.bin $B/drv_$t.bin $B/drv_$t.map $B/drv_$t.sys
done
# VCGA.SYS = VEGA.SYS mit -DCGA (Grafikmodul der DOS-Box fuer die CGA)
wcc $KCFLAGS -zu -DCGA -fo=$B/drv_vcga.o drivers/vega.c
wlink $LINK name $B/drv_vcga.bin option map=$B/drv_vcga.map \
      file $B/dcrt0.o file $B/drv_vcga.o 2>&1 | grep -v W1014 || true
python3 tools/mkfx.py --driver $B/stub.bin $B/drv_vcga.bin $B/drv_vcga.map $B/drv_vcga.sys

echo "Netzwerk"
# 286-Befehle ($CFLAGS): NE2000.SYS und TCPIP.SYS sollen auch auf 5.x laufen
for t in tcpip ne2000; do
    S=net; [ $t = ne2000 ] && S=drivers
    wcc $CFLAGS -zu -fo=$B/net_$t.o $S/$t.c
    wlink $LINK name $B/net_$t.bin option map=$B/net_$t.map \
          file $B/dcrt0.o file $B/net_$t.o 2>&1 | grep -v W1014 || true
    python3 tools/mkfx.py --driver $B/stub.bin $B/net_$t.bin $B/net_$t.map $B/net_$t.sys
done

echo "Dateisysteme (IFS)"
for t in iso9660; do
    wcc $KCFLAGS -zu -fo=$B/ifs_$t.o ifs/$t.c
    wlink $LINK name $B/ifs_$t.bin option map=$B/ifs_$t.map \
          file $B/dcrt0.o file $B/ifs_$t.o 2>&1 | grep -v W1014 || true
    python3 tools/mkfx.py --driver $B/stub.bin $B/ifs_$t.bin $B/ifs_$t.map $B/ifs_$t.ifs
done

echo "Diskette (FAT12)"
IMG=$B/atdos-fd.img
export MTOOLS_SKIP_CHECK=1
truncate -s 0 $IMG
truncate -s 1474560 $IMG
dd if=$B/boot.bin of=$IMG conv=notrunc status=none
# beide FATs initialisieren: Medienbyte F0h, Cluster 1 = Ende
for fat in 1 10; do
    printf '\360\377\377' | dd of=$IMG bs=1 seek=$((fat * 512)) conv=notrunc status=none
done
# ATDOS.SYS als erste Datei, damit sie zusammenhaengend ab Cluster 2 liegt
# (der Bootsektor liest so viele Sektoren, wie die Datei gross ist)
cp $B/kernel.bin $B/ATDOS.SYS
mcopy -i $IMG $B/ATDOS.SYS ::ATDOS.SYS
mattrib -i $IMG +r +s +h ::ATDOS.SYS
# alle uebrigen Dateien: Quelle Ziel (Stamm), dann \DOS
FILES="$B/task_cmd.exe CMD.EXE
       CONFIG.SYS CONFIG.SYS  AUTOEXEC.CMD AUTOEXEC.CMD"
DOSFILES="LINUX.MAC LINUX.MAC  $B/task_dmesg.exe DMESG.EXE  $B/task_keyb.exe KEYB.EXE  $B/task_mem.exe MEM.EXE
       $B/task_more.exe MORE.EXE  $B/task_find.exe FIND.EXE  $B/task_grep.exe GREP.EXE
       $B/task_less.exe LESS.EXE  $B/task_chkdsk.exe CHKDSK.EXE
       $B/task_attrib.exe ATTRIB.EXE  $B/task_move.exe MOVE.EXE  $B/task_deltree.exe DELTREE.EXE
       $B/task_tree.exe TREE.EXE  $B/task_label.exe LABEL.EXE  $B/task_choice.exe CHOICE.EXE
       $B/task_sort.exe SORT.EXE  $B/task_fc.exe FC.EXE  $B/task_xcopy.exe XCOPY.EXE
       $B/task_format.exe FORMAT.EXE  $B/task_sys.exe SYS.EXE  $B/task_convert.exe CONVERT.EXE
       $B/task_edit.exe EDIT.EXE  $B/task_mode.exe MODE.EXE  $B/task_fdisk.exe FDISK.EXE
       $B/task_diskcopy.exe DISKCOPY.EXE  $B/task_subst.exe SUBST.EXE  $B/task_join.exe JOIN.EXE  $B/task_ansi.exe ANSI.EXE  $B/task_fm.exe FM.EXE  $B/task_diskcomp.exe DISKCOMP.EXE  $B/task_comp.exe COMP.EXE  $B/task_replace.exe REPLACE.EXE  $B/task_expand.exe EXPAND.EXE  $B/task_play.exe PLAY.EXE  $B/task_mixer.exe MIXER.EXE  $B/task_eject.exe EJECT.EXE  $B/task_devload.exe DEVLOAD.EXE  $B/task_setup.exe SETUP.EXE
       $B/task_undel.exe UNDELETE.EXE  $B/task_scandisk.exe SCANDISK.EXE
       $B/task_msd.exe MSD.EXE  $B/task_print.exe PRINT.EXE  $B/task_at.exe AT.EXE  $B/task_defrag.exe DEFRAG.EXE $B/task_setver.exe SETVER.EXE  $B/task_vesa.exe VESA.EXE  $B/task_taskman.exe TASKMAN.EXE  $B/task_ping.exe PING.EXE  $B/task_ipconfig.exe IPCONFIG.EXE  $B/task_nslookup.exe NSLOOKUP.EXE  $B/task_nc.exe NC.EXE  $B/task_wget.exe WGET.EXE  $B/task_ftp.exe FTP.EXE  $B/task_telnet.exe TELNET.EXE  $B/task_sntp.exe SNTP.EXE  $B/task_smbcli.exe SMBCLI.EXE  $B/task_kinit.exe KINIT.EXE  $B/task_domjoin.exe DOMJOIN.EXE $B/task_netstat.exe NETSTAT.EXE  $B/task_firewall.exe FIREWALL.EXE
       apps/dosbox.cmd DOSBOX.CMD"
# \DOS\SYSTEM: was das System nachlaedt (Treiber, IFS, Pruefprogramme, Belegungen)
SYSFILES="$B/drv_aha154x.sys AHA154X.SYS  $B/drv_ide.sys IDE.SYS  $B/drv_mouse.sys MOUSE.SYS  $B/drv_ncr5380.sys NCR5380.SYS  $B/drv_ncr8xx.sys NCR8XX.SYS  $B/drv_aic7xxx.sys AIC7XXX.SYS  $B/drv_ahci.sys AHCI.SYS  $B/drv_sb16.sys SB16.SYS  $B/drv_rtl8139.sys RTL8139.SYS  $B/drv_pcnet.sys PCNET.SYS  $B/drv_e1000.sys E1000.SYS  $B/net_tcpip.sys TCPIP.SYS  $B/net_ne2000.sys NE2000.SYS  $B/drv_vcom.sys VCOM.SYS  $B/drv_vsb.sys VSB.SYS  $B/drv_vems.sys VEMS.SYS  $B/drv_vega.sys VEGA.SYS  $B/drv_vcga.sys VCGA.SYS  $B/drv_vmouse.sys VMOUSE.SYS  $B/drv_vioblk.sys VIOBLK.SYS  $B/drv_vionet.sys VIONET.SYS  FIREWALL.INI FIREWALL.INI  HOSTS HOSTS
       $B/ifs_iso9660.ifs ISO9660.IFS  $B/task_ufat.exe UFAT.EXE  $B/task_ufat32.exe UFAT32.EXE
       $B/US.KBL US.KBL  $B/GB.KBL GB.KBL  $B/FR.KBL FR.KBL  $B/SP.KBL SP.KBL  $B/IT.KBL IT.KBL
       $B/DK.KBL DK.KBL  $B/NO.KBL NO.KBL  $B/SV.KBL SV.KBL  $B/PL.KBL PL.KBL"
python3 tools/mkaudio.py $B                # Klangbeispiele fuer PLAY
DEMOFILES="$B/bioscmp.com BIOSCMP.COM  $B/task_a.exe TASKA.EXE  $B/task_b.exe TASKB.EXE  $B/task_c.exe TASKC.EXE
       $B/task_grafik.exe GRAFIK.EXE  $B/hallo.com HALLO.COM  $B/boxtest.com BOXTEST.COM
       $B/ODE.MID ODE.MID  $B/ODE.IMF ODE.IMF"
# Werkzeuge der Setup-Diskette 2 (Packprogramme, GNU-Textwerkzeuge, ...)
HDDOS="$B/task_logon.exe LOGON.EXE  $B/task_dig.exe DIG.EXE  $B/task_hostname.exe HOSTNAME.EXE  $B/task_backup.exe BACKUP.EXE  $B/task_restore.exe RESTORE.EXE  $B/task_klist.exe KLIST.EXE  $B/task_kdestroy.exe KDESTROY.EXE  $B/task_gzip.exe GZIP.EXE  $B/task_gunzip.exe GUNZIP.EXE  $B/task_tar.exe TAR.EXE  $B/task_zip.exe ZIP.EXE  $B/task_unzip.exe UNZIP.EXE  $B/task_unarj.exe UNARJ.EXE  $B/task_unzoo.exe UNZOO.EXE  $B/task_unlha.exe UNLHA.EXE  $B/task_7zdec.exe 7ZDEC.EXE  $B/task_lha.exe LHA.EXE  $B/task_arj.exe ARJ.EXE  $B/task_zoo.exe ZOO.EXE  $B/task_7za.exe 7ZA.EXE  $B/task_cat.exe CAT.EXE  $B/task_strings.exe STRINGS.EXE  $B/task_md5sum.exe MD5SUM.EXE  $B/task_sha1sum.exe SHA1SUM.EXE  $B/task_hexdump.exe HEXDUMP.EXE  $B/task_xxd.exe XXD.EXE  $B/task_loop.exe LOOP.EXE  $B/task_packdisk.exe PACKDISK.EXE  $B/task_mkdosimg.exe MKDOSIMG.EXE  $B/task_aweload.exe AWELOAD.EXE  $B/task_cut.exe CUT.EXE  $B/task_head.exe HEAD.EXE  $B/task_tail.exe TAIL.EXE  $B/task_wc.exe WC.EXE  $B/task_uniq.exe UNIQ.EXE  $B/task_tr.exe TR.EXE  $B/task_compress.exe COMPRESS.EXE  $B/task_ssh.exe SSH.EXE  $B/task_sftp.exe SFTP.EXE"
# Testprogramme (Setup-Diskette 2)
HDDEMO="$B/dosbench.com DOSBENCH.COM  $B/dosstruc.com DOSSTRUC.COM  $B/task_fragtest.exe FRAGTEST.EXE  $B/task_mtest.exe MAUSTEST.EXE  $B/task_fputest.exe FPUTEST.EXE  $B/task_busy.exe BUSY.EXE  $B/task_joytest.exe JOYTEST.EXE  $B/bgtest.com BGTEST.COM $B/bankt.com BANKT.COM  $B/svbg.com SVBG.COM  $B/opltest.com OPLTEST.COM  $B/vbetest.com VBETEST.COM  $B/task_vdemo.exe VDEMO.EXE"
# Startabbild der DOS-Box (Phase 3): 360-KB-Diskette mit FreeDOS-Bootsektor,
# KERNEL.SYS zuerst (der Bootsektor folgt der FAT-Kette), FreeCOM, BOXSHELL
echo "DOS-Box: Startabbild freedos.img"
nasm -f bin apps/boxshell.asm -o $B/boxshell.com
rm -f $B/freedos.img
mformat -i $B/freedos.img -C -f 360 -B freedos/fat12com.bin -v FREEDOS ::
for f in KERNEL.SYS COMMAND.COM CONFIG.SYS COUNTRY.SYS; do
    mcopy -i $B/freedos.img freedos/$f ::$f
done
mcopy -i $B/freedos.img $B/boxshell.com ::BOXSHELL.COM
nasm -f bin apps/atbox.asm -o $B/atbox.sys     # damit FreeDOS die UMBs nimmt
nasm -f bin apps/lock.asm -o $B/lock.com       # LOCK/UNLOCK wie MS-DOS 7
nasm -f bin -DUNLOCK apps/lock.asm -o $B/unlock.com
mcopy -i $B/freedos.img $B/atbox.sys ::ATBOX.SYS
BOXFILES="$B/atdoscm.sys ATDOSCM.SYS  atdoscm/CONFIG.SYS CONFIG.SYS  $B/boxshell.com BOXSHELL.COM  $B/atbox.sys ATBOX.SYS  freedos/COMMAND.COM COMMAND.COM  freedos/MEM.EXE MEM.EXE  freedos/MEM.DE MEM.DE  freedos/DOSBOX.INI DOSBOX.INI  freedos/BOXAUTO.BAT BOXAUTO.BAT  freedos/README.txt README.TXT  freedos/COPYING COPYING"
# Diskette 3 (Diskette 2 ist voll): FreeDOS-Abbild (Rueckfall, DOS_KERNEL=
# FREEDOS.IMG), COMMAND.COM 4.0 (msdos4/, MIT), LOCK/UNLOCK
HDMDOS="$B/freedos.img FREEDOS.IMG  msdos4/COMMAND4.COM COMMAND4.COM  msdos4/README.txt COMMAND4.TXT  msdos4/LICENSE COMMAND4.LIC  $B/lock.com LOCK.COM  $B/unlock.com UNLOCK.COM"
# Diskette 3, \DOS (Disketten 1 und 2 sind voll): NET mit Unterbefehlen
HDDOS3="$B/task_net.exe NET.EXE  $B/task_net1.exe NET1.EXE"
# Diskette 1 (startfaehig, Notfall): System, Werkzeuge in \DOS, Treiber;
# Diskette 2: DOS-Box, Zeichensaetze, Packprogramme, Demos. Die Platte
# bekommt beides (copy_files + copy_extra), SETUP.EXE kopiert dasselbe.
CPFILES=""
for f in freedos/cp/CP*.FNT; do CPFILES="$CPFILES $f $(basename $f)"; done
copy_files() {   # Ziel-Laufwerk fuer mcopy, z. B. "-i atdos-fd.img ::"
    copy_list "$FILES" ""
    mmd $TARGET "${TO}DOS" "${TO}DOS/SYSTEM" "${TO}DOS/SYSTEM/SFBANK"
    copy_list "$DOSFILES" "DOS/"
    copy_list "$SYSFILES" "DOS/SYSTEM/"
    copy_list "certs/cacert.pem CACERT.PEM" "DOS/SYSTEM/"     # TLS (MPL-2.0)
}
copy_extra() {
    mmd $TARGET "${TO}DOS/MDOS" "${TO}DEMO"
    copy_list "$BOXFILES" "DOS/MDOS/"
    copy_list "$CPFILES" "DOS/SYSTEM/"
    copy_list "$HDDOS" "DOS/"
    copy_list "$DEMOFILES" "DEMO/"
    copy_list "$B/KLANG.WAV KLANG.WAV" "DEMO/"
    copy_list "$HDDEMO" "DEMO/"
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
# Startdatei der Diskette 1 ruft SETUP auf (SETUP laesst die Zeile weg)
{ cat AUTOEXEC.CMD; printf 'SETUP /START\r\n'; } > $B/autoexec.fd
mcopy -o -i $IMG $B/autoexec.fd ::AUTOEXEC.CMD
mlabel -i $IMG '::AT-DOS 1'
# Diskette 2 (nicht startfaehig)
IMG2=$B/atdos-fd2.img
rm -f $IMG2
mformat -i $IMG2 -C -f 1440 -v 'AT-DOS 2' ::
mmd -i $IMG2 ::DOS ::DOS/SYSTEM
TARGET="-i $IMG2" TO="::" copy_extra
# Diskette 3 (nicht startfaehig, Erlaubnis des Nutzers: weitere Disketten,
# wenn 2 nicht reichen)
IMG3=$B/atdos-fd3.img
rm -f $IMG3
mformat -i $IMG3 -C -f 1440 -v 'AT-DOS 3' ::
mmd -i $IMG3 ::DOS ::DOS/MDOS
TARGET="-i $IMG3" TO="::" copy_list "$HDMDOS" "DOS/MDOS/"
TARGET="-i $IMG3" TO="::" copy_list "$HDDOS3" "DOS/"
# SETUP.ID: Nummer, belegte KB aller Disketten (Fortschritt, Platz; SETUP
# fordert so viele Disketten an, wie Groessen dastehen)
used_kb() { mdu -s -i "$1" :: | awk '{print int($2 / 2)}'; }
KB1=$(used_kb $IMG); KB2=$(used_kb $IMG2); KB3=$(used_kb $IMG3)
for n in 1 2 3; do
    printf 'ATDOS %s %s %s %s\r\n' $n $KB1 $KB2 $KB3 > $B/setup$n.id
done
mcopy -i $IMG $B/setup1.id ::SETUP.ID
mcopy -i $IMG2 $B/setup2.id ::SETUP.ID
mcopy -i $IMG3 $B/setup3.id ::SETUP.ID
mdir -a -i $IMG :: | sed 's/^/  /'
mdir -i $IMG ::DOS | sed 's/^/  /'
echo "Setup-Diskette 2 (atdos-fd2.img)"
mdir -i $IMG2 :: | tail -2 | sed 's/^/  /'
echo "Setup-Diskette 3 (atdos-fd3.img)"
mdir -i $IMG3 :: | tail -2 | sed 's/^/  /'

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
TARGET="" TO="c:" copy_extra
TARGET="" TO="c:" copy_list "$HDMDOS" "DOS/MDOS/"
TARGET="" TO="c:" copy_list "$HDDOS3" "DOS/"
mdir -a c: | sed 's/^/  /'
mdir c:DOS | sed 's/^/  /'
unset MTOOLSRC
rm -f $B/ATDOS.SYS
