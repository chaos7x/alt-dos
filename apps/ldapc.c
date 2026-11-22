/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - LDAP-Client fuer DOMJOIN und LOGON (aus task_domjoin.c
 * herausgeloest): Verbindung zum DC (Adresse des KDC, TCP 389), rootDSE,
 * SASL GSSAPI mit Vertraulichkeit (RFC 4752), Nachrichten als BER ueber
 * krb5.c (der_*). Das Programm stellt fail() und das TGT in tgt.
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "util.h"
#include "krb5.h"
#include "ldapc.h"

#define DosNet(fn, p, n)    fapi_call(FAPI_NET, (fn), (unsigned short)(p), (n))

char base[160], dchost[96], diag[160];
struct kcred tgt, svc;
u8 sb[LBUF + 64], tb[LBUF + 64], rb[LBUF + 64];
unsigned sh, msgid, sealed;
struct tlv creds;
unsigned ld_nocache;
static struct fapi_socket so;
static struct fapi_sockio io;
static u8 *rp_;                         /* noch nicht gelesene Nachrichten */
static unsigned rn_;

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

/* ---- Verbindung ---------------------------------------------------------- */
static int xfer(unsigned fn, u8 *d, unsigned n)
{
    unsigned r;
    while (n) {
        io.h = sh;
        io.buf = (unsigned)d;
        io.len = n;
        io.flags = 0;
        io.timeout = 15000;
        r = DosNet(fn, &io, sizeof(io));
        if (r == 0 || FAPI_FAILED(r))
            return 1;
        d += r;
        n -= r;
    }
    return 0;
}

int cr_cmp(const void *a, const void *b, unsigned n)
{
    return !cr_eq(a, b, n);
}

char *utoa(unsigned v, char *z)
{
    char t[12];
    unsigned i = 0, k = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i)
        z[k++] = t[--i];
    z[k] = 0;
    return z;
}

void lconnect(void)
{
    unsigned k, i = 0;
    for (;;) {
        so.family = AF_INET6;
        so.type = SOCK_STREAM;
        sh = DosNet(NET_SOCKET, &so, sizeof(so));
        if (FAPI_FAILED(sh)) {
            sh = 0;
            fail("Kein Netz (TCPIP.SYS fehlt) oder kein Socket frei.", 1);
        }
        io.addr.len = 28;
        io.addr.family = AF_INET6;
        io.addr.port = (unsigned short)(389 << 8 | 389 >> 8);
        for (k = 0; k < 16; k++)
            io.addr.a6[k] = krb_kdc[k];
        io.h = sh;
        io.timeout = krb_nkdc > 1 ? 4000 : 10000;
        if (!DosNet(NET_CONNECT, &io, sizeof(io)))
            return;
        DosClose(sh);
        sh = 0;
        /* naechster DC der Liste (krb5.c), vom letzten wieder von vorn */
        if (++i >= krb_nkdc)
            fail("Dom\x84nencontroller nicht erreichbar (LDAP, TCP-Port 389).", 1);
        if (!krb_kdc_next())
            cr_cpy(krb_kdc, krb_kdcs[0], 16);
    }
}

/* Nachricht sb[0..n) senden, nach der Anmeldung in einer GSS-Huelle */
void lsend(unsigned n)
{
    u8 l[4];
    if (!n)
        fail("LDAP-Anfrage zu lang.", 1);
    if (sealed) {
        n = krb_wrap(svc.key, sb, n, tb);
        l[0] = 0; l[1] = 0; l[2] = (u8)(n >> 8); l[3] = (u8)n;
        if (xfer(NET_SEND, l, 4) || xfer(NET_SEND, tb, n))
            fail("Verbindung zum DC abgebrochen.", 1);
    } else if (xfer(NET_SEND, sb, n))
        fail("Verbindung zum DC abgebrochen.", 1);
}

