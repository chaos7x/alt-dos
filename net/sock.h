/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TCPIP.SYS, Stufe 2a: Sockets (Puffer, UDP), Namensaufloesung (DNS),
 * Firewall-Grundregeln. Wird von tcpip.c eingebunden (gemeinsame Daten).
 *
 * Adressen sind innen 16 Byte (IPv4 als ::ffff:a.b.c.d). Jeder Socket hat
 * einen Empfangsring von 4 KB aus einem Kernelsegment (16 Bloecke).
 * Ein Datagramm im Ring: Laenge (2), Absender (16), Port (2), Daten;
 * passt es nicht mehr ans Ende, steht dort FFFFh und es geht vorn weiter.
 *
 * Firewall (docs/NETZWERK.md, ab Werk an): eingehende Datagramme nur fuer
 * verbundene Sockets von ihrer Gegenseite oder von Adressen, an die der
 * Socket in den letzten 2 Minuten gesendet hat. Geschlossene Ports
 * schweigen (keine ICMP-Antwort). Regeln je Programm folgen mit TCP.
 */

static void __far *uptr(unsigned sel, unsigned off, unsigned n);
static const char __far *ip_parse(const char __far *a, unsigned char __far *ip);

/* ---- Pufferbloecke ---------------------------------------------------- */
#define NBLK    16
#define BLK     4096u
static unsigned psel;
static unsigned blk_used;
#define BP(k)   ((unsigned char __far *)MK_FP(psel, (unsigned)(k) * BLK))

static int blk_get(void)
{
    int k;
    for (k = 0; k < NBLK; k++)
        if (!(blk_used & (1u << k))) {
            blk_used |= 1u << k;
            return k;
        }
    return -1;
}

static void blk_put(int k)
{
    if (k >= 0)
        blk_used &= ~(1u << k);
}

/* ---- Sockets ---------------------------------------------------------- */
#define NSOCK   16
#define SF_BOUND    0x01                /* ausdruecklich gebunden          */
#define SF_CONN     0x02                /* Gegenseite fest (connect)       */
#define SF_NB       0x04                /* nicht blockierend               */
#define SF_BCAST    0x08                /* Rundruf erlaubt                 */
#define SF_V6ONLY   0x10
#define SF_NORD     0x20                /* shutdown lesen                  */
#define SF_NOWR     0x40                /* shutdown schreiben              */
#define NPEER   4

static struct sock {
    unsigned char  used, type, fam, flags;
    signed char    task;
    signed char    rb;                  /* Empfangsblock                   */
    unsigned       lport, rport;
    unsigned char  laddr[16], raddr[16];
    unsigned       rh, rt, ru;          /* Ring: Kopf, Ende, belegt        */
    unsigned       rto;                 /* Vorgabefrist (ms, 0 = ewig)     */
    struct {
        unsigned char a[16];
        unsigned      port;
        unsigned long t;
    } peer[NPEER];                      /* Firewall: wem gesendet wurde    */
    unsigned char  pn;
    char           prog[13];            /* Programm (Firewall, NETSTAT)    */
    /* TCP (tcp.h) */
    unsigned char  st;                  /* TS_*                            */
    signed char    tb;                  /* Sendeblock                      */
    signed char    parent;              /* Listener, bis accept            */
    unsigned char  backlog;
    unsigned char  err;                 /* 1 Reset, 2 Frist                */
    unsigned char  rxfin;               /* FIN der Gegenseite gelesen      */
    unsigned char  ackp;                /* ACK faellig (verzoegert)        */
    unsigned char  retries, dupacks;
    unsigned char  finq, finsent, orphan, rtt_on;
    unsigned long  iss, snd_una, snd_nxt, snd_max, rcv_nxt, tx_seq, rtt_seq;
    unsigned       snd_wnd, cwnd, ssthresh, ca_cnt, mss;
    unsigned       ts, tq;              /* Sendering: Anfang, Menge        */
    unsigned       trto, srtt, rttv;    /* Takte (srtt * 8, rttv * 4)      */
    unsigned       rtt_t;
    unsigned long  tmr, ack_t, tw;      /* Wiederholung, ACK, Zeitgrenze   */
} sk[NSOCK];
#define SEV(i)  (0x200 + (i))           /* Weckereignis je Socket          */
#define SEL_EV  0x2FF                   /* Weckereignis fuer NET_SELECT    */
static unsigned char sel_n;             /* Tasks, die in SELECT warten     */

