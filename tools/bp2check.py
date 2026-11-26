#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Prueft die GUI-Objekte (wdis-Ausgabe auf stdin): Exporte, die mit get_bp2()
# das DS des Aufrufers lesen (mov ax,[bp+2] = 8B 46 02), duerfen nach dem
# __loadds-Prolog vor `push bp` nichts sichern - sonst liegt bei [bp+2] SI
# oder DI statt DS (MAIN.CPL: LocalAlloc arbeitete auf DS 0).
import re, sys
cur = None
pro = []
bad = []
for l in sys.stdin:
    m = re.match(r'^[0-9A-F]{4}\s+(\w+):$', l.rstrip())
    if m:
        cur, pro = m.group(1), []
        continue
    if cur is None:
        continue
    p = l.rstrip('\n').split('\t')
    if len(p) < 4:
        continue
    code = p[0][4:].strip()
    ins = ' '.join(x for x in p[3:] if x).replace(' ', '')
    pro.append(ins)
    if len(pro) <= 14 and code.startswith('8B 46 02'):
        try:
            i = next(k for k, x in enumerate(pro) if x == 'movds,ax')
            j = next(k for k, x in enumerate(pro) if x == 'pushbp')
        except StopIteration:
            continue
        if j != i + 1:
            bad.append(cur)
if bad:
    print('get_bp2 nach gesichertem Register (Export als duennen Mantel schreiben):', ' '.join(sorted(set(bad))))
    sys.exit(1)
