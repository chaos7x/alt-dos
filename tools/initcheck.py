#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""initcheck.py - welche Funktionen laufen nur beim Start? (docs/INIT-SEGMENT.md)

Liest die Ausgabe von wdis fuer ein oder mehrere Objekte, baut den
Aufrufgraphen (call/jmp auf Symbole, "offset name" = Adresse genommen)
und meldet, welche Funktionen nur von den Wurzeln aus erreichbar sind.

  initcheck.py --roots init_ obj.o [...]      Vorschlag (nur melden)
  initcheck.py --roots init_ --seg ITEXT [--near-ok] obj.o
        prueft zusaetzlich: nur Startcode ruft etwas in ITEXT, niemand
        nimmt dessen Adresse, was in ITEXT liegt, laeuft nur beim Start,
        und Aufrufe ueber die Segmentgrenze von ITEXT sind fern (call far;
        --near-ok fuer Treiber: ein Segment) -> rc 1
"""
import re, subprocess, sys

def parse(obj):
    out = subprocess.run(['wdis', obj], capture_output=True, text=True).stdout
    funcs, seg_of, edges, addr = {}, {}, {}, set()
    far = set()                         # (Aufrufer, Ziel) mit call/jmp far
    seg = cur = None
    start, segsize = {}, {}
    for ln in out.splitlines():
        m = re.match(r'Segment: (\S+) \S+ \S+ ([0-9A-F]+) bytes', ln)
        if m:
            seg = m.group(1)
            segsize[seg] = int(m.group(2), 16)
            cur = None
            continue
        m = re.match(r'^([0-9A-F]{4,8})\s+(\w+):\s*$', ln)
        if m and not m.group(2).startswith('L$') and seg is not None:
            cur = m.group(2)
            funcs.setdefault(cur, 0)
            seg_of[cur] = seg
            start[cur] = int(m.group(1), 16)
            edges.setdefault(cur, set())
            continue
        if cur is None:
            continue
        m = re.search(r'\b(call|jmp|j\w+)\s+(?:near ptr |far ptr )?(\w+_)\b', ln)
        if m:
            edges[cur].add(m.group(2))
            op = re.match(r'^[0-9A-F]{4,8}\s+([0-9A-F]{2})', ln)
            if op and op.group(1) in ('9A', 'EA'):
                far.add((cur, m.group(2)))
        for m in re.finditer(r'offset (\w+_)\b', ln):
            addr.add((cur, m.group(1)))
    by = {}                             # Groesse = Abstand zur naechsten Marke
    for f, a in start.items():
        by.setdefault(seg_of[f], []).append((a, f))
    for sg, l in by.items():
        l.sort()
        for i, (a, f) in enumerate(l):
            e = l[i + 1][0] if i + 1 < len(l) else segsize.get(sg, a)
            funcs[f] = e - a
    return funcs, seg_of, edges, addr, far

def main():
    a = sys.argv[1:]
    roots, segname, quiet, nearok = set(), None, False, False
    objs = []
    i = 0
    while i < len(a):
        if a[i] == '--roots':
            roots |= set(a[i + 1].split(','))
            i += 2
        elif a[i] == '--near-ok':        # Treiber: ein Segment, nah geht
            nearok = True
            i += 1
        elif a[i] == '-q':
            quiet = True
            i += 1
        elif a[i] == '--seg':
            segname = a[i + 1]
            i += 2
        else:
            objs.append(a[i])
            i += 1
    funcs, seg_of, edges, addr, far = {}, {}, {}, set(), set()
    parsed = [(o,) + parse(o) for o in objs]
    count = {}
    for o, f, s, e, d, fr in parsed:
        for n in f:
            count[n] = count.get(n, 0) + 1
    def q(o, n, local):                 # gleichnamige (statische) Funktionen
        return n if count.get(n, 0) < 2 else (n + '@' + o if n in local else n)
    for o, f, s, e, d, fr in parsed:    # mehrerer Dateien auseinanderhalten
        L = set(f)
        for n in f:
            funcs[q(o, n, L)] = f[n]
            seg_of[q(o, n, L)] = s[n]
        for k, v in e.items():
            edges.setdefault(q(o, k, L), set()).update(q(o, t, L) for t in v)
        addr |= {(q(o, a, L), q(o, b, L)) for (a, b) in d}
        far |= {(q(o, a, L), q(o, b, L)) for (a, b) in fr}
        for k, v in e.items():
            edges.setdefault(k, set()).update(v)
    callers = {}
    for k, v in edges.items():
        for t in v:
            callers.setdefault(t, set()).add(k)
    taken = {t for (_, t) in addr}
    init = set(r for r in roots if r in funcs)
    changed = True
    while changed:                      # nur von Startfunktionen erreicht
        changed = False
        for f in funcs:
            if f in init or f in taken:
                continue
            c = callers.get(f, set()) - {f}     # Rekursion zaehlt nicht
            if c and c <= init:
                init.add(f)
                changed = True
    total = sum(v for f, v in funcs.items() if 'TEXT' in (seg_of.get(f) or ''))
    isz = sum(funcs[f] for f in init if 'TEXT' in (seg_of.get(f) or ''))
    if not quiet:
        print('%s: Code %d, nur Start %d (%d %%)' % (' '.join(objs), total, isz,
              100 * isz // max(total, 1)))
        for f in sorted(init, key=lambda f: -funcs[f]):
            print('  %-28s %6d  %s' % (f, funcs[f], seg_of.get(f, '?')))
    if not segname:
        return 0
    bad = 0
    for f in funcs:                     # Laufzeitcode darf nichts in ITEXT
        sf = seg_of.get(f)              # rufen; ueber Segmentgrenzen nur fern
        for t in edges.get(f, ()):
            st = seg_of.get(t)
            if st == segname and sf != segname and f not in init and t not in roots:
                print('FEHLER: %s (%s) ruft %s in %s' % (f, sf, t, segname))
                bad = 1
            if not nearok and segname in (sf, st) and sf and st and sf != st \
               and (f, t) not in far:
                print('FEHLER: %s (%s) ruft %s (%s) nah ueber die Segmentgrenze' % (f, sf, t, st))
                bad = 1
    for (f, t) in addr:
        if seg_of.get(t) == segname and seg_of.get(f) != segname:
            print('FEHLER: %s nimmt die Adresse von %s in %s' % (f, t, segname))
            bad = 1
    for f in funcs:                     # was in ITEXT liegt, muss Startcode sein
        if seg_of.get(f) == segname and f not in init and f not in roots:
            print('FEHLER: %s liegt in %s, wird aber auch zur Laufzeit gerufen' % (f, segname))
            bad = 1
    return bad

sys.exit(main())
