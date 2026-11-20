# SPDX-License-Identifier: GPL-2.0-or-later
# Erzeugt Pruefzeilen fuer tools/pkitest.c (Schluessel, Signaturen,
# Zertifikate mit Python cryptography). Aufruf: python3 tools/pkitest.py [cacert.pem]
import sys, os, hashlib, random, datetime, ipaddress
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding, utils
from cryptography import x509
from cryptography.x509.oid import NameOID
H = {1: hashes.SHA256(), 2: hashes.SHA384(), 3: hashes.SHA512()}
HL = {1: hashlib.sha256, 2: hashlib.sha384, 3: hashlib.sha512}
r = random.Random(5)
for alg in (2, 3):
    for n in list(range(0, 260, 7)) + [111, 112, 127, 128, 129, 239, 240, 1000]:
        d = bytes(r.getrandbits(8) for _ in range(n))
        print('sha', alg, d.hex() or '-', HL[alg](d).hexdigest())
def ib(v, n=None):
    n = n or (v.bit_length() + 7) // 8
    return v.to_bytes(n, 'big')
for bits in (2048, 3072, 4096):
    k = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    pn = k.public_key().public_numbers()
    for alg in (1, 2, 3):
        for pss in (0, 1):
            for t in range(2):
                m = os.urandom(50)
                h = HL[alg](m).digest()
                if pss:
                    sig = k.sign(m, padding.PSS(padding.MGF1(H[alg]), padding.PSS.DIGEST_LENGTH if t == 0 else padding.PSS.MAX_LENGTH), H[alg])
                else:
                    sig = k.sign(m, padding.PKCS1v15(), H[alg])
                print('sig 1', 2 if pss else 1, alg, h.hex(), sig.hex(), ib(pn.n).hex(), ib(pn.e).hex(), 1)
                bad = bytearray(sig); bad[r.randrange(len(bad))] ^= 1 << r.randrange(8)
                print('sig 1', 2 if pss else 1, alg, h.hex(), bytes(bad).hex(), ib(pn.n).hex(), ib(pn.e).hex(), 0)
                hb = bytearray(h); hb[0] ^= 1
                print('sig 1', 2 if pss else 1, alg, bytes(hb).hex(), sig.hex(), ib(pn.n).hex(), ib(pn.e).hex(), 0)
for curve, typ, sz in ((ec.SECP256R1(), 2, 32), (ec.SECP384R1(), 3, 48)):
    for t in range(12):
        k = ec.generate_private_key(curve)
        pt = k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        for alg in (1, 2):
            m = os.urandom(40)
            h = HL[alg](m).digest()
            sig = k.sign(m, ec.ECDSA(H[alg]))
            print('sig', typ, 3, alg, h.hex(), sig.hex(), pt.hex(), '00', 1)
            rr, ss = utils.decode_dss_signature(sig)
            print('sig', typ, 3, alg, h.hex(), utils.encode_dss_signature(rr, ss ^ 1).hex(), pt.hex(), '00', 0)
            hb = bytearray(h); hb[0] ^= 0x80
            print('sig', typ, 3, alg, bytes(hb).hex(), sig.hex(), pt.hex(), '00', 0)
if len(sys.argv) > 1:
    for c in x509.load_pem_x509_certificates(open(sys.argv[1], 'rb').read()):
        print('crt', c.public_bytes(serialization.Encoding.DER).hex())
# Namen
k = ec.generate_private_key(ec.SECP256R1())
def mk(cn, sans):
    b = x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])) \
        .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'ca')])).public_key(k.public_key()) \
        .serial_number(1).not_valid_before(datetime.datetime(2020, 1, 1)).not_valid_after(datetime.datetime(2030, 1, 1))
    if sans is not None:
        b = b.add_extension(x509.SubjectAlternativeName(sans), False)
    return b.sign(k, hashes.SHA256()).public_bytes(serialization.Encoding.DER).hex()
c1 = mk('nix.example', [x509.DNSName('www.Example.COM'), x509.DNSName('*.test.org'), x509.IPAddress(ipaddress.ip_address('10.0.2.2'))])
for h, e in (('www.example.com', 1), ('example.com', 0), ('a.test.org', 1), ('a.b.test.org', 0), ('test.org', 0),
             ('10.0.2.2', 1), ('10.0.2.3', 0), ('nix.example', 0), ('WWW.EXAMPLE.COM', 1)):
    print('name', c1, h, e)
c2 = mk('alt.example', None)
print('name', c2, 'alt.example', 1)
print('name', c2, 'other.example', 0)
