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
conv = {}
for f in sorted(glob.glob(os.path.join(gui, '*.c'))):
    for l in open(f, encoding='latin-1'):
        mm = re.match(r'^[A-Za-z][\w \*]*\b(APIC?)\s+(\w+)\s*\(', l)
        if mm:
            names.append(mm.group(2))
            conv[mm.group(2)] = mm.group(1)
MODS = ('KERNEL', 'USER', 'GDI', 'KEYBOARD', 'COMMDLG', 'SHELL', 'OLECLI', 'MMSYSTEM', 'OLESVR')
out = dict((m, []) for m in MODS)
miss = []
for n in names:
    k = ords.get(n.upper())
    if not k or k[0] not in out:
        miss.append(n)
        continue
    out[k[0]].append((k[1], n))
# Register-Schnittstellen in Assembler
out['KERNEL'] += [(91, 'InitTask'), (102, 'Dos3Call')]
with open(os.path.join(out_dir, 'exp_all.h'), 'w') as f:
    f.write('/* erzeugt von tools/mkexp.py */\n')
    for m in MODS:
        f.write('static const struct ex __far t_%s[] = {\n' % m)
        for o, n in sorted(out[m]):
            f.write('    X(%d, %s),\n' % (o, n))
        f.write('    { 0, 0 }\n};\n')
    f.write('static const struct { const char *name; const struct ex __far *t; } modtab[] = {\n')
    for m in MODS:
        f.write('    { "%s", t_%s },\n' % (m, m))
    f.write('    { 0, 0 }\n};\n')
hdr = open(os.path.join(gui, 'gui.h'), encoding='latin-1').read()
with open(os.path.join(out_dir, 'expproto.h'), 'w') as f:
    f.write('/* erzeugt von tools/mkexp.py */\n')
    for n in names:
        if n not in miss and not re.search(r'APIC?\s+' + n + r'\s*\(', hdr):
            f.write('void %s %s(%s);\n' % (conv[n], n, 'int, ...' if conv[n] == 'APIC' else 'void'))
if miss:
    print('ohne Ordinalzahl:', ' '.join(miss))
print('Exporte: ' + ', '.join('%s %d' % (m, len(out[m])) for m in MODS))