static void wake(int i)                 /* Socket i hat sich geaendert     */
{
    proc_run(SEV(i));
    if (sel_n)
        proc_run(SEL_EV);
}

static unsigned eph;                    /* naechster freie Absenderport    */

static unsigned swap16(unsigned v)
{
    return v << 8 | v >> 8;
}

static int port_used(unsigned char type, unsigned p)
{
    int i;
    for (i = 0; i < NSOCK; i++)
        if (sk[i].used && sk[i].type == type && sk[i].lport == p)
            return 1;
    return 0;
}

static unsigned port_new(unsigned char type)
{
    unsigned k, p;
    eph = 49152 + ((unsigned)rnd32() & 0x3FFF); /* RFC 6056: zufaellig */
    for (k = 0; k < 16384; k++) {
        p = eph++;
        if (eph < 49152)
            eph = 49152;
        if (!port_used(type, p))
            return p;
    }
    return 0;
}

/* sockaddr des Programms -> Adresse (16) + Port */
static int sa_get(const struct fapi_sockaddr __far *a, unsigned char __far *ad, unsigned __far *port)
{
    if (a->family == AF_INET)
        map4(ad, a->a4);
    else if (a->family == AF_INET6)
        cpy(ad, a->a6, 16);
    else
        return -1;
    *port = swap16(a->port);
    return 0;
}

static void sa_put(struct fapi_sockaddr __far *a, int fam, const unsigned char __far *ad,
                   unsigned port)
{
    zero(a, sizeof(*a));
    if (fam == AF_INET6) {
        a->len = 28;
        a->family = AF_INET6;
        cpy(a->a6, ad, 16);
    } else {
        a->len = 16;
        a->family = AF_INET;
        cpy(a->a4, ad + 12, 4);
    }
    a->port = swap16(port);
}

static void peer_note(struct sock *s, const unsigned char __far *a16, unsigned port)
{
    int k, o = 0;
    for (k = 0; k < NPEER; k++) {
        if (eq(s->peer[k].a, a16, 16) && s->peer[k].port == port) {
            s->peer[k].t = secs;
            return;
        }
        if (s->peer[k].t < s->peer[o].t)
            o = k;
    }
    cpy(s->peer[o].a, a16, 16);
    s->peer[o].port = port;
    s->peer[o].t = secs ? secs : 1;
}

static int peer_ok(struct sock *s, const unsigned char __far *a16, unsigned port)
{
    int k;
    for (k = 0; k < NPEER; k++)
        if (s->peer[k].t && secs - s->peer[k].t < 120 &&
            eq(s->peer[k].a, a16, 16) && s->peer[k].port == port)
            return 1;
    return 0;
}

/* UDP senden an d (16 Byte, beide Familien), Absenderport sport */
static int udp_tx(const unsigned char __far *d, unsigned sport, unsigned dport,
                  const void __far *data, unsigned len)
{
    unsigned char src[16], *h;
    unsigned cs;
    int i;
    if (len > (is_v4(d) ? 1472 : 1452) || (i = route_any(d, src)) < 0)
        return -1;
    h = txb + L4OFF(d);
    cpy(h + 8, data, len);
    P16(h, sport);
    P16(h + 2, dport);
    P16(h + 4, 8 + len);
    P16(h + 6, 0);
    cs = l4sum(src, d, 17, h, 8 + len);
    P16(h + 6, cs ? cs : 0xFFFF);
    return ip_send(i, src, d, 17, 8 + len, 64) < 0 ? -1 : 0;
}

/* passt Paket der Familie von rx_s zum Socket? (AF_INET nur IPv4,
   AF_INET6 beide ausser mit IPV6_V6ONLY) */
static int fam_ok(struct sock *s, const unsigned char __far *a)
{
    if (is_v4(a))
        return s->fam == AF_INET || !(s->flags & SF_V6ONLY);
    return s->fam == AF_INET6;
}

/* Datagramm in den Ring des Sockets */
static int ring_put(struct sock *s, const unsigned char __far *src16, unsigned sport,
                    const unsigned char __far *data, unsigned n)
{
    unsigned char __far *b = BP(s->rb);
    unsigned r = (20 + n + 1) & ~1u;
    if (s->ru + r > BLK)
        return -1;
    if (s->rh + r > BLK) {              /* ans Ende passt es nicht        */
        if (s->ru + (BLK - s->rh) + r > BLK)
            return -1;
        *(unsigned __far *)(b + s->rh) = 0xFFFF;
        s->ru += BLK - s->rh;
        s->rh = 0;
    }
    *(unsigned __far *)(b + s->rh) = n;
    cpy(b + s->rh + 2, src16, 16);
    *(unsigned __far *)(b + s->rh + 18) = sport;
    cpy(b + s->rh + 20, data, n);
    s->rh += r;
    if (s->rh == BLK)
        s->rh = 0;
    s->ru += r;
    return 0;
}

