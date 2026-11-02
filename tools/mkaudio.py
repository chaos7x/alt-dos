#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Klangbeispiele fuer \\DEMO erzeugen: mkaudio.py ZIELVERZEICHNIS

KLANG.WAV  Akkord-Arpeggio C-E-G-C, 11025 Hz, 8 Bit mono (PLAY, digital)
ODE.MID    "Ode an die Freude" (Beethoven, gemeinfrei) mit Bass, Format 1
ODE.IMF    dieselbe Melodie als AdLib-Registerfolge (560 Hz) fuer den OPL
"""
import math, struct, sys, wave

out = sys.argv[1]

# ---- WAV ----------------------------------------------------------------
rate = 11025
data = bytearray()
for i, note in enumerate((60, 64, 67, 72)):
    f = 440 * 2 ** ((note - 69) / 12)
    for k in range(int(rate * 0.5)):
        t = k / rate
        env = math.exp(-3 * t)
        v = sum(math.sin(2 * math.pi * f * h * t) / h for h in (1, 2, 3)) * 0.45 * env
        data.append(max(0, min(255, int(128 + v * 127))))
w = wave.open(out + '/KLANG.WAV', 'wb')
w.setnchannels(1); w.setsampwidth(1); w.setframerate(rate)
w.writeframes(bytes(data)); w.close()

# ---- Melodie (Viertel = 1) ----------------------------------------------
E, F, G, A, D, C, B = 64, 65, 67, 69, 62, 60, 59
mel = [(E,1),(E,1),(F,1),(G,1),(G,1),(F,1),(E,1),(D,1),(C,1),(C,1),(D,1),(E,1),(E,1.5),(D,.5),(D,2),
       (E,1),(E,1),(F,1),(G,1),(G,1),(F,1),(E,1),(D,1),(C,1),(C,1),(D,1),(E,1),(D,1.5),(C,.5),(C,2)]
bass = [(48,4),(43,4),(48,4),(43,2),(43,2),(48,4),(43,4),(48,4),(43,2),(48,2)]

def vlq(n):
    b = [n & 0x7F]; n >>= 7
    while n:
        b.insert(0, (n & 0x7F) | 0x80); n >>= 7
    return bytes(b)

div = 240
def track(notes, ch, prog, vel):
    ev = vlq(0) + bytes([0xC0 | ch, prog])
    for n, d in notes:
        ev += vlq(0) + bytes([0x90 | ch, n, vel]) + vlq(int(d * div * 0.9)) + bytes([0x80 | ch, n, 0])
        ev += vlq(int(d * div) - int(d * div * 0.9)) + bytes([0xB0 | ch, 0x50, 0])   # Luecke
    ev += vlq(0) + bytes([0xFF, 0x2F, 0])
    return b'MTrk' + struct.pack('>I', len(ev)) + ev

t0 = vlq(0) + bytes([0xFF, 0x51, 3]) + (500000).to_bytes(3, 'big') + vlq(0) + bytes([0xFF, 0x2F, 0])
mid = b'MThd' + struct.pack('>IHHH', 6, 1, 3, div) + b'MTrk' + struct.pack('>I', len(t0)) + t0
mid += track(mel, 0, 0, 100) + track(bass, 1, 32, 80)
open(out + '/ODE.MID', 'wb').write(mid)

# ---- IMF (560 Hz, Kanal 0 Melodie, Kanal 1 Bass) ------------------------
def fnum(n):
    f = 440 * 2 ** ((n - 69) / 12)
    for b in range(8):
        fn = round(f * 2 ** (20 - b) / 49716)
        if fn <= 1023:
            return fn, b
    return 1023, 7

regs = []
def r(reg, val, delay=0):
    regs.append(struct.pack('<BBH', reg, val, delay))
r(0x01, 0x20)
for c, op in ((0, 0), (1, 1)):          # Instrument: weiches Klavier
    r(0x20 + op, 0x01); r(0x23 + op, 0x01); r(0x40 + op, 0x1C); r(0x43 + op, 0x00)
    r(0x60 + op, 0xF3); r(0x63 + op, 0xF2); r(0x80 + op, 0x34); r(0x83 + op, 0x45)
    r(0xC0 + c, 0x0A)
q = 280                                  # Viertel = 0,5 s = 280 Takte
ev = []                                  # (Zeit, Kanal, Note, an)
t = 0
for n, d in mel:
    ev.append((t, 0, n, 1)); ev.append((t + int(d * q * 0.9), 0, n, 0)); t += int(d * q)
t = 0
for n, d in bass:
    ev.append((t, 1, n, 1)); ev.append((t + int(d * q * 0.9), 1, n, 0)); t += int(d * q)
ev.sort(key=lambda e: (e[0], e[3]))
for i, (t, c, n, on) in enumerate(ev):
    fn, b = fnum(n)
    nxt = ev[i + 1][0] if i + 1 < len(ev) else t + 280
    if on:
        r(0xA0 + c, fn & 0xFF)
    r(0xB0 + c, (0x20 if on else 0) | (b << 2) | (fn >> 8), nxt - t)
body = b''.join(regs)
open(out + '/ODE.IMF', 'wb').write(struct.pack('<H', len(body)) + body)
