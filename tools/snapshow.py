#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SNAPn.BIN (tools/snap.asm) als Text zeigen: snapshow.py SNAP0.BIN [Zeilen]"""
import sys
d=open(sys.argv[1],'rb').read()
rows=int(sys.argv[2]) if len(sys.argv)>2 else 25
for r in range(rows):
    print(''.join(chr(c) if 32<=c<127 else ('#' if c in (0xB3,0xBA,0xC4,0xCD) else ('.' if c>=128 else ' ')) for c in d[r*160:r*160+160:2]).rstrip())
