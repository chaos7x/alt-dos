/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dnsq.h - kleiner DNS-Client fuer DIG und NSLOOKUP (per #include, nach
 * fapi.h, util.h und net.h mit NET_RESOLVE_FN).
 *
 * Eigene Anfragen statt NET_RESOLVE von TCPIP.SYS, weil die Werkzeuge
 * jeden Satztyp und jeden Server fragen sollen. Anfrage per UDP (EDNS0,
 * 1232 Byte), zwei Versuche; ist die Antwort abgeschnitten (TC), noch
 * einmal ueber TCP (Laenge vorn, RFC 1035 4.2.2). Nachrichtenformat nach
 * RFC 1035, AAAA RFC 3596, SRV RFC 2782, CAA RFC 8659.
 */
#ifndef DNSQ_H
#define DNSQ_H

#define DQ_MAX  4096                /* Antwortpuffer (TCP bis hier)     */

static unsigned char dq_msg[DQ_MAX + 2];
static unsigned dq_len;             /* Laenge der Antwort               */
static unsigned dq_ms;              /* Laufzeit in ms (10-ms-Schritte)  */
static int      dq_tcp;             /* Antwort kam ueber TCP            */
static unsigned dq_tmo = 3;         /* Sekunden je Versuch              */
static unsigned dq_tries = 2;

static const struct { const char *n; unsigned t; } dq_types[] = {
    { "A", 1 }, { "NS", 2 }, { "CNAME", 5 }, { "SOA", 6 }, { "PTR", 12 },
    { "HINFO", 13 }, { "MX", 15 }, { "TXT", 16 }, { "AAAA", 28 },
    { "SRV", 33 }, { "NAPTR", 35 }, { "DS", 43 }, { "DNSKEY", 48 },
    { "CAA", 257 }, { "ANY", 255 }, { "AXFR", 252 }, { 0, 0 }
};

static const char *dq_rcodes[] = {
    "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED"
};

static unsigned dq_g16(unsigned o)
{
    return (unsigned)dq_msg[o] << 8 | dq_msg[o + 1];
}

static unsigned long dq_g32(unsigned o)
{
    return (unsigned long)dq_g16(o) << 16 | dq_g16(o + 2);
}

static int dq_ieq(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return !*a && !*b;
}

/* Typname -> Nummer (auch TYPEnnn), 0 = unbekannt */
static unsigned dq_type(const char *s)
{
    unsigned i, v = 0;
    for (i = 0; dq_types[i].n; i++)
        if (dq_ieq(s, dq_types[i].n))
            return dq_types[i].t;
    if (up(s[0]) == 'T' && up(s[1]) == 'Y' && up(s[2]) == 'P' && up(s[3]) == 'E') {
        for (s += 4; *s >= '0' && *s <= '9'; s++)
            v = v * 10 + (*s - '0');
        return *s ? 0 : v;
    }
    return 0;
}

static char *dq_num(char *o, unsigned long v)
{
    char t[12];
    int k = 0;
    static const unsigned long pw[10] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    int i, st = 0;
    for (i = 0; i < 10; i++) {
        char c = '0';
        while (v >= pw[i])
            v -= pw[i], c++;
        if (c != '0' || st || i == 9)
            t[k++] = c, st = 1;
    }
    for (i = 0; i < k; i++)
        *o++ = t[i];
    *o = 0;
    return o;
}

static void dq_tname(char *o, unsigned t)
{
    unsigned i;
    for (i = 0; dq_types[i].n; i++)
        if (dq_types[i].t == t) {
            scopy(o, dq_types[i].n, 12);
            return;
        }
    scopy(o, "TYPE", 12);
    dq_num(o + 4, t);
}

