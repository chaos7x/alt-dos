#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""rc2dlg.py - DIALOG-Bloecke einer .rc-Datei -> 16-Bit-DLGTEMPLATEs als C.

Aufruf: rc2dlg.py eingabe.rc ausgabe.h
Kennt nur, was gui/res/commdlg.rc braucht: DIALOG mit STYLE/CAPTION/FONT,
LTEXT/RTEXT/CTEXT, EDITTEXT, LISTBOX, COMBOBOX, GROUPBOX, (DEF)PUSHBUTTON,
CHECKBOX, RADIOBUTTON, ICON, CONTROL; Stile als Namen mit | und NOT.
Texte werden in Windows-1252 (ANSI) abgelegt. Selbstgezeichnete Listen
(OWNERDRAW) werden zu gewoehnlichen: die Dialoge fuellen Text ein.
Je Dialog entsteht `static const unsigned char dlg_NAME[]`.
"""
import re, sys

SYM = {
    'IDOK': 1, 'IDCANCEL': 2,
    'WS_POPUP': 0x80000000, 'WS_CHILD': 0x40000000, 'WS_VISIBLE': 0x10000000,
    'WS_DISABLED': 0x08000000, 'WS_CAPTION': 0x00C00000, 'WS_BORDER': 0x00800000,
    'WS_DLGFRAME': 0x00400000, 'WS_VSCROLL': 0x00200000, 'WS_HSCROLL': 0x00100000,
    'WS_SYSMENU': 0x00080000, 'WS_GROUP': 0x00020000, 'WS_TABSTOP': 0x00010000,
    'DS_MODALFRAME': 0x80, 'DS_SETFONT': 0x40,
    'SS_LEFT': 0, 'SS_CENTER': 1, 'SS_RIGHT': 2, 'SS_ICON': 3, 'SS_SIMPLE': 0x0B,
    'SS_NOPREFIX': 0x80, 'SS_SUNKEN': 0x1000,
    'ES_AUTOHSCROLL': 0x80, 'ES_OEMCONVERT': 0x400,
    'LBS_NOTIFY': 1, 'LBS_SORT': 2, 'LBS_OWNERDRAWFIXED': 0x10, 'LBS_HASSTRINGS': 0x40,
    'LBS_DISABLENOSCROLL': 0x1000, 'LBS_STANDARD': 0x00A00003,
    'CBS_SIMPLE': 1, 'CBS_DROPDOWN': 2, 'CBS_DROPDOWNLIST': 3, 'CBS_OWNERDRAWFIXED': 0x10,
    'CBS_AUTOHSCROLL': 0x40, 'CBS_SORT': 0x100, 'CBS_HASSTRINGS': 0x200,
    'CBS_DISABLENOSCROLL': 0x800,
    'BS_PUSHBUTTON': 0, 'BS_DEFPUSHBUTTON': 1, 'BS_CHECKBOX': 2, 'BS_AUTOCHECKBOX': 3,
    'BS_RADIOBUTTON': 4, 'BS_GROUPBOX': 7, 'BS_AUTORADIOBUTTON': 9,
}
for pre, base in (('psh', 0x400), ('chx', 0x410), ('rad', 0x420), ('grp', 0x430),
                  ('ico', 0x43C), ('stc', 0x440), ('lst', 0x460), ('cmb', 0x470),
                  ('edt', 0x480)):
    for i in range(1, 33 if pre == 'stc' else 17):
        SYM['%s%d' % (pre, i)] = base + i - 1
SYM['pshHelp'] = SYM['psh15']
for i, n in enumerate('HUESCROLL SATSCROLL LUMSCROLL HUE SAT LUM RED GREEN BLUE CURRENT '
                      'RAINBOW SAVE ADD SOLID TUNE SCHEMES ELEMENT SAMPLES PALETTE MIX '
                      'BOX1 CUSTOM1 _ HUEACCEL SATACCEL LUMACCEL REDACCEL GREENACCEL '
                      'BLUEACCEL _ SOLID_LEFT SOLID_RIGHT'.split()):
    SYM['COLOR_' + n] = 700 + i

CLS = {'BUTTON': 0x80, 'EDIT': 0x81, 'STATIC': 0x82, 'LISTBOX': 0x83,
       'SCROLLBAR': 0x84, 'COMBOBOX': 0x85}
# Anweisung -> Klasse, Grundstil (ohne WS_CHILD|WS_VISIBLE), hat Text
STMT = {
    'LTEXT': ('STATIC', SYM['WS_GROUP'] | 0, 1), 'RTEXT': ('STATIC', SYM['WS_GROUP'] | 2, 1),
    'CTEXT': ('STATIC', SYM['WS_GROUP'] | 1, 1),
    'EDITTEXT': ('EDIT', SYM['WS_BORDER'] | SYM['WS_TABSTOP'], 0),
    'LISTBOX': ('LISTBOX', SYM['LBS_NOTIFY'] | SYM['WS_BORDER'], 0),
    'COMBOBOX': ('COMBOBOX', SYM['CBS_SIMPLE'] | SYM['WS_TABSTOP'], 0),
    'GROUPBOX': ('BUTTON', 7, 1),
    'PUSHBUTTON': ('BUTTON', SYM['WS_TABSTOP'], 1),
    'DEFPUSHBUTTON': ('BUTTON', 1 | SYM['WS_TABSTOP'], 1),
    'CHECKBOX': ('BUTTON', 2 | SYM['WS_TABSTOP'], 1),
    'RADIOBUTTON': ('BUTTON', 4, 1),
    'ICON': ('STATIC', 3, 1),
}


def num(t):
    t = t.strip()
    if t in SYM:
        return SYM[t]
    return int(t, 0)


def style(expr, base=0):
    v = base
    for part in re.split(r'\|', expr):
        part = part.strip()
        if not part:
            continue
        if part.startswith('NOT '):
            v &= ~num(part[4:])
        else:
            v |= num(part)
    return v & 0xFFFFFFFF


def toks(line):
    """Kommagetrennte Felder, Zeichenketten bleiben ganz."""
    out, cur, q = [], '', False
    for ch in line:
        if ch == '"':
            q = not q
            cur += ch
        elif ch == ',' and not q:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def sz(s):
    return s.encode('cp1252') + b'\0'


def unq(s):
    assert s[0] == '"' and s[-1] == '"', s
    return s[1:-1]


def dialog(name, head, body):
    x, y, cx, cy = [num(v) for v in head]
    st, cap, font, items = 0, '', None, []
    lines = []
    for ln in body:            # Fortsetzungszeilen (mit | am Ende) zusammen
        ln = ln.strip()
        if not ln:
            continue
        if lines and (lines[-1].endswith('|') or lines[-1].endswith(',')):
            lines[-1] += ' ' + ln
        else:
            lines.append(ln)
    for ln in lines:
        w = ln.split(None, 1)
        kw, rest = w[0], (w[1] if len(w) > 1 else '')
        if kw == 'STYLE':
            st = style(rest)
        elif kw == 'CAPTION':
            cap = unq(rest.strip())
        elif kw == 'FONT':
            f = toks(rest)
            font = (num(f[0]), unq(f[1]))
        elif kw in ('{', '}', 'BEGIN', 'END'):
            continue
        elif kw == 'CONTROL':
            f = toks(rest)
            txt, iid, cls = unq(f[0]), num(f[1]), unq(f[2])
            s = style(f[3], SYM['WS_CHILD'] | SYM['WS_VISIBLE'])
            items.append((txt, iid, cls, s, [num(v) for v in f[4:8]]))
        elif kw in STMT:
            cls, base, hastxt = STMT[kw]
            f = toks(rest)
            txt = unq(f.pop(0)) if hastxt else ''
            iid = num(f[0])
            pos = [num(v) for v in f[1:5]]
            s = style(f[5] if len(f) > 5 else '', base | SYM['WS_CHILD'] | SYM['WS_VISIBLE'])
            items.append((txt, iid, cls, s, pos))
        else:
            raise SystemExit('rc2dlg: unbekannt in %s: %s' % (name, ln))
    if font:
        st |= SYM['DS_SETFONT']
    b = bytearray()
    b += st.to_bytes(4, 'little') + bytes([len(items)])
    for v in (x, y, cx, cy):
        b += v.to_bytes(2, 'little', signed=True)
    b += b'\0\0' + sz(cap)
    if font:
        b += font[0].to_bytes(2, 'little') + sz(font[1])
    for txt, iid, cls, s, pos in items:
        if cls in ('LISTBOX', 'COMBOBOX'):
            s &= ~0x30                          # OWNERDRAW weg
        for v in pos:
            b += v.to_bytes(2, 'little', signed=True)
        b += (iid & 0xFFFF).to_bytes(2, 'little') + s.to_bytes(4, 'little')
        b += bytes([CLS[cls]]) if cls in CLS else sz(cls)
        b += sz(txt) + b'\0'
    return b


def main():
    src = open(sys.argv[1], encoding='utf-8').read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    out = ['/* erzeugt von tools/rc2dlg.py aus %s - nicht von Hand aendern */' % sys.argv[1]]
    for m in re.finditer(r'^(\w+)\s+DIALOG\s+([^\n]*)\n(.*?)^\}', src, re.S | re.M):
        b = dialog(m.group(1), m.group(2).split(','), m.group(3).split('\n'))
        out.append('const unsigned char __far dlg_%s[%d] = {' % (m.group(1), len(b)))
        for i in range(0, len(b), 16):
            out.append('  ' + ','.join('%d' % c for c in b[i:i + 16]) + ',')
        out.append('};')
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')


main()