/* naechste LDAP-Nachricht -> t (Inhalt der SEQUENCE) */
void lrecv(struct tlv *t)
{
    const u8 *p;
    unsigned n, k;
    u8 *pt;
    if (!rn_) {
        if (sealed) {
            if (xfer(NET_RECV, rb, 4))
                fail("Keine Antwort vom DC.", 1);
            n = (unsigned)rb[2] << 8 | rb[3];
            if (rb[0] || rb[1] || n > LBUF || xfer(NET_RECV, rb, n))
                fail("Antwort des DC zu gro\xE1 oder abgebrochen.", 1);
            if (krb_unwrap(svc.key, rb, n, &pt, &k))
                fail("Antwort des DC nicht entschl\x81sselbar.", 1);
            rp_ = pt;
            rn_ = k;
        } else {
            if (xfer(NET_RECV, rb, 2))
                fail("Keine Antwort vom DC.", 1);
            n = rb[1];
            k = 2;
            if (n & 0x80) {             /* lange Laengenangabe            */
                k += n & 0x7F;
                if ((n & 0x7F) > 3 || xfer(NET_RECV, rb + 2, n & 0x7F))
                    fail("Antwort des DC ung\x81ltig.", 1);
                n = rb[k - 1] | (unsigned)(k > 3 ? rb[k - 2] : 0) << 8;
                if (k > 4 && rb[k - 3])
                    n = LBUF + 1;
            }
            if (n > LBUF - k || xfer(NET_RECV, rb + k, n))
                fail("Antwort des DC zu gro\xE1 oder abgebrochen.", 1);
            rp_ = rb;
            rn_ = n + k;
        }
    }
    p = rp_;
    if (der_get(&p, rp_ + rn_, t) || t->tag != 0x30)
        fail("Antwort des DC ung\x81ltig.", 1);
    rn_ -= (unsigned)(p - rp_);
    rp_ = (u8 *)p;
}

/* ---- LDAP ---------------------------------------------------------------- */
void oct(const char *s) { der_prim(0x04, s, slen(s)); }

void msg_beg(void)
{
    der_beg(sb, LBUF);
    der_open(0x30);
    der_num(0x02, ++msgid);
}

unsigned msg_end(void)
{
    der_close();
    return der_len();
}

/* LDAPResult der Antwort mit Kennung op: Code, Meldung, ggf. [7] creds */

u32 result(u8 op)
{
    struct tlv m, r, t;
    const u8 *p;
    unsigned k;
    lrecv(&m);
    p = m.p;
    if (der_get(&p, m.p + m.n, &t) || der_get(&p, m.p + m.n, &r) || r.tag != op)
        fail("Unerwartete Antwort des DC.", 1);
    p = r.p;
    creds.n = 0;
    diag[0] = 0;
    if (der_get(&p, r.p + r.n, &t) || t.tag != 0x0A)
        fail("Antwort des DC ung\x81ltig.", 1);
    k = (unsigned)der_uint(&t);
    der_get(&p, r.p + r.n, &t);         /* matchedDN                      */
    if (!der_get(&p, r.p + r.n, &t) && t.tag == 0x04) {
        unsigned n = t.n < sizeof(diag) - 1 ? t.n : sizeof(diag) - 1;
        cr_cpy(diag, t.p, n);
        diag[n] = 0;
    }
    while (p < r.p + r.n && !der_get(&p, r.p + r.n, &t))
        if (t.tag == 0x87)
            creds = t;
    return k;
}

void ldap_fail(const char *was, u32 code)
{
    char z[8];
    out(was);
    out(" (LDAP-Fehler ");
    out(utoa((unsigned)code, z));
    out(")");
    if (diag[0]) {
        out(": ");
        out(diag);
    }
    fail("", 1);
}

/* rootDSE anonym: defaultNamingContext, dnsHostName */
void rootdse(void)
{
    struct tlv m, e, t, a, v, s;
    const u8 *p, *q, *r;
    unsigned n;
    msg_beg();
    der_open(0x63);
    oct("");
    der_num(0x0A, 0);                   /* baseObject                     */
    der_num(0x0A, 0);
    der_num(0x02, 0);
    der_num(0x02, 0);
    der_prim(0x01, "\0", 1);
    der_prim(0x87, "objectClass", 11);  /* present                        */
    der_open(0x30);
    oct("defaultNamingContext");
    oct("dnsHostName");
    der_close();
    der_close();
    lsend(msg_end());
    for (;;) {
        lrecv(&m);
        p = m.p;
        if (der_get(&p, m.p + m.n, &t) || der_get(&p, m.p + m.n, &e))
            fail("Antwort des DC ung\x81ltig.", 1);
        if (e.tag == 0x65)              /* searchResDone                  */
            break;
        if (e.tag != 0x64)
            continue;
        q = e.p;
        if (der_get(&q, e.p + e.n, &t) || der_get(&q, e.p + e.n, &a))
            continue;
        for (q = a.p; !der_get(&q, a.p + a.n, &s);) {
            r = s.p;
            if (der_get(&r, s.p + s.n, &t) || der_get(&r, s.p + s.n, &v))
                continue;
            r = v.p;
            if (der_get(&r, v.p + v.n, &v) || v.tag != 0x04)
                continue;
            if (t.n == 20 && !cr_cmp(t.p, "defaultNamingContext", 20)) {
                n = v.n < sizeof(base) - 1 ? v.n : sizeof(base) - 1;
                cr_cpy(base, v.p, n);
                base[n] = 0;
            } else if (t.n == 11 && !cr_cmp(t.p, "dnsHostName", 11)) {
                n = v.n < sizeof(dchost) - 1 ? v.n : sizeof(dchost) - 1;
                cr_cpy(dchost, v.p, n);
                dchost[n] = 0;
            }
        }
    }
    if (!base[0] || !dchost[0])
        fail("Der DC nennt keine Dom\x84ne (rootDSE) - kein Active Directory?", 1);
}