/* naechstes Datagramm -> Stelle im Block, -1 = keins */
static int ring_peek(struct sock *s)
{
    if (!s->ru)
        return -1;
    if (*(unsigned __far *)(BP(s->rb) + s->rt) == 0xFFFF) {
        s->ru -= BLK - s->rt;
        s->rt = 0;
        if (!s->ru)
            return -1;
    }
    return (int)s->rt;
}

static void ring_drop(struct sock *s)
{
    unsigned n = *(unsigned __far *)(BP(s->rb) + s->rt);
    unsigned r = (20 + n + 1) & ~1u;
    s->rt += r;
    if (s->rt >= BLK)
        s->rt = 0;
    s->ru -= r;
    if (!s->ru)
        s->rh = s->rt = 0;
}

/* ---- DNS-Aufloeser ---------------------------------------------------- */
#define DNS_EV  0x300
static unsigned char dns_r[512];
static unsigned dns_rlen, dns_id, dns_lport;
static unsigned char dns_cur[16];      /* gefragter Server               */
static volatile unsigned char dns_done, dns_busy;
static struct {
    char           n[64];
    unsigned char  a[4][16];
    unsigned char  cnt, fam;
    unsigned long  exp;
} dcache[8];
static unsigned char dc_next;

/* UDP-Datagramm u (sl Byte bis Paketende), Adressen in rx_s/rx_d */
static void udp_in(int i, fp u, unsigned sl)
{
    unsigned ul = G16(u + 4), sp = G16(u), dp = G16(u + 2), k;
    unsigned char src[16];
    struct sock *s;
    if (ul < 8 || ul > sl)
        return;
    if ((G16(u + 6) || !is_v4(rx_s)) && l4sum(rx_s, rx_d, 17, u, ul))
        return;                         /* Pruefsumme falsch (bei IPv6 Pflicht) */
    if (dp == 68 && sp == 67 && is_v4(rx_s)) {
        dhcp_in(i, u + 8, ul - 8);
        return;
    }
    if (dns_busy && dp == dns_lport && sp == 53 && !dns_done && eq(rx_s, dns_cur, 16)) {
        if (ul - 8 >= 12 && G16(u + 8) == dns_id) {
            dns_rlen = ul - 8 > sizeof(dns_r) ? sizeof(dns_r) : ul - 8;
            cpy(dns_r, u + 8, dns_rlen);
            dns_done = 1;
            proc_run(DNS_EV);
        }
        return;
    }
    cpy(src, rx_s, 16);
    for (k = 0; k < NSOCK; k++) {
        s = &sk[k];
        if (!s->used || s->type != SOCK_DGRAM || s->lport != dp || (s->flags & SF_NORD) ||
            !fam_ok(s, src))
            continue;
        if (s->flags & SF_CONN) {
            if (s->rport != sp || !eq(s->raddr, src, 16))
                continue;
        } else if (fw_on && !peer_ok(s, src, sp) &&
                   !fw_allow(s->prog, FWP_UDP, dp, src, sp, i))
            return;                     /* niemand hat gefragt, keine Regel */
        if (ring_put(s, src, sp, u + 8, ul - 8) == 0)
            wake(k);
        return;
    }
    fw_drop++;                          /* kein Socket: schweigen          */
}

static unsigned dns_build(const char __far *name, unsigned char __far *q, unsigned qt)
{
    unsigned n = 12, l, k;
    zero(q, 12);
    P16(q, dns_id);
    P16(q + 2, 0x0100);                 /* Rekursion erwuenscht            */
    P16(q + 4, 1);
    while (*name) {
        for (l = 0; name[l] && name[l] != '.'; l++)
            ;
        if (l == 0 || l > 63 || n + l + 6 > 300)
            return 0;
        q[n++] = (unsigned char)l;
        for (k = 0; k < l; k++)
            q[n++] = (unsigned char)name[k];
        name += l;
        if (*name == '.')
            name++;
    }
    q[n++] = 0;
    P16(q + n, qt);                     /* A (1) oder AAAA (28)            */
    P16(q + n + 2, 1);                  /* IN                              */
    return n + 4;
}

