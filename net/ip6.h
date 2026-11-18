/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TCPIP.SYS - IPv6 (in tcpip.c eingebunden, hinter DHCP und vor sock.h).
 * Entwurf docs/NETZWERK.md, Abschnitt IPv6.
 *
 * - Adresse link-lokal fe80::EUI-64 (RFC 4291 Anhang A) beim Anmelden der
 *   Karte, Duplicate Address Detection (RFC 4862, 5.4: ein NS von ::, 1 s)
 * - Router Solicitation (bis 3, je 4 s), Router Advertisement: Standard-
 *   Router, Praefix mit A-Bit und Laenge 64 -> globale Adresse (SLAAC, auch
 *   mit DAD), RDNSS (RFC 8106) als DNS-Server, Quell-MAC des Routers
 * - Neighbor Discovery (RFC 4861): NS/NA ueber den gemeinsamen Nachbar-
 *   cache in tcpip.c (arp_send parkt, nd_ask fragt)
 * - ICMPv6 (RFC 4443): Echo beantworten (nicht an Multicast; von ausserhalb
 *   der eigenen Netze nur mit Firewall-Regel), Echo fuer NET_PING, Fehler
 *   "unerreichbar"/"Hop-Limit" zu eigenem Echo
 * Nicht: DHCPv6, MLD-Meldungen (Switches ohne MLD-Snooping reichen
 * Multicast ohnehin durch), Pfad-MTU (gesendet wird hoechstens 1500),
 * Fragmente, Datenschutzadressen.
 */

static const unsigned char allnodes[16] = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static const unsigned char allrtrs[16]  = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

static unsigned long G32x(const unsigned char __far *p)
{
    return (unsigned long)G16(p) << 16 | G16(p + 2);
}

/* Solicited-Node-Gruppe zu t: ff02::1:ffXX:XXXX */
static void snm(unsigned char __far *d, const unsigned char __far *t)
{
    zero(d, 16);
    d[0] = 0xFF;
    d[1] = 2;
    d[11] = 1;
    d[12] = 0xFF;
    cpy(d + 13, t + 13, 3);
}

static void v6_log(const char *t, const unsigned char __far *a)
{
    static char l[80];
    char *p = scat(l, t);
    unsigned k, w;
    static const char hx[] = "0123456789abcdef";
    for (k = 0; k < 16; k += 2) {
        w = G16(a + k);
        if (w >= 0x1000) *p++ = hx[w >> 12];
        if (w >= 0x100) *p++ = hx[(w >> 8) & 15];
        if (w >= 0x10) *p++ = hx[(w >> 4) & 15];
        *p++ = hx[w & 15];
        if (k < 14)
            *p++ = ':';
    }
    *p++ = '\n';
    devhlp(DH_KLOG, 0, (unsigned)l, p - l);
}

/* Absender fuer Ziel d an Karte i (RFC 6724 vereinfacht): global, wenn das
   Ziel nicht link-lokal/Multicast ist und es eine gibt, sonst link-lokal */
static int v6_src(int i, const unsigned char __far *d, unsigned char __far *src)
{
    struct nif *n = &nif[i];
    if (!IS_LL(d) && !IS_MC(d) && n->gast == 2) {
        cpy(src, n->ga, 16);
        return 0;
    }
    if (n->llst == 2) {
        cpy(src, n->ll, 16);
        return 0;
    }
    return -1;
}

/* Karte fuer IPv6-Ziel d -> Karte (src gesetzt) oder -1 */
static int route6(const unsigned char __far *d, unsigned char __far *src)
{
    int i;
    if (!v6_on)
        return -1;
    for (i = 0; i < NNETIF; i++)        /* direkt erreichbar               */
        if (nif[i].up && nif[i].llst == 2 && !IS_LL(d) && v6_onlink(i, d))
            return v6_src(i, d, src) ? -1 : i;
    for (i = 0; i < NNETIF; i++)
        if (nif[i].up && nif[i].llst == 2 &&
            (IS_LL(d) || IS_MC(d) || !is_zero16(nif[i].rtr)))
            return v6_src(i, d, src) ? -1 : i;
    return -1;
}

