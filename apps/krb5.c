/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Kerberos v5 (Schnittstelle und Quellen in krb5.h)
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "util.h"
#include "arcutil.h"
#include "tls.h"
#include "krb5.h"

#define DosNet(fn, p, n)    fapi_call(FAPI_NET, (fn), (unsigned short)(p), (n))

unsigned long au_mul16k(unsigned a, unsigned b);
#pragma aux au_mul16k = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];

long krb_off;
u8   krb_kdc[16];

/* ---- Zeit --------------------------------------------------------------- */
static const unsigned short mdy[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

static u32 local_now(void)
{
    unsigned d, t;
    fat_now(&d, &t);
    return fat2unix(d, t);
}

u32 krb_now(void)
{
    return local_now() + (u32)krb_off;
}

/* Unix-Zeit -> Jahr .. Sekunde */
static void civil(u32 t, unsigned *v)
{
    u32 r, days = au_div(t, 86400UL, &r);
    unsigned y = 1970, m, yl, s = (unsigned)(r >> 1);   /* halbe Sekunden < 43200 */
    v[3] = s / 1800;
    s = (s - v[3] * 1800U) * 2 + (unsigned)(r & 1);   /* Sek. in der Stunde */
    v[4] = s / 60;
    v[5] = s % 60;
    for (;;) {
        yl = (y & 3) == 0 ? 366 : 365;
        if (days < yl)
            break;
        days -= yl;
        y++;
    }
    for (m = 11; m > 0; m--)
        if ((unsigned)days >= mdy[m] + ((y & 3) == 0 && m >= 2))
            break;
    days -= mdy[m] + ((y & 3) == 0 && m >= 2);
    v[0] = y;
    v[1] = m + 1;
    v[2] = (unsigned)days + 1;
}

static void d2(char *p, unsigned v)
{
    p[0] = (char)('0' + v / 10);
    p[1] = (char)('0' + v % 10);
}

/* KerberosTime "YYYYMMDDHHMMSSZ" */
static void gtime(u32 t, char *o)
{
    unsigned v[6], i;
    civil(t, v);
    d2(o, v[0] / 100);
    d2(o + 2, v[0] % 100);
    for (i = 1; i < 6; i++)
        d2(o + 2 * i + 2, v[i]);
    o[14] = 'Z';
}

static u32 ptime(const u8 *p, unsigned n)
{
    unsigned v[6], i, y, k;
    u32 days;
    if (n < 14)
        return 0;
    for (i = 0; i < 6; i++)
        v[i] = (p[2 * i + (i ? 2 : 0)] - '0') * 10 + (p[2 * i + 1 + (i ? 2 : 0)] - '0');
    y = ((p[0] - '0') * 10 + (p[1] - '0')) * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    v[0] = y;
    if (v[1] < 1 || v[1] > 12)
        return 0;
    days = 0;
    for (k = 1970; k < y; k++)
        days += (k & 3) == 0 ? 366 : 365;
    days += mdy[v[1] - 1] + v[2] - 1 + ((y & 3) == 0 && v[1] > 2);
    return (au_mul16k((unsigned)days, 43200U) << 1) + au_mul16k(v[3], 3600) + v[4] * 60U + v[5];
}

void krb_tstr(u32 t, char *o)
{
    unsigned v[6];
    civil(t - (u32)krb_off, v);
    d2(o, v[2]); o[2] = '.';
    d2(o + 3, v[1]); o[5] = '.';
    d2(o + 6, v[0] / 100); d2(o + 8, v[0] % 100); o[10] = ' ';
    d2(o + 11, v[3]); o[13] = ':';
    d2(o + 14, v[4]); o[16] = ':';
    d2(o + 17, v[5]); o[19] = 0;
}

/* ---- DER schreiben ------------------------------------------------------
   Vorwaerts mit Platzhalter: auf() schreibt Kennung + 3 Byte Laenge, zu()
   setzt die kuerzeste Laenge ein und rueckt den Inhalt nach.             */
static u8 *ab;
static unsigned ap, am, ad_, ast[16], aerr;

static void abeg(u8 *b, unsigned max) { ab = b; ap = 0; am = max; ad_ = 0; aerr = 0; }

struct asave { u8 *b; unsigned p, m, d, e, st[16]; };
static void apush(struct asave *v)
{
    v->b = ab; v->p = ap; v->m = am; v->d = ad_; v->e = aerr;
    cr_cpy(v->st, ast, sizeof(ast));
}
static void apop(const struct asave *v)
{
    unsigned e = aerr;
    ab = v->b; ap = v->p; am = v->m; ad_ = v->d; aerr = v->e | e;
    cr_cpy(ast, v->st, sizeof(ast));
}

static void araw(const void *d, unsigned n)
{
    if (ap + n > am) {
        aerr = 1;
        return;
    }
    cr_cpy(ab + ap, d, n);
    ap += n;
}

static void auf(u8 tag)
{
    u8 h[4];
    h[0] = tag;
    h[1] = h[2] = h[3] = 0;
    if (ad_ < 16)
        ast[ad_++] = ap;
    araw(h, 4);
}

static void zu(void)
{
    unsigned s, n, hl, i;
    if (!ad_ || aerr)
        return;
    s = ast[--ad_];
    n = ap - s - 4;
    hl = n < 128 ? 1 : n < 256 ? 2 : 3;
    if (hl == 1)
        ab[s + 1] = (u8)n;
    else if (hl == 2) {
        ab[s + 1] = 0x81;
        ab[s + 2] = (u8)n;
    } else {
        ab[s + 1] = 0x82;
        ab[s + 2] = (u8)(n >> 8);
        ab[s + 3] = (u8)n;
    }
    for (i = 0; i < n; i++)             /* Inhalt nach vorn ruecken       */
        ab[s + 1 + hl + i] = ab[s + 4 + i];
    ap -= 3 - hl;
}

static void aprim(u8 tag, const void *d, unsigned n)
{
    auf(tag);
    araw(d, n);
    zu();
}

static void aint(u32 v)
{
    u8 b[5];
    unsigned n = 0, i;
    b[0] = 0;
    for (i = 0; i < 4; i++)
        b[1 + i] = (u8)(v >> (24 - 8 * i));
    i = 0;                              /* kuerzeste Form (mit Vorzeichen) */
    while (i < 4 && b[i] == 0 && !(b[i + 1] & 0x80))
        i++;
    n = 5 - i;
    aprim(0x02, b + i, n);
}

static void actx_int(u8 ctx, u32 v) { auf(ctx); aint(v); zu(); }

static void astr(u8 ctx, const char *s)  /* [ctx] GeneralString          */
{
    auf(ctx);
    aprim(0x1B, s, slen(s));
    zu();
}

static void aoct(u8 ctx, const u8 *d, unsigned n)
{
    auf(ctx);
    aprim(0x04, d, n);
    zu();
}

static void atime(u8 ctx, u32 t)
{
    char g[15];
    gtime(t, g);
    auf(ctx);
    aprim(0x18, g, 15);
    zu();
}

static void abits(u8 ctx, u32 v)
{
    u8 b[5];
    b[0] = 0;
    b[1] = (u8)(v >> 24);
    b[2] = (u8)(v >> 16);
    b[3] = (u8)(v >> 8);
    b[4] = (u8)v;
    auf(ctx);
    aprim(0x03, b, 5);
    zu();
}

/* PrincipalName: Typ, Teile durch '/' getrennt */
static void aname(u8 ctx, unsigned type, const char *s)
{
    char t[96];
    unsigned k;
    auf(ctx);
    auf(0x30);
    actx_int(0xA0, type);
    auf(0xA1);
    auf(0x30);
    while (*s) {
        for (k = 0; s[k] && s[k] != '/' && k < sizeof(t) - 1; k++)
            t[k] = s[k];
        t[k] = 0;
        aprim(0x1B, t, k);
        s += k;
        if (*s == '/')
            s++;
    }
    zu(); zu(); zu(); zu();
}

/* EncryptedData */
static void aenc(u8 ctx, u16 etype, const u8 *c, unsigned n)
{
    auf(ctx);
    auf(0x30);
    actx_int(0xA0, etype);
    aoct(0xA2, c, n);
    zu();
    zu();
}

/* ---- DER lesen ---------------------------------------------------------- */
struct tlv { const u8 *p; unsigned n; u8 tag; };

/* naechstes Element ab *pp (bis e) -> 0 ok */
static int tget(const u8 **pp, const u8 *e, struct tlv *t)
{
    const u8 *p = *pp;
    unsigned n, k;
    if (p + 2 > e)
        return 1;
    t->tag = *p++;
    n = *p++;
    if (n & 0x80) {
        k = n & 0x7F;
        if (k < 1 || k > 2 || p + k > e)
            return 1;
        n = *p++;
        if (k == 2)
            n = n << 8 | *p++;
    }
    if (n > (unsigned)(e - p))
        return 1;
    t->p = p;
    t->n = n;
    *pp = p + n;
    return 0;
}

/* Element mit Kennung tag in der Folge (Inhalt t) */
static int tfind(const struct tlv *seq, u8 tag, struct tlv *t)
{
    const u8 *p = seq->p, *e = seq->p + seq->n;
    while (p < e) {
        if (tget(&p, e, t))
            return 1;
        if (t->tag == tag)
            return 0;
    }
    return 1;
}

/* Inhalt eines [ctx]-Elements = genau ein inneres Element */
static int tin(const struct tlv *seq, u8 ctx, struct tlv *t)
{
    struct tlv o;
    const u8 *p;
    if (tfind(seq, ctx, &o))
        return 1;
    p = o.p;
    return tget(&p, o.p + o.n, t);
}

static u32 tint(const struct tlv *t)
{
    u32 v = (t->n && (t->p[0] & 0x80)) ? 0xFFFFFFFFUL : 0;
    unsigned i;
    for (i = 0; i < t->n && i < 4; i++)
        v = v << 8 | t->p[i];
    return v;
}

static int tctx_int(const struct tlv *seq, u8 ctx, u32 *v)
{
    struct tlv t;
    if (tin(seq, ctx, &t) || t.tag != 0x02)
        return 1;
    *v = tint(&t);
    return 0;
}

static void tstr(const struct tlv *t, char *o, unsigned max)
{
    unsigned n = t->n < max - 1 ? t->n : max - 1;
    cr_cpy(o, t->p, n);
    o[n] = 0;
}

/* PrincipalName -> "a/b" */
static void tname(const struct tlv *pn, char *o, unsigned max)
{
    struct tlv l, s;
    const u8 *p;
    unsigned n = 0, k;
    o[0] = 0;
    if (tin(pn, 0xA1, &l) || l.tag != 0x30)
        return;
    for (p = l.p; !tget(&p, l.p + l.n, &s);) {
        if (n && n < max - 1)
            o[n++] = '/';
        for (k = 0; k < s.n && n < max - 1; k++)
            o[n++] = (char)s.p[k];
    }
    o[n] = 0;
}

/* ---- KDC ueber TCP ------------------------------------------------------ */
static struct fapi_socket so;
static struct fapi_sockio io;
static u8 *rq, *rp;
static unsigned rpl;

void krb_bufs(u8 *q, u8 *r) { rq = q; rp = r; }

static int xfer(unsigned h, unsigned fn, u8 *d, unsigned n)
{
    unsigned r;
    while (n) {
        io.h = h;
        io.buf = (unsigned)d;
        io.len = n;
        io.flags = 0;
        io.timeout = 10000;
        r = DosNet(fn, &io, sizeof(io));
        if (r == 0 || FAPI_FAILED(r))
            return 1;
        d += r;
        n -= r;
    }
    return 0;
}

/* rq[0..n) an den KDC, Antwort nach rp/rpl -> 0 oder Meldung */
static const char *kdc_call(unsigned n)
{
    unsigned h, k;
    u8 l[4];
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(h))
        return "Kein Netz (TCPIP.SYS fehlt) oder kein Socket frei.";
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = (unsigned short)(88 << 8);   /* htons(88)             */
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = krb_kdc[k];
    io.h = h;
    io.timeout = 10000;
    if (DosNet(NET_CONNECT, &io, sizeof(io))) {
        DosClose(h);
        return "KDC nicht erreichbar (TCP-Port 88).";
    }
    l[0] = l[1] = 0;
    l[2] = (u8)(n >> 8);
    l[3] = (u8)n;
    if (xfer(h, NET_SEND, l, 4) || xfer(h, NET_SEND, rq, n) ||
        xfer(h, NET_RECV, l, 4)) {
        DosClose(h);
        return "Keine Antwort vom KDC.";
    }
    rpl = (unsigned)l[2] << 8 | l[3];
    if (l[0] || l[1] || rpl > KBUF || xfer(h, NET_RECV, rp, rpl)) {
        DosClose(h);
        return "Antwort des KDC zu gross oder abgebrochen.";
    }
    DosClose(h);
    return 0;
}

