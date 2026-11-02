#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bootsektor-Code in eine FAT16-Partition schreiben, BPB bleibt erhalten.

    sysboot.py ABBILD OFFSET_BYTES hdboot.bin

Wie SYS unter MS-DOS: Bytes 0-2 (Sprung) und ab 3Eh (Code, Signatur) kommen
aus hdboot.bin, Bytes 3-3Dh (BPB des Dateisystems) bleiben stehen.
ATSYS.COM macht dasselbe unter MS-DOS.
"""
import sys

img, off, code = sys.argv[1], int(sys.argv[2]), open(sys.argv[3], 'rb').read()
assert len(code) == 512
with open(img, 'r+b') as f:
    f.seek(off)
    old = f.read(512)
    if old[0x26] != 0x29 or old[0x36:0x3E] != b'FAT16   ' or old[11:13] != b'\x00\x02':
        sys.exit('sysboot: keine FAT16-Partition mit erweitertem BPB')
    new = code[:3] + old[3:0x3E] + code[0x3E:]
    f.seek(off)
    f.write(new)