/* IPv6-Kopf in txb (Nutzdaten ab 54 liegen schon), senden -> wie arp_send */
static int ip6_out(int i, const unsigned char __far *src, const unsigned char __far *dst,
                   unsigned char nh, unsigned dlen, unsigned char hl)
{
    struct nif *n = &nif[i];
    unsigned char hop[16];
    txb[14] = 0x60;
    txb[15] = txb[16] = txb[17] = 0;
    P16(txb + 18, dlen);
    txb[20] = nh;
    txb[21] = hl;
    cpy(txb + 22, src, 16);
    cpy(txb + 38, dst, 16);
    cpy(txb + 6, n->mac, 6);
    P16(txb + 12, 0x86DD);
    if (IS_MC(dst)) {                   /* 33:33 + letzte 4 Byte          */
        txb[0] = txb[1] = 0x33;
        cpy(txb + 2, dst + 12, 4);
        return eth_send(i, 54 + dlen);
    }
    if (v6_onlink(i, dst))
        cpy(hop, dst, 16);
    else if (!is_zero16(n->rtr))
        cpy(hop, n->rtr, 16);
    else
        return -1;
    return arp_send(i, hop, 54 + dlen);
}

/* ICMPv6-Nachricht ab txb+54 (len Byte): Pruefsumme, senden */
static int icmp6_send(int i, const unsigned char __far *src, const unsigned char __far *dst,
                      unsigned len, unsigned char hl)
{
    P16(txb + 56, 0);
    P16(txb + 56, l4sum(src, dst, 58, txb + 54, len));
    return ip6_out(i, src, dst, 58, len, hl);
}

/* Neighbor Solicitation nach t; dad: Absender ::, ohne eigene MAC */
static void ns_send(int i, const unsigned char __far *t, int dad)
{
    struct nif *n = &nif[i];
    unsigned char dst[16], src[16], tg[16], *m = txb + 54;
    unsigned len = 24;
    cpy(tg, t, 16);
    snm(dst, tg);
    if (dad)
        zero(src, 16);
    else if (n->llst == 2)
        v6_src(i, tg, src);
    else
        return;
    m[0] = 135;
    m[1] = 0;
    zero(m + 4, 4);
    cpy(m + 8, tg, 16);
    if (!dad) {                         /* Option: eigene MAC             */
        m[24] = 1;
        m[25] = 1;
        cpy(m + 26, n->mac, 6);
        len = 32;
    }
    icmp6_send(i, src, dst, len, 255);
}

static void nd_ask(int i, const unsigned char __far *ip16)
{
    ns_send(i, ip16, 0);
}

/* Neighbor Advertisement fuer eigene Adresse t an to */
static void na_send(int i, const unsigned char __far *to, const unsigned char __far *t,
                    unsigned char fl)
{
    unsigned char *m = txb + 54, tg[16], d[16];
    cpy(tg, t, 16);
    cpy(d, to, 16);
    m[0] = 136;
    m[1] = 0;
    m[4] = fl;
    zero(m + 5, 3);
    cpy(m + 8, tg, 16);
    m[24] = 2;                          /* Option: eigene MAC             */
    m[25] = 1;
    cpy(m + 26, nif[i].mac, 6);
    icmp6_send(i, tg, d, 32, 255);
}

static void rs_send(int i)
{
    struct nif *n = &nif[i];
    unsigned char *m = txb + 54, src[16];
    unsigned len = 8;
    m[0] = 133;
    m[1] = 0;
    zero(m + 4, 4);
    if (n->llst == 2) {
        cpy(src, n->ll, 16);
        m[8] = 1;
        m[9] = 1;
        cpy(m + 10, n->mac, 6);
        len = 16;
    } else
        zero(src, 16);
    icmp6_send(i, src, allrtrs, len, 255);
}

static void v6_start(int i)             /* Karte da: link-lokal + DAD     */
{
    struct nif *n = &nif[i];
    zero(n->ll, 16);
    n->ll[0] = 0xFE;
    n->ll[1] = 0x80;
    n->ll[8] = n->mac[0] ^ 2;           /* EUI-64                         */
    n->ll[9] = n->mac[1];
    n->ll[10] = n->mac[2];
    n->ll[11] = 0xFF;
    n->ll[12] = 0xFE;
    cpy(n->ll + 13, n->mac + 3, 3);
    n->llst = 1;
    n->lldad = now + 100;
    ns_send(i, n->ll, 1);
}