/* KRB-ERROR (Kennung 7Eh) auswerten: Code, stime -> Uhr stellen */
static u32 kerr_code;
static struct tlv kerr_edata;

static int krb_error(const struct tlv *top)
{
    struct tlv s, t;
    const u8 *p = top->p;
    u32 st;
    if (top->tag != 0x7E || tget(&p, top->p + top->n, &s) || s.tag != 0x30)
        return 0;
    kerr_code = 0xFFFF;
    tctx_int(&s, 0xA6, &kerr_code);
    if (!tin(&s, 0xA4, &t) && t.tag == 0x18 && (st = ptime(t.p, t.n)) != 0)
        krb_off = (long)(st - local_now());
    kerr_edata.n = 0;
    if (!tin(&s, 0xAC, &t) && t.tag == 0x04)
        kerr_edata = t;
    return 1;
}

static const char *kerr_text(void)
{
    switch ((unsigned)kerr_code) {
    case 6:  return "Benutzer in der Dom\x84ne unbekannt.";
    case 7:  return "Dienst in der Dom\x84ne unbekannt (Rechnername?).";
    case 12: return "Anmeldung gesperrt (Richtlinie).";
    case 14: return "Der KDC kennt kein AES-256 f\x81r dieses Konto.";
    case 18: return "Konto gesperrt oder abgelaufen.";
    case 23: return "Kennwort abgelaufen.";
    case 24: return "Kennwort falsch.";
    case 37: return "Uhrzeit weicht zu stark ab.";
    case 68: return "Falsche Dom\x84ne (Realm).";
    }
    return "Der KDC hat abgelehnt.";
}

