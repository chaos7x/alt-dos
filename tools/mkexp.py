#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Exporttabellen der GUI (exp_*.h, expproto.h im Ausgabeverzeichnis) aus den mit API
# markierten Funktionen in gui/*.c und den Ordinalzahlen in gui/ordinals.txt.
import re, sys, glob, os
gui = sys.argv[1] if len(sys.argv) > 1 else 'gui'
out_dir = sys.argv[2] if len(sys.argv) > 2 else gui
ords = {}
for l in open(os.path.join(gui, 'ordinals.txt')):
    if l.startswith('#') or not l.strip():
        continue
    m, o, n = l.split()
    ords.setdefault(n, (m, int(o)))
names = []
for f in sorted(glob.glob(os.path.join(gui, '*.c'))):
    for l in open(f, encoding='latin-1'):
        mm = re.match(r'^[A-Za-z][\w \*]*\bAPI\s+(\w+)\s*\(', l)
        if mm:
            names.append(mm.group(1))
out = {'KERNEL': [], 'USER': [], 'GDI': []}
miss = []
for n in names:
    k = ords.get(n.upper())
    if not k or k[0] not in out:
        miss.append(n)
        continue
    out[k[0]].append((k[1], n))
# Register-Schnittstellen in Assembler
out['KERNEL'] += [(91, 'InitTask'), (102, 'Dos3Call')]
for m, fn in (('KERNEL', 'exp_k.h'), ('USER', 'exp_u.h'), ('GDI', 'exp_g.h')):
    with open(os.path.join(out_dir, fn), 'w') as f:
        f.write('/* erzeugt von tools/mkexp.py */\n')
        for o, n in sorted(out[m]):
            f.write('    X(%d, %s),\n' % (o, n))
hdr = open(os.path.join(gui, 'gui.h'), encoding='latin-1').read()
with open(os.path.join(out_dir, 'expproto.h'), 'w') as f:
    f.write('/* erzeugt von tools/mkexp.py */\n')
    for n in names:
        if n not in miss and not re.search(r'API\s+' + n + r'\s*\(', hdr):
            f.write('void API %s(void);\n' % n)
if miss:
    print('ohne Ordinalzahl:', ' '.join(miss))
print('Exporte: KERNEL %d, USER %d, GDI %d' % (len(out['KERNEL']), len(out['USER']), len(out['GDI'])))
