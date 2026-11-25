#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Importe fuer Win16-Programme der GUI ohne Importbibliothek: liest die
# externen Namen (OMF EXTDEF) der Objektdateien und gibt fuer jeden, der in
# gui/ordinals.txt steht, eine wlink-Zeile "import NAME MODUL.nummer" aus.
import struct, sys, os
ords = {}
for l in open(os.path.join(os.path.dirname(__file__), '..', 'gui', 'ordinals.txt')):
    if l.startswith('#') or not l.strip():
        continue
    m, o, n = l.split()
    ords.setdefault(n, (m, o))
seen = []
for f in sys.argv[1:]:
    d = open(f, 'rb').read()
    i = 0
    while i + 3 <= len(d):
        t = d[i]; l = struct.unpack_from('<H', d, i + 1)[0]
        r = d[i + 3:i + 2 + l]
        if t == 0x8C:
            p = 0
            while p < len(r):
                n = r[p]; nm = r[p + 1:p + 1 + n].decode(); p += 1 + n
                p += 1 if r[p] < 0x80 else 2           # Typindex
                if nm in ords and nm not in seen:
                    seen.append(nm)
        i += 3 + l
for nm in seen:
    print('import %s %s.%s' % (nm, ords[nm][0], ords[nm][1]))