/* Antwort AS-REP/TGS-REP (Kennung rtag) -> Ticket und entschluesselten
   Teil; kusage = Schluesselverwendung, key = Schluessel dafuer */
static const char *kdc_rep(u8 rtag, const u8 *key, u32 kusage, u32 nonce,
                           struct kcred *c)
{
    struct tlv top, s, t, e, ek, kv;
    const u8 *p = rp;
    u8 *pt;
    unsigned pl;
    u32 v;
    if (tget(&p, rp + rpl, &top))
        return "Unverst\x84ndliche Antwort des KDC.";
    if (krb_error(&top))
        return kerr_text();
    p = top.p;
    if (top.tag != rtag || tget(&p, top.p + top.n, &s) || s.tag != 0x30)
        return "Unverst\x84ndliche Antwort des KDC.";
    if (!tin(&s, 0xA3, &t))
        tstr(&t, c->realm, sizeof(c->realm));
    if (!tfind(&s, 0xA4, &t)) {
        struct tlv pn;
        const u8 *q = t.p;
        if (!tget(&q, t.p + t.n, &pn))
            tname(&pn, c->cname, sizeof(c->cname));
    }
    if (tfind(&s, 0xA5, &t) || t.n > KTKT)
        return "Ticket fehlt oder ist zu gross.";
    cr_cpy(c->tkt, t.p, t.n);
    c->tlen = t.n;
    if (tin(&s, 0xA6, &e) || e.tag != 0x30 || tctx_int(&e, 0xA0, &v) || v != 18 ||
        tin(&e, 0xA2, &t) || t.tag != 0x04)
        return "Antwort nicht mit AES-256 verschl\x81sselt.";
    if (krb_dec(key, kusage, (u8 *)t.p, t.n, &pt, &pl))
        return "Antwort l\x84sst sich nicht entschl\x81sseln (Kennwort falsch?).";
    /* EncKDCRepPart [APPLICATION 25/26] */
    p = pt;
    if (tget(&p, pt + pl, &t) || (t.tag != 0x79 && t.tag != 0x7A))
        return "Antwort unverst\x84ndlich (entschl\x81sselt).";
    p = t.p;
    if (tget(&p, t.p + t.n, &s) || s.tag != 0x30)
        return "Antwort unverst\x84ndlich (entschl\x81sselt).";
    if (tin(&s, 0xA0, &ek) || tctx_int(&ek, 0xA0, &v) || v != 18 ||
        tin(&ek, 0xA1, &kv) || kv.tag != 0x04 || kv.n != 32)
        return "Sitzungsschl\x81ssel ist nicht AES-256.";
    cr_cpy(c->key, kv.p, 32);
    c->etype = 18;
    if (tctx_int(&s, 0xA2, &v) || v != nonce)
        return "Antwort passt nicht zur Anfrage (Nonce).";
    c->auth = c->end = c->renew = 0;
    if (!tin(&s, 0xA5, &t)) c->auth = ptime(t.p, t.n);
    if (!tin(&s, 0xA7, &t)) c->end = ptime(t.p, t.n);
    if (!tin(&s, 0xA8, &t)) c->renew = ptime(t.p, t.n);
    if (!tfind(&s, 0xAA, &t)) {         /* sname                         */
        struct tlv pn;
        const u8 *q = t.p;
        if (!tget(&q, t.p + t.n, &pn))
            tname(&pn, c->sname, sizeof(c->sname));
    }
    return 0;
}