static unsigned dns_skip(unsigned p)    /* Name ueberspringen              */
{
    while (p < dns_rlen) {
        unsigned c = dns_r[p];
        if (!c)
            return p + 1;
        if ((c & 0xC0) == 0xC0)
            return p + 2;
        p += c + 1;
    }
    return dns_rlen;
}

/* eine Anfrage (qt = 1 A, 28 AAAA); -> Zahl der Adressen (in out, je 16
   Byte, hoechstens max), 0 = Name unbekannt, -1 = keine Antwort/kein Server.
   Server: die IPv4-Server der Karte, dann der aus dem RA (RDNSS). */
static int dns_ask(const char __far *name, unsigned qt, unsigned char __far *out, int max,
                   unsigned long __far *ttl)
{
    static unsigned char q[300];        /* Stapel ist knapp; dns_busy     */
    static unsigned char srv[3][16];
    unsigned ql, try, w, an, p, k, rl, t0, ns = 0;
    int i, cnt;

    for (i = 0; i < NNETIF; i++)
        if (nif[i].up) {
            for (k = 0; k < 2 && ns < 3; k++)
                if (!is_zero4(nif[i].dns[k]))
                    map4(srv[ns++], nif[i].dns[k]);
            if (!is_zero16(nif[i].dns6) && nif[i].llst == 2 && ns < 3)
                cpy(srv[ns++], nif[i].dns6, 16);
        }
    if (!ns)
        return -1;
    for (try = 0; try < 4; try++) {
        dns_lport = port_new(SOCK_DGRAM);       /* Port und Kennung je   */
        dns_id = (unsigned)rnd32();             /* Anfrage zufaellig     */
        cpy(dns_cur, srv[try % ns], 16);
        if (!(ql = dns_build(name, q, qt)))
            return 0;
        dns_done = 0;
        if (udp_tx(srv[try % ns], dns_lport, 53, q, ql) < 0)
            continue;
        t0 = ticks();
        while (!dns_done && (w = ticks() - t0) < 200) {
            cli();
            if (!dns_done)
                proc_block(DNS_EV, 200 - w);
            else
                sti();
        }
        if (!dns_done)
            continue;
        if (!(dns_r[2] & 0x80))
            continue;
        if ((dns_r[3] & 15) == 3)
            return 0;                   /* NXDOMAIN                        */
        if (dns_r[3] & 15)
            continue;
        an = G16(dns_r + 6);
        p = dns_skip(12) + 4;           /* Frage ueberspringen             */
        cnt = 0;
        *ttl = 3600;
        for (k = 0; k < an && p + 10 <= dns_rlen; k++) {
            p = dns_skip(p);
            if (p + 10 > dns_rlen)
                break;
            rl = G16(dns_r + p + 8);
            if (rl > dns_rlen)
                break;
            if (G16(dns_r + p) == qt && G16(dns_r + p + 2) == 1 &&
                rl == (qt == 1 ? 4 : 16) && p + 10 + rl <= dns_rlen && cnt < max) {
                if (qt == 1)
                    map4(out + 16 * cnt, dns_r + p + 10);
                else
                    cpy(out + 16 * cnt, dns_r + p + 10, 16);
                cnt++;
                *ttl = (unsigned long)G16(dns_r + p + 4) << 16 | G16(dns_r + p + 6);
            }
            p += 10 + rl;
        }
        return cnt;
    }
    return -1;
}

static int name_eq(const char __far *a, const char __far *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32))
            return 0;
    return !*a && !*b;
}

