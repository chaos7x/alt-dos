#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
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
FX_HDR = 16             # Version 1 (Treiber, IFS)
FX_HDR2 = 18            # Version 2 (Programme): + code_size


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


def groups(mapfile):
    """Gruppen aus einer Map von "format dos": Name -> (Offset, Groesse)"""
    g = {}
    for line in open(mapfile):
        f = line.split()
        if len(f) == 3 and ':' in f[1] and f[0].endswith('GROUP'):
            try:
                sg, o = f[1].split(':')
                g[f[0]] = (int(sg, 16) * 16 + int(o, 16), int(f[2], 16))
            except ValueError:
                pass
    return g


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
    code_size = 0
    if image[:2] == b'MZ':
        # Version 2: wlink "format dos" (wie der Kernel) - Kopf ab, Code
        # (CGROUP) ab 0, Daten (DGROUP) ab dem naechsten Paragraphen
        nrel, hdr = struct.unpack('<HH', image[6:10])
        if nrel:
            sys.exit('%s: %d Segment-Relokationen (seg-Bezug im Programm?)'
                     % (image_f, nrel))
        image = image[hdr * 16:]
        g = groups(map_f)
        if 'DGROUP' not in g:
            sys.exit('%s: keine DGROUP in der Map' % image_f)
        code_size = g['DGROUP'][0]
        if code_size & 15 or code_size < g.get('CGROUP', (0, 0))[1]:
            sys.exit('%s: DGROUP liegt nicht hinter dem Code' % image_f)
        data_init = max(0, len(image) - code_size)
        image = image[:code_size + data_init]
        image += b'\0' * (code_size + data_init - len(image))
        bss = max(0, g['DGROUP'][1] - data_init)
        total = data_init + bss + stack
        if code_size > 0x10000 or total > 0x10000:
            sys.exit('%s: Code %d oder Daten + BSS + Stack %d Bytes, mehr als 64 KB'
                     % (image_f, code_size, total))
    else:
        bss = max(0, group_end(map_f) - len(image))
        total = len(image) + bss + stack
        if total > 0x10000:
            sys.exit('%s: Abbild + BSS + Stack = %d Bytes, mehr als 64 KB'
                     % (image_f, total))

    fx_hdr = FX_HDR2 if code_size else FX_HDR
    fx_off = (MZ_HDR + len(stub) + 15) & ~15
    if bind:
        struct.pack_into('<H', stub, 2, fx_off - MZ_HDR)
        stub = bytes(stub)
    image_off = (fx_off + fx_hdr + 15) & ~15    # Abbild auf Paragraph (DOS-Stub)

    flags = (1 if driver else 0) | (2 if lfn else 0) | (4 if bind else 0)
    if code_size:
        fx = struct.pack('<2sHHHHHHHH', b'CH', 2, image_off, len(image),
                         bss, stack, 0, flags, code_size)
    else:
        fx = struct.pack('<2sHHHHHHH', b'CH', 1, image_off, len(image),
                         bss, stack, 0, flags)

    body = stub + b'\0' * (fx_off - MZ_HDR - len(stub)) + fx
    body += b'\0' * (image_off - MZ_HDR - len(body)) + image
    size = MZ_HDR + len(body)
    if bind:                    # DOS: Platz fuer das Datensegment (V2) bzw.
        end = code_size + total if code_size else 0x10000   # das ganze
        extra = (image_off - MZ_HDR) + end - len(body)       # 64-KB-Segment
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
    print('  %-12s %sDaten %5d, BSS %5d, Stack %5d -> %5d Bytes%s'
          % (out_f.split('/')[-1], 'Code %5d, ' % code_size if code_size else '',
             len(image) - code_size, bss, stack, total,
             ', DOS-Stub %d' % len(stub) if bind else ''))


if __name__ == '__main__':
    main()