static u32 rnd31(void)
{
    u8 b[4];
    tls_random(b, 4);
    return ((u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3]) & 0x7FFFFFFFUL;
}

static void upcase(char *d, const char *s, unsigned max)
{
    unsigned i;
    for (i = 0; s[i] && i < max - 1; i++)
        d[i] = (char)up(s[i]);
    d[i] = 0;
}

/* KDC-REQ-BODY nach bb (fuer AS und TGS gleich bis auf cname) */
static u8 bb[512];
static unsigned bbl;

static void req_body(const char *cname, const char *realm, unsigned stype,
                     const char *sname, u32 nonce)
{
    static struct asave sv;
    apush(&sv);
    abeg(bb, sizeof(bb));
    auf(0x30);
    abits(0xA0, 0x40810010UL);          /* forwardable renewable canonicalize renewable-ok */
    if (cname)
        aname(0xA1, 1, cname);
    astr(0xA2, realm);
    aname(0xA3, stype, sname);
    atime(0xA5, krb_now() + 36000UL);   /* till: 10 Std.                 */
    atime(0xA6, krb_now() + 7UL * 86400);
    actx_int(0xA7, nonce);
    auf(0xA8);
    auf(0x30);
    aint(18);
    zu();
    zu();
    zu();
    bbl = ap;
    apop(&sv);
}

