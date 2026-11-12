#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tastaturbelegungen fuer AT-DOS 5.0.

    mkkbl.py AUSGABEVERZEICHNIS

Erzeugt US, GB, FR, SP, IT, DK, NO, SV, PL als .KBL (Belegungsdateien fuer
KEYB.EXE, Version 2) und kbd_gr.h (die deutsche Belegung, fest im Kernel
eingebaut, Version 1).

Version 2 (728 Bytes): "KBL" 02h, Kennung, dann vier Tabellen zu 90 Worten
in Unicode: normal, Umschalt, AltGr, Umschalt+AltGr.

Version 1 (278 Bytes, kbd_gr.h; KEYB nimmt sie weiter an):
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



# Version 2: Unicode, vier Ebenen (normal, Umschalt, AltGr, Umschalt+AltGr).
# Der Kernel leitet daraus die Zeichen der aktiven Codepage ab; was dort
# fehlt, meldet KEYB. Tottasten gibt es nicht - Akzente sind Zeichen.
def utab(rows, extra=None):
    t = [0] * N
    for start, chars in rows:
        for k, c in enumerate(chars):
            t[start + k] = ord(c)
    for sc, c in (extra or {}).items():
        t[sc] = ord(c) if isinstance(c, str) else c
    return t


def from437(t):
    """CP437-Tabelle -> Unicode (Steuerzeichen unter 20h bleiben)."""
    return [c if c < 0x80 else ord(bytes([c]).decode('cp437')) for c in t]


UCOMMON = {sc: (c if isinstance(c, int) else ord(c)) for sc, c in COMMON.items()}
NORD = [(0x10, 'qwertyuiop'), (0x1E, 'asdfghjkl'), (0x2C, 'zxcvbnm,.-')]
NORDS = [(0x10, 'QWERTYUIOP'), (0x1E, 'ASDFGHJKL'), (0x2C, 'ZXCVBNM;:_')]
NUM = (0x02, '1234567890')

