# SPDX-License-Identifier: GPL-2.0-or-later
# Stoersender fuer TCPIP.SYS: QEMU -netdev socket,mcast=230.0.0.1:12347
# (Karte rtl8139 oder ne2k_pci, im Gast TCPIP.SYS /NOFW mit DHCP,
# DETACH NC -l -k 5556 und eine NSLOOKUP-Schleife). Ausgaben in $NETFUZZ_DIR.
# Gegenstelle 10.9.0.1 / fe80::1, fd00::1: ARP, ND, DHCP, RA, DNS (teils kaputt),
# dann Runden mit verstuemmelten Paketen; nach jeder Runde Lebenszeichen
# (Echo v4 + v6, TCP-Handshake an 5556). Aufruf: fuzz.py <runden> <je_runde> <seed>
import socket, struct, time, random, sys, os
OUT = os.environ.get('NETFUZZ_DIR', '.')
R = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 20
PER = int(sys.argv[2]) if len(sys.argv) > 2 else 300
GRP = ('230.0.0.1', 12347)
MAC = bytes.fromhex('020000000001')
IP4 = bytes([10, 9, 0, 1]); GIP4 = bytes([10, 9, 0, 2])
def p6(s):
    a, b = s.split('::'); a = [x for x in a.split(':') if x]; b = [x for x in b.split(':') if x]
    return b''.join(struct.pack('>H', int(x, 16)) for x in a + ['0'] * (8 - len(a) - len(b)) + b)
LL = p6('fe80::1'); GA = p6('fd00::1'); PFX = p6('fd00::')
LOG = open(os.path.join(OUT, 'fuzz.log'), 'a', buffering=1)
def log(*a): LOG.write(' '.join(str(x) for x in a) + '\n')
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(GRP)
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(GRP[0]) + socket.inet_aton('127.0.0.1'))
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton('127.0.0.1'))
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
s.settimeout(0.01)
G = {'mac': None, 'xid': None, 'ga': None, 'll': None, 'dnsbad': 0.0}
sent = [0]
PC = open(os.path.join(OUT, 'sent.bin'), 'wb')
def tx(fr):
    PC.write(struct.pack('<IH', sent[0], len(fr)) + fr); PC.flush()
    if len(fr) < 60 and R.random() < 0.9: fr += b'\0' * (60 - len(fr))
    s.sendto(fr, GRP); sent[0] += 1
    PC.write(b'')