static void as_req(const u8 *pa, unsigned pal)
{
    abeg(rq, KBUF);
    auf(0x6A);
    auf(0x30);
    actx_int(0xA1, 5);
    actx_int(0xA2, 10);
    if (pa) {
        auf(0xA3);
        auf(0x30);
        auf(0x30);
        actx_int(0xA1, 2);              /* PA-ENC-TIMESTAMP              */
        aoct(0xA2, pa, pal);
        zu();
        zu();
        zu();
    }
    auf(0xA4);
    araw(bb, bbl);
    zu();
    zu();
    zu();
}

const char *krb_kinit(const char *user, const char *realm, const char *pw,
                      struct kcred *tgt)
{
    static char rl[64], salt[160], sn[80];
    static u8 key[32], ts[64], enc[96];
    struct tlv top, md, pa, v, ent, t;
    const u8 *p;
    u32 nonce = rnd31(), iter = 4096, et;
    unsigned tl, i;
    const char *e;

    upcase(rl, realm, sizeof(rl));
    scopy(sn, "krbtgt/", sizeof(sn));
    scopy(sn + 7, rl, sizeof(sn) - 7);
    cr_zero(tgt, sizeof(*tgt));
    scopy(tgt->cname, user, sizeof(tgt->cname));
    scopy(tgt->realm, rl, sizeof(tgt->realm));
    scopy(salt, rl, sizeof(salt));      /* Vorgabe-Salz: REALM + Name    */
    scopy(salt + slen(salt), user, sizeof(salt) - slen(salt));

