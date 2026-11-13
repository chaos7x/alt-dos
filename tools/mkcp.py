#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Zeichensaetze fuer CHCP/CODEPAGE= aus den CPI-Dateien von FreeDOS.

Eingabe: entpackte CPI-Dateien (FreeDOS-Paket cpidos 3.0a, Henrique Peron,
GPL v2; die .CPX sind UPX-gepackt: `upx -d -o EGA.cpi EGA.CPX`).
Ausgabe je Codepage CPnnn.FNT, 6400 Byte:
  0     8x16-Zeichensatz, 256 x 16 Byte
  4096  8x8-Zeichensatz,  256 x 8 Byte
  6144  Unicode der Zeichen 80h-FFh, 128 Worte (aus den Python-Codecs)
Aufruf: mkcp.py <CPI-Verzeichnis> <Zielverzeichnis>
"""
import glob, os, struct, sys

WANT = (850, 852, 857, 858, 860, 863, 865, 866, 737)

def fonts(path):
    d = open(path, 'rb').read()
    if d[1:8] != b'FONT   ':
        return {}
    fih = struct.unpack_from('<I', d, 0x13)[0]
    n = struct.unpack_from('<H', d, fih)[0]
    p = fih + 2
    out = {}
    for _ in range(n):
        size, nxt, dt, dev, cp, _r, cpih = struct.unpack_from('<HIH8sH6sI', d, p)
        q = cpih
        ver, nf, sz = struct.unpack_from('<HHH', d, q)
        q += 6
        f = {}
        for _k in range(nf):
            h, w, ya, xa, nc = struct.unpack_from('<BBBBH', d, q)
            q += 6
            f[h] = d[q:q + h * nc]
            q += h * nc
        out[cp] = f
        p = nxt
    return out

def uc(cp, b):
    """Unicode eines Zeichens; in der Codepage nicht belegt -> 0"""
    try:
        return ord(bytes([b]).decode('cp%d' % cp))
    except UnicodeDecodeError:
        return 0

def main():
    src, dst = sys.argv[1], sys.argv[2]
    have = {}
    for fn in sorted(glob.glob(os.path.join(src, '*.cpi'))):
        for cp, f in fonts(fn).items():
            if cp in WANT and cp not in have and 16 in f and 8 in f:
                have[cp] = f
    for cp in WANT:
        f = have[cp]
        uni = b''.join(struct.pack('<H', uc(cp, b)) for b in range(0x80, 0x100))
        data = f[16][:4096] + f[8][:2048] + uni
        assert len(data) == 6400
        open(os.path.join(dst, 'CP%d.FNT' % cp), 'wb').write(data)
        print('CP%d.FNT' % cp)

if __name__ == '__main__':
    main()