/* Anfrage bauen: Name (ohne/mit Punkt am Ende) -> Laenge, 0 = Fehler */
static unsigned dq_build(unsigned char *b, unsigned id, const char *name,
                         unsigned type, int rd)
{
    unsigned o = 12, lp, n;
    b[0] = (unsigned char)(id >> 8);
    b[1] = (unsigned char)id;
    b[2] = (unsigned char)(rd ? 1 : 0);
    b[3] = 0;
    b[4] = 0; b[5] = 1;                 /* eine Frage                   */
    b[6] = b[7] = b[8] = b[9] = 0;
    b[10] = 0; b[11] = 1;               /* ein Zusatzsatz: OPT          */
    while (*name) {
        lp = o++;
        for (n = 0; *name && *name != '.'; name++, n++) {
            if (n >= 63 || o >= 280)
                return 0;
            b[o++] = (unsigned char)*name;
        }
        if (!n)
            return 0;
        b[lp] = (unsigned char)n;
        if (*name == '.')
            name++;
    }
    b[o++] = 0;
    b[o++] = (unsigned char)(type >> 8);
    b[o++] = (unsigned char)type;
    b[o++] = 0; b[o++] = 1;             /* IN                           */
    b[o++] = 0;                         /* OPT: Name ., Typ 41, 1232    */
    b[o++] = 0; b[o++] = 41;
    b[o++] = 0x04; b[o++] = 0xD0;
    b[o++] = 0; b[o++] = 0; b[o++] = 0; b[o++] = 0;
    b[o++] = 0; b[o++] = 0;
    return o;
}

/* Name fuer -x: IPv4 d.c.b.a.in-addr.arpa, IPv6 Halbbytes.ip6.arpa */
static void dq_rev(char *o, const unsigned char *a)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    *o = 0;
    if (net_is4(a)) {
        for (i = 15; i >= 12; i--) {
            o = dq_num(o, a[i]);
            *o++ = '.';
        }
        scopy(o, "in-addr.arpa", 16);
        return;
    }
    for (i = 15; i >= 0; i--) {
        *o++ = hx[a[i] & 15];
        *o++ = '.';
        *o++ = hx[a[i] >> 4];
        *o++ = '.';
    }
    scopy(o, "ip6.arpa", 12);
}

/* Name ab o lesen (Zeiger), Ausgabe mit Punkt am Ende; -> Stelle hinter
   dem Namen im Satz, 0 = kaputt */
static unsigned dq_name(unsigned o, char *out, unsigned max)
{
    unsigned end = 0, k = 0, hops = 0, n, i;
    for (;;) {
        if (o >= dq_len)
            return 0;
        n = dq_msg[o];
        if ((n & 0xC0) == 0xC0) {
            if (o + 1 >= dq_len || ++hops > 32)
                return 0;
            if (!end)
                end = o + 2;
            o = (n & 0x3F) << 8 | dq_msg[o + 1];
            continue;
        }
        if (n & 0xC0)
            return 0;
        o++;
        if (!n)
            break;
        if (o + n > dq_len)
            return 0;
        for (i = 0; i < n; i++) {
            char c = (char)dq_msg[o + i];
            if (k + 5 < max) {
                if (c == '.' || c == '\\')
                    out[k++] = '\\';
                if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E)
                    c = '?';
                out[k++] = c;
            }
        }
        if (k + 1 < max)
            out[k++] = '.';
        o += n;
    }
    if (!k)
        out[k++] = '.';
    out[k] = 0;
    return end ? end : o;
}