    /* 1. ohne Vorauthentisierung: liefert Uhrzeit, Salz, Iterationen */
    req_body(user, rl, 2, sn, nonce);
    as_req(0, 0);
    if (aerr)
        return "Name zu lang.";
    if ((e = kdc_call(ap)) != 0)
        return e;
    p = rp;
    if (tget(&p, rp + rpl, &top))
        return "Unverst\x84ndliche Antwort des KDC.";
    if (top.tag == 0x7E) {
        krb_error(&top);
        if (kerr_code != 25)            /* PREAUTH_REQUIRED erwartet     */
            return kerr_text();
        /* METHOD-DATA: PA-ETYPE-INFO2 (19) suchen */
        p = kerr_edata.p;
        if (kerr_edata.n && !tget(&p, kerr_edata.p + kerr_edata.n, &md) && md.tag == 0x30) {
            const u8 *q = md.p;
            while (!tget(&q, md.p + md.n, &pa)) {
                u32 ty;
                if (pa.tag != 0x30 || tctx_int(&pa, 0xA1, &ty) || ty != 19 ||
                    tin(&pa, 0xA2, &v) || v.tag != 0x04)
                    continue;
                {
                    const u8 *r = v.p;
                    struct tlv lst;
                    if (tget(&r, v.p + v.n, &lst))
                        break;
                    r = lst.p;
                    while (!tget(&r, lst.p + lst.n, &ent)) {
                        if (tctx_int(&ent, 0xA0, &et) || et != 18)
                            continue;
                        if (!tin(&ent, 0xA1, &t))
                            tstr(&t, salt, sizeof(salt));
                        if (!tin(&ent, 0xA2, &t) && t.n == 4)
                            iter = (u32)t.p[0] << 24 | (u32)t.p[1] << 16 |
                                   (u32)t.p[2] << 8 | t.p[3];
                    }
                }
            }
        }
        if (iter > 100000UL)
            return "Zu viele Iterationen verlangt.";
        krb_s2k(pw, slen(pw), salt, slen(salt), iter, key);

        /* 2. mit PA-ENC-TIMESTAMP (Verwendung 1) */
        {
            static u8 ed[128];
            u8 conf[16];
            abeg(ts, sizeof(ts));
            auf(0x30);
            atime(0xA0, krb_now());
            actx_int(0xA1, rnd31() >> 12);
            zu();
            tl = ap;
            tls_random(conf, 16);
            krb_enc(key, 1, conf, ts, tl, enc);
            abeg(ed, sizeof(ed));
            auf(0x30);
            actx_int(0xA0, 18);
            aoct(0xA2, enc, tl + 28);
            zu();
            i = ap;
            nonce = rnd31();
            req_body(user, rl, 2, sn, nonce);
            as_req(ed, i);
        }
        if (aerr)
            return "Anfrage zu lang.";
        if ((e = kdc_call(ap)) != 0)
            return e;
    } else
        return "Der KDC verlangt keine Vorauthentisierung (nicht unterst\x81tzt).";
    e = kdc_rep(0x6B, key, 3, nonce, tgt);
    cr_zero(key, 32);
    if (!e && !tgt->sname[0])
        scopy(tgt->sname, sn, sizeof(tgt->sname));
    return e;
}

/* ---- AP-REQ ------------------------------------------------------------- */
/* Authenticator verschluesselt (Verwendung us) als AP-REQ ab ap in ab.
   ck: Pruefsumme (Typ ckt, n Byte) oder 0 */
