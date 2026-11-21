/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LOGON.EXE - Anmeldung an der Domaene (AT-DOS 6.3)
 *
 *   LOGON [Name] [/NOHOME] [/Q]
 *
 * Gedacht fuer AUTOEXEC.CMD eines Rechners, der mit DOMJOIN beigetreten
 * ist. Ablauf wie die Anmeldung von Windows NT an einer Domaene:
 *   1. Name (Vorgabe %USERNAME%) und Kennwort abfragen, Ticket fuer
 *      krbtgt holen (krb_kinit, bis drei Versuche an der Tastatur).
 *   2. KDC pruefen: Dienstticket fuer den eigenen Rechner (host/name.dom)
 *      holen und mit dem Rechnerschluessel aus DOMAIN.KEY entschluesseln.
 *      Nur der echte DC kennt diesen Schluessel - ein falscher KDC, der
 *      jedes Kennwort annimmt, faellt hier auf (wie die "KDC-Pruefung"
 *      von pam_krb5/sssd).
 *   3. Tickets ablegen, Umgebung des Aufrufers setzen (FAPI_SETENV mit
 *      ENV_CALLER): USERNAME, USERDOMAIN, USERDNSDOMAIN, LOGONSERVER.
 *   4. Heimlaufwerk: per LDAP (SASL GSSAPI, ldapc.c) homeDrive und
 *      homeDirectory des Kontos lesen, \\Rechner\Freigabe mit NET USE
 *      verbinden (Kerberos), HOMEDRIVE/HOMESHARE/HOMEPATH setzen.
 * ERRORLEVEL: 0 ok, 1 Anmeldung gescheitert, 2 Aufruf, 3 angemeldet, aber
 * Heimlaufwerk nicht verbunden.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#include "krb5.h"
#include "ldapc.h"

static char args[FAPI_ARGLEN], w[256], kpath[FAPI_PATHLEN], user[64], pass[128];
static char fq[96], hdir[160], hdrv[4], ts[20], ev[200];
static struct djfile df;
static struct kcred hs;
static u8 kb1[KBUF], kb2[KBUF];
static unsigned quiet, phase;           /* phase 1: Heimlaufwerk          */

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }
static void say(const char *s) { if (!quiet) out(s); }

void fail(const char *m, unsigned rc)
{
    if (phase && rc == 1) {             /* Anmeldung steht schon          */
        out("Heimlaufwerk nicht verbunden: ");
        rc = 3;
    }
    out(m);
    out("\r\n");
    if (sh)
        DosClose(sh);
    cr_zero(&df, sizeof(df));
    cr_zero(pass, sizeof(pass));
    DosExit(rc);
}

static int kread(void)
{
    unsigned h;
    int r;
    if (FAPI_FAILED(DosGetEnv("COMSPEC", kpath, sizeof(kpath))) || kpath[1] != ':')
        return 1;
    kpath[2] = 0;
    scat(kpath, "\\DOS\\SYSTEM\\DOMAIN.KEY", sizeof(kpath));
    h = DosOpen(kpath);
    if (FAPI_FAILED(h))
        return 1;
    r = DosRead(h, &df, sizeof(df)) != sizeof(df) || cr_cmp(df.magic, "ATDOMJ1", 8);
    DosClose(h);
    return r;
}

/* Eingabe einer Zeile; hide: Kennwort (nicht zeigen) */
static void ask(const char *prompt, char *b, unsigned max, int hide)
{
    unsigned n = 0, c;
    if (DosQHandType(STDIN) != HT_CONSOLE) {    /* umgeleitet: eine Zeile */
        char ch;
        while (n < max - 1 && DosRead(STDIN, &ch, 1) == 1)
            if (ch != '\r' && ch != '\n')
                b[n++] = ch;
            else if (n)
                break;
        b[n] = 0;
        return;
    }
    out(prompt);
    for (;;) {
        c = KbdCharIn();
        if ((c & 0xFF) == '\r' || FAPI_FAILED(c))
            break;
        if ((c & 0xFF) == '\b') {
            if (n) {
                n--;
                if (!hide)
                    out("\b \b");
            }
        } else if (!KEY_EXT(c & 0xFF, c >> 8) && (c & 0xFF) >= ' ' && n < max - 1) {
            b[n++] = (char)c;
            if (!hide) {
                char z[2];
                z[0] = (char)c;
                z[1] = 0;
                out(z);
            }
        }
    }
    b[n] = 0;
    out("\r\n");
}

/* Ticket hs mit dem Rechnerschluessel oeffnen; Sitzungsschluessel darin
   muss der sein, den der KDC uns gab -> 0 = KDC echt */
