/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DOMJOIN.EXE - Rechner einer AD-Domaene beitreten (AT-DOS 6.3)
 *
 *   DOMJOIN Name [/OU:DN]   Rechnerkonto anlegen (oder Kennwort neu setzen)
 *   DOMJOIN /LEAVE          Rechnerkonto loeschen, Daten entfernen
 *   DOMJOIN                 Stand zeigen
 *
 * Vorher KINIT mit einem Konto, das Rechner anlegen darf (z. B.
 * Administrator). Ablauf wie bei Windows/Samba: LDAP zum DC (Adresse des
 * KDC aus der Ticket-Ablage), rootDSE anonym lesen (dnsHostName,
 * defaultNamingContext), Anmeldung per SASL GSSAPI mit dem Dienstticket
 * ldap/<dc> und verschluesselter Verbindung (RFC 4752, Sicherheitsstufe
 * Vertraulichkeit - AD setzt unicodePwd nur so), dann addRequest
 * CN=Name,CN=Computers,<Basis> mit sAMAccountName Name$,
 * userAccountControl 4096 (Arbeitsstation), dNSHostName, SPN HOST/... und
 * zufaelligem Kennwort. Gibt es das Konto schon, wird nur das Kennwort
 * neu gesetzt (wie ein erneuter Beitritt unter Windows). Zur Probe meldet
 * sich DOMJOIN danach als Name$ beim KDC an.
 *
 * Ergebnis in X:\DOS\SYSTEM\DOMAIN.KEY (Startlaufwerk, versteckt+System):
 * Domaene, Realm, Name, DC, DN, Kennwort und AES-256-Schluessel des
 * Rechners - Grundlage fuer die Anmeldung beim Start und den Wechsel des
 * Maschinenkennworts. Wer die Datei lesen kann, kann sich als der Rechner
 * ausgeben (wie secrets.tdb bei Samba).
 *
 * LDAP (RFC 4511) als BER ueber krb5.c (der_*), GSS-Huellen krb_wrap/
 * krb_unwrap. ERRORLEVEL: 0 ok, 1 Fehler, 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#define NET_NO_PTON
#include "net.h"
#include "tls.h"
#include "krb5.h"

#define LBUF    8192

struct djfile {
    char magic[8];                      /* "ATDOMJ1"                     */
    char realm[64], domain[64], name[16], dc[96], dn[192], pw[48];
    u8   key[32];                       /* aes256 des Rechners           */
    u32  joined;                        /* Unix-Zeit                     */
};

static char args[FAPI_ARGLEN], w[256], ou[192], kpath[FAPI_PATHLEN];
static char base[160], dchost[96], diag[160], ts[20];
static struct djfile df;
static struct kcred tgt, svc, mc;
static u8 kb1[KBUF], kb2[KBUF];
static u8 sb[LBUF + 64], tb[LBUF + 64], rb[LBUF + 64];
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned sh, msgid, sealed;
static u8 *rp_;                         /* noch nicht gelesene Nachrichten */
static unsigned rn_;

static void lower(char *d, const char *s, unsigned max);
static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

static void fail(const char *m, unsigned rc)
{
    out(m);
    out("\r\n");
    if (sh)
        DosClose(sh);
    cr_zero(&df, sizeof(df));
    DosExit(rc);
}

static void usage(void)
{
    fail("DOMJOIN Name [/OU:DN]   Rechner der Dom\x84ne beitreten\r\n"
         "DOMJOIN /LEAVE          Dom\x84ne verlassen\r\n"
         "DOMJOIN                 Stand zeigen\r\n"
         "Vorher KINIT mit einem Konto, das Rechner anlegen darf.", 2);
}

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

static int cr_cmp(const void *a, const void *b, unsigned n)
{
    return !cr_eq(a, b, n);
}

static char *utoa(unsigned v, char *z)
{
    char t[12];
    unsigned i = 0, k = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i)
        z[k++] = t[--i];
    z[k] = 0;
    return z;
}