static void ap_req(const struct kcred *c, u32 us, u32 ckt, const u8 *ck, unsigned ckn)
{
    static u8 au[512], en[560];
    static struct asave sv;
    u8 conf[16];
    unsigned al;
    apush(&sv);
    abeg(au, sizeof(au));
    auf(0x62);
    auf(0x30);
    actx_int(0xA0, 5);
    astr(0xA1, c->realm);
    aname(0xA2, 1, c->cname);
    if (ck) {
        auf(0xA3);
        auf(0x30);
        actx_int(0xA0, ckt);
        aoct(0xA1, ck, ckn);
        zu();
        zu();
    }
    actx_int(0xA4, (rnd31() >> 12));
    atime(0xA5, krb_now());
    zu();
    zu();
    al = ap;
    apop(&sv);
    tls_random(conf, 16);
    krb_enc(c->key, us, conf, au, al, en);
    auf(0x6E);
    auf(0x30);
    actx_int(0xA0, 5);
    actx_int(0xA1, 14);
    abits(0xA2, 0);
    auf(0xA3);
    araw(c->tkt, c->tlen);
    zu();
    aenc(0xA4, 18, en, al + 28);
    zu();
    zu();
}

const char *krb_tgs(const struct kcred *tgt, const char *svc, const char *host,
                    struct kcred *out)
{
    static char sn[96];
    u8 ck[12], *pa = rp;                /* AP-REQ vorlaeufig im Antwortpuffer */
    u32 nonce = rnd31();
    unsigned pl, n;
    const char *e;
    scopy(sn, svc, sizeof(sn));
    n = slen(sn);
    sn[n++] = '/';
    for (; *host && n < sizeof(sn) - 1; host++)
        sn[n++] = (char)(*host >= 'A' && *host <= 'Z' ? *host + 32 : *host);
    sn[n] = 0;
    req_body(0, tgt->realm, 2, sn, nonce);
    krb_cksum(tgt->key, 6, bb, bbl, ck);
    abeg(pa, KBUF);
    ap_req(tgt, 7, 16, ck, 12);         /* hmac-sha1-96-aes256           */
    pl = ap;
    if (aerr)
        return "Ticket zu gross.";
    abeg(rq, KBUF);
    auf(0x6C);
    auf(0x30);
    actx_int(0xA1, 5);
    actx_int(0xA2, 12);
    auf(0xA3);
    auf(0x30);
    auf(0x30);
    actx_int(0xA1, 1);                  /* PA-TGS-REQ                    */
    aoct(0xA2, pa, pl);
    zu();
    zu();
    zu();
    auf(0xA4);
    araw(bb, bbl);
    zu();
    zu();
    zu();
    if (aerr)
        return "Anfrage zu lang.";
    if ((e = kdc_call(ap)) != 0)
        return e;
    cr_zero(out, sizeof(*out));
    scopy(out->realm, tgt->realm, sizeof(out->realm));
    scopy(out->cname, tgt->cname, sizeof(out->cname));
    e = kdc_rep(0x6D, tgt->key, 8, nonce, out);
    if (!e && !out->sname[0])
        scopy(out->sname, sn, sizeof(out->sname));
    return e;
}

/* GSS-Huelle (RFC 4121 4.1): 60 { OID krb5, 01 00, AP-REQ }; Pruefsumme
   0x8003: Laenge 16, Kanalbindung 0, Merker (Integritaet + Vertraulich-
   keit, ohne gegenseitige Anmeldung -> kein AP-REP, Sitzungsschluessel =
   der des Tickets) */
static const u8 oid_krb5[] = { 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x12, 0x01, 0x02, 0x02 };

unsigned krb_gss(const struct kcred *c, u8 *out, unsigned max)
{
    u8 ck[24];
    cr_zero(ck, 24);
    ck[0] = 16;
    ck[20] = 0x30;                      /* GSS_C_CONF | GSS_C_INTEG      */
    abeg(out, max);
    auf(0x60);
    araw(oid_krb5, sizeof(oid_krb5));
    araw("\x01\x00", 2);
    ap_req(c, 11, 0x8003UL, ck, 24);
    zu();
    return aerr ? 0 : ap;
}