static unsigned net_resolve(struct net_call __far *c)
{
    struct fapi_resolve __far *u = uptr(c->usel, c->p, sizeof(struct fapi_resolve));
    unsigned char ip[4];
    static unsigned char out[64];       /* unter dns_busy                  */
    static char full[128];
    unsigned long ttl;
    const char __far *e;
    unsigned k, l, w, want6;
    int n, j, got = 0;                  /* got: ein Server hat geantwortet */

    if (!u || c->n < sizeof(struct fapi_resolve))
        return FAPI_ERROR;
    u->name[sizeof(u->name) - 1] = 0;
    u->count = 0;
    if ((e = ip_parse(u->name, ip)) != 0 && !*e) {     /* schon eine Adresse */
        map4(u->addr[0], ip);
        u->count = 1;
        u->ttl = 0;
        return 0;
    }
    for (k = 0; k < 8; k++)             /* Cache                           */
        if (dcache[k].cnt && dcache[k].fam == u->family && secs < dcache[k].exp &&
            name_eq(dcache[k].n, u->name)) {
            for (j = 0; j < dcache[k].cnt; j++)
                cpy(u->addr[j], dcache[k].a[j], 16);
            u->count = dcache[k].cnt;
            u->ttl = dcache[k].exp - secs;
            return 0;
        }
    for (w = 0; dns_busy && w < 500; w++) {  /* eine Anfrage zur Zeit      */
        cli();
        if (dns_busy)
            proc_block(DNS_EV + 1, 2);
        else
            sti();
    }
    if (dns_busy)
        return FAPI_ERR_TIMEOUT;
    dns_busy = 1;
    for (l = 0; u->name[l]; l++)
        ;
    /* ohne Punkt: erst mit dem Suffix aus DHCP */
    full[0] = 0;
    for (k = 0; k < l && u->name[k] != '.'; k++)
        ;
    if (k == l) {
        int i;
        for (i = 0; i < NNETIF && !(nif[i].up && nif[i].domain[0]); i++)
            ;
        if (i < NNETIF) {
            cpy(full, u->name, l);
            full[l] = '.';
            for (k = 0; nif[i].domain[k] && l + 1 + k < sizeof(full) - 1; k++)
                full[l + 1 + k] = nif[i].domain[k];
            full[l + 1 + k] = 0;
        }
    }
    /* Familie 0: AAAA zuerst, wenn es eine globale IPv6-Adresse gibt
       (RFC 6724 vereinfacht), dann A; sonst nur A */
    for (k = 0; k < NNETIF && nif[k].gast != 2; k++)
        ;
    want6 = u->family == AF_INET6 || (u->family == 0 && k < NNETIF);
    n = 0;
    ttl = 3600;
    for (j = want6 ? 0 : 1; j < (u->family == AF_INET6 ? 1 : 2) && n < 4; j++) {
        int r = -1;
        if (full[0])
            r = dns_ask(full, j ? 1 : 28, out + 16 * n, 4 - n, &ttl);
        if (r <= 0)
            r = dns_ask(u->name, j ? 1 : 28, out + 16 * n, 4 - n, &ttl);
        if (r > 0)
            n += r;
        if (r >= 0)
            got = 1;
    }
    dns_busy = 0;
    proc_run(DNS_EV + 1);
    if (!n)
        return got ? FAPI_ERROR : FAPI_ERR_TIMEOUT;
    for (j = 0; j < n; j++)
        cpy(u->addr[j], out + 16 * j, 16);
    u->count = n;
    u->ttl = ttl;
    if (l < 64) {                       /* merken (hoechstens 1 Tag)       */
        k = dc_next++ & 7;
        cpy(dcache[k].n, u->name, l + 1);
        for (j = 0; j < n; j++)
            cpy(dcache[k].a[j], out + 16 * j, 16);
        dcache[k].cnt = (unsigned char)n;
        dcache[k].fam = (unsigned char)u->family;
        dcache[k].exp = secs + (ttl > 86400UL ? 86400UL : ttl);
    }
    return 0;
}

/* ---- Socket-Aufrufe --------------------------------------------------- */
static void sk_free(int i)
{
    struct sock *s = &sk[i];
    if (!s->used)
        return;
    blk_put(s->rb);
    if (s->type == SOCK_STREAM)
        blk_put(s->tb);
    s->used = 0;
    wake(i);                   /* Wartende wecken                 */
}

/* warten, bis cond() oder Frist (ms, 0 = ewig) */
static unsigned sk_wait(int i, unsigned ms, unsigned t0)
{
    unsigned lim = ms ? ms / 10 + 1 : 0, w = ticks() - t0;
    if (lim && w >= lim)
        return FAPI_ERR_TIMEOUT;
    proc_block(SEV(i), lim ? lim - w : 100);
    return 0;
}

#include "tcp.h"                        /* TCP (Stufe 2b)                  */