void bind_req(const u8 *c, unsigned n)
{
    msg_beg();
    der_open(0x60);
    der_num(0x02, 3);
    oct("");
    der_open(0xA3);
    oct("GSSAPI");
    der_prim(0x04, c, n);
    der_close();
    der_close();
    lsend(msg_end());
}

/* SASL GSSAPI (RFC 4752) mit Sicherheitsstufe Vertraulichkeit */
void sasl_bind(void)
{
    const char *e;
    unsigned n, i, k;
    u32 code;
    u8 *pt, sel[4 + 16];
    static char sn[112];
    scopy(sn, "ldap/", sizeof(sn));
    lower(sn + 5, dchost, sizeof(sn) - 5);
    if (ld_nocache || krb_cc_find(sn, &svc) < 0 || krb_now() + 60 >= svc.end) {
        if ((e = krb_tgs(&tgt, "ldap", dchost, &svc)) != 0)
            fail(e, 1);
        if (!ld_nocache)
            krb_cc_put(&svc);
    }
    if (svc.etype != 18)
        fail("Der DC gibt f\x81r LDAP nur einen RC4-Schl\x81ssel aus - "
             "DOMJOIN braucht AES-256.", 1);
    n = krb_gss(&svc, tb, LBUF);
    if (!n)
        fail("Ticket zu gro\xE1.", 1);
    krb_gss_reset();
    bind_req(tb, n);
    for (i = 0; i < 4; i++) {
        code = result(0x61);
        if (code != 14)
            ldap_fail("Anmeldung am DC abgelehnt", code);
        if (creds.n >= 28 && creds.p[0] == 5 && creds.p[1] == 4)
            break;                      /* Angebot der Sicherheitsstufen  */
        bind_req(tb, 0);                /* (leeres Token weiter)          */
    }
    if (i == 4)
        fail("Anmeldung am DC: keine Sicherheitsstufe angeboten.", 1);
    cr_cpy(tb, creds.p, creds.n);
    if (krb_unwrap(svc.key, tb, creds.n, &pt, &k) || k != 4)
        fail("Anmeldung am DC: Angebot nicht pr\x81" "fbar.", 1);
    if (!(pt[0] & 4))
        fail("Der DC bietet keine verschl\x81sselte Verbindung an.", 1);
    sel[0] = 4;                         /* Vertraulichkeit, Puffer 8 KB   */
    sel[1] = 0;
    sel[2] = 0x20;
    sel[3] = 0;
    n = krb_wrap(svc.key, sel, 4, tb);
    bind_req(tb, n);
    code = result(0x61);
    if (code)
        ldap_fail("Anmeldung am DC abgelehnt", code);
    sealed = 1;
}

void lower(char *d, const char *s, unsigned max)
{
    unsigned i;
    for (i = 0; s[i] && i < max - 1; i++)
        d[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    d[i] = 0;
}


void ldap_close(void)
{
    msgid++;
    der_beg(sb, LBUF);                  /* unbind                         */
    der_open(0x30);
    der_num(0x02, msgid);
    der_prim(0x42, "", 0);
    der_close();
    lsend(der_len());
    DosClose(sh);
    sh = 0;
    sealed = 0;
}

/* ---- DOMAIN.KEY ----------------------------------------------------------- */
int dj_path(char *p, unsigned max)
{
    if (FAPI_FAILED(DosGetEnv("COMSPEC", p, max)) || p[1] != ':')
        return 1;
    p[2] = 0;
    scat(p, "\\DOS\\SYSTEM\\DOMAIN.KEY", max);
    return 0;
}

int dj_read(struct djfile *d, const char *p)
{
    unsigned h, n;
    h = DosOpen(p);
    if (FAPI_FAILED(h))
        return 1;
    cr_zero(d, sizeof(*d));
    n = DosRead(h, d, sizeof(*d));
    DosClose(h);
    if ((n != sizeof(*d) && n != DJ_OLDSIZE) || cr_cmp(d->magic, "ATDOMJ1", 8))
        return 1;
    if (n == DJ_OLDSIZE)
        d->pwset = d->joined;
    return 0;
}
