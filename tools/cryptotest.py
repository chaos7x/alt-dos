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
    elif t == 'prf': e = prf(x[0], b'key expansion', x[1], 40); g = x[2]
    else: print('?', l.strip()); bad += 1; continue
    if e == g: ok += 1
    else: bad += 1; print('FALSCH', t, l[:120])
print('gleich', ok, 'falsch', bad)