static void setattr(unsigned a)
{
    struct fapi_finfo fi;
    fi.mask = 1;
    fi.attr = a;
    DosSetFInfo(kpath, &fi);
}

static void lconnect(void)
{
    unsigned k;
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
    io.timeout = 10000;
    if (DosNet(NET_CONNECT, &io, sizeof(io)))
        fail("Dom\x84nencontroller nicht erreichbar (LDAP, TCP-Port 389).", 1);
}

/* Nachricht sb[0..n) senden, nach der Anmeldung in einer GSS-Huelle */
static void lsend(unsigned n)
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
static void lrecv(struct tlv *t)
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
static void oct(const char *s) { der_prim(0x04, s, slen(s)); }

static void msg_beg(void)
{
    der_beg(sb, LBUF);
    der_open(0x30);
    der_num(0x02, ++msgid);
}

static unsigned msg_end(void)
{
    der_close();
    return der_len();
}

/* LDAPResult der Antwort mit Kennung op: Code, Meldung, ggf. [7] creds */
static struct tlv creds;

static u32 result(u8 op)
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

static void ldap_fail(const char *was, u32 code)
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
static void rootdse(void)
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

static void bind_req(const u8 *c, unsigned n)
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
static void sasl_bind(void)
{
    const char *e;
    unsigned n, i, k;
    u32 code;
    u8 *pt, sel[4 + 16];
    static char sn[112];
    scopy(sn, "ldap/", sizeof(sn));
    lower(sn + 5, dchost, sizeof(sn) - 5);
    if (krb_cc_find(sn, &svc) < 0 || krb_now() + 60 >= svc.end) {
        if ((e = krb_tgs(&tgt, "ldap", dchost, &svc)) != 0)
            fail(e, 1);
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

/* ---- Rechnerkonto -------------------------------------------------------- */
static void attr1(const char *type, const void *v, unsigned n)
{
    der_open(0x30);
    oct(type);
    der_open(0x31);
    der_prim(0x04, v, n);
    der_close();
    der_close();
}

static unsigned upw(u8 *d)              /* "\"pw\"" als UTF-16LE          */
{
    unsigned i, n = 0;
    d[n++] = '"'; d[n++] = 0;
    for (i = 0; df.pw[i]; i++) {
        d[n++] = (u8)df.pw[i];
        d[n++] = 0;
    }
    d[n++] = '"'; d[n++] = 0;
    return n;
}

static void mkpass(void)
{
    static const char az[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    u8 r[28];
    unsigned i;
    tls_random(r, sizeof(r));
    df.pw[0] = 'A';
    df.pw[1] = 'd';
    df.pw[2] = '7';
    df.pw[3] = '-';
    for (i = 0; i < sizeof(r); i++)
        df.pw[4 + i] = az[r[i] % (sizeof(az) - 1)];
    df.pw[4 + sizeof(r)] = 0;
    cr_zero(r, sizeof(r));
}

static void lower(char *d, const char *s, unsigned max)
{
    unsigned i;
    for (i = 0; s[i] && i < max - 1; i++)
        d[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    d[i] = 0;
}

static void do_join(void)
{
    static u8 pwu[120];
    static char fq[96], san[20], spn[120], salt[200];
    unsigned n;
    u32 code;
    const char *e;

    lower(fq, df.name, sizeof(fq));
    scat(fq, ".", sizeof(fq));
    scat(fq, df.domain, sizeof(fq));
    scopy(san, df.name, sizeof(san));
    scat(san, "$", sizeof(san));
    scopy(df.dn, "CN=", sizeof(df.dn));
    scat(df.dn, df.name, sizeof(df.dn));
    scat(df.dn, ",", sizeof(df.dn));
    scat(df.dn, ou[0] ? ou : "CN=Computers", sizeof(df.dn));
    if (!ou[0]) {
        scat(df.dn, ",", sizeof(df.dn));
        scat(df.dn, base, sizeof(df.dn));
    }
    mkpass();
    n = upw(pwu);

    msg_beg();
    der_open(0x68);
    oct(df.dn);
    der_open(0x30);
    attr1("objectClass", "computer", 8);
    attr1("sAMAccountName", san, slen(san));
    attr1("userAccountControl", "4096", 4);
    attr1("dNSHostName", fq, slen(fq));
    der_open(0x30);
    oct("servicePrincipalName");
    der_open(0x31);
    scopy(spn, "HOST/", sizeof(spn));
    scat(spn, df.name, sizeof(spn));
    oct(spn);
    scopy(spn, "HOST/", sizeof(spn));
    scat(spn, fq, sizeof(spn));
    oct(spn);
    der_close();
    der_close();
    attr1("unicodePwd", pwu, n);
    der_close();
    der_close();
    lsend(msg_end());
    code = result(0x69);
    if (code == 68) {                   /* gibt es schon: Kennwort neu    */
        out("Rechnerkonto ");
        out(san);
        out(" gibt es schon - Kennwort wird neu gesetzt.\r\n");
        msg_beg();
        der_open(0x66);
        oct(df.dn);
        der_open(0x30);
        der_open(0x30);
        der_num(0x0A, 2);               /* replace                        */
        attr1("unicodePwd", pwu, n);
        der_close();
        der_close();
        der_close();
        lsend(msg_end());
        code = result(0x67);
        if (code == 32)
            fail("Ein Konto mit diesem Namen steht an anderer Stelle (/OU:).", 1);
    }
    cr_zero(pwu, sizeof(pwu));
    if (code == 50)
        fail("Keine Berechtigung, Rechner anzulegen (KINIT mit einem anderen Konto).", 1);
    if (code)
        ldap_fail("Rechnerkonto nicht angelegt", code);

    /* Schluessel wie der KDC: Salz REALM + "host" + name.domain */
    scopy(salt, df.realm, sizeof(salt));
    scat(salt, "host", sizeof(salt));
    scat(salt, fq, sizeof(salt));
    krb_s2k(df.pw, slen(df.pw), salt, slen(salt), 4096, df.key);
    df.joined = krb_now();

    /* Probe: als Rechner anmelden */
    if ((e = krb_kinit(san, df.realm, df.pw, &mc)) != 0) {
        out("Probe-Anmeldung als ");
        out(san);
        out(" gescheitert: ");
        fail(e, 1);
    }
    cr_zero(&mc, sizeof(mc));
}

/* ---- Ablage DOMAIN.KEY ---------------------------------------------------- */
static int keypath(void)
{
    if (FAPI_FAILED(DosGetEnv("COMSPEC", kpath, sizeof(kpath))) || kpath[1] != ':')
        return 1;
    kpath[2] = 0;
    scat(kpath, "\\DOS\\SYSTEM\\DOMAIN.KEY", sizeof(kpath));
    return 0;
}

static int kread(void)
{
    unsigned h;
    int r;
    if (keypath())
        return 1;
    h = DosOpen(kpath);
    if (FAPI_FAILED(h))
        return 1;
    r = DosRead(h, &df, sizeof(df)) != sizeof(df) || cr_cmp(df.magic, "ATDOMJ1", 8);
    DosClose(h);
    return r;
}

static int kwrite(void)
{
    unsigned h;
    int r;
    if (keypath())
        return 1;
    setattr(0);
    h = DosCreate(kpath);
    if (FAPI_FAILED(h))
        return 1;
    cr_cpy(df.magic, "ATDOMJ1", 8);
    r = DosWrite(h, &df, sizeof(df)) != sizeof(df);
    DosClose(h);
    setattr(0x06);            /* versteckt + System             */
    return r;
}

static void kdelete(void)
{
    unsigned h;
    if (keypath())
        return;
    setattr(0);
    h = DosOpenRW(kpath);               /* erst ueberschreiben            */
    if (!FAPI_FAILED(h)) {
        cr_zero(&df, sizeof(df));
        DosWrite(h, &df, sizeof(df));
        DosClose(h);
    }
    DosDelete(kpath);
}

/* ---- Hauptprogramm -------------------------------------------------------- */
static void need_tgt(void)
{
    if (krb_cc_open() <= 0 || krb_cc_get(0, &tgt) || krb_now() >= tgt.end)
        fail("Keine g\x81ltigen Kerberos-Tickets - erst KINIT mit einem Konto,\r\n"
             "das Rechner anlegen darf (z. B. KINIT Administrator@DOM\x8eNE).", 1);
}

static void status(void)
{
    if (kread())
        fail("Dieser Rechner ist keiner Dom\x84ne beigetreten.", 1);
    krb_tstr(df.joined, ts);
    out("Rechner ");
    out(df.name);
    out(" in der Dom\x84ne ");
    out(df.domain);
    out(" (Realm ");
    out(df.realm);
    out(")\r\nKonto:  ");
    out(df.dn);
    out("\r\nDC:     ");
    out(df.dc);
    out("\r\nSeit:   ");
    out(ts);
    out("\r\n");
    cr_zero(&df, sizeof(df));
    DosExit(0);
}

static void leave(void)
{
    u32 code;
    if (kread())
        fail("Dieser Rechner ist keiner Dom\x84ne beigetreten.", 1);
    need_tgt();
    lconnect();
    rootdse();
    sasl_bind();
    msg_beg();
    der_prim(0x4A, df.dn, slen(df.dn));
    lsend(msg_end());
    code = result(0x6B);
    if (code && code != 32)             /* 32: gibt es nicht mehr         */
        ldap_fail("Rechnerkonto nicht gel\x94scht", code);
    out("Rechner ");
    out(df.name);
    out(" hat die Dom\x84ne ");
    out(df.domain);
    out(" verlassen.\r\n");
    kdelete();
    DosClose(sh);
    sh = 0;
    DosExit(0);
}

void main(void)
{
    char *a;
    unsigned i, lv = 0;
    df.name[0] = 0;
    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (w[0] == '/' && up(w[1]) == 'O' && up(w[2]) == 'U' && w[3] == ':')
            scopy(ou, w + 4, sizeof(ou));
        else if (w[0] == '/' && up(w[1]) == 'L')
            lv = 1;
        else if (w[0] == '/' || w[0] == '-')
            usage();
        else {
            if (slen(w) > 15)
                fail("Rechnername h\x94" "chstens 15 Zeichen.", 2);
            for (i = 0; w[i]; i++) {
                char c = (char)up(w[i]);
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                    fail("Rechnername nur aus A-Z, 0-9 und -.", 2);
                df.name[i] = c;
            }
            df.name[i] = 0;
        }
    }
    krb_bufs(kb1, kb2);
    if (lv)
        leave();
    if (!df.name[0])
        status();

    need_tgt();
    scopy(df.realm, tgt.realm, sizeof(df.realm));
    lower(df.domain, tgt.realm, sizeof(df.domain));
    lconnect();
    rootdse();
    scopy(df.dc, dchost, sizeof(df.dc));
    sasl_bind();
    do_join();
    msgid++;
    der_beg(sb, LBUF);                  /* unbind                         */
    der_open(0x30);
    der_num(0x02, msgid);
    der_prim(0x42, "", 0);
    der_close();
    lsend(der_len());
    DosClose(sh);
    sh = 0;
    if (kwrite())
        fail("Beigetreten, aber DOMAIN.KEY nicht schreibbar.", 1);
    out("Rechner ");
    out(df.name);
    out(" ist der Dom\x84ne ");
    out(df.domain);
    out(" beigetreten (DC ");
    out(df.dc);
    out(", ");
    out(net_ntop(krb_kdc, diag));
    out(").\r\nKonto ");
    out(df.dn);
    out(", Probe-Anmeldung als ");
    out(df.name);
    out("$ erfolgreich.\r\n");
    cr_zero(&df, sizeof(df));
    DosExit(0);
}
