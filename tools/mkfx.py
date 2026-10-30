#!/usr/bin/env python3
"""Baut aus einem rohen Programmabbild eine FX-Datei mit MZ-Stub.

    mkfx.py STUB.BIN IMAGE.BIN IMAGE.MAP AUSGABE.EXE [STACK]

Die BSS-Groesse wird aus der Watcom-Map gelesen (Ende von DGROUP minus
Dateigroesse des Abbilds). Format siehe include/fx.h.
"""
import struct
import sys

MZ_HDR = 64             # Kopfgroesse, Stub beginnt danach
FX_HDR = 16


def group_end(mapfile):
    end = 0
    for line in open(mapfile):
        f = line.split()
        if len(f) >= 5 and f[2] == 'DGROUP':
            try:
                end = max(end, int(f[3], 16) + int(f[4], 16))
            except ValueError:
                pass
    return end


def main():
    stub_f, image_f, map_f, out_f = sys.argv[1:5]
    stack = int(sys.argv[5]) if len(sys.argv) > 5 else 2048

    stub = open(stub_f, 'rb').read()
    image = open(image_f, 'rb').read()
    bss = max(0, group_end(map_f) - len(image))

    fx_off = (MZ_HDR + len(stub) + 15) & ~15
    image_off = fx_off + FX_HDR
    total = len(image) + bss + stack
    if total > 0x10000:
        sys.exit('%s: Abbild + BSS + Stack = %d Bytes, mehr als 64 KB'
                 % (image_f, total))

    fx = struct.pack('<2sHHHHHHH', b'CH', 1, image_off, len(image),
                     bss, stack, 0, 0)

    body = stub + b'\0' * (fx_off - MZ_HDR - len(stub)) + fx + image
    size = MZ_HDR + len(body)
    mz = struct.pack('<2sHHHHHHHHHHHH',
                     b'MZ',
                     size % 512,                # Bytes auf letzter Seite
                     (size + 511) // 512,       # Seiten
                     0,                         # Relokationen
                     MZ_HDR // 16,              # Kopf in Paragraphen
                     0x10,                      # min. Zusatzspeicher
                     0xFFFF,                    # max. Zusatzspeicher
                     0, 0x100 + len(body),      # SS:SP hinter dem Modul
                     0,                         # Pruefsumme
                     0, 0,                      # CS:IP = Stub
                     0x40)                      # Relokationstabelle
    mz += b'\0' * (0x3C - len(mz)) + struct.pack('<I', fx_off)
    assert len(mz) == MZ_HDR

    open(out_f, 'wb').write(mz + body)
    print('  %-12s Abbild %5d, BSS %5d, Stack %5d -> Segment %5d Bytes'
          % (out_f.split('/')[-1], len(image), bss, stack, total))


if __name__ == '__main__':
    main()