static int verify_kdc(void)
{
    struct tlv t, s, x, c, k;
    const u8 *p, *e;
    u8 *pt;
    unsigned pl, kl;
    u32 et = 0;
    if (hs.tlen > LBUF)
        return 1;
    cr_cpy(tb, hs.tkt, hs.tlen);
    p = tb;
    if (der_get(&p, tb + hs.tlen, &t) || t.tag != 0x61)
        return 1;
    p = t.p;
    if (der_get(&p, t.p + t.n, &s) || s.tag != 0x30)
        return 1;
    for (p = s.p, c.n = 0; !der_get(&p, s.p + s.n, &x);)
        if (x.tag == 0xA3) {            /* enc-part: EncryptedData        */
            e = x.p;
            if (der_get(&e, x.p + x.n, &x) || x.tag != 0x30)
                return 1;
            for (e = x.p; !der_get(&e, x.p + x.n, &t);) {
                const u8 *q = t.p;
                if (der_get(&q, t.p + t.n, &k))
                    return 1;
                if (t.tag == 0xA0)
                    et = der_uint(&k);
                else if (t.tag == 0xA2 && k.tag == 0x04)
                    c = k;
            }
            break;
        }
    if (!c.n)
        return 1;
    if (et == 18) {
        if (krb_dec(df.key, 2, (u8 *)c.p, c.n, &pt, &pl))
            return 1;
    } else if (et == 23) {
        u8 k23[16];
        int r;
        krb_key23(df.pw, slen(df.pw), k23);
        r = krb_dec23(k23, 2, (u8 *)c.p, c.n, &pt, &pl);
        cr_zero(k23, sizeof(k23));
        if (r)
            return 1;
    } else
        return 1;
    /* EncTicketPart ::= [APPLICATION 3] SEQUENCE { [0] flags, [1] key } */
    p = pt;
    if (der_get(&p, pt + pl, &t) || t.tag != 0x63)
        return 1;
    p = t.p;
    if (der_get(&p, t.p + t.n, &s) || s.tag != 0x30)
        return 1;
    for (p = s.p; !der_get(&p, s.p + s.n, &x);)
        if (x.tag == 0xA1) {
            e = x.p;
            if (der_get(&e, x.p + x.n, &x) || x.tag != 0x30)
                return 1;
            for (e = x.p; !der_get(&e, x.p + x.n, &t);)
                if (t.tag == 0xA1) {
                    const u8 *q = t.p;
                    if (der_get(&q, t.p + t.n, &k) || k.tag != 0x04)
                        return 1;
                    kl = hs.etype == 18 ? 32 : 16;
                    return k.n != kl || !cr_eq(k.p, hs.key, kl);
                }
            return 1;
        }
    return 1;
}

static void setenv2(const char *name, const char *val)
{
    scopy(ev, name, sizeof(ev));
    scat(ev, "=", sizeof(ev));
    scat(ev, val, sizeof(ev));
    DosSetEnvCaller(ev);
    DosSetEnv(ev);
}

/* homeDirectory/homeDrive des Kontos per LDAP */
static void home_ldap(void)
{
    struct tlv m, e, t, a, v, s;
    const u8 *p, *q, *r;
    unsigned n;
    lconnect();
    rootdse();
    sasl_bind();
    msg_beg();
    der_open(0x63);
    oct(base);
    der_num(0x0A, 2);                   /* subtree                        */
    der_num(0x0A, 0);
    der_num(0x02, 1);
    der_num(0x02, 30);
    der_prim(0x01, "\0", 1);
    der_open(0xA3);                     /* (sAMAccountName=user)          */
    oct("sAMAccountName");
    oct(user);
    der_close();
    der_open(0x30);
    oct("homeDirectory");
    oct("homeDrive");
    der_close();
    der_close();
    lsend(msg_end());
    for (;;) {
        lrecv(&m);
        p = m.p;
        if (der_get(&p, m.p + m.n, &t) || der_get(&p, m.p + m.n, &e))
            fail("Antwort des DC ung\x81ltig.", 1);
        if (e.tag == 0x65)
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
            if (t.n == 13 && !cr_cmp(t.p, "homeDirectory", 13)) {
                n = v.n < sizeof(hdir) - 1 ? v.n : sizeof(hdir) - 1;
                cr_cpy(hdir, v.p, n);
                hdir[n] = 0;
            } else if (t.n == 9 && !cr_cmp(t.p, "homeDrive", 9) && v.n == 2) {
                hdrv[0] = (char)up(v.p[0]);
                hdrv[1] = v.p[1];
                hdrv[2] = 0;
            }
        }
    }
    msgid++;
    der_beg(sb, LBUF);                  /* unbind                         */
    der_open(0x30);
    der_num(0x02, msgid);
    der_prim(0x42, "", 0);
    der_close();
    lsend(der_len());
    DosClose(sh);
    sh = 0;
}

