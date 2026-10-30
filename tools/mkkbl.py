#!/usr/bin/env python3
"""Tastaturbelegungen fuer AT-DOS 5.0.

    mkkbl.py AUSGABEVERZEICHNIS

Erzeugt GR.KBL, US.KBL, GB.KBL (Belegungsdateien fuer KEYB.EXE) und
kbd_gr.h (die deutsche Belegung, fest im Kernel eingebaut).

Format einer .KBL-Datei (278 Bytes):
    4 Bytes  "KBL" 01h
    4 Bytes  Kennung, z. B. "US" mit Nullen aufgefuellt
    90 Bytes Zeichen ohne Umschalttaste   (Scancode 00h-59h, Satz 1)
    90 Bytes Zeichen mit Umschalttaste
    90 Bytes Zeichen mit AltGr
Zeichen in Codepage 437, 0 = Taste erzeugt kein Zeichen.
"""
import os
import sys

N = 0x5A

# CP437-Sonderzeichen
AE, OE, UE = 0x84, 0x94, 0x81
AE_, OE_, UE_ = 0x8E, 0x99, 0x9A
SZ, PARA, DEG, SQ2, POUND, NOT = 0xE1, 0x15, 0xF8, 0xFD, 0x9C, 0xAA


def table(rows, extra=None):
    """rows: Liste (Startscancode, Zeichenfolge); Zeichen als str oder int."""
    t = [0] * N
    for start, chars in rows:
        for i, c in enumerate(chars):
            t[start + i] = c if isinstance(c, int) else ord(c)
    for sc, c in (extra or {}).items():
        t[sc] = c if isinstance(c, int) else ord(c)
    return t


# gemeinsam: Esc, Ruecktaste, Tab, Enter, Leertaste, Ziffernblock-*
COMMON = {0x01: 27, 0x0E: 8, 0x0F: 9, 0x1C: 13, 0x39: ' ', 0x37: '*'}

LAYOUTS = {
    'GR': (
        table([(0x02, '1234567890'), (0x0C, [SZ, "'"]), (0x10, 'qwertzuiop'),
               (0x1A, [UE, '+']), (0x1E, 'asdfghjkl'), (0x27, [OE, AE, '^']),
               (0x2B, '#'), (0x2C, 'yxcvbnm,.-')], {**COMMON, 0x56: '<'}),
        table([(0x02, ['!', '"', PARA, '$', '%', '&', '/', '(', ')', '=']),
               (0x0C, '?`'), (0x10, 'QWERTZUIOP'), (0x1A, [UE_, '*']),
               (0x1E, 'ASDFGHJKL'), (0x27, [OE_, AE_, DEG]), (0x2B, "'"),
               (0x2C, 'YXCVBNM;:_')], {**COMMON, 0x56: '>'}),
        table([], {0x03: SQ2, 0x08: '{', 0x09: '[', 0x0A: ']', 0x0B: '}',
                   0x0C: '\\', 0x10: '@', 0x1B: '~', 0x56: '|'}),
    ),
    'US': (
        table([(0x02, '1234567890-='), (0x10, 'qwertyuiop[]'),
               (0x1E, "asdfghjkl;'`"), (0x2B, '\\'), (0x2C, 'zxcvbnm,./')],
              {**COMMON, 0x56: '\\'}),
        table([(0x02, '!@#$%^&*()_+'), (0x10, 'QWERTYUIOP{}'),
               (0x1E, 'ASDFGHJKL:"~'), (0x2B, '|'), (0x2C, 'ZXCVBNM<>?')],
              {**COMMON, 0x56: '|'}),
        table([]),
    ),
    'GB': (
        table([(0x02, '1234567890-='), (0x10, 'qwertyuiop[]'),
               (0x1E, "asdfghjkl;'`"), (0x2B, '#'), (0x2C, 'zxcvbnm,./')],
              {**COMMON, 0x56: '\\'}),
        table([(0x02, ['!', '"', POUND, '$', '%', '^', '&', '*', '(', ')', '_', '+']),
               (0x10, 'QWERTYUIOP{}'), (0x1E, ['A', 'S', 'D', 'F', 'G', 'H', 'J',
                                               'K', 'L', ':', '@', NOT]),
               (0x2B, '~'), (0x2C, 'ZXCVBNM<>?')], {**COMMON, 0x56: '|'}),
        table([], {0x29: '|'}),
    ),
}


def blob(ident, tabs):
    return b'KBL\x01' + ident.encode().ljust(4, b'\0') + b''.join(bytes(t) for t in tabs)


def main():
    out = sys.argv[1]
    for ident, tabs in LAYOUTS.items():
        data = blob(ident, tabs)
        assert len(data) == 8 + 3 * N
        if ident != 'GR':
            open(os.path.join(out, ident + '.KBL'), 'wb').write(data)
    data = blob('GR', LAYOUTS['GR'])
    with open(os.path.join(out, 'kbd_gr.h'), 'w') as f:
        f.write('/* erzeugt von tools/mkkbl.py - nicht von Hand aendern */\n')
        f.write('static const unsigned char kbl_gr[%d] = {\n' % len(data))
        for i in range(0, len(data), 16):
            f.write('    ' + ', '.join('0x%02X' % b for b in data[i:i + 16]) + ',\n')
        f.write('};\n')


if __name__ == '__main__':
    main()
