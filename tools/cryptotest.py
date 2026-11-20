# SPDX-License-Identifier: GPL-2.0-or-later
# Rechnet die Zeilen von tools/cryptotest.c mit Python nach (siehe dort).
import sys, hashlib, hmac
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives import serialization
def prf(sec, label, seed, n):
    seed = label + seed; a = seed; out = b''
    while len(out) < n:
        a = hmac.new(sec, a, hashlib.sha256).digest()
        out += hmac.new(sec, a + seed, hashlib.sha256).digest()
    return out[:n]
def md4(m):   # (hashlib hat MD4 oft nicht mehr)
    import struct
    def F(x, y, z): return (x & y) | (~x & z)
    def G(x, y, z): return (x & y) | (x & z) | (y & z)
    def H(x, y, z): return x ^ y ^ z
    def rol(v, s): v &= 0xffffffff; return ((v << s) | (v >> (32 - s))) & 0xffffffff
    l = len(m); m = m + b'\x80' + b'\0' * ((55 - l) % 64) + struct.pack('<Q', l * 8)
    a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    for i in range(0, len(m), 64):
        X = struct.unpack('<16I', m[i:i + 64]); A, B, C, D = a, b, c, d
        for k in range(16):
            s = (3, 7, 11, 19)[k % 4]; A, B, C, D = D, rol(A + F(B, C, D) + X[k], s), B, C
        for k, j in enumerate((0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15)):
            s = (3, 5, 9, 13)[k % 4]; A, B, C, D = D, rol(A + G(B, C, D) + X[j] + 0x5A827999, s), B, C
        for k, j in enumerate((0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)):
            s = (3, 9, 11, 15)[k % 4]; A, B, C, D = D, rol(A + H(B, C, D) + X[j] + 0x6ED9EBA1, s), B, C
        a, b, c, d = (a + A) & 0xffffffff, (b + B) & 0xffffffff, (c + C) & 0xffffffff, (d + D) & 0xffffffff
    return struct.pack('<4I', a, b, c, d)
assert md4(b'abc').hex() == 'a448017aaf21d8525fc10ae87aa6729d'
ok = bad = 0
for l in sys.stdin:
    f = l.strip().split('|'); t = f[0]; x = [bytes.fromhex(v) for v in f[1:]]
    if t == 'sha': e = hashlib.sha256(x[0]).digest(); g = x[1]
    elif t == 'hmac': e = hmac.new(x[0], x[1], hashlib.sha256).digest(); g = x[2]
    elif t == 'gcm':
        k, iv, ad, pt, g = x
        e = AESGCM(k).encrypt(iv, pt, ad)
    elif t == 'x25519':
        e = X25519PrivateKey.from_private_bytes(x[0]).exchange(X25519PublicKey.from_public_bytes(x[1]))
        g = x[2]
    elif t == 'md5': e = hashlib.md5(x[0]).digest(); g = x[1]
    elif t == 'md4':
        e = md4(x[0]); g = x[1]
    elif t == 'hmd5': e = hmac.new(x[0], x[1], hashlib.md5).digest(); g = x[2]
    elif t == 'prf': e = prf(x[0], b'key expansion', x[1], 40); g = x[2]
    else: print('?', l.strip()); bad += 1; continue
    if e == g: ok += 1
    else: bad += 1; print('FALSCH', t, l[:120])
print('gleich', ok, 'falsch', bad)