static unsigned sk_recv(int i, unsigned usel, unsigned buf, unsigned len, unsigned flags,
                        unsigned ms, struct fapi_sockaddr __far *from)
{
    struct sock *s = &sk[i];
    unsigned char __far *b;
    unsigned char __far *dst;
    unsigned n, t0 = ticks(), r;
    int at;

    if (!(dst = uptr(usel, buf, len ? len : 1)))
        return FAPI_ERROR;
    if (!ms)
        ms = s->rto;
    for (;;) {
        if (!s->used)
            return FAPI_ERR_RESET;
        if ((at = ring_peek(s)) >= 0)
            break;
        if (s->flags & (SF_NB | SF_NORD))
            return s->flags & SF_NORD ? 0 : FAPI_ERR_AGAIN;
        cli();
        if (ring_peek(s) < 0) {
            if ((r = sk_wait(i, ms, t0)) != 0) {
                sti();
                return r;
            }
        } else
            sti();
    }
    b = BP(s->rb) + at;
    n = *(unsigned __far *)b;
    if (n > len)
        n = len;                        /* Rest des Datagramms geht verloren */
    cpy(dst, b + 20, n);
    if (from)
        sa_put(from, s->fam, b + 2, *(unsigned __far *)(b + 18));
    if (!(flags & MSG_PEEK))
        ring_drop(s);
    return n;
}

static unsigned sk_send(int i, const unsigned char __far *d16, unsigned dport,
                        unsigned usel, unsigned buf, unsigned len)
{
    struct sock *s = &sk[i];
    const unsigned char __far *src;
    if (s->flags & SF_NOWR)
        return FAPI_ERR_RESET;
    if (!(src = uptr(usel, buf, len ? len : 1)))
        return FAPI_ERROR;
    if (!fam_ok(s, d16))
        return FAPI_ERROR;              /* IPv6-Ziel fuer AF_INET usw.     */
    if (len > (is_v4(d16) ? 1472 : 1452))
        return FAPI_ERROR;
    if (is_v4(d16) && d16[12] == 255 && d16[13] == 255 && d16[14] == 255 &&
        d16[15] == 255 && !(s->flags & SF_BCAST))
        return FAPI_ERR_DENIED;
    if (!s->lport && !(s->lport = port_new(SOCK_DGRAM)))
        return FAPI_ERR_FULL;
    peer_note(s, d16, dport);
    if (udp_tx(d16, s->lport, dport, src, len) < 0)
        return FAPI_ERR_NONET;
    return len;
}