static void nd_tick(int i)
{
    struct nif *n = &nif[i];
    if (!v6_on)
        return;
    if (n->llst == 1 && now >= n->lldad) {
        n->llst = 2;                    /* niemand hat widersprochen      */
        n->rsn = 0;
    }
    if (n->gast == 1 && now >= n->gadad)
        n->gast = 2;
    if (n->llst == 2 && is_zero16(n->rtr) && n->rsn < 3 &&
        (!n->rsn || now - n->rst >= 400)) {
        rs_send(i);
        n->rsn++;
        n->rst = now;
    }
    if (n->gast == 2 && secs >= n->gaexp)
        n->gast = 0;                    /* Praefix abgelaufen             */
    if (!is_zero16(n->rtr) && secs >= n->rtrexp)
        zero(n->rtr, 16);
}

/* ND-Optionen einer Nachricht m (ab off) durchgehen: Typ t -> Zeiger */
static fp nd_opt(fp m, unsigned ml, unsigned off, unsigned char t)
{
    fp o = m + off, e = m + ml;
    while (o + 2 <= e && o[1]) {
        if (o + o[1] * 8 > e)
            return 0;
        if (o[0] == t)
            return o;
        o += o[1] * 8;
    }
    return 0;
}

static unsigned char *nd_mine(struct nif *n, const unsigned char __far *t)
{
    if (n->llst && eq(t, n->ll, 16))
        return &n->llst;
    if (n->gast && eq(t, n->ga, 16))
        return &n->gast;
    return 0;
}

static void icmp6_in(int i, fp m, unsigned ml)
{
    struct nif *n = &nif[i];
    unsigned char *st, a[16], b[16];
    unsigned k, id, seq, r;
    fp o;
    if (ml < 8 || l4sum(rx_s, rx_d, 58, m, ml))
        return;
    switch (m[0]) {
    case 128:                           /* Echo-Anfrage                   */
        if (IS_MC(rx_d) || ml > 1500 - 40)
            return;                     /* kein Echo auf Multicast        */
        if (fw_on && !v6_onlink(i, rx_s) && !fw_allow("", FWP_ICMP, 0, rx_s, 0, i))
            return;
        cpy(a, rx_d, 16);
        cpy(b, rx_s, 16);
        cpy(txb + 54, m, ml);
        txb[54] = 129;
        icmp6_send(i, a, b, ml, 64);
        return;
    case 129:                           /* Echo-Antwort                   */
        id = G16(m + 4);
        seq = G16(m + 6);
        r = 0;
        break;
    case 1:                             /* unerreichbar, Hop-Limit:       */
    case 3:                             /* zu unserem Echo?               */
        if (ml < 8 + 40 + 8 || m[8 + 6] != 58 || m[8 + 40] != 128)
            return;
        id = G16(m + 8 + 40 + 4);
        seq = G16(m + 8 + 40 + 6);
        r = m[0] == 1 ? 3 : 11;         /* wie bei IPv4 melden (PING)     */
        break;
    case 135:                           /* Neighbor Solicitation          */
        if (rx_hl != 255 || ml < 24 || IS_MC(m + 8) || !(st = nd_mine(n, m + 8)))
            return;
        if (is_zero16(rx_s)) {          /* fremde DAD auf unsere Adresse  */
            if (*st == 1) {
                *st = 3;
                v6_log("IPv6: Adresse schon vergeben: ", m + 8);
            }
            return;
        }
        if (*st != 2)
            return;
        if ((o = nd_opt(m, ml, 24, 1)) != 0 && o[1] == 1)
            arp_put(i, rx_s, o + 2);
        na_send(i, rx_s, m + 8, 0x60);  /* angefragt, ueberschreiben     */
        return;
    case 136:                           /* Neighbor Advertisement         */
        if (rx_hl != 255 || ml < 24 || IS_MC(m + 8))
            return;
        if ((st = nd_mine(n, m + 8)) != 0) {
            if (*st == 1) {
                *st = 3;
                v6_log("IPv6: Adresse schon vergeben: ", m + 8);
            }
            return;
        }
        if ((o = nd_opt(m, ml, 24, 2)) != 0 && o[1] == 1)
            arp_upd(i, m + 8, o + 2);
        return;
    case 134:                           /* Router Advertisement           */
        if (rx_hl != 255 || ml < 16 || !IS_LL(rx_s))
            return;
        if (G16(m + 6)) {
            cpy(n->rtr, rx_s, 16);
            n->rtrexp = secs + G16(m + 6);
        } else if (eq(n->rtr, rx_s, 16))
            zero(n->rtr, 16);
        if ((o = nd_opt(m, ml, 16, 1)) != 0 && o[1] == 1)
            arp_put(i, rx_s, o + 2);
        for (o = m + 16; o + 2 <= m + ml && o[1] && o + o[1] * 8 <= m + ml; o += o[1] * 8) {
            if (o[0] == 3 && o[1] == 4 && o[2] == 64 && (o[3] & 0x40) && G32x(o + 4)) {
                cpy(a, o + 16, 8);      /* Praefix + eigene Kennung       */
                cpy(a + 8, n->ll + 8, 8);
                if (!n->gast || !eq(a, n->ga, 16)) {
                    cpy(n->ga, a, 16);
                    n->gplen = 64;
                    n->gast = 1;
                    n->gadad = now + 100;
                    ns_send(i, n->ga, 1);
                }
                n->gaexp = G32x(o + 4) == 0xFFFFFFFFUL ? 0xFFFFFFFFUL : secs + G32x(o + 4);
            }
            if (o[0] == 25 && o[1] >= 3 && G32x(o + 4))
                cpy(n->dns6, o + 8, 16);        /* RDNSS: erster Server   */
        }
        return;
    default:
        return;
    }
    for (k = 0; k < NPING; k++)
        if (pg[k].used && !pg[k].done && pg[k].id == id && pg[k].seq == seq) {
            pg[k].done = 1;
            pg[k].t1 = (unsigned)now;
            cpy(pg[k].from, rx_s, 16);
            pg[k].ttl = rx_hl;
            pg[k].type = (unsigned char)r;
            pg[k].code = m[1];
            proc_run(0x100 + k);
        }
}