/* Inhalt eines Satzes (Typ t, Daten ab o, Laenge rl) als Text */
static void dq_rdata(char *out, unsigned max, unsigned t, unsigned o, unsigned rl)
{
    static unsigned char a[16];
    static const char hx[] = "0123456789ABCDEF";
    char *p = out;
    unsigned i, n, e = o + rl;
    out[0] = 0;
    if (e > dq_len)
        return;
    switch (t) {
    case 1:                             /* A                            */
        if (rl != 4)
            break;
        for (i = 0; i < 10; i++) a[i] = 0;
        a[10] = a[11] = 0xFF;
        for (i = 0; i < 4; i++) a[12 + i] = dq_msg[o + i];
        net_ntop(a, out);
        return;
    case 28:                            /* AAAA                         */
        if (rl != 16)
            break;
        for (i = 0; i < 16; i++) a[i] = dq_msg[o + i];
        if (net_is4(a)) {               /* ::ffff:a.b.c.d nicht als v4  */
            scopy(out, "::ffff:", max);
            net_ntop(a, out + 7);
        } else
            net_ntop(a, out);
        return;
    case 2: case 5: case 12:            /* NS, CNAME, PTR               */
        dq_name(o, out, max);
        return;
    case 15:                            /* MX                           */
        p = dq_num(p, dq_g16(o));
        *p++ = ' ';
        dq_name(o + 2, p, max - (unsigned)(p - out));
        return;
    case 33:                            /* SRV                          */
        p = dq_num(p, dq_g16(o)); *p++ = ' ';
        p = dq_num(p, dq_g16(o + 2)); *p++ = ' ';
        p = dq_num(p, dq_g16(o + 4)); *p++ = ' ';
        dq_name(o + 6, p, max - (unsigned)(p - out));
        return;
    case 6: {                           /* SOA                          */
        unsigned q = dq_name(o, p, max);
        if (!q) return;
        p += slen(p); *p++ = ' ';
        q = dq_name(q, p, max - (unsigned)(p - out));
        if (!q || q + 20 > e) return;
        p += slen(p);
        for (i = 0; i < 5; i++) {
            *p++ = ' ';
            p = dq_num(p, dq_g32(q + i * 4));
        }
        return;
    }
    case 16: case 13:                   /* TXT, HINFO: Zeichenketten    */
        while (o < e && (unsigned)(p - out) + 8 < max) {
            n = dq_msg[o++];
            if (p != out) *p++ = ' ';
            *p++ = '"';
            for (i = 0; i < n && o < e && (unsigned)(p - out) + 4 < max; i++, o++) {
                char c = (char)dq_msg[o];
                if (c == '"' || c == '\\') *p++ = '\\';
                *p++ = ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) ? '?' : c;
            }
            *p++ = '"';
        }
        *p = 0;
        return;
    case 257:                           /* CAA                          */
        if (rl < 2) break;
        p = dq_num(p, dq_msg[o]); *p++ = ' ';
        n = dq_msg[o + 1];
        for (i = 0; i < n && o + 2 + i < e; i++) *p++ = (char)dq_msg[o + 2 + i];
        *p++ = ' '; *p++ = '"';
        for (i = o + 2 + n; i < e && (unsigned)(p - out) + 4 < max; i++) *p++ = (char)dq_msg[i];
        *p++ = '"'; *p = 0;
        return;
    }
    /* unbekannt: \# Laenge Hex (RFC 3597) */
    *p++ = '\\'; *p++ = '#'; *p++ = ' ';
    p = dq_num(p, rl);
    if (rl) *p++ = ' ';
    for (i = 0; i < rl && (unsigned)(p - out) + 3 < max; i++) {
        *p++ = hx[dq_msg[o + i] >> 4];
        *p++ = hx[dq_msg[o + i] & 15];
    }
    *p = 0;
}

/* Satz ab o: Name, Typ, Klasse, TTL, Daten -> Stelle dahinter, 0 = kaputt */
struct dq_rr {
    char          name[256];
    unsigned      type, cls, rl, rd;
    unsigned long ttl;
};

static unsigned dq_rr(unsigned o, struct dq_rr *r)
{
    o = dq_name(o, r->name, sizeof(r->name));
    if (!o || o + 10 > dq_len)
        return 0;
    r->type = dq_g16(o);
    r->cls = dq_g16(o + 2);
    r->ttl = dq_g32(o + 4);
    r->rl = dq_g16(o + 8);
    r->rd = o + 10;
    if (r->rd + r->rl > dq_len)
        return 0;
    return r->rd + r->rl;
}

/* Stelle hinter der Frage (Name + 4) */
static unsigned dq_skipq(unsigned o)
{
    static char t[256];
    o = dq_name(o, t, sizeof(t));
    return o ? o + 4 : 0;
}

static unsigned dq_udp(const unsigned char *srv, unsigned port,
                       unsigned char *q, unsigned ql)
{
    static struct fapi_socket so;
    static struct fapi_sockio io;
    unsigned h, r, k, t;
    so.family = AF_INET6;
    so.type = SOCK_DGRAM;
    so.proto = 0;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(h))
        return h;
    for (t = 0; t < dq_tries; t++) {
        io.h = h;
        io.buf = (unsigned)q;
        io.len = ql;
        io.flags = 0;
        io.timeout = 0;
        io.addr.len = 28;
        io.addr.family = AF_INET6;
        io.addr.port = htons(port);
        for (k = 0; k < 16; k++)
            io.addr.a6[k] = srv[k];
        if (DosNet(NET_SENDTO, &io, sizeof(io)) != ql) {
            DosClose(h);
            return FAPI_ERROR;
        }
        for (;;) {
            io.h = h;
            io.buf = (unsigned)dq_msg;
            io.len = DQ_MAX;
            io.flags = 0;
            io.timeout = dq_tmo * 1000;
            r = DosNet(NET_RECVFROM, &io, sizeof(io));
            if (FAPI_FAILED(r))
                break;
            if (r >= 12 && dq_msg[0] == q[0] && dq_msg[1] == q[1] && (dq_msg[2] & 0x80)) {
                DosClose(h);
                dq_len = r;
                return 0;
            }
        }
        if (r != FAPI_ERR_TIMEOUT)
            break;
    }
    DosClose(h);
    return r;
}

