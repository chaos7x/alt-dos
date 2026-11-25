#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""initcheck.py - welche Funktionen laufen nur beim Start? (docs/INIT-SEGMENT.md)

Liest die Ausgabe von wdis fuer ein oder mehrere Objekte, baut den
Aufrufgraphen (call/jmp auf Symbole, "offset name" = Adresse genommen)
und meldet, welche Funktionen nur von den Wurzeln aus erreichbar sind.

  initcheck.py --roots init_ obj.o [...]      Vorschlag (nur melden)
  initcheck.py --roots init_ --seg ITEXT obj.o
        prueft zusaetzlich: kein Code ausserhalb von ITEXT ruft etwas in
        ITEXT oder nimmt dessen Adresse (ausser den Wurzeln) -> rc 1
"""
import re, subprocess, sys

def parse(obj):
    out = subprocess.run(['wdis', obj], capture_output=True, text=True).stdout
    funcs, seg_of, edges, addr = {}, {}, {}, set()
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
    return funcs, seg_of, edges, addr

def main():
    a = sys.argv[1:]
    roots, segname, quiet = set(), None, False
    objs = []
    i = 0
    while i < len(a):
        if a[i] == '--roots':
            roots |= set(a[i + 1].split(','))
            i += 2
        elif a[i] == '-q':
            quiet = True
            i += 1
        elif a[i] == '--seg':
            segname = a[i + 1]
            i += 2
        else:
            objs.append(a[i])
            i += 1
    funcs, seg_of, edges, addr = {}, {}, {}, set()
    for o in objs:
        f, s, e, d = parse(o)
        funcs.update(f); seg_of.update(s); addr |= d
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
            c = callers.get(f)
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
        if seg_of.get(f) == segname:    # rufen
            continue
        for t in edges.get(f, ()):
            if seg_of.get(t) == segname and t not in roots:
                print('FEHLER: %s (%s) ruft %s in %s' % (f, seg_of.get(f), t, segname))
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