/* ---- Ablage ------------------------------------------------------------- */
void krb_cc_path(char *p, unsigned max)
{
    unsigned n;
    if (!FAPI_FAILED(DosGetEnv("KRB5CCNAME", p, max)) && p[0])
        return;
    if (FAPI_FAILED(DosGetEnv("TEMP", p, max - 14)) || !p[0])
        scopy(p, "C:", max);
    n = slen(p);
    if (p[n - 1] != '\\')
        p[n++] = '\\';
    scopy(p + n, "KRB5CC.DAT", max - n);
}

static struct kchdr hd;

static unsigned cc_seek(unsigned h, unsigned i, unsigned add)
{
    unsigned long pos = sizeof(hd) + au_mul16k(i, sizeof(struct kcred)) + add;
    return DosSeek(h, &pos);
}

/* nur sname von Eintrag i lesen */
static int cc_sname(unsigned i, char *sn)
{
    char p[FAPI_PATHLEN];
    unsigned h, n;
    struct kcred *z = 0;
    krb_cc_path(p, sizeof(p));
    h = DosOpen(p);
    if (FAPI_FAILED(h))
        return 1;
    cc_seek(h, i, (unsigned)z->sname);
    n = DosRead(h, sn, sizeof(z->sname));
    DosClose(h);
    sn[sizeof(z->sname) - 1] = 0;
    return n != sizeof(z->sname);
}

int krb_cc_open(void)
{
    char p[FAPI_PATHLEN];
    unsigned h, n;
    krb_cc_path(p, sizeof(p));
    h = DosOpen(p);
    if (FAPI_FAILED(h))
        return -1;
    n = DosRead(h, &hd, sizeof(hd));
    DosClose(h);
    if (n != sizeof(hd) || !cr_eq(hd.magic, "ATKRB5\0\1", 8) || hd.n > KNCRED)
        return -1;
    krb_off = hd.off;
    cr_cpy(krb_kdc, hd.kdc, 16);
    return hd.n;
}

int krb_cc_get(unsigned i, struct kcred *c)
{
    char p[FAPI_PATHLEN];
    unsigned h, n;
    if (i >= hd.n)
        return 1;
    krb_cc_path(p, sizeof(p));
    h = DosOpen(p);
    if (FAPI_FAILED(h))
        return 1;
    cc_seek(h, i, 0);
    n = DosRead(h, c, sizeof(*c));
    DosClose(h);
    return n != sizeof(*c);
}

int krb_cc_find(const char *sname, struct kcred *c)
{
    static char sn[96];
    unsigned i;
    for (i = 0; i < hd.n; i++)
        if (!cc_sname(i, sn) && same(sn, sname))
            return krb_cc_get(i, c) ? -1 : (int)i;
    return -1;
}

static int cc_write(unsigned i, const struct kcred *c, int create)
{
    char p[FAPI_PATHLEN];
    unsigned h, r;
    krb_cc_path(p, sizeof(p));
    h = create ? DosCreate(p) : DosOpenRW(p);
    if (FAPI_FAILED(h))
        return 1;
    cr_cpy(hd.magic, "ATKRB5\0\1", 8);
    hd.off = krb_off;
    cr_cpy(hd.kdc, krb_kdc, 16);
    r = DosWrite(h, &hd, sizeof(hd)) != sizeof(hd);
    if (!r) {
        cc_seek(h, i, 0);
        r = DosWrite(h, c, sizeof(*c)) != sizeof(*c);
    }
    DosClose(h);
    return r;
}

int krb_cc_init(const struct kcred *tgt)
{
    hd.n = 1;
    return cc_write(0, tgt, 1);
}

int krb_cc_put(const struct kcred *c)
{
    static char sn[96];
    unsigned i;
    for (i = 1; i < hd.n; i++)          /* Eintrag 0 = TGT bleibt          */
        if (!cc_sname(i, sn) && same(sn, c->sname))
            break;
    if (i >= KNCRED)
        i = KNCRED - 1;                 /* letzten Dienst ersetzen        */
    if (i >= hd.n)
        hd.n = i + 1;
    return cc_write(i, c, 0);
}
