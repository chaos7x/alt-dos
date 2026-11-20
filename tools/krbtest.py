# SPDX-License-Identifier: GPL-2.0-or-later
# Rechnet die Zeilen von tools/krbtest.c mit Python nach (RFC 3961/3962).
import sys, hashlib, hmac
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
def ecb(k, b, dec=False):
    c = Cipher(algorithms.AES(k), modes.ECB())
    o = c.decryptor() if dec else c.encryptor()
    return o.update(b) + o.finalize()
def nfold(s, n):
    l = len(s); m = l * n
    import math; lcm = l * n // math.gcd(l, n)
    bits = l * 8; v = int.from_bytes(s, 'big'); big = b''
    for k in range(lcm // l):
        r = 13 * k % bits
        w = ((v >> r) | (v << (bits - r))) & ((1 << bits) - 1)
        big += w.to_bytes(l, 'big')
    t = 0; mod = (1 << (8 * n)) - 1
    for i in range(0, lcm, n):
        t += int.from_bytes(big[i:i + n], 'big')
        while t > mod: t = (t & mod) + (t >> (8 * n))
    return t.to_bytes(n, 'big')
def dk(key, c):
    b = nfold(c, 16); o = b''
    while len(o) < 32: b = ecb(key, b); o += b
    return o[:32]
def s2k(pw, salt, it):
    return dk(hashlib.pbkdf2_hmac('sha1', pw, salt, it, 32), b'kerberos')
def cts(k, d):
    if len(d) == 16: return ecb(k, d)
    p = d + b'\0' * (-len(d) % 16); prev = b'\0' * 16; out = []
    for i in range(0, len(p), 16):
        prev = ecb(k, bytes(x ^ y for x, y in zip(p[i:i + 16], prev))); out.append(prev)
    r = len(d) - 16 * (len(out) - 1)
    out[-1], out[-2] = out[-2], out[-1]
    return b''.join(out[:-1]) + out[-1][:r]
def uk(k, u, x): return dk(k, u.to_bytes(4, 'big') + bytes([x]))
ok = bad = 0
fix = {'s2k1': 'fe697b52bc0d3ce14432ba036a92e65bbb52280990a2fa27883998d72af30161',
       's2k1200': '55a6ac740ad17b4846941051e1e8b0a7548d93b0ab30a8bc3ff16280382b8c2a',
       'nf': '6b65726265726f737b9b5b2b93132b93', 'nf6': 'be072631276b1955'}
for l in sys.stdin:
    f = l.strip().split('|'); t = f[0]
    if t == 'fix': e, g = fix[f[1]], f[2]
    else:
        x = [bytes.fromhex(v) if not v.isdigit() or t not in ('enc', 'ck') or i != 1 else int(v) for i, v in enumerate(f[1:])]
        if t == 'sha1': e, g = hashlib.sha1(x[0]).digest(), x[1]
        elif t == 'hmac1': e, g = hmac.new(x[0], x[1], hashlib.sha1).digest(), x[2]
        elif t == 'aes': e, g = ecb(x[0], x[1]) + x[1], x[2]
        elif t == 'enc':
            k, u, c, d, g = x; p = c + d
            e = cts(uk(k, u, 0xAA), p) + hmac.new(uk(k, u, 0x55), p, hashlib.sha1).digest()[:12]
        elif t == 'ck':
            k, u, d, g = x; e = hmac.new(uk(k, u, 0x99), d, hashlib.sha1).digest()[:12]
        else: print('?', l[:100]); bad += 1; continue
    if e == g: ok += 1
    else: bad += 1; print('FALSCH', t, l[:150])
print('gleich', ok, 'falsch', bad)