def csum(b):
    if len(b) & 1: b += b'\0'
    t = sum(struct.unpack('>%dH' % (len(b) // 2), b))
    while t >> 16: t = (t & 0xFFFF) + (t >> 16)
    return t
def ck(b): return (~csum(b)) & 0xFFFF
def eth(dmac, et, pl): return dmac + MAC + struct.pack('>H', et) + pl
def ip4(src, dst, proto, pl, ttl=64, fix=True, ihl=5, tl=None, frag=0, opts=b''):
    h = struct.pack('>BBHHHBBH4s4s', 0x40 | ihl, 0, tl if tl is not None else 20 + len(opts) + len(pl),
                    R.randrange(65536), frag, ttl, proto, 0, src, dst) + opts
    if fix: h = h[:10] + struct.pack('>H', ck(h)) + h[12:]
    return h + pl
def ps4(src, dst, proto, n): return src + dst + struct.pack('>BBH', 0, proto, n)
def ps6(src, dst, proto, n): return src + dst + struct.pack('>IxxxB', n, proto)
def l4fix(seg, off, ph): return seg[:off] + struct.pack('>H', ck(ph + seg[:off] + b'\0\0' + seg[off + 2:])) + seg[off + 2:]
def udp(sp, dp, data, ph=None, ulen=None):
    u = struct.pack('>HHHH', sp, dp, ulen if ulen is not None else 8 + len(data), 0) + data
    return l4fix(u, 6, ph(len(u))) if ph else u
def ip6(src, dst, nh, pl, hl=64, plen=None):
    return struct.pack('>IHBB', 0x60000000, plen if plen is not None else len(pl), nh, hl) + src + dst + pl
def icmp6(src, dst, t, c, body, hl=255):
    m = bytes([t, c]) + b'\0\0' + body
    return ip6(src, dst, 58, l4fix(m, 2, ps6(src, dst, 58, len(m))), hl)
def tcp(sp, dp, seq, ack, fl, data=b'', opts=b'', wnd=8192, ph=None, doff=None):
    if len(opts) & 3: opts += b'\0' * (4 - (len(opts) & 3))
    do = doff if doff is not None else 5 + len(opts) // 4
    t = struct.pack('>HHIIBBHHH', sp, dp, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF, (do & 15) << 4, fl, wnd, 0, 0) + opts + data
    return l4fix(t, 16, ph(len(t))) if ph else t
def snm(a): return p6('ff02::1:ff00:0')[:13] + a[13:]
def mcmac(a): return b'\x33\x33' + a[12:]
BC = b'\xff' * 6
# ---- normale Dienste -----------------------------------------------------
def dhcp_reply(typ, xid, bad=False):
    b = struct.pack('>BBBB4sHH4s4s4s4s', 2, 1, 6, 0, xid, 0, 0, b'\0' * 4, GIP4, IP4, b'\0' * 4)
    b += G['mac'] + b'\0' * 10 + b'\0' * 192 + bytes([99, 130, 83, 99])
    o = bytes([53, 1, typ, 54, 4]) + IP4 + bytes([1, 4, 255, 255, 255, 0, 3, 4]) + IP4 + bytes([6, 4]) + IP4
    o += bytes([51, 4, 0, 0, 0x0e, 0x10, 15, 3]) + b'lan' + b'\xff'
    if bad:
        k = R.randrange(4)
        if k == 0: o = bytes([53, 1, typ]) + bytes(R.randrange(256) for _ in range(R.randrange(60)))
        elif k == 1: o = bytes([53, 1, typ, 6, 255]) + IP4
        elif k == 2: o = bytes([53, 1, typ, 15, 200]) + b'A' * 40
        else: o = bytes([53, 1, typ, 0, 0, 0, 1, 0, 3, 250]) + b'\x01\x02'
    pl = udp(67, 68, b + o, lambda n: ps4(IP4, b'\xff' * 4, 17, n))
    tx(eth(G['mac'], 0x0800, ip4(IP4, b'\xff' * 4, 17, pl)))
def dns_reply(q, sport, dst):
    qid = q[:2]; qn = q[12:]
    end = qn.index(b'\0') + 5; question = qn[:end]
    qt = struct.unpack('>H', question[-4:-2])[0]
    r = qid + b'\x81\x80' + struct.pack('>HHHH', 1, 1, 0, 0) + question
    if qt == 1: r += b'\xc0\x0c' + struct.pack('>HHIH', 1, 1, 60, 4) + bytes([10, 9, 0, 7])
    else: r += b'\xc0\x0c' + struct.pack('>HHIH', 28, 1, 60, 16) + p6('fd00::7')
    if R.random() < G['dnsbad']:
        k = R.randrange(7)
        if k == 0: r = qid + b'\x81\x80' + struct.pack('>HHHH', 1, 50, 0, 0) + question + b'\xc0\x0c' * 300
        elif k == 1: r = qid + b'\x81\x80' + struct.pack('>HHHH', 1, 3, 0, 0) + question + b'\xc0\x0c' + struct.pack('>HHIH', qt, 1, 60, 0xFFF0) + b'x' * 20
        elif k == 2: r = qid + b'\x81\x80' + struct.pack('>HHHH', 0xFFFF, 0xFFFF, 0, 0) + b'\x3f' * 200
        elif k == 3: r = qid + b'\x81\x80' + struct.pack('>HHHH', 1, 1, 0, 0) + b'\xc0\x0c'  # Zeiger auf sich
        elif k == 4: r = r[:R.randrange(2, len(r))]
        elif k == 5: r = qid + bytes(R.randrange(256) for _ in range(R.randrange(600)))
        else: r = qid + b'\x81\x80' + struct.pack('>HHHH', 1, 400, 0, 0) + question + (b'\xc0\x0c' + struct.pack('>HHIH', qt, 1, 60, 4 if qt == 1 else 16) + b'\x0a\x09\x00\x07' * 4)[:26] * 60
    if len(dst) == 4:
        pl = udp(53, sport, r, lambda n: ps4(IP4, dst, 17, n)); tx(eth(G['mac'], 0x0800, ip4(IP4, dst, 17, pl)))
    else:
        pl = udp(53, sport, r, lambda n: ps6(GA, dst, 17, n)); tx(eth(G['mac'], 0x86DD, ip6(GA, dst, 17, pl)))
def ra(bad=False):
    body = struct.pack('>BBHII', 64, 0, 1800, 0, 0) + bytes([1, 1]) + MAC
    body += struct.pack('>BBBBIII', 3, 4, 64, 0xC0, 86400, 14400, 0) + PFX
    body += struct.pack('>BBHI', 25, 3, 0, 600) + GA
    tx(eth(mcmac(p6('ff02::1')), 0x86DD, icmp6(LL, p6('ff02::1'), 134, 0, body)))
pend = {}   # Lebenszeichen
tcpc = {}
def handle(f):
    if len(f) < 14 or f[6:12] == MAC: return
    et = struct.unpack('>H', f[12:14])[0]; G['mac'] = f[6:12]
    if et == 0x0806 and len(f) >= 42 and f[38:42] == IP4 and f[20:22] == b'\0\1':
        tx(eth(f[6:12], 0x0806, struct.pack('>HHBBH', 1, 0x800, 6, 4, 2) + MAC + IP4 + f[22:28] + f[28:32]))
    elif et == 0x0800 and len(f) >= 34:
        hl = (f[14] & 15) * 4; p = f[23]; pl = f[14 + hl:14 + struct.unpack('>H', f[16:18])[0]]
        if p == 17 and len(pl) >= 8:
            sp, dp = struct.unpack('>HH', pl[:4]); d = pl[8:]
            if dp == 67 and len(d) > 240:
                G['xid'] = d[4:8]; o = d[240:]; typ = o[2] if o[:2] == b'\x35\x01' else 0
                for _ in range(3): dhcp_reply(2 if typ == 1 else 5, rb(4), bad=True)
                dhcp_reply(2 if typ == 1 else 5, G['xid'])
            elif dp == 53: dns_reply(d, sp, f[26:30])
        elif p == 1 and pl[:1] == b'\0': pend[('4', struct.unpack('>H', pl[6:8])[0])] = time.time()
        elif p == 6: tcp_in(pl, f[26:30])
    elif et == 0x86DD and len(f) >= 54:
        nh = f[20]; src = f[22:38]; dst = f[38:54]; pl = f[54:54 + struct.unpack('>H', f[18:20])[0]]
        if nh == 58 and pl:
            t = pl[0]
            if t == 133: ra()
            elif t == 135 and len(pl) >= 24:
                tg = pl[8:24]
                if src == b'\0' * 16:
                    if tg[:2] == b'\xfe\x80': G['ll'] = tg
                    else: G['ga'] = tg
                elif tg in (LL, GA):
                    tx(eth(f[6:12], 0x86DD, icmp6(tg, src, 136, 0, struct.pack('>I', 0x60000000) + tg + bytes([2, 1]) + MAC)))
            elif t == 129: pend[('6', struct.unpack('>H', pl[6:8])[0])] = time.time()
        elif nh == 17 and len(pl) >= 8:
            sp, dp = struct.unpack('>HH', pl[:4])
            if dp == 53: dns_reply(pl[8:], sp, src)
        elif nh == 6: tcp_in(pl, src)
def tcp_in(t, src):
    sp, dp, seq, ack, off, fl = struct.unpack('>HHIIBB', t[:14])
    c = tcpc.get(dp)
    if c is not None and sp == 5556 and (fl & 0x12) == 0x12: c['ok'] = True; c['rs'] = seq + 1; c['ack'] = ack
    if c is not None and sp == 5556 and fl & 0x04: c['rst'] = True
def pump(tmo):
    e = time.time() + tmo
    while time.time() < e:
        try: f, a = s.recvfrom(2000)
        except socket.timeout: continue
        handle(f)
# ---- Lebenszeichen -------------------------------------------------------
seqn = [1000]
def alive():
    ok = {}
    for tr in range(3):
        seqn[0] += 1; n = seqn[0]
        pl = struct.pack('>BBHHH', 8, 0, 0, 0x4242, n) + b'lebst du'
        pl = pl[:2] + struct.pack('>H', ck(pl)) + pl[4:]
        tx(eth(G['mac'], 0x0800, ip4(IP4, GIP4, 1, pl)))
        if G['ga']: tx(eth(G['mac'], 0x86DD, icmp6(GA, G['ga'], 128, 0, struct.pack('>HH', 0x4242, n) + b'lebst du', 64)))
        pump(1.0)
        ok['4'] = ok.get('4') or ('4', n) in pend
        ok['6'] = ok.get('6') or (('6', n) in pend) or not G['ga']
        if ok['4'] and ok['6']: break
    sp = R.randrange(20000, 30000); tcpc[sp] = c = {}
    tx(eth(G['mac'], 0x0800, ip4(IP4, GIP4, 6, tcp(sp, 5556, 777, 0, 0x02, opts=b'\x02\x04\x05\xb4', ph=lambda n: ps4(IP4, GIP4, 6, n)))))
    pump(1.5)
    ok['tcp'] = c.get('ok', False)
    if c.get('ok'):
        ph = lambda n: ps4(IP4, GIP4, 6, n)
        tx(eth(G['mac'], 0x0800, ip4(IP4, GIP4, 6, tcp(sp, 5556, 778, c['rs'], 0x10, ph=ph))))
        tx(eth(G['mac'], 0x0800, ip4(IP4, GIP4, 6, tcp(sp, 5556, 778, c['rs'], 0x04, ph=ph))))
    return ok
# ---- Stoerungen ----------------------------------------------------------
def rb(n): return bytes(R.randrange(256) for _ in range(n))
def mut(b):
    b = bytearray(b)
    for _ in range(R.randrange(1, 4)):
        if not b: break
        k = R.randrange(3)
        if k == 0: b[R.randrange(len(b))] = R.randrange(256)
        elif k == 1: b = b[:R.randrange(len(b))]
        else: b += rb(R.randrange(1, 40))
    return bytes(b)
def ph4(p): return lambda n: ps4(IP4, GIP4, p, n)
def ph6(p): return lambda n: ps6(LL, G['ll'] or p6('fe80::2'), p, n)
GL = lambda: G['ll'] or p6('fe80::2')
def topt():
    o = b''
    for _ in range(R.randrange(1, 6)):
        k = R.choice([0, 1, 2, 3, 4, 8, 99, 2, 2])
        if k in (0, 1): o += bytes([k])
        elif k == 2: o += bytes([2, R.choice([0, 1, 2, 4, 4, 255])]) + struct.pack('>H', R.choice([0, 1, 2, 63, 64, 536, 65535]))
        else: o += bytes([k, R.randrange(256)]) + rb(R.randrange(10))
    return o[:40]
def gen():
    k = R.randrange(22)
    gm = G['mac']
    if k == 0: return eth(gm, R.choice([0x800, 0x86DD, 0x806]), rb(R.randrange(0, 80)))
    if k == 1:   # IPv4-Kopf verbogen, Pruefsumme richtig
        ihl = R.choice([0, 1, 4, 5, 5, 6, 15]); tl = R.choice([0, 1, 19, 20, 28, 60, 1500, 65535, None])
        return eth(gm, 0x800, ip4(IP4, GIP4, R.choice([1, 6, 17, 99]), rb(R.randrange(40)), ihl=ihl, tl=tl,
                                  opts=rb(4 * max(0, ihl - 5)) if ihl > 5 else b''))
    if k == 2: return eth(gm, 0x800, ip4(IP4, GIP4, 6, b'', frag=R.choice([0x2000, 0x0001, 0x4000 | 1])))
    if k == 3:   # UDP-Laenge falsch
        d = rb(R.randrange(30))
        return eth(gm, 0x800, ip4(IP4, GIP4, 17, udp(R.randrange(65536), R.choice([53, 68, 5556, 7]), d, ph4(17), ulen=R.choice([0, 7, 8, 9, 65535, len(d) + 20]))))
    if k == 4:   # TCP mit kaputten Optionen / Datenoffset
        return eth(gm, 0x800, ip4(IP4, GIP4, 6, tcp(R.randrange(1024, 65536), 5556, R.randrange(1 << 32), R.randrange(1 << 32),
                    R.choice([0x02, 0x02, 0x12, 0x10, 0x04, 0x01, 0x3f, 0x00, 0x18]), rb(R.randrange(20)), topt(),
                    doff=R.choice([None, None, 0, 1, 4, 15]), ph=ph4(6))))
    if k == 5:   # SYN-Flut von wechselnden Adressen
        src = bytes([10, 9, R.randrange(256), R.randrange(1, 255)])
        return eth(gm, 0x800, ip4(src, GIP4, 6, tcp(R.randrange(1024, 65536), 5556, R.randrange(1 << 32), 0, 0x02,
                    opts=b'\x02\x04\x00\x01', ph=lambda n: ps4(src, GIP4, 6, n))))
    if k == 6:   # ICMP-Fehler mit Muell innen
        inner = rb(R.randrange(0, 60))
        if R.random() < .5: inner = bytes([R.choice([0x40, 0x4f, 0x45])]) + rb(8) + b'\x01' + rb(10) + b'\x08' + rb(R.randrange(20))
        m = bytes([R.choice([3, 11, 5, 0, 8]), 0, 0, 0]) + rb(4) + inner
        m = m[:2] + struct.pack('>H', ck(m)) + m[4:]
        return eth(gm, 0x800, ip4(IP4, GIP4, 1, m))
    if k == 7:   # Echo an Rundruf / von unsinnigen Absendern
        m = b'\x08\x00\x00\x00\x00\x01\x00\x01'; m = m[:2] + struct.pack('>H', ck(m)) + m[4:]
        return eth(BC, 0x800, ip4(R.choice([b'\0\0\0\0', b'\x7f\0\0\1', b'\xe0\0\0\1', GIP4, IP4]), R.choice([b'\xff' * 4, GIP4]), 1, m))
    if k == 8:   # ARP Muell
        return eth(R.choice([gm, BC]), 0x806, mut(struct.pack('>HHBBH', 1, 0x800, 6, 4, R.choice([1, 2, 3])) + rb(6) + rb(4) + rb(6) + R.choice([GIP4, rb(4)])))
    if k == 9:   # IPv6 Erweiterungskoepfe
        ext = b''; nh0 = None; chain = []
        for _ in range(R.randrange(1, 6)):
            chain.append(R.choice([0, 60, 43, 44, 0, 60]))
        nh_first = chain[0]; body = b''
        for idx, h in enumerate(chain):
            nxt = chain[idx + 1] if idx + 1 < len(chain) else R.choice([58, 6, 17, 59])
            ln = R.choice([0, 0, 1, 255, 3])
            body += bytes([nxt, ln]) + rb(6 + ln * 8)[:R.choice([6 + ln * 8, 6 + ln * 8, 2])]
        body += rb(R.randrange(30))
        return eth(gm, 0x86DD, ip6(LL, GL(), nh_first, body, plen=R.choice([None, None, 0, 7, 65535])))
    if k == 10:  # ND/RA mit kaputten Optionen, gueltige Pruefsumme
        t = R.choice([134, 135, 136, 133, 137])
        opt = b''
        for _ in range(R.randrange(1, 5)):
            ot = R.choice([1, 2, 3, 5, 25, 99]); ol = R.choice([0, 1, 2, 3, 4, 255])
            opt += bytes([ot, ol]) + rb(max(0, ol * 8 - 2))[:R.choice([max(0, ol * 8 - 2), R.randrange(10)])]
        if t == 134: body = struct.pack('>BBHII', 64, 0, R.choice([0, 1, 65535]), 0, 0) + opt
        else: body = b'\0\0\0\0' + R.choice([GL(), G['ga'] or LL, rb(16), p6('ff02::1')]) + opt
        return eth(R.choice([gm, mcmac(p6('ff02::1'))]), 0x86DD, icmp6(LL, R.choice([GL(), p6('ff02::1')]), t, 0, body, hl=R.choice([255, 255, 64])))
    if k == 11:  # ICMPv6 Fehler / Echo mit Muell
        t = R.choice([1, 3, 128, 129, 2, 4]); body = rb(R.randrange(0, 70))
        return eth(gm, 0x86DD, icmp6(LL, GL(), t, R.randrange(4), body, hl=64))
    if k == 12:  # unsinnige IPv6-Absender
        src = R.choice([b'\0' * 16, p6('::1'), b'\0'*10+b'\xff\xff'+IP4, b'\0'*12+IP4, GL(), p6('ff02::1')])
        dst = R.choice([GL(), p6('ff02::1'), snm(GL())])
        seg = tcp(R.randrange(1024, 65536), 5556, 5, 0, 0x02, ph=lambda n: ps6(src, dst, 6, n)) if R.random() < .5 else \
              udp(1234, 5556, b'hallo', lambda n: ps6(src, dst, 17, n))
        return eth(gm, 0x86DD, ip6(src, dst, 6 if seg[13:14] and len(seg) == 20 else 17, seg))
    if k == 13:  # TCP ueber IPv6 mit Optionen
        return eth(gm, 0x86DD, ip6(LL, GL(), 6, tcp(R.randrange(1024, 65536), 5556, R.randrange(1 << 32), R.randrange(1 << 32),
                   R.choice([0x02, 0x10, 0x04, 0x11, 0x3f]), rb(R.randrange(20)), topt(), doff=R.choice([None, 0, 15]), ph=ph6(6))))
    if k == 14:  # UDP v6 ohne Pruefsumme / Laenge falsch
        d = rb(R.randrange(20))
        u = udp(R.randrange(65536), R.choice([53, 5556, 547]), d, R.choice([None, ph6(17)]), ulen=R.choice([None, 0, 7, 500]))
        return eth(gm, 0x86DD, ip6(LL, GL(), 17, u))
    if k == 15:  # TCP an Gruppe / Rundruf
        m = tcp(R.randrange(1024, 65536), 5556, 1, 0, 0x02, ph=lambda n: ps4(IP4, b'\xff' * 4, 6, n))
        return eth(BC, 0x800, ip4(IP4, b'\xff' * 4, 6, m))
    if k == 16:  # verstuemmelter gueltiger Rahmen (IPv4)
        b = eth(gm, 0x800, ip4(IP4, GIP4, R.choice([1, 6, 17]), tcp(R.randrange(65536), 5556, 1, 1, 0x12, ph=ph4(6))))
        return mut(b)
    if k == 17:  # verstuemmelter gueltiger Rahmen (IPv6)
        return mut(eth(gm, 0x86DD, icmp6(LL, GL(), 128, 0, b'\0\1\0\1' + rb(20), hl=64)))
    if k == 18:  # Riesenrahmen / Mini
        return eth(gm, R.choice([0x800, 0x86DD]), rb(R.choice([0, 1, 1486, 1500, 2000])))
    if k == 19:  # DHCP-Antworten ohne Anfrage
        if G['xid']: dhcp_reply(R.choice([2, 5, 6]), R.choice([G['xid'], rb(4)]), bad=True)
        return None
    if k == 20:  # RST/ACK-Sturm an eine bestehende Verbindung
        return eth(gm, 0x800, ip4(IP4, GIP4, 6, tcp(R.randrange(20000, 30000), 5556, R.randrange(1 << 32), R.randrange(1 << 32),
                   R.choice([0x04, 0x14, 0x10, 0x11]), ph=ph4(6))))
    if k == 21:  # Echo v6 an Multicast / Riesen-Echo
        return eth(mcmac(p6('ff02::1')), 0x86DD, icmp6(LL, p6('ff02::1'), 128, 0, b'\0\1\0\1' + rb(R.choice([0, 1400, 1452])), hl=64))
def diag():
    got = []
    def h(f):
        if f[6:12] != MAC: got.append(f[12:14].hex() + ' ' + f[14:60].hex())
    tx(eth(BC, 0x806, struct.pack('>HHBBH', 1, 0x800, 6, 4, 1) + MAC + IP4 + b'\0' * 6 + GIP4))
    pl = struct.pack('>BBHHH', 8, 0, 0, 0x4242, 9) + b'x'; pl = pl[:2] + struct.pack('>H', ck(pl)) + pl[4:]
    tx(eth(G['mac'], 0x0800, ip4(IP4, GIP4, 1, pl)))
    e = time.time() + 3
    while time.time() < e:
        try: f, a = s.recvfrom(2000)
        except socket.timeout: continue
        h(f)
    for g in got: print('  diag', g)
# ---- Ablauf --------------------------------------------------------------
log('--- Start', time.ctime())
t0 = time.time()
while time.time() - t0 < 240:       # auf DHCP + SLAAC warten
    pump(0.5)
    if G['mac'] and G['ga']:
        break
pump(3)
log('Gast', G['mac'] and G['mac'].hex(), 'll', G['ll'] and G['ll'].hex(), 'ga', G['ga'] and G['ga'].hex())
a = alive(); log('vorher', a)
if not a['4']:
    log('Gast antwortet nicht vorher'); print('vorher tot', a); sys.exit(1)
G['dnsbad'] = 0.7
bad = 0
for r in range(ROUNDS):
    for _ in range(PER):
        f = gen()
        if f: tx(f)
        if R.random() < .05: pump(0.002)
    pump(0.3)
    a = alive(); log('Runde', r, sent[0], a)
    print('Runde', r, 'gesendet', sent[0], a, flush=True)
    if not (a['4'] and a['6'] and a['tcp']):
        bad += 1
        pump(3); a = alive(); log('  nochmal', a); print('  nochmal', a, flush=True)
        if not a['4']:
            diag(); print('TOT nach Runde', r); sys.exit(2)
G['dnsbad'] = 0
pump(2)
print('fertig, Ausfaelle', bad)
