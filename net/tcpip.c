/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - TCPIP.SYS: Netzwerkstapel (Ring 0), Stufe 1
 * Entwurf docs/NETZWERK.md, Schnittstelle include/net.h
 *
 * Stufe 1: Ethernet, ARP (RFC 826), IPv4 (RFC 791, ohne Optionen und
 * Zusammensetzen), ICMP Echo (RFC 792, beantworten und NET_PING), DHCP-
 * Client (RFC 2131/2132). TCP, UDP-Sockets, DNS und IPv6 folgen.
 *
 * Wo was laeuft: rx/done im IRQ der Karte (nur Ring verwalten), alles
 * andere im Nachlauf (run, aus dem Scheduler) oder in FAPI_NET (call, im
 * Zusammenhang des Programms, darf schlafen). Beide unterbrechen sich nie
 * gegenseitig, nur an Schlafpunkten von call (proc_block) laeuft run.
 *
 * Schalter: /IP:a.b.c.d/n /GW:a.b.c.d /DNS:a.b.c.d[,a.b.c.d] /HOST:name
 *           /NOFW (Firewall aus), /NOIPV6
 *           (/IP schaltet DHCP fuer die erste Karte ab)
 */
#include "../drivers/drvlib.h"
#include "../drivers/netlib.h"
#include "../include/fapi_abi.h"

typedef unsigned char __far *fp;

#define G16(p)      ((unsigned)(p)[0] << 8 | (p)[1])
#define P16(p, v)   ((p)[0] = (unsigned char)((v) >> 8), (p)[1] = (unsigned char)(v))

unsigned seglim(unsigned sel);
#pragma aux seglim = "xor cx,cx" "lsl cx,dx" __parm [__dx] __value [__cx];

/* ---- Empfangsring (Kernelsegment, IRQ schreibt, run liest) ------------- */
#define NSLOT   16
#define SLOT    1536
static unsigned rsel;
static volatile unsigned char rhead, rtail;
static unsigned rlen[NSLOT];
static unsigned char rifc[NSLOT];
static unsigned long rx_drop;

/* ---- Schnittstellen -------------------------------------------------- */
struct nif {
    unsigned char  up;
    unsigned char  poll;                /* Karte ohne IRQ: abfragen      */
    unsigned char  mac[6];
    unsigned char  ip[4], mask[4], gw[4], dns[2][4];
    unsigned char  fixed;               /* feste Adresse (/IP:)          */
    /* DHCP */
    unsigned char  dstate;              /* D_*                           */
    unsigned char  dsrv[4], doffer[4];
    unsigned long  xid;
    unsigned long  dnext;               /* naechster Versuch (Sekunden)  */
    unsigned       dtry;
    unsigned char  dwait;               /* Erneuern gefragt, ACK erwartet */
    unsigned long  lease, lstart;       /* Sekunden                      */
    char           domain[32];
    /* IPv6 (ip6.h): Zustaende 0 keine, 1 DAD laeuft, 2 gilt, 3 doppelt */
    unsigned char  ll[16], ga[16];      /* link-lokal, global (SLAAC)    */
    unsigned char  ua[16];              /* lokal (ULA fc00::/7, SLAAC)   */
    unsigned char  llst, gast, gplen, uast;
    unsigned long  lldad, gadad, uadad; /* Takt: DAD fertig              */
    unsigned long  gaexp, rtrexp, uaexp;/* Sekunden: gueltig bis         */
    unsigned char  rtr[16];             /* Standard-Router (link-lokal)  */
    unsigned char  dns6[16];            /* RDNSS aus dem RA              */
    unsigned char  rsn;                 /* Router Solicitations gesendet */
    unsigned long  rst;
};
#define D_OFF       0
#define D_DISCOVER  1
#define D_REQUEST   2
#define D_BOUND     3
static struct nif nif[NNETIF];

static unsigned char bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static char host[32] = "ATDOS";
static unsigned char cfg_ip[4], cfg_mask[4], cfg_gw[4], cfg_dns[2][4], cfg_fixed;

static unsigned char fw_on = 1;         /* Firewall (ab Werk an, /NOFW)  */
static unsigned char v6_on = 1;         /* IPv6 (/NOIPV6 schaltet ab)    */
static unsigned long now;               /* Takt (100 Hz)                 */
static unsigned long secs, sec_base;    /* Sekunden seit dem Laden       */
static unsigned ipid = 1;

/* ---- Senden ---------------------------------------------------------- */
static unsigned char txb[1536];
static struct net_pkt tp;

static int eth_send(int i, unsigned len)
{
    unsigned k, r;
    tp.ifc = i;
    tp.len = len;
    tp.off = (unsigned)txb;
    tp.sel = getds();
    for (k = 0; k < 2000; k++) {        /* alle Puffer belegt: kurz warten */
        r = devhlp(DH_NETSEND, 0, (unsigned)&tp, 0);
        if (r != NS_FULL)
            return r == NS_OK ? 0 : -1;
    }
    return -1;
}

static void cpy(void __far *d, const void __far *s, unsigned n)
{
    unsigned char __far *a = d;
    const unsigned char __far *b = s;
    while (n--)
        *a++ = *b++;
}