static unsigned dq_tcpq(const unsigned char *srv, unsigned port,
                        unsigned char *q, unsigned ql)
{
    static struct fapi_socket so;
    static struct fapi_sockio io;
    static unsigned char lb[2];
    unsigned h, r, k, got = 0, want = 0;
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    so.proto = 0;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(h))
        return h;
    io.h = h;
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = srv[k];
    io.timeout = dq_tmo * 1000 + 2000;
    r = DosNet(NET_CONNECT, &io, sizeof(io));
    if (r) {
        DosClose(h);
        return r;
    }
    lb[0] = (unsigned char)(ql >> 8);
    lb[1] = (unsigned char)ql;
    io.h = h;                           /* ueber NET_SEND: geht auch im */
    io.buf = (unsigned)lb;              /* Stub (DOS-Box)               */
    io.len = 2;
    io.flags = 0;
    if (DosNet(NET_SEND, &io, sizeof(io)) != 2) {
        DosClose(h);
        return FAPI_ERROR;
    }
    io.h = h;
    io.buf = (unsigned)q;
    io.len = ql;
    io.flags = 0;
    if (DosNet(NET_SEND, &io, sizeof(io)) != ql) {
        DosClose(h);
        return FAPI_ERROR;
    }
    for (;;) {                          /* Laenge, dann die Nachricht   */
        unsigned need = want ? want + 2 : 2;
        io.h = h;
        io.buf = (unsigned)(dq_msg + got);
        io.len = need - got;
        io.flags = 0;
        io.timeout = dq_tmo * 1000 + 2000;
        r = DosNet(NET_RECV, &io, sizeof(io));
        if (FAPI_FAILED(r) || !r) {
            DosClose(h);
            return FAPI_FAILED(r) ? r : FAPI_ERROR;
        }
        got += r;
        if (!want && got >= 2) {
            want = dq_g16(0);
            if (want > DQ_MAX)
                want = DQ_MAX;
            if (want < 12) {
                DosClose(h);
                return FAPI_ERROR;
            }
        }
        if (want && got >= want + 2)
            break;
    }
    DosClose(h);
    for (k = 0; k < want; k++)          /* Laenge vorn weg              */
        dq_msg[k] = dq_msg[k + 2];
    dq_len = want;
    return 0;
}

/* Frage stellen: 0 = Antwort in dq_msg, sonst FAPI-Fehler
   (FAPI_ERR_TIMEOUT = keine Antwort) */
static unsigned dq_ask(const unsigned char *srv, unsigned port, const char *name,
                       unsigned type, int rd, int tcp)
{
    static unsigned char q[300];
    unsigned ql, r, t0, id;
    id = (unsigned)DosQTicks() * 251u + 0x1D35u;
    ql = dq_build(q, id, name, type, rd);
    if (!ql)
        return FAPI_ERROR;
    t0 = (unsigned)DosQTicks();
    dq_tcp = tcp;
    r = tcp ? dq_tcpq(srv, port, q, ql) : dq_udp(srv, port, q, ql);
    if (!r && !tcp && (dq_msg[2] & 2)) {    /* abgeschnitten: TCP       */
        dq_tcp = 1;
        r = dq_tcpq(srv, port, q, ql);
    }
    dq_ms = ((unsigned)DosQTicks() - t0) * 10;
    return r;
}

/* Server: Name oder Zahl, sonst DNS der ersten Karte */
static int dq_server(const char *s, unsigned char *a)
{
    static struct fapi_netif ni;
    unsigned k;
    if (s && *s)
        return net_lookup(s, a) ? -1 : 0;
    ni.ifc = 0;
    if (DosNet(NET_IFINFO, &ni, sizeof(ni)))
        return -1;
    for (k = 0; k < 2; k++)
        if (ni.dns[k][0] || ni.dns[k][10] || ni.dns[k][15]) {
            unsigned i;
            for (i = 0; i < 16; i++)
                a[i] = ni.dns[k][i];
            return 0;
        }
    return -1;
}

#endif