/* ---- beide Familien ---------------------------------------------------- */
/* Karte und Absender (16 Byte) fuer Ziel d -> Karte oder -1 */
static int route_any(const unsigned char __far *d, unsigned char __far *src)
{
    unsigned char hop[4];
    int i;
    if (!is_v4(d))
        return route6(d, src);
    if (d[12] == 255 && d[13] == 255 && d[14] == 255 && d[15] == 255) {
        for (i = 0; i < NNETIF && !(nif[i].up && !is_zero4(nif[i].ip)); i++)
            ;
        if (i == NNETIF)
            return -1;
    } else if ((i = route4(d + 12, hop)) < 0)
        return -1;
    map4(src, nif[i].ip);
    return i;
}

/* Nutzdaten liegen ab txb + L4OFF(d) */
#define L4OFF(d)    (is_v4(d) ? 34 : 54)

static int ip_send(int i, const unsigned char __far *src, const unsigned char __far *d,
                   unsigned char proto, unsigned dlen, unsigned char ttl)
{
    if (is_v4(d))
        return ip4_out(i, d + 12, proto, dlen, ttl);
    return ip6_out(i, src, d, proto, dlen, ttl);
}

/* NET_PING fuer IPv6 (net_ping in tcpip.c hat u schon geprueft) */
static unsigned ping6(struct net_call __far *c, struct fapi_ping __far *u)
{
    unsigned char dst[16], src[16];
    unsigned k, len = u->len, j, to;
    int i;
    struct ping *q;
    cpy(dst, u->addr, 16);
    if (len > 1452)
        return FAPI_ERROR;
    if ((i = route6(dst, src)) < 0)
        return FAPI_ERR_NONET;
    for (k = 0; k < NPING && pg[k].used; k++)
        ;
    if (k == NPING)
        return FAPI_ERR_FULL;
    q = &pg[k];
    q->used = 1;
    q->done = 0;
    q->id = 0x4154 ^ c->task;
    q->seq = u->seq;
    txb[54] = 128;
    txb[55] = 0;
    P16(txb + 58, q->id);
    P16(txb + 60, q->seq);
    for (j = 0; j < len; j++)
        txb[62 + j] = (unsigned char)('a' + j % 23);
    q->t0 = ticks();
    if (icmp6_send(i, src, dst, 8 + len, u->ttl_out ? (unsigned char)u->ttl_out : 128) < 0) {
        q->used = 0;
        return FAPI_ERROR;
    }
    to = u->timeout / 10 + 1;
    while (!q->done) {
        unsigned w = ticks() - q->t0;
        if (w >= to)
            break;
        cli();
        if (!q->done)
            proc_block(0x100 + k, to - w);
        else
            sti();
    }
    if (!q->done) {
        q->used = 0;
        return FAPI_ERR_TIMEOUT;
    }
    u->rtt = (q->t1 - q->t0) * 10;
    u->ttl = q->ttl;
    u->type = q->type;
    u->code = q->code;
    cpy(u->from, q->from, 16);
    q->used = 0;
    return 0;
}