static unsigned sk_call(struct net_call __far *c)
{
    struct fapi_sockio __far *u;
    struct sock *s;
    unsigned char a[16];
    unsigned port, i = c->sock;

    if (c->fn == NET_SOCKET) {
        struct fapi_socket __far *q = uptr(c->usel, c->p, sizeof(struct fapi_socket));
        int b;
        if (!q || (q->family != AF_INET && q->family != AF_INET6))
            return FAPI_ERROR;
        if (q->type == SOCK_STREAM) {
            if ((b = sock_new()) < 0)
                return FAPI_ERR_FULL;
            sk[b].fam = (unsigned char)q->family;
            sk[b].task = (signed char)c->task;
            cpy(sk[b].prog, c->prog, 13);
            sk[b].prog[12] = 0;
            return b;
        }
        if (q->type != SOCK_DGRAM)
            return FAPI_ERROR;
        for (i = 0; i < NSOCK && sk[i].used; i++)
            ;
        if (i == NSOCK || (b = blk_get()) < 0)
            return FAPI_ERR_FULL;
        s = &sk[i];
        zero(s, sizeof(*s));
        s->used = 1;
        s->type = (unsigned char)q->type;
        s->fam = (unsigned char)q->family;
        s->task = (signed char)c->task;
        cpy(s->prog, c->prog, 13);
        s->prog[12] = 0;
        s->rb = (signed char)b;
        s->tb = s->parent = -1;
        return i;
    }
    if (i >= NSOCK || !sk[i].used)
        return FAPI_ERROR;
    s = &sk[i];
    switch (c->fn) {
    case NETK_CLOSE:
        if (s->type == SOCK_STREAM)
            tcp_close(i);
        else
            sk_free(i);
        return 0;
    case NETK_READ:
        if (s->type == SOCK_STREAM)
            return tcp_recv(i, c->usel, c->p, c->n, 0, 0);
        return sk_recv(i, c->usel, c->p, c->n, 0, 0, 0);
    case NETK_WRITE:
        if (s->type == SOCK_STREAM)
            return tcp_send(i, c->usel, c->p, c->n);
        if (!(s->flags & SF_CONN))
            return FAPI_ERROR;
        return sk_send(i, s->raddr, s->rport, c->usel, c->p, c->n);
    }
    if (!(u = uptr(c->usel, c->p, sizeof(struct fapi_sockio))) || c->n < sizeof(struct fapi_sockio))
        return FAPI_ERROR;
    switch (c->fn) {
    case NET_BIND:
        if (sa_get(&u->addr, a, &port))
            return FAPI_ERROR;
        if (!port && !(port = port_new(s->type)))
            return FAPI_ERR_FULL;
        if (port_used(s->type, port) && s->lport != port)
            return FAPI_ERR_INUSE;
        s->lport = port;
        cpy(s->laddr, a, 16);
        s->flags |= SF_BOUND;
        return 0;
    case NET_CONNECT:
        if (sa_get(&u->addr, s->raddr, &s->rport) || !fam_ok(s, s->raddr))
            return FAPI_ERROR;
        if (s->type == SOCK_STREAM)
            return tcp_connect(i, u->timeout);
        if (!s->lport && !(s->lport = port_new(s->type)))
            return FAPI_ERR_FULL;
        s->flags |= SF_CONN;
        return 0;
    case NET_LISTEN:
        if (s->type != SOCK_STREAM || s->st != TS_CLOSED)
            return FAPI_ERROR;
        if (!s->lport && !(s->lport = port_new(SOCK_STREAM)))
            return FAPI_ERR_FULL;
        s->backlog = (unsigned char)(u->len && u->len < 8 ? u->len : 4);
        tcp_free_tx(s);                 /* Listener sendet nichts          */
        s->st = TS_LISTEN;
        return 0;
    case NET_ACCEPT:
        if (s->type != SOCK_STREAM)
            return FAPI_ERROR;
        return tcp_accept(i, u->timeout, &u->addr);
    }
    if (s->type == SOCK_STREAM)
        switch (c->fn) {
        case NET_SEND:
        case NET_SENDTO:
            return tcp_send(i, c->usel, u->buf, u->len);
        case NET_RECV:
            return tcp_recv(i, c->usel, u->buf, u->len, u->flags, u->timeout);
        case NET_RECVFROM:
            sa_put(&u->addr, s->fam, s->raddr, s->rport);
            return tcp_recv(i, c->usel, u->buf, u->len, u->flags, u->timeout);
        case NET_SHUTDOWN:
            if (u->len != 0) {
                s->flags |= SF_NOWR;
                tcp_fin(s);
            }
            if (u->len != 1)
                s->flags |= SF_NORD;
            wake(i);
            return 0;
        case NET_GETPEER:
            if (s->st < TS_SYNRCVD)
                return FAPI_ERROR;
            sa_put(&u->addr, s->fam, s->raddr, s->rport);
            return 0;
        }
    switch (c->fn) {
    case NET_SEND:
        if (!(s->flags & SF_CONN))
            return FAPI_ERROR;
        return sk_send(i, s->raddr, s->rport, c->usel, u->buf, u->len);
    case NET_SENDTO:
        if (sa_get(&u->addr, a, &port))
            return FAPI_ERROR;
        return sk_send(i, a, port, c->usel, u->buf, u->len);
    case NET_RECV:
        return sk_recv(i, c->usel, u->buf, u->len, u->flags, u->timeout, 0);
    case NET_RECVFROM:
        return sk_recv(i, c->usel, u->buf, u->len, u->flags, u->timeout, &u->addr);
    case NET_GETNAME:
        if ((is_zero16(s->laddr) || (is_v4(s->laddr) && is_zero4(s->laddr + 12))) &&
            (s->rport ? route_any(s->raddr, a) >= 0 : 0)) {
            sa_put(&u->addr, s->fam, a, s->lport);  /* Adresse der Karte   */
            return 0;
        }
        if (is_zero4(s->laddr + 12) && is_v4(s->laddr))
            for (port = 0; port < NNETIF; port++)
                if (nif[port].up && !is_zero4(nif[port].ip)) {
                    map4(a, nif[port].ip);
                    sa_put(&u->addr, s->fam, a, s->lport);
                    return 0;
                }
        sa_put(&u->addr, s->fam, s->laddr, s->lport);
        return 0;
    case NET_GETPEER:
        if (!(s->flags & SF_CONN))
            return FAPI_ERROR;
        sa_put(&u->addr, s->fam, s->raddr, s->rport);
        return 0;
    case NET_SHUTDOWN:
        s->flags |= u->len == 0 ? SF_NORD : u->len == 1 ? SF_NOWR : SF_NORD | SF_NOWR;
        wake(i);
        return 0;
    case NET_SETOPT:
    case NET_GETOPT: {
        static const unsigned char bit[5] = { 0, SF_NB, 0, SF_BCAST, SF_V6ONLY };
        if (u->len == SO_RCVTIMEO) {
            if (c->fn == NET_SETOPT)
                s->rto = u->flags;
            return s->rto;
        }
        if (u->len < 1 || u->len > 4 || !bit[u->len])
            return FAPI_ERROR;
        if (c->fn == NET_SETOPT) {
            if (u->flags)
                s->flags |= bit[u->len];
            else
                s->flags &= ~bit[u->len];
        }
        return (s->flags & bit[u->len]) != 0;
    }
    }
    return FAPI_ERROR;                  /* LISTEN/ACCEPT: nur TCP          */
}