static int eq(const void __far *a, const void __far *b, unsigned n)
{
    const unsigned char __far *x = a, __far *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static void zero(void __far *d, unsigned n)
{
    unsigned char __far *a = d;
    while (n--)
        *a++ = 0;
}

static unsigned long csum_add(unsigned long s, const unsigned char __far *p, unsigned n)
{
    for (; n > 1; p += 2, n -= 2)
        s += (unsigned)p[0] << 8 | p[1];
    if (n)
        s += (unsigned)p[0] << 8;
    return s;
}

static unsigned csum(unsigned long s)
{
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return ~(unsigned)s;
}

/* Zufall (Haertung): ein 32-Bit-Speicher, gefuettert mit der Ankunftszeit
   und Laenge jedes Rahmens (IRQ) und Bytes aus den Kopfdaten; ausgegeben
   ueber xorshift. Nicht kryptographisch, aber von aussen nicht aus der
   Laufzeit abzulesen - reicht fuer ISN (RFC 6528) und Absenderports
   (RFC 6056), Multiplikationen gibt es hier nicht (sonst __U4M). */
static unsigned long rnd_s = 0x6D2B79F5UL;

static unsigned long rotl7(unsigned long x)
{
    return x << 7 | x >> 25;
}

static void rnd_mix(unsigned long v)
{
    rnd_s = rotl7(rnd_s ^ v) + 0x9E3779B9UL;
}

static unsigned long rnd32(void)
{
    unsigned long x = rnd_s ^ ticks();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rnd_s = x;
    return x;
}

static int is_zero4(const unsigned char __far *a)
{
    return !(a[0] | a[1] | a[2] | a[3]);
}

static int is_v4(const unsigned char __far *a)  /* ::ffff:a.b.c.d       */
{
    static const unsigned char v4[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
    return eq(a, v4, 12);
}

static void map4(unsigned char __far *d, const unsigned char __far *a)
{
    zero(d, 10);
    d[10] = d[11] = 0xFF;
    cpy(d + 12, a, 4);
}

static int is_zero12(const unsigned char __far *a)     /* ::x.x.x.x  */
{
    int k;
    for (k = 0; k < 12; k++)
        if (a[k])
            return 0;
    return 1;
}

static int is_zero16(const unsigned char __far *a)
{
    int k;
    for (k = 0; k < 16; k++)
        if (a[k])
            return 0;
    return 1;
}

/* Pruefsumme mit Pseudokopf, Adressen 16 Byte: IPv4 (RFC 793) nimmt die
   letzten 4, IPv6 (RFC 8200, 8.1) alle 16 - Laenge und Protokoll gehen
   bei beiden einfach in die Summe ein. */
static unsigned l4sum(const unsigned char __far *src, const unsigned char __far *dst,
                      unsigned char proto, const unsigned char __far *seg, unsigned len)
{
    unsigned n = is_v4(src) ? 4 : 16;
    unsigned long s = csum_add(0, src + 16 - n, n);
    s = csum_add(s, dst + 16 - n, n);
    s += proto;
    s += len;
    return csum(csum_add(s, seg, len));
}

/* Paket in Arbeit (ip4_in/ip6_in fuer UDP, TCP, ICMPv6) */
static unsigned char rx_s[16], rx_d[16];        /* Absender, Ziel        */
static unsigned char rx_hl;                     /* TTL / Hop-Limit       */

/* ---- Nachbarn: ARP (IPv4) und Neighbor Discovery (IPv6) --------------
   Ein Cache fuer beide, Schluessel ist die 16-Byte-Adresse (IPv4 als
   ::ffff:a.b.c.d). Unbekanntes Ziel: Rahmen parken (4 Plaetze) und fragen -
   ARP-Anfrage bzw. Neighbor Solicitation (nd_ask, ip6.h). */
#define NARP    16
static struct arp {
    unsigned char  ip[16], mac[6], ifc, state;  /* 0 frei, 1 fragt, 2 da */
    unsigned long  t, rq;               /* angelegt, letzte Frage        */
    unsigned       plen;                /* wartender Rahmen (0 = keiner) */
    unsigned char  pend[1514];
} arp[4];                               /* nur 4 mit Puffer             */
static struct { unsigned char ip[16], mac[6], ifc, state; unsigned long t; } arpc[NARP];

static void nd_ask(int i, const unsigned char __far *ip16);    /* ip6.h  */

static void arp_put(int i, const unsigned char __far *ip, const unsigned char __far *mac)
{
    int k, f = -1, o = 0;
    for (k = 0; k < NARP; k++) {
        if (arpc[k].state && arpc[k].ifc == i && eq(arpc[k].ip, ip, 16)) {
            f = k;
            break;
        }
        if (!arpc[k].state && f < 0)
            f = k;
        if (arpc[k].t < arpc[o].t)
            o = k;
    }
    if (f < 0)
        f = o;                          /* aeltesten ersetzen            */
    cpy(arpc[f].ip, ip, 16);
    cpy(arpc[f].mac, mac, 6);
    arpc[f].ifc = (unsigned char)i;
    arpc[f].state = 2;
    arpc[f].t = now;
    for (k = 0; k < 4; k++)             /* wartende Rahmen losschicken   */
        if (arp[k].state == 1 && arp[k].ifc == i && eq(arp[k].ip, ip, 16)) {
            arp[k].state = 0;
            if (arp[k].plen) {
                cpy(txb, arp[k].pend, arp[k].plen);
                cpy(txb, mac, 6);
                eth_send(i, arp[k].plen);
                arp[k].plen = 0;
            }
        }
}

/* bekannt oder gefragt? Dann auffrischen (Antworten, die niemand bestellt
   hat, legen nichts an) */
static void arp_upd(int i, const unsigned char __far *ip, const unsigned char __far *mac)
{
    int k;
    for (k = 0; k < NARP; k++)
        if (arpc[k].state && arpc[k].ifc == i && eq(arpc[k].ip, ip, 16)) {
            arp_put(i, ip, mac);
            return;
        }
    for (k = 0; k < 4; k++)
        if (arp[k].state == 1 && arp[k].ifc == i && eq(arp[k].ip, ip, 16)) {
            arp_put(i, ip, mac);
            return;
        }
}

static void arp_request(int i, const unsigned char __far *ip16)
{
    struct nif *n = &nif[i];
    if (!is_v4(ip16)) {
        nd_ask(i, ip16);
        return;
    }
    cpy(txb, bcast, 6);
    cpy(txb + 6, n->mac, 6);
    P16(txb + 12, 0x0806);
    P16(txb + 14, 1);
    P16(txb + 16, 0x0800);
    txb[18] = 6;
    txb[19] = 4;
    P16(txb + 20, 1);
    cpy(txb + 22, n->mac, 6);
    cpy(txb + 28, n->ip, 4);
    zero(txb + 32, 6);
    cpy(txb + 38, ip16 + 12, 4);
    eth_send(i, 42);
}

static void arp_in(int i, fp f, unsigned len)
{
    struct nif *n = &nif[i];
    unsigned op;
    unsigned char a16[16];
    if (len < 42 || G16(f + 14) != 1 || G16(f + 16) != 0x0800 || f[18] != 6 || f[19] != 4)
        return;
    op = G16(f + 20);
    map4(a16, f + 28);
    if (!is_zero4(n->ip) && eq(f + 38, n->ip, 4)) {
        arp_put(i, a16, f + 22);        /* wer fragt, wird gemerkt       */
        if (op == 1) {                  /* Antwort                        */
            cpy(txb, f + 22, 6);
            cpy(txb + 6, n->mac, 6);
            P16(txb + 12, 0x0806);
            cpy(txb + 14, f + 14, 6);
            P16(txb + 20, 2);
            cpy(txb + 22, n->mac, 6);
            cpy(txb + 28, n->ip, 4);
            cpy(txb + 32, f + 22, 6);
            cpy(txb + 38, f + 28, 4);
            eth_send(i, 42);
        }
    } else if (op == 2)
        arp_upd(i, a16, f + 22);
}

/* Ziel-MAC fuer ip16 in txb eintragen; unbekannt -> Rahmen parken, fragen.
   -> 0 gesendet, 1 geparkt, -1 kein Platz */
static int arp_send(int i, const unsigned char __far *ip, unsigned len)
{
    int k, f = -1;
    for (k = 0; k < NARP; k++)
        if (arpc[k].state == 2 && arpc[k].ifc == i && eq(arpc[k].ip, ip, 16)) {
            cpy(txb, arpc[k].mac, 6);
            return eth_send(i, len);
        }
    for (k = 0; k < 4; k++)
        if (arp[k].state == 1 && arp[k].ifc == i && eq(arp[k].ip, ip, 16)) {
            f = k;                      /* fragt schon: Rahmen ersetzen  */
            break;
        } else if (!arp[k].state && f < 0)
            f = k;
    if (f < 0)
        return -1;
    arp[f].state = 1;
    arp[f].ifc = (unsigned char)i;
    cpy(arp[f].ip, ip, 16);
    arp[f].t = arp[f].rq = now;
    arp[f].plen = len;
    cpy(arp[f].pend, txb, len);
    arp_request(i, arp[f].ip);
    return 1;
}

static void arp_tick(void)
{
    int k;
    for (k = 0; k < 4; k++)             /* 1 s ohne Antwort: nochmal, 3 s: aus */
        if (arp[k].state == 1) {
            if (now - arp[k].t > 300) {
                arp[k].state = 0;
                arp[k].plen = 0;
            } else if (now - arp[k].rq >= 100) {
                arp[k].rq = now;
                arp_request(arp[k].ifc, arp[k].ip);
            }
        }
    for (k = 0; k < NARP; k++)          /* 10 min alt: vergessen         */
        if (arpc[k].state && now - arpc[k].t > 60000UL)
            arpc[k].state = 0;
}

/* ---- IPv4 ------------------------------------------------------------ */
static int same_net(struct nif *n, const unsigned char __far *a)
{
    int k;
    for (k = 0; k < 4; k++)
        if ((a[k] & n->mask[k]) != (n->ip[k] & n->mask[k]))
            return 0;
    return 1;
}

#define IS_LL(a)    ((a)[0] == 0xFE && ((a)[1] & 0xC0) == 0x80)
#define IS_MC(a)    ((a)[0] == 0xFF)

/* IPv6-Ziel a direkt an Karte i (link-lokal oder eigenes Praefix)? */
static int v6_onlink(int i, const unsigned char __far *a)
{
    return IS_LL(a) || (nif[i].gast == 2 && eq(a, nif[i].ga, 8)) ||
           (nif[i].uast == 2 && eq(a, nif[i].ua, 8));
}

#include "fw.h"                         /* Firewall-Regeln               */

/* Karte und naechsten Halt fuer Ziel a -> Karte oder -1 */
static int route4(const unsigned char __far *a, unsigned char __far *hop)
{
    int i;
    for (i = 0; i < NNETIF; i++)
        if (nif[i].up && !is_zero4(nif[i].ip) && same_net(&nif[i], a)) {
            cpy(hop, a, 4);
            return i;
        }
    for (i = 0; i < NNETIF; i++)
        if (nif[i].up && !is_zero4(nif[i].ip) && !is_zero4(nif[i].gw)) {
            cpy(hop, nif[i].gw, 4);
            return i;
        }
    return -1;
}

/* IPv4-Kopf in txb (Nutzdaten ab 34 liegen schon), senden.
   dst 255.255.255.255 -> Rundruf ohne ARP. -> wie arp_send */
static int ip4_out(int i, const unsigned char __far *dst, unsigned char proto,
                   unsigned dlen, unsigned char ttl)
{
    unsigned char hop[16];
    struct nif *n = &nif[i];
    txb[14] = 0x45;
    txb[15] = 0;
    P16(txb + 16, 20 + dlen);
    P16(txb + 18, ipid);
    ipid++;
    P16(txb + 20, 0x4000);              /* nicht zerlegen                */
    txb[22] = ttl;
    txb[23] = proto;
    P16(txb + 24, 0);
    cpy(txb + 26, n->ip, 4);
    cpy(txb + 30, dst, 4);
    P16(txb + 24, csum(csum_add(0, txb + 14, 20)));
    cpy(txb + 6, n->mac, 6);
    P16(txb + 12, 0x0800);
    if (dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255) {
        cpy(txb, bcast, 6);
        return eth_send(i, 34 + dlen);
    }
    map4(hop, same_net(n, dst) ? dst : n->gw);
    return arp_send(i, hop, 34 + dlen);
}

/* ---- ICMP und PING --------------------------------------------------- */
#define NPING   4
static struct ping {
    unsigned char  used, done;
    unsigned       id, seq;
    unsigned       t0, t1;              /* Takt, untere 16 Bit           */
    unsigned char  from[16], ttl, type, code;
} pg[NPING];

static void icmp_in(int i, fp f, unsigned len, unsigned hl)
{
    fp m = f + 14 + hl;
    unsigned ml = G16(f + 16) - hl, k, id, seq;
    if (ml + 14 + hl > len || ml < 8 || csum(csum_add(0, m, ml)))
        return;
    if (m[0] == 8 && ml <= sizeof(txb) - 34) {  /* Echo-Anfrage: antworten,
                                       mit Firewall nur im eigenen Netz  */
        unsigned char src[16];
        if (f[30] == 255 && f[31] == 255 && f[32] == 255 && f[33] == 255)
            return;                     /* kein Echo auf Rundruf           */
        map4(src, f + 26);
        if (fw_on && !same_net(&nif[i], f + 26) &&
            !fw_allow("", FWP_ICMP, 0, src, 0, i))
            return;
        cpy(txb + 34, m, ml);
        txb[34] = 0;
        P16(txb + 36, 0);
        P16(txb + 36, csum(csum_add(0, txb + 34, ml)));
        ip4_out(i, src + 12, 1, ml, 64);
        return;
    }
    if (m[0] == 0) {                    /* Echo-Antwort                  */
        id = G16(m + 4);
        seq = G16(m + 6);
    } else if ((m[0] == 3 || m[0] == 11) && ml >= 36 && m[8 + 9] == 1 &&
               m[8 + 20] == 8) {        /* Fehler zu unserem Echo        */
        unsigned ihl = (m[8] & 15) * 4;
        if (ml < 8 + ihl + 8)
            return;
        id = G16(m + 8 + ihl + 4);
        seq = G16(m + 8 + ihl + 6);
    } else
        return;
    for (k = 0; k < NPING; k++)
        if (pg[k].used && !pg[k].done && pg[k].id == id && pg[k].seq == seq) {
            pg[k].done = 1;
            pg[k].t1 = (unsigned)now;
            map4(pg[k].from, f + 26);
            pg[k].ttl = f[22];
            pg[k].type = m[0];
            pg[k].code = m[1];
            proc_run(0x100 + k);
        }
}

/* ---- DHCP ------------------------------------------------------------ */
static void dhcp_send(int i, unsigned char type)
{
    struct nif *n = &nif[i];
    unsigned char *d = txb + 42, *o;
    unsigned ln;
    static const unsigned char any[4] = { 255, 255, 255, 255 };

    zero(d, 300);
    d[0] = 1;                           /* BOOTREQUEST                   */
    d[1] = 1;
    d[2] = 6;
    cpy(d + 4, &n->xid, 4);
    P16(d + 10, 0x8000);                /* Antwort als Rundruf           */
    if (n->dstate == D_BOUND)
        cpy(d + 12, n->ip, 4);          /* ciaddr beim Erneuern          */
    cpy(d + 28, n->mac, 6);
    d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
    o = d + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; cpy(o, n->mac, 6); o += 6;
    if (type == 3 && n->dstate == D_REQUEST) {
        *o++ = 50; *o++ = 4; cpy(o, n->doffer, 4); o += 4;
        *o++ = 54; *o++ = 4; cpy(o, n->dsrv, 4); o += 4;
    }
    for (ln = 0; host[ln] && ln < 31; ln++)
        ;
    *o++ = 12; *o++ = (unsigned char)ln; cpy(o, host, ln); o += ln;
    *o++ = 55; *o++ = 5; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 51;
    *o++ = 255;
    ln = (unsigned)(o - d);
    if (ln < 300)
        ln = 300;
    P16(txb + 34, 68);
    P16(txb + 36, 67);
    P16(txb + 38, 8 + ln);
    P16(txb + 40, 0);                   /* ohne UDP-Pruefsumme (erlaubt) */
    if (type == 7 || (n->dstate == D_BOUND && type == 3))
        ip4_out(i, n->dsrv, 17, 8 + ln, 64);    /* freigeben/erneuern direkt */
    else {
        unsigned char ip0[4];
        cpy(ip0, n->ip, 4);
        zero(n->ip, 4);                 /* Absender 0.0.0.0              */
        ip4_out(i, any, 17, 8 + ln, 64);
        cpy(n->ip, ip0, 4);
    }
}

static void dhcp_start(int i)
{
    struct nif *n = &nif[i];
    n->xid = rnd32();                   /* nicht erratbar                 */
    n->dwait = 0;
    n->dstate = D_DISCOVER;
    n->dtry = 0;
    n->dnext = secs;
}

static int dhcp_sane(const unsigned char __far *a)    /* angebotene Adresse */
{
    return a[0] && a[0] != 127 && a[0] < 224;
}

static void dhcp_in(int i, fp d, unsigned dl)
{
    struct nif *n = &nif[i];
    unsigned char type = 0, mask[4], gw[4], dns[2][4], srv[4], ndns = 0;
    unsigned long lease = 86400UL;
    fp o, e;
    unsigned k;

    if (dl < 240 || d[0] != 2 || !eq(d + 4, &n->xid, 4) || !eq(d + 28, n->mac, 6) ||
        d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
        return;
    zero(mask, 4); zero(gw, 4); zero(dns, 8); zero(srv, 4);
    n->domain[0] = 0;
    for (o = d + 240, e = d + dl; o < e && *o != 255; ) {
        unsigned c = *o, l;
        if (c == 0) { o++; continue; }
        if (o + 2 > e || o + 2 + o[1] > e)
            break;
        l = o[1];
        if (c == 53 && l >= 1) type = o[2];
        if (c == 1 && l >= 4) cpy(mask, o + 2, 4);
        if (c == 3 && l >= 4) cpy(gw, o + 2, 4);
        if (c == 6)
            for (k = 0; k + 4 <= l && ndns < 2; k += 4)
                cpy(dns[ndns++], o + 2 + k, 4);
        if (c == 54 && l >= 4) cpy(srv, o + 2, 4);
        if (c == 51 && l >= 4)
            lease = (unsigned long)G16(o + 2) << 16 | G16(o + 4);
        if (c == 15) {
            for (k = 0; k < l && k < 31; k++)
                n->domain[k] = (char)o[2 + k];
            n->domain[k] = 0;
        }
        o += 2 + l;
    }
    if (type == 2 && n->dstate == D_DISCOVER && dhcp_sane(d + 16)) {   /* OFFER */
        cpy(n->doffer, d + 16, 4);
        cpy(n->dsrv, srv, 4);
        n->dstate = D_REQUEST;
        n->dtry = 0;
        dhcp_send(i, 3);
        n->dnext = secs + 2;
    } else if (type == 5 && (n->dstate == D_REQUEST || (n->dstate == D_BOUND && n->dwait)) &&
               dhcp_sane(d + 16) && (n->dstate == D_REQUEST || is_zero4(srv) ||
                                     eq(srv, n->dsrv, 4))) {
        n->dwait = 0;                   /* nur ACK auf eigene Anfrage     */
        cpy(n->ip, d + 16, 4);          /* ACK                           */
        cpy(n->mask, mask, 4);
        cpy(n->gw, gw, 4);
        cpy(n->dns, is_zero4(cfg_dns[0]) ? dns[0] : cfg_dns[0], 8); /* /DNS geht vor */
        if (!is_zero4(srv))
            cpy(n->dsrv, srv, 4);
        n->lease = lease;
        n->lstart = secs;
        n->dstate = D_BOUND;
        n->dnext = secs + (lease >> 1); /* T1: erneuern                  */
        n->dtry = 0;
    } else if (type == 6 && (n->dstate == D_REQUEST || n->dwait)) {   /* NAK */
        n->dwait = 0;
        zero(n->ip, 4);
        dhcp_start(i);
    }
}

static void dhcp_tick(int i)
{
    struct nif *n = &nif[i];
    if (n->dstate == D_OFF || secs < n->dnext)
        return;
    if (n->dstate == D_BOUND) {
        if (secs - n->lstart >= n->lease) {     /* abgelaufen            */
            zero(n->ip, 4);
            dhcp_start(i);
            return;
        }
        n->xid = rnd32();               /* neue Kennung je Anfrage        */
        n->dwait = 1;
        dhcp_send(i, 3);                /* erneuern                      */
        n->dnext = secs + 60;
        return;
    }
    if (n->dstate == D_REQUEST && ++n->dtry > 3) {
        dhcp_start(i);                  /* kein ACK: wieder suchen       */
        return;
    }
    dhcp_send(i, n->dstate == D_DISCOVER ? 1 : 3);
    n->dtry += n->dstate == D_DISCOVER;
    n->dnext = secs + (n->dtry < 4 ? 1u << n->dtry : 16u);
}

#include "ip6.h"                        /* IPv6: ND, SLAAC, ICMPv6       */
#include "sock.h"                       /* Sockets, UDP, DNS (Stufe 2a)  */

/* ---- Eingang --------------------------------------------------------- */
static void ip4_in(int i, fp f, unsigned len)
{
    struct nif *n = &nif[i];
    unsigned hl = (f[14] & 15) * 4, tl;
    if (len < 34 || (f[14] >> 4) != 4 || hl < 20 || len < 14 + hl)
        return;
    tl = G16(f + 16);
    if (tl < hl || 14 + tl > len || csum(csum_add(0, f + 14, hl)))
        return;
    if (G16(f + 20) & 0x3FFF)           /* Bruchstueck: noch nicht       */
        return;
    if (hl != 20)                       /* IP-Optionen (Source-Route ...): */
        return;                         /* gar nicht erst ansehen          */
    if (f[26] == 0 || f[26] == 127 || f[26] >= 224 || eq(f + 26, n->ip, 4))
        return;                         /* unsinniger Absender             */
    rnd_mix((unsigned long)G16(f + 18) << 16 | G16(f + 24));
    if (!(eq(f + 30, n->ip, 4) && !is_zero4(n->ip)) &&
        !(f[30] == 255 && f[31] == 255 && f[32] == 255 && f[33] == 255) &&
        n->dstate != D_DISCOVER && n->dstate != D_REQUEST)
        return;
    if (!eq(f + 30, n->ip, 4) || is_zero4(n->ip)) {
        if (f[23] != 17)                /* Rundruf/DHCP-Phase: nur UDP     */
            return;
    }
    if (f[23] == 1) {
        icmp_in(i, f, len, hl);
        return;
    }
    map4(rx_s, f + 26);
    map4(rx_d, f + 30);
    rx_hl = f[22];
    if (f[23] == 17 && tl >= hl + 8)
        udp_in(i, f + 14 + hl, tl - hl);
    else if (f[23] == 6)
        tcp_in(i, f + 14 + hl, tl - hl);
}

/* IPv6 (RFC 8200): Hop-by-Hop- und Zieloptionen werden uebersprungen,
   Routing- und Fragmentkoepfe verworfen (zerlegt wird nur beim Absender,
   Mindest-MTU 1280). Angenommen wird an die eigenen gueltigen Adressen,
   an alle Knoten (ff02::1) und an die eigene Solicited-Node-Gruppe. */
static void ip6_in(int i, fp f, unsigned len)
{
    struct nif *n = &nif[i];
    unsigned pl, o = 54, e;
    unsigned char nh;
    fp d = f + 38;
    if (!v6_on || len < 54 || (f[14] >> 4) != 6)
        return;
    pl = G16(f + 18);
    if (54 + pl > len)
        return;
    e = 54 + pl;
    if (IS_MC(f + 22) || eq(f + 22, d, 16) || is_v4(f + 22) || is_v4(d) ||
        (is_zero12(f + 22) && !is_zero16(f + 22)))
        return;                         /* Absender Multicast, ::1, ::a.b.c.d,
                                           ::ffff:a.b.c.d (taeuschte IPv4
                                           vor) oder gleich dem Ziel      */
    if (!((n->llst == 2 && eq(d, n->ll, 16)) || (n->gast == 2 && eq(d, n->ga, 16)) ||
          (n->uast == 2 && eq(d, n->ua, 16)) ||
          eq(d, allnodes, 16) ||
          (eq(d, allnodes, 11) && d[11] == 1 && d[12] == 0xFF && eq(d + 13, n->ll + 13, 3))))
        return;
    nh = f[20];
    while (nh == 0 || nh == 60) {       /* Optionskoepfe ueberspringen    */
        if (o + 8 > e)
            return;
        nh = f[o];
        o += (f[o + 1] + 1) * 8;
    }
    if (o > e)
        return;
    cpy(rx_s, f + 22, 16);
    cpy(rx_d, d, 16);
    rx_hl = f[21];
    rnd_mix((unsigned long)G16(f + 16) << 16 | G16(f + 52));
    if (nh != 58 && is_zero16(rx_s))
        return;                         /* von :: nur ND (DAD)             */
    if (nh == 6 && IS_MC(d))
        return;                         /* TCP an eine Gruppe: unsinnig    */
    if (nh == 58)
        icmp6_in(i, f + o, e - o);
    else if (nh == 17 && e - o >= 8)
        udp_in(i, f + o, e - o);
    else if (nh == 6)
        tcp_in(i, f + o, e - o);
}

static void frame_in(int i, fp f, unsigned len)
{
    unsigned t;
    if (i >= NNETIF || !nif[i].up || len < 14)
        return;
    if (eq(f + 6, nif[i].mac, 6))
        return;                         /* eigener Rahmen zurueckgespiegelt
                                           (sonst haelt DAD die eigene NS
                                           fuer einen Widerspruch, RFC 7527) */
    t = G16(f + 12);
    if (t == 0x0806)
        arp_in(i, f, len);
    else if (t == 0x0800)
        ip4_in(i, f, len);
    else if (t == 0x86DD)
        ip6_in(i, f, len);
}

/* ---- Einspruenge vom Kernel ------------------------------------------ */
static unsigned e_rx(void __far *v)     /* IRQ: Platz vergeben           */
{
    struct net_pkt __far *p = v;
    unsigned char h = rhead;
    if (p->len > SLOT || (unsigned char)((h + 1) % NSLOT) == rtail) {
        rx_drop++;
        return 0;
    }
    p->sel = rsel;
    p->off = h * SLOT;
    return 0;
}

static unsigned e_done(void __far *v)   /* IRQ: Rahmen liegt da          */
{
    struct net_pkt __far *p = v;
    unsigned char h = rhead;
    if (p->sel != rsel || p->off != h * SLOT)
        return 1;
    rlen[h] = p->len;
    rifc[h] = (unsigned char)p->ifc;
    rnd_mix((unsigned long)ticks() << 16 | p->len);
    rhead = (unsigned char)((h + 1) % NSLOT);
    return 0;
}

static struct net_reg ri;
static struct net_ctl nc;

static unsigned e_run(void __far *v)    /* Nachlauf                      */
{
    struct net_run __far *r = v;
    int i;
    now = r->ticks;
    while (now - sec_base >= 100) {
        sec_base += 100;
        secs++;
    }
    for (i = 0; i < NNETIF; i++)
        if (nif[i].up && nif[i].poll) {
            nc.ifc = i;
            nc.cmd = NC_POLL;
            devhlp(DH_NETCTL, 0, (unsigned)&nc, 0);
        }
    while (rtail != rhead) {
        unsigned char t = rtail;
        frame_in(rifc[t], (fp)MK_FP(rsel, t * SLOT), rlen[t]);
        rtail = (unsigned char)((t + 1) % NSLOT);
    }
    arp_tick();
    tcp_tick();
    for (i = 0; i < NNETIF; i++)
        if (nif[i].up) {
            dhcp_tick(i);
            nd_tick(i);
        }
    return 1;                           /* je Takt wieder                */
}


static unsigned e_link(void __far *v)   /* neue Karte                    */
{
    struct net_pkt __far *p = v;
    int i = p->ifc;
    struct nif *n;
    if (i >= NNETIF || devhlp(DH_NETINFO, 0, i, (unsigned)&ri))
        return 1;
    n = &nif[i];
    zero(n, sizeof(*n));
    cpy(n->mac, ri.mac, 6);
    n->poll = ri.irq == 0;
    nc.ifc = i;                         /* eigene Adresse + Rundruf, mit */
    nc.cmd = NC_MCAST;                  /* IPv6 alle Knoten und die       */
    nc.len = 0;                         /* Solicited-Node-Gruppe          */
    nc.sel = getds();
    nc.off = (unsigned)txb;
    if (v6_on) {
        static const unsigned char mc[12] = { 0x33, 0x33, 0, 0, 0, 1, 0x33, 0x33, 0xFF };
        cpy(txb, mc, 12);
        cpy(txb + 9, n->mac + 3, 3);    /* EUI-64: dieselben 3 Byte       */
        nc.len = 12;
    }
    devhlp(DH_NETCTL, 0, (unsigned)&nc, 0);
    n->up = 1;
    if (v6_on)
        v6_start(i);
    if (cfg_fixed && i == 0) {
        cpy(n->ip, cfg_ip, 4);
        cpy(n->mask, cfg_mask, 4);
        cpy(n->gw, cfg_gw, 4);
        cpy(n->dns, cfg_dns, 8);
        n->fixed = 1;
    } else
        dhcp_start(i);
    devhlp(DH_NETKICK, 0, 0, 0);
    return 0;
}

/* Zeiger des Programms pruefen (Grenze des Selektors) */
static void __far *uptr(unsigned sel, unsigned off, unsigned n)
{
    unsigned l = seglim(sel);
    if (!l || n == 0 || off > l || l - off < n - 1)
        return 0;
    return MK_FP(sel, off);
}


static unsigned net_ifinfo(struct net_call __far *c)
{
    struct fapi_netif __far *u = uptr(c->usel, c->p, sizeof(struct fapi_netif));
    struct nif *n;
    unsigned i, k;
    static struct net_stats ss;
    if (!u || c->n < sizeof(struct fapi_netif))
        return FAPI_ERROR;
    i = u->ifc;
    if (i >= NNETIF || !nif[i].up || devhlp(DH_NETINFO, 0, i, (unsigned)&ri))
        return FAPI_ERROR;
    n = &nif[i];
    zero(u, sizeof(*u));
    u->ifc = i;
    cpy(u->name, ri.name, 12);
    cpy(u->mac, ri.mac, 6);
    u->mtu = ri.mtu;
    u->irq = ri.irq;
    u->io = ri.io;
    nc.ifc = i;
    nc.cmd = NC_LINK;
    nc.result = 0;
    devhlp(DH_NETCTL, 0, (unsigned)&nc, 0);
    u->link = nc.result;
    if (!is_zero4(n->ip)) {
        map4(u->addr[0].a, n->ip);
        for (k = 0; k < 32 && (n->mask[k >> 3] & (0x80 >> (k & 7))); k++)
            ;
        u->addr[0].plen = (unsigned char)(96 + k);
        u->addr[0].kind = n->fixed ? NA_STATIC : NA_DHCP;
        u->naddr = 1;
    }
    if (n->gast == 2) {
        cpy(u->addr[u->naddr].a, n->ga, 16);
        u->addr[u->naddr].plen = n->gplen;
        u->addr[u->naddr++].kind = NA_SLAAC;
    }
    if (n->uast == 2 && u->naddr < NET_MAXADDR - 1) {
        cpy(u->addr[u->naddr].a, n->ua, 16);
        u->addr[u->naddr].plen = 64;
        u->addr[u->naddr++].kind = NA_SLAAC;
    }
    if (n->llst == 2) {
        cpy(u->addr[u->naddr].a, n->ll, 16);
        u->addr[u->naddr].plen = 64;
        u->addr[u->naddr++].kind = NA_LINK;
    }
    if (!is_zero16(n->rtr))
        cpy(u->gw6, n->rtr, 16);
    if (!is_zero4(n->gw))
        map4(u->gw4, n->gw);
    for (k = 0; k < 2; k++)
        if (!is_zero4(n->dns[k]))
            map4(u->dns[k], n->dns[k]);
    if (!is_zero16(n->dns6))            /* IPv6-DNS vor einem zweiten v4  */
        cpy(u->dns[is_zero16(u->dns[0]) ? 0 : 1], n->dns6, 16);
    if (n->dstate == D_BOUND) {
        map4(u->dhcp, n->dsrv);
        u->lease = n->lease - (secs - n->lstart);
    }
    nc.cmd = NC_STATS;
    nc.sel = getds();
    nc.off = (unsigned)&ss;
    nc.len = sizeof(ss);
    zero(&ss, sizeof(ss));
    devhlp(DH_NETCTL, 0, (unsigned)&nc, 0);
    u->rx_ok = ss.rx_ok;
    u->tx_ok = ss.tx_ok;
    u->rx_err = ss.rx_err;
    u->tx_err = ss.tx_err;
    u->rx_drop = ss.rx_drop + rx_drop;
    cpy(u->host, host, 32);
    cpy(u->domain, n->domain, 32);
    return 0;
}

static unsigned net_ping(struct net_call __far *c)
{
    struct fapi_ping __far *u = uptr(c->usel, c->p, sizeof(struct fapi_ping));
    unsigned char dst[4], hop[4];
    unsigned k, len, j, to;
    int i;
    struct ping *q;
    static const unsigned char v4[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };

    if (!u || c->n < sizeof(struct fapi_ping))
        return FAPI_ERROR;
    len = u->len;
    if (!eq(u->addr, v4, 12))           /* IPv6: Echo ueber ICMPv6        */
        return ping6(c, u);
    cpy(dst, u->addr + 12, 4);
    if (len > 1472)
        return FAPI_ERROR;
    if ((i = route4(dst, hop)) < 0)
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
    txb[34] = 8;
    txb[35] = 0;
    P16(txb + 36, 0);
    P16(txb + 38, q->id);
    P16(txb + 40, q->seq);
    for (j = 0; j < len; j++)
        txb[42 + j] = (unsigned char)('a' + j % 23);    /* wie Windows   */
    P16(txb + 36, csum(csum_add(0, txb + 34, 8 + len)));
    q->t0 = ticks();
    if (ip4_out(i, dst, 1, 8 + len, u->ttl_out ? (unsigned char)u->ttl_out : 128) < 0) {
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

static unsigned net_dhcp(struct net_call __far *c)
{
    struct fapi_netcmd __far *u = uptr(c->usel, c->p, sizeof(struct fapi_netcmd));
    struct nif *n;
    if (!u || u->ifc >= NNETIF || !nif[u->ifc].up)
        return FAPI_ERROR;
    n = &nif[u->ifc];
    if (n->fixed)
        return FAPI_ERR_DENIED;
    if (u->arg == 1) {                  /* freigeben                     */
        if (n->dstate == D_BOUND)
            dhcp_send(u->ifc, 7);
        zero(n->ip, 4);
        zero(n->gw, 4);
        n->dstate = D_OFF;
        return 0;
    }
    zero(n->ip, 4);
    dhcp_start(u->ifc);
    devhlp(DH_NETKICK, 0, 0, 0);
    return 0;
}

/* Rechnername setzen (HOSTNAME.EXE); DHCP meldet ihn beim naechsten
   Erneuern (Option 12) - gebunden: gleich erneuern */
static unsigned net_hostname(struct net_call __far *c)
{
    struct fapi_hostname __far *u = uptr(c->usel, c->p, sizeof(struct fapi_hostname));
    unsigned k;
    char ch;
    if (!u)
        return FAPI_ERROR;
    for (k = 0; k < 32 && u->name[k]; k++) {
        ch = u->name[k];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              (ch == '-' && k)))
            return FAPI_ERROR;
    }
    if (k == 0 || k > 31)
        return FAPI_ERROR;
    for (k = 0; u->name[k]; k++)
        host[k] = u->name[k];
    host[k] = 0;
    for (k = 0; k < NNETIF; k++)
        if (nif[k].up && nif[k].dstate == D_BOUND)
            nif[k].dnext = secs;
    devhlp(DH_NETKICK, 0, 0, 0);
    return 0;
}

/* Nachbar-Cache lesen (ARP -a) */
static unsigned net_neigh(struct net_call __far *c)
{
    struct fapi_neigh __far *u = uptr(c->usel, c->p, sizeof(struct fapi_neigh));
    unsigned i;
    if (!u || c->n < sizeof(struct fapi_neigh) || u->idx >= NARP)
        return FAPI_ERROR;
    i = u->idx;
    zero((unsigned char __far *)u + 2, sizeof(struct fapi_neigh) - 2);
    if (!arpc[i].state)
        return 0;
    cpy(u->ip, arpc[i].ip, 16);
    cpy(u->mac, arpc[i].mac, 6);
    u->ifc = arpc[i].ifc;
    u->state = arpc[i].state;
    {
        unsigned long d = now - arpc[i].t;  /* Takte -> s ohne __U4D    */
        unsigned q = 0;
        while (d >= 6400UL && q < 65000u) {
            d -= 6400UL;
            q += 64;
        }
        u->age = q + (unsigned)d / 100;
    }
    return 0;
}

static unsigned e_call(void __far *v)   /* FAPI_NET                      */
{
    struct net_call __far *c = v;
    switch (c->fn) {
    case NET_IFINFO:
        return net_ifinfo(c);
    case NET_PING:
        return net_ping(c);
    case NET_DHCP:
        return net_dhcp(c);
    case NET_RESOLVE:
        return net_resolve(c);
    case NET_FWCTL:
        return fw_ctl(c);
    case NET_SOCKLIST:
        return sk_list(c);
    case NET_HOSTNAME:
        return net_hostname(c);
    case NET_NEIGH:
        return net_neigh(c);
    case NET_SELECT:
        return sk_select(c);
    }
    if (c->fn >= NET_SOCKET)
        return sk_call(c);
    return FAPI_ERROR;
}

/* ---- Laden ----------------------------------------------------------- */
#pragma code_seg("ITEXT", "ICODE")   /* nur beim INIT */
static const char __far *opt(const char __far *a, const char *k)
{
    int i;
    for (; *a; a++) {
        for (i = 0; k[i] && (a[i] & 0xDF) == (k[i] & 0xDF); i++)
            ;
        if (!k[i])
            return a + i;
    }
    return 0;
}
#pragma code_seg("_TEXT", "CODE")

static const char __far *ip_parse(const char __far *a, unsigned char __far *ip)
{
    unsigned k, v;
    for (k = 0; k < 4; k++) {
        if (*a < '0' || *a > '9')
            return 0;
        for (v = 0; *a >= '0' && *a <= '9'; a++)
            v = v * 10 + (*a - '0');
        if (v > 255)
            return 0;
        ip[k] = (unsigned char)v;
        if (k < 3 && *a++ != '.')
            return 0;
    }
    return a;
}

static struct net_proto pr;
static char line[96];

#pragma code_seg("ITEXT", "ICODE")   /* nur beim INIT */
static void init(void)
{
    struct drv_req __far *r = REQ;
    const char __far *a;
    unsigned k, b;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    if ((a = opt(r->args, "/IP:")) != 0) {
        if (!(a = ip_parse(a, cfg_ip))) {
            dputs("  TCPIP.SYS: /IP:a.b.c.d/n erwartet\n");
            return;
        }
        b = 24;
        if (*a == '/')
            for (a++, b = 0; *a >= '0' && *a <= '9'; a++)
                b = b * 10 + (*a - '0');
        for (k = 0; k < 32 && k < b; k++)
            cfg_mask[k >> 3] |= (unsigned char)(0x80 >> (k & 7));
        cfg_fixed = 1;
    }
    if ((a = opt(r->args, "/GW:")) != 0)
        ip_parse(a, cfg_gw);
    if ((a = opt(r->args, "/DNS:")) != 0 && (a = ip_parse(a, cfg_dns[0])) != 0 && *a == ',')
        ip_parse(a + 1, cfg_dns[1]);
    if ((a = opt(r->args, "/HOST:")) != 0) {
        for (k = 0; k < 31 && a[k] > ' '; k++)
            host[k] = a[k];
        host[k] = 0;
    }
    if (has_opt(r->args, "/NOFW"))
        fw_on = 0;
    if (has_opt(r->args, "/NOIPV6"))
        v6_on = 0;
    rsel = devhlp(DH_KSEG, 0, NSLOT * SLOT, 0);
    psel = devhlp(DH_KSEG, 0, 0, 0);    /* 64 KB: Socketpuffer           */
    if (!rsel || !psel) {
        dputs("  TCPIP.SYS: kein Speicher f\x81r den Empfangsring\n");
        return;
    }
    pr.version = 1;
    pr.size = sizeof(pr);
    pr.rx_off = fent(0, e_rx);
    pr.done_off = fent(1, e_done);
    pr.run_off = fent(2, e_run);
    pr.call_off = fent(3, e_call);
    pr.link_off = fent(4, e_link);
    now = ticks();
    sec_base = now;
    p = scat(line, v6_on ? "  TCP/IP: IPv4 und IPv6, " : "  TCP/IP: IPv4, ");
    if (cfg_fixed) {
        p = scat(p, "Adresse ");
        for (k = 0; k < 4; k++) {
            p = udec(p, cfg_ip[k]);
            if (k < 3)
                *p++ = '.';
        }
    } else
        p = scat(p, "Adresse per DHCP");
    p = scat(p, ", Rechner ");
    p = scat(p, host);
    p = scat(p, fw_on ? ", Firewall an" : ", Firewall aus");
    scat(p, "\n");
    dputs(line);
    if (devhlp(DH_NETPROTO, 0, (unsigned)&pr, 0)) {
        dputs("  TCPIP.SYS: es ist schon ein Netzwerkstapel geladen\n");
        return;
    }
    r->status = DRV_OK;
}
#pragma code_seg("_TEXT", "CODE")

void strategy(void)
{
    struct drv_req __far *r = REQ;
    int k;

    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: nur ohne Wartende */
        if (sk_any() || dns_busy) {
            r->status = DRV_ERR_IO;
            return;
        }
        for (k = 0; k < NPING; k++)
            if (pg[k].used) {
                r->status = DRV_ERR_IO;
                return;
            }
        for (k = 0; k < NNETIF; k++)
            if (nif[k].dstate == D_BOUND)
                dhcp_send(k, 7);        /* Adresse zurueckgeben          */
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
