#!/usr/bin/env python3
"""Baut aus einem rohen Programmabbild eine FX-Datei mit MZ-Stub.

    mkfx.py [--driver] [--lfn] [--bind=DSTUB.BIN,DSTUB.MAP] STUB.BIN IMAGE.BIN
            IMAGE.MAP AUSGABE.EXE [STACK]

--bind: statt des kleinen Stubs (Meldung "nicht im Real Mode") kommt der
DOS-Stub mit der FAPI-Uebersetzung davor (stub/, wie BIND bei OS/2); die
.EXE laeuft dann auch unter MS-DOS 3.1+. Der Stub wird um sein BSS und
einen eigenen Stack ergaenzt, bei Offset 2 steht der Abstand zum FX-Kopf.

Die BSS-Groesse wird aus der Watcom-Map gelesen (Ende von DGROUP minus
Dateigroesse des Abbilds). Format siehe include/fx.h.
"""
import struct
import sys

MZ_HDR = 64             # Kopfgroesse, Stub beginnt danach
STUB_STACK = 1536       # Stack des DOS-Stubs (Gate, DOS-Aufrufe)
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
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    driver = '--driver' in sys.argv
    lfn = '--lfn' in sys.argv          # FX_FLAG_LONGNAMES
    stub_f, image_f, map_f, out_f = args[:4]
    stack = int(args[4]) if len(args) > 4 else (0 if driver else 2048)

    bind = [a[7:] for a in sys.argv if a.startswith('--bind=')]
    if bind:
        dstub, dmap = bind[0].split(',')
        stub = bytearray(open(dstub, 'rb').read())
        stub += b'\0' * (max(group_end(dmap), len(stub)) - len(stub))
        stub += b'\0' * STUB_STACK
        stub += b'\0' * (-len(stub) & 15)
    else:
        stub = open(stub_f, 'rb').read()
    image = open(image_f, 'rb').read()
    bss = max(0, group_end(map_f) - len(image))

    fx_off = (MZ_HDR + len(stub) + 15) & ~15
    if bind:
        struct.pack_into('<H', stub, 2, fx_off - MZ_HDR)
        stub = bytes(stub)
    image_off = fx_off + FX_HDR
    total = len(image) + bss + stack
    if total > 0x10000:
        sys.exit('%s: Abbild + BSS + Stack = %d Bytes, mehr als 64 KB'
                 % (image_f, total))

    fx = struct.pack('<2sHHHHHHH', b'CH', 1, image_off, len(image),
                     bss, stack, 0, (1 if driver else 0) | (2 if lfn else 0) |
                     (4 if bind else 0))

    body = stub + b'\0' * (fx_off - MZ_HDR - len(stub)) + fx + image
    size = MZ_HDR + len(body)
    if bind:                    # DOS: Platz fuer das ganze 64-KB-Segment,
        extra = (image_off - MZ_HDR) + 0x10000 - len(body)  # Stack im Stub
        minalloc, ss_sp = (max(extra, 0) + 15) // 16, (0, len(stub))
    else:
        minalloc, ss_sp = 0x10, (0, 0x100 + len(body))
    mz = struct.pack('<2sHHHHHHHHHHHH',
                     b'MZ',
                     size % 512,                # Bytes auf letzter Seite
                     (size + 511) // 512,       # Seiten
                     0,                         # Relokationen
                     MZ_HDR // 16,              # Kopf in Paragraphen
                     minalloc,                  # min. Zusatzspeicher
                     0xFFFF,                    # max. Zusatzspeicher
                     ss_sp[0], ss_sp[1],        # SS:SP
                     0,                         # Pruefsumme
                     0, 0,                      # CS:IP = Stub
                     0x40)                      # Relokationstabelle
    mz += b'\0' * (0x3C - len(mz)) + struct.pack('<I', fx_off)
    assert len(mz) == MZ_HDR

    open(out_f, 'wb').write(mz + body)
    print('  %-12s Abbild %5d, BSS %5d, Stack %5d -> Segment %5d Bytes%s'
          % (out_f.split('/')[-1], len(image), bss, stack, total,
             ', DOS-Stub %d' % len(stub) if bind else ''))


if __name__ == '__main__':
    main()