ULAYOUTS = {
    'US': [from437(t) for t in LAYOUTS['US']] + [[0] * N],
    'GB': [from437(t) for t in LAYOUTS['GB']] + [[0] * N],
    'FR': (
        utab([(0x02, '&é"\'(-è_çà)='), (0x10, 'azertyuiop^$'),
              (0x1E, 'qsdfghjklmù'), (0x2C, 'wxcvbn,;:!')],
             {**UCOMMON, 0x29: '²', 0x2B: '*', 0x56: '<'}),
        utab([(0x02, '1234567890°+'), (0x10, 'AZERTYUIOP¨£'),
              (0x1E, 'QSDFGHJKLM%'), (0x2C, 'WXCVBN?./§')],
             {**UCOMMON, 0x2B: 'µ', 0x56: '>'}),
        utab([(0x03, '~#{[|`\\^@]}')], {0x1B: '¤'}),
        [0] * N,
    ),
    'SP': (
        utab([(0x02, "1234567890'¡"), (0x10, 'qwertyuiop`+'),
              (0x1E, 'asdfghjklñ´'), (0x2C, 'zxcvbnm,.-')],
             {**UCOMMON, 0x29: 'º', 0x2B: 'ç', 0x56: '<'}),
        utab([(0x02, '!"·$%&/()=?¿'), (0x10, 'QWERTYUIOP^*'),
              (0x1E, 'ASDFGHJKLÑ¨'), (0x2C, 'ZXCVBNM;:_')],
             {**UCOMMON, 0x29: 'ª', 0x2B: 'Ç', 0x56: '>'}),
        utab([(0x02, '|@#~'), (0x1A, '[]')],
             {0x29: '\\', 0x07: '¬', 0x28: '{', 0x2B: '}'}),
        [0] * N,
    ),
    'IT': (
        utab([(0x02, "1234567890'ì"), (0x10, 'qwertyuiopè+'),
              (0x1E, 'asdfghjklòà'), (0x2C, 'zxcvbnm,.-')],
             {**UCOMMON, 0x29: '\\', 0x2B: 'ù', 0x56: '<'}),
        utab([(0x02, '!"£$%&/()=?^'), (0x10, 'QWERTYUIOPé*'),
              (0x1E, 'ASDFGHJKLç°'), (0x2C, 'ZXCVBNM;:_')],
             {**UCOMMON, 0x29: '|', 0x2B: '§', 0x56: '>'}),
        utab([(0x1A, '[]'), (0x27, '@#')]),
        utab([(0x1A, '{}')]),
    ),
    'DK': (
        utab([NUM, (0x0C, '+´'), (0x1A, 'å¨'), (0x27, 'æø')] + NORD,
             {**UCOMMON, 0x29: '½', 0x2B: "'", 0x56: '<'}),
        utab([(0x02, '!"#¤%&/()=?`'), (0x1A, 'Å^'), (0x27, 'ÆØ')] + NORDS,
             {**UCOMMON, 0x29: '§', 0x2B: '*', 0x56: '>'}),
        utab([(0x03, '@£$'), (0x08, '{[]}')], {0x0D: '|', 0x1B: '~', 0x56: '\\'}),
        [0] * N,
    ),
    'NO': (
        utab([NUM, (0x0C, '+\\'), (0x1A, 'å¨'), (0x27, 'øæ')] + NORD,
             {**UCOMMON, 0x29: '|', 0x2B: "'", 0x56: '<'}),
        utab([(0x02, '!"#¤%&/()=?`'), (0x1A, 'Å^'), (0x27, 'ØÆ')] + NORDS,
             {**UCOMMON, 0x29: '§', 0x2B: '*', 0x56: '>'}),
        utab([(0x03, '@£$'), (0x08, '{[]}')], {0x0D: '´', 0x1B: '~'}),
        [0] * N,
    ),
    'SV': (
        utab([NUM, (0x0C, '+´'), (0x1A, 'å¨'), (0x27, 'öä')] + NORD,
             {**UCOMMON, 0x29: '§', 0x2B: "'", 0x56: '<'}),
        utab([(0x02, '!"#¤%&/()=?`'), (0x1A, 'Å^'), (0x27, 'ÖÄ')] + NORDS,
             {**UCOMMON, 0x29: '½', 0x2B: '*', 0x56: '>'}),
        utab([(0x03, '@£$'), (0x08, '{[]}\\')], {0x1B: '~', 0x56: '|'}),
        [0] * N,
    ),
    # Polnisch (Programmierer): US, dazu AltGr + Buchstabe
    'PL': (
        from437(LAYOUTS['US'][0]), from437(LAYOUTS['US'][1]),
        utab([], {0x1E: 'ą', 0x2E: 'ć', 0x12: 'ę', 0x26: 'ł',
                  0x31: 'ń', 0x18: 'ó', 0x1F: 'ś', 0x2D: 'ź',
                  0x2C: 'ż'}),
        utab([], {0x1E: 'Ą', 0x2E: 'Ć', 0x12: 'Ę', 0x26: 'Ł',
                  0x31: 'Ń', 0x18: 'Ó', 0x1F: 'Ś', 0x2D: 'Ź',
                  0x2C: 'Ż'}),
    ),
}


def blob2(ident, tabs):
    return (b'KBL\x02' + ident.encode().ljust(4, b'\0') +
            b''.join(c.to_bytes(2, 'little') for t in tabs for c in t))


def blob(ident, tabs):
    return b'KBL\x01' + ident.encode().ljust(4, b'\0') + b''.join(bytes(t) for t in tabs)


def main():
    out = sys.argv[1]
    for ident, tabs in ULAYOUTS.items():
        assert len(tabs) == 4 and all(len(t) == N for t in tabs), ident
        data = blob2(ident, tabs)
        assert len(data) == 8 + 8 * N
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