static int sk_any(void)                 /* noch Sockets offen?             */
{
    int i;
    for (i = 0; i < NSOCK; i++)
        if (sk[i].used)
            return 1;
    return 0;
}

/* NETSTAT: Platz idx beschreiben (frei: type 0) */
static unsigned sk_list(struct net_call __far *c)
{
    struct fapi_sockinfo __far *u = uptr(c->usel, c->p, sizeof(struct fapi_sockinfo));
    struct sock *s;
    if (!u || c->n < sizeof(struct fapi_sockinfo))
        return FAPI_ERROR;
    if (u->idx >= NSOCK)
        return FAPI_ERROR;
    s = &sk[u->idx];
    zero((unsigned char __far *)u + 2, sizeof(struct fapi_sockinfo) - 2);
    if (!s->used)
        return 0;
    u->type = s->type;
    u->state = s->type == SOCK_STREAM ? s->st : 0;
    u->family = s->fam;
    u->task = (unsigned char)s->task;
    u->lport = s->lport;
    u->rport = s->rport;
    cpy(u->laddr, s->laddr, 16);
    cpy(u->raddr, s->raddr, 16);
    if ((is_zero16(u->laddr) || (is_v4(u->laddr) && is_zero4(u->laddr + 12))) &&
        s->rport && !is_zero16(s->raddr)) {
        unsigned char a[16];            /* verbunden: Adresse der Karte    */
        if (route_any(s->raddr, a) >= 0)
            cpy(u->laddr, a, 16);
    }
    cpy(u->prog, s->prog, 13);
    u->rq = s->ru;
    u->sq = s->type == SOCK_STREAM ? s->tq : 0;
    return 0;
}

/* NET_SELECT: Bereitschaft von Socket i (SEL_*) */
static unsigned char sk_ready(int i)
{
    struct sock *s = &sk[i];
    unsigned char r = 0;
    int c;
    if (i < 0 || i >= NSOCK || !s->used)
        return SEL_ERR;
    if (s->type == SOCK_DGRAM)
        return (s->ru ? SEL_READ : 0) | SEL_WRITE;
    if (s->st == TS_LISTEN) {
        for (c = 0; c < NSOCK; c++)
            if (sk[c].used && sk[c].parent == i &&
                (sk[c].st >= TS_EST || (sk[c].st == TS_CLOSED && sk[c].rxfin)))
                return SEL_READ;
        return 0;
    }
    if (s->ru || s->rxfin || (s->flags & SF_NORD))
        r |= SEL_READ;
    if ((s->st == TS_EST || s->st == TS_CLOSEWT) && s->tq < BLK && !(s->flags & SF_NOWR))
        r |= SEL_WRITE;
    if (s->err || (s->st == TS_CLOSED && s->rport))
        r |= SEL_ERR | SEL_READ;
    return r;
}

/* Der Kernel hat die Handles schon in Socketnummern uebersetzt (Kopie auf
   seinem Stapel) und legt ready zurueck. */
static unsigned sk_select(struct net_call __far *c)
{
    struct fapi_select __far *u = uptr(c->usel, c->p, sizeof(struct fapi_select));
    unsigned k, n, t0 = ticks(), lim, w;
    if (!u || u->n == 0 || u->n > NSEL_MAX)
        return FAPI_ERROR;
    lim = u->timeout == 0xFFFF ? 0 : u->timeout / 10 + 1;
    for (;;) {
        cli();
        for (k = n = 0; k < u->n; k++) {
            u->e[k].ready = sk_ready(u->e[k].h) & (u->e[k].want | SEL_ERR);
            if (u->e[k].ready)
                n++;
        }
        if (n || u->timeout == 0) {
            sti();
            return n;
        }
        w = ticks() - t0;
        if (lim && w >= lim) {
            sti();
            return 0;
        }
        sel_n++;
        proc_block(SEL_EV, lim ? lim - w : 100);
        sel_n--;
    }
}