static void home(void)
{
    static char unc[160], cl[220];
    unsigned i, k;
    const char *rest;
    phase = 1;
    home_ldap();
    if (!hdir[0])
        return;                         /* kein Heimverzeichnis im Konto  */
    if (hdir[0] != '\\' || hdir[1] != '\\') {
        setenv2("HOMEPATH", hdir);      /* lokaler Pfad                   */
        return;
    }
    if (!hdrv[0] || hdrv[1] != ':' || hdrv[0] < 'A' || hdrv[0] > 'Z')
        scopy(hdrv, "H:", sizeof(hdrv));
    /* \\Rechner\Freigabe[\Rest] */
    for (i = 2, k = 0; hdir[i]; i++)
        if (hdir[i] == '\\' && ++k == 2)
            break;
    scopy(unc, hdir, sizeof(unc));
    unc[i] = 0;
    rest = hdir[i] ? hdir + i : "\\";
    setenv2("HOMEDRIVE", hdrv);
    setenv2("HOMESHARE", unc);
    setenv2("HOMEPATH", rest);
    if (FAPI_FAILED(DosGetEnv("COMSPEC", cl, sizeof(cl))) || !cl[0])
        scopy(cl, "C:", sizeof(cl));
    cl[2] = 0;
    scat(cl, "\\DOS\\NET.EXE USE ", sizeof(cl));
    scat(cl, hdrv, sizeof(cl));
    scat(cl, " \"", sizeof(cl));
    scat(cl, unc, sizeof(cl));
    scat(cl, "\"", sizeof(cl));
    if (DosExecPgm(cl, EXEC_SYNC))
        fail("NET.EXE nicht gefunden (\\DOS).", 1);
    if (DosQChildRC())
        fail("NET USE gescheitert.", 1);
}

void main(void)
{
    char *a;
    const char *e;
    unsigned i, nohome = 0, tries;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (w[0] == '/' && up(w[1]) == 'N')
            nohome = 1;
        else if (w[0] == '/' && up(w[1]) == 'Q')
            quiet = 1;
        else if (w[0] == '/' || w[0] == '-')
            fail("LOGON [Name] [/NOHOME] [/Q]   an der Dom\x84ne anmelden", 2);
        else
            scopy(user, w, sizeof(user));
    }
    if (kread())
        fail("Dieser Rechner ist keiner Dom\x84ne beigetreten (DOMJOIN).", 1);
    if (net_lookup(df.dc, krb_kdc) && net_lookup(df.domain, krb_kdc))
        fail("Dom\x84nencontroller nicht gefunden (Netz, DNS).", 1);
    if (!user[0] && FAPI_FAILED(DosGetEnv("USERNAME", user, sizeof(user))))
        user[0] = 0;
    krb_bufs(kb1, kb2);
    say("Anmeldung an der Dom\x84ne ");
    say(df.domain);
    say("\r\n");
    for (tries = 0;; tries++) {
        if (!user[0])
            ask("Benutzername: ", user, sizeof(user), 0);
        if (!user[0])
            fail("Kein Name angegeben.", 2);
        scopy(w, "Kennwort f\x81r ", sizeof(w));
        scat(w, user, sizeof(w));
        scat(w, ": ", sizeof(w));
        ask(w, pass, sizeof(pass), 1);
        e = krb_kinit(user, df.realm, pass, &tgt);
        cr_zero(pass, sizeof(pass));
        if (!e)
            break;
        out(e);
        out("\r\n");
        if (tries >= 2 || DosQHandType(STDIN) != HT_CONSOLE)
            fail("Anmeldung gescheitert.", 1);
    }

    /* KDC pruefen */
    lower(fq, df.name, sizeof(fq));
    scat(fq, ".", sizeof(fq));
    scat(fq, df.domain, sizeof(fq));
    if ((e = krb_tgs(&tgt, "host", fq, &hs)) != 0) {
        out("Rechnerkonto ");
        out(df.name);
        out("$ beim DC nicht nutzbar: ");
        fail(e, 1);
    }
    if (verify_kdc()) {
        cr_zero(&tgt, sizeof(tgt));
        fail("Der DC konnte sich nicht ausweisen (Rechnerschl\x81ssel passt nicht) -\r\n"
             "Anmeldung abgelehnt. Falscher DC oder Rechnerkonto neu angelegt (DOMJOIN).", 1);
    }
    if (krb_cc_init(&tgt))
        fail("Kann die Ablage nicht schreiben (%TEMP%).", 1);
    krb_cc_put(&hs);
    cr_zero(&hs, sizeof(hs));

    /* Umgebung */
    setenv2("USERNAME", user);
    for (i = 0; df.domain[i] && df.domain[i] != '.' && i < sizeof(w) - 1; i++)
        w[i] = (char)up(df.domain[i]);
    w[i] = 0;
    setenv2("USERDOMAIN", w);
    setenv2("USERDNSDOMAIN", df.realm);
    w[0] = w[1] = '\\';
    for (i = 0; df.dc[i] && df.dc[i] != '.' && i < sizeof(w) - 3; i++)
        w[2 + i] = (char)up(df.dc[i]);
    w[2 + i] = 0;
    setenv2("LOGONSERVER", w);
    krb_tstr(tgt.end, ts);
    say("Angemeldet als ");
    say(tgt.cname);
    say("@");
    say(tgt.realm);
    say(" (DC ");
    say(net_ntop(krb_kdc, w));
    say("), Ticket g\x81ltig bis ");
    say(ts);
    say(".\r\n");
    if (!nohome)
        home();
    cr_zero(&df, sizeof(df));
    DosExit(0);
}
