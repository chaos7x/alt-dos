/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NET1.EXE - Domaenen-Teil von NET (AT-DOS 6.4), wie net1.exe unter
 * Windows von NET.EXE aufgerufen:
 *   NET USER [Name] /DOMAIN     Benutzer der Domaene bzw. ein Konto
 *   NET GROUP [Gruppe] /DOMAIN  Gruppen bzw. Mitglieder einer Gruppe
 *   NET ACCOUNTS /DOMAIN        Kennwort- und Sperrregeln der Domaene
 * Nur lesend, ueber LDAP (ldapc.c: SASL GSSAPI mit Vertraulichkeit) mit
 * dem Kerberos-Ticket aus der Ablage (LOGON oder KINIT). AT-DOS hat keine
 * lokalen Konten - ohne /DOMAIN gibt es einen Hinweis.
 * ERRORLEVEL: 0 ok, 1 Fehler (Netz, LDAP), 2 Aufruf/nicht gefunden.
 */
#include "fapi.h"
#include "util.h"
#include "arcutil.h"
#include "tz.h"
#include "krb5.h"
#include "ldapc.h"
#define CTRY_NO_DPAT
#include "country.h"

#define OK_MSG "Der Befehl wurde erfolgreich ausgef\x81hrt.\r\n"
#define NLEN    64                      /* Platz je Name in der Liste     */
#define NMAX    (65000u / NLEN)

static char args[FAPI_ARGLEN], w[256], nm[80];
static u8 kb1[KBUF], kb2[KBUF];
static unsigned lseg, ln;               /* Namensliste (fern)             */

/* gesammelte Werte eines Eintrags */
static char a_sam[64], a_disp[80], a_desc[128], a_uac[16], a_exp[24];
static char a_pls[24], a_ll[24], a_llt[24], a_script[80], a_prof[128];
static char a_home[160], a_ws[80], a_pgid[12];
static char d_minl[8], d_maxa[24], d_mina[24], d_hist[8], d_lth[8];
static char d_ldur[24], d_lwin[24], d_flog[24];
static u8 d_sid[28], g_sid[32];
static unsigned d_sidn, mode;
#define M_LIST  1                       /* sAMAccountName -> Liste        */
#define M_USER  2
#define M_DOM   3
#define M_MEMB  4                       /* member/memberOf -> CN in Liste */

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

void fail(const char *m, unsigned rc)
{
    out(m);
    out("\r\n");
    if (sh)
        DosClose(sh);
    DosExit(rc);
}

unsigned long n1_mul(unsigned a, unsigned b);
#pragma aux n1_mul = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];
unsigned long n1_dv(unsigned long n, unsigned d);
#pragma aux n1_dv = "div bx" __parm [__dx __ax] [__bx] __value [__dx __ax];

/* 64-Bit (4 Worte, w[3] oben) / d -> Rest */
static unsigned dv64(unsigned *x, unsigned d)
{
    unsigned i = 4, r = 0;
    unsigned long q;
    while (i--) {
        q = n1_dv((unsigned long)r << 16 | x[i], d);
        x[i] = (unsigned)q;
        r = (unsigned)(q >> 16);
    }
    return r;
}

/* Dezimalzahl (LDAP-Text, Vorzeichen uebergangen) -> 64 Bit */
static void dec64(const char *s, unsigned *x)
{
    unsigned i, c;
    unsigned long p;
    x[0] = x[1] = x[2] = x[3] = 0;
    if (*s == '-')
        s++;
    for (; *s >= '0' && *s <= '9'; s++) {
        c = *s - '0';
        for (i = 0; i < 4; i++) {
            p = n1_mul(x[i], 10) + c;
            x[i] = (unsigned)p;
            c = (unsigned)(p >> 16);
        }
    }
}

static int is_never(const unsigned *x)  /* 0 oder >= 2^63 (MAXINT)       */
{
    return !(x[0] | x[1] | x[2] | x[3]) || (x[3] & 0x8000) || x[3] == 0x7FFF;
}

/* Zeitspanne in 100 ns -> Sekunden (gekappt) */
static unsigned long span_s(const char *s, int *never)
{
    unsigned x[4];
    dec64(s, x);
    *never = is_never(x);
    dv64(x, 10000);
    dv64(x, 1000);
    if (x[3] || x[2])
        return 0xFFFFFFFFUL;
    return (unsigned long)x[1] << 16 | x[0];
}

/* FILETIME (Text) -> Unix-Zeit UTC, 0 = nie */
static unsigned long ft_unix(const char *s)
{
    unsigned x[4], k, n;
    dec64(s, x);
    if (is_never(x))
        return 0;
    dv64(x, 10000);
    dv64(x, 1000);
    k = x[0] < 0x9100;                  /* - 11644473600 (1601 -> 1970)  */
    x[0] -= 0x9100;
    n = (unsigned long)x[1] < 0xB610UL + k;
    x[1] -= 0xB610 + k;
    k = (unsigned long)x[2] < 2UL + n;
    x[2] -= 2 + n;
    x[3] -= k;
    if (x[3] || x[2])
        return 0;
    return (unsigned long)x[1] << 16 | x[0];
}

static char *put_dec(unsigned long v, char *p)
{
    char t[12];
    unsigned i = 0;
    unsigned long r;
    do {
        v = au_div(v, 10, &r);
        t[i++] = (char)('0' + (unsigned)r);
    } while (v);
    while (i)
        *p++ = t[--i];
    *p = 0;
    return p;
}

static unsigned fw = 38;                /* Breite der Feldnamen           */

static void pad(const char *s, unsigned wd)  /* wd > 0: mind. 1 Leerzeichen */
{
    unsigned n = slen(s);
    out(s);
    if (wd && n >= wd)
        out(" ");
    while (n++ < wd)
        out(" ");
}

static void field(const char *name, const char *val)
{
    pad(name, fw);
    out(val);
    out("\r\n");
}

static void tfield(const char *name, unsigned long utc)
{
    unsigned date, time;
    int dst;
    char *p;
    if (!utc) {
        field(name, "Nie");
        return;
    }
    unix2fat(utc + tz_off(utc, &dst), &date, &time);
    ctry_date(w, date);
    p = w + slen(w);
    *p++ = ' ';
    ctry_time(p, time, 1);
    field(name, w);
}

static void nfield(const char *name, unsigned long v, const char *never)
{
    if (never)
        field(name, never);
    else {
        put_dec(v, w);
        field(name, w);
    }
}

/* ---- Liste von Namen (fern, sortiert) ------------------------------------ */

static char __far *ent(unsigned i)
{
    return (char __far *)((unsigned long)lseg << 16 | i * NLEN);
}

static void l_add(const u8 *s, unsigned n)
{
    char __far *d;
    unsigned i;
    if (ln >= NMAX)
        return;
    d = ent(ln++);
    if (n > NLEN - 1)
        n = NLEN - 1;
    for (i = 0; i < n; i++)
        d[i] = (char)s[i];
    d[n] = 0;
}

static int l_cmp(unsigned a, unsigned b)
{
    char __far *p = ent(a), __far *q = ent(b);
    char x, y;
    for (;; p++, q++) {
        x = up(*p);
        y = up(*q);
        if (x != y || !x)
            return x - y;
    }
}

static void l_sort(void)
{
    unsigned g, i, j, k;
    char t[NLEN];
    for (g = ln / 2; g; g /= 2)
        for (i = g; i < ln; i++)
            for (j = i; j >= g && l_cmp(j - g, j) > 0; j -= g) {
                char __far *p = ent(j - g), __far *q = ent(j);
                for (k = 0; k < NLEN; k++) {
                    t[k] = p[k];
                    p[k] = q[k];
                    q[k] = t[k];
                }
            }
}

/* Spalten zu 25 Zeichen, star = '*' davor */
static void l_print(unsigned cols, unsigned wd, unsigned ind, int star)
{
    unsigned i, k, c = 0;
    char __far *p;
    for (i = 0; i < ln; i++) {
        p = ent(i);
        if (c == cols) {
            out("\r\n");
            pad("", ind);
            c = 0;
        }
        k = 0;
        if (star)
            w[k++] = '*';
        while (*p && k < sizeof(w) - 1)
            w[k++] = *p++;
        w[k] = 0;
        pad(w, c + 1 < cols ? wd : 0);
        c++;
    }
    out("\r\n");
}

/* ---- LDAP-Suche ---------------------------------------------------------- */

static void vcopy(char *d, unsigned max, const u8 *v, unsigned n)
{
    if (n > max - 1)
        n = max - 1;
    cr_cpy(d, v, n);
    d[n] = 0;
}

static int is(const u8 *a, unsigned an, const char *s)
{
    unsigned n = slen(s), i;
    if (an != n)
        return 0;
    for (i = 0; i < n; i++)
        if (up((char)a[i]) != up(s[i]))
            return 0;
    return 1;
}

/* CN aus einem DN ("CN=Name,OU=...") */
static void l_cn(const u8 *v, unsigned n)
{
    static u8 c[NLEN];
    unsigned i = 3, k = 0;
    if (n < 4 || up((char)v[0]) != 'C' || up((char)v[1]) != 'N' || v[2] != '=')
        return;
    for (; i < n && v[i] != ','; i++) {
        if (v[i] == '\\' && i + 1 < n)
            i++;
        if (k < NLEN - 1)
            c[k++] = v[i];
    }
    l_add(c, k);
}

static void take(const u8 *a, unsigned an, const u8 *v, unsigned vn)
{
    switch (mode) {
    case M_LIST:
        if (is(a, an, "sAMAccountName"))
            l_add(v, vn);
        break;
    case M_MEMB:
        if (is(a, an, "member") || is(a, an, "memberOf"))
            l_cn(v, vn);
        else if (is(a, an, "description"))
            vcopy(a_desc, sizeof(a_desc), v, vn);
        else if (is(a, an, "sAMAccountName"))
            vcopy(a_sam, sizeof(a_sam), v, vn);
        break;
    case M_USER:
        if (is(a, an, "memberOf"))
            l_cn(v, vn);
        else if (is(a, an, "sAMAccountName")) vcopy(a_sam, sizeof(a_sam), v, vn);
        else if (is(a, an, "displayName")) vcopy(a_disp, sizeof(a_disp), v, vn);
        else if (is(a, an, "description")) vcopy(a_desc, sizeof(a_desc), v, vn);
        else if (is(a, an, "userAccountControl")) vcopy(a_uac, sizeof(a_uac), v, vn);
        else if (is(a, an, "accountExpires")) vcopy(a_exp, sizeof(a_exp), v, vn);
        else if (is(a, an, "pwdLastSet")) vcopy(a_pls, sizeof(a_pls), v, vn);
        else if (is(a, an, "lastLogon")) vcopy(a_ll, sizeof(a_ll), v, vn);
        else if (is(a, an, "lastLogonTimestamp")) vcopy(a_llt, sizeof(a_llt), v, vn);
        else if (is(a, an, "scriptPath")) vcopy(a_script, sizeof(a_script), v, vn);
        else if (is(a, an, "profilePath")) vcopy(a_prof, sizeof(a_prof), v, vn);
        else if (is(a, an, "homeDirectory")) vcopy(a_home, sizeof(a_home), v, vn);
        else if (is(a, an, "userWorkstations")) vcopy(a_ws, sizeof(a_ws), v, vn);
        else if (is(a, an, "primaryGroupID")) vcopy(a_pgid, sizeof(a_pgid), v, vn);
        break;
    case M_DOM:
        if (is(a, an, "objectSid") && vn <= sizeof(d_sid) - 4) {
            cr_cpy(d_sid, v, vn);
            d_sidn = vn;
        } else if (is(a, an, "minPwdLength")) vcopy(d_minl, sizeof(d_minl), v, vn);
        else if (is(a, an, "maxPwdAge")) vcopy(d_maxa, sizeof(d_maxa), v, vn);
        else if (is(a, an, "minPwdAge")) vcopy(d_mina, sizeof(d_mina), v, vn);
        else if (is(a, an, "pwdHistoryLength")) vcopy(d_hist, sizeof(d_hist), v, vn);
        else if (is(a, an, "lockoutThreshold")) vcopy(d_lth, sizeof(d_lth), v, vn);
        else if (is(a, an, "lockoutDuration")) vcopy(d_ldur, sizeof(d_ldur), v, vn);
        else if (is(a, an, "lockOutObservationWindow")) vcopy(d_lwin, sizeof(d_lwin), v, vn);
        else if (is(a, an, "forceLogoff")) vcopy(d_flog, sizeof(d_flog), v, vn);
        break;
    }
}

static void f_eq(const char *a, const void *v, unsigned n)
{
    der_open(0xA3);
    oct(a);
    der_prim(0x04, v, n);
    der_close();
}

/* Suche: kind 1 Benutzer, 2 Benutzer nm, 3 Gruppen, 4 Gruppe nm,
   5 Basisobjekt (objectClass vorhanden), 6 objectSid = g_sid (n Byte) */
static unsigned search(const char *bdn, unsigned kind, unsigned sidn,
                       const char *const *attrs)
{
    struct tlv m, e, t, a, v, s, x;
    const u8 *p, *q, *r, *z;
    unsigned code, n = 0;
    msg_beg();
    der_open(0x63);
    oct(bdn);
    der_num(0x0A, kind == 5 ? 0 : 2);   /* baseObject / subtree          */
    der_num(0x0A, 0);
    der_num(0x02, 0);
    der_num(0x02, 60);
    der_prim(0x01, "\0", 1);
    if (kind == 2 || kind == 4)
        der_open(0xA0);                 /* & */
    if (kind <= 2)
        f_eq("sAMAccountType", "805306368", 9);
    else if (kind <= 4)
        f_eq("objectClass", "group", 5);
    else if (kind == 5)
        der_prim(0x87, "objectClass", 11);
    else
        f_eq("objectSid", g_sid, sidn);
    if (kind == 2 || kind == 4) {
        f_eq("sAMAccountName", nm, slen(nm));
        der_close();
    }
    der_open(0x30);
    for (; *attrs; attrs++)
        oct(*attrs);
    der_close();
    der_close();
    lsend(msg_end());
    for (;;) {
        lrecv(&m);
        p = m.p;
        if (der_get(&p, m.p + m.n, &t) || der_get(&p, m.p + m.n, &e))
            fail("Antwort des DC ung\x81ltig.", 1);
        if (e.tag == 0x65) {            /* searchResDone                  */
            q = e.p;
            code = der_get(&q, e.p + e.n, &t) ? 1 : (unsigned)der_uint(&t);
            if (code && code != 4)      /* 4: Groessengrenze erreicht     */
                ldap_fail("Suche abgelehnt", code);
            return n;
        }
        if (e.tag != 0x64)
            continue;
        n++;
        q = e.p;
        if (der_get(&q, e.p + e.n, &t) || der_get(&q, e.p + e.n, &a))
            continue;
        for (q = a.p; !der_get(&q, a.p + a.n, &s);) {
            r = s.p;
            if (der_get(&r, s.p + s.n, &t) || der_get(&r, s.p + s.n, &v))
                continue;
            for (z = v.p; !der_get(&z, v.p + v.n, &x);)
                if (x.tag == 0x04)
                    take(t.p, t.n, x.p, x.n);
        }
    }
}

static const char *const at_dom[] = { "objectSid", "minPwdLength", "maxPwdAge",
    "minPwdAge", "pwdHistoryLength", "lockoutThreshold", "lockoutDuration",
    "lockOutObservationWindow", "forceLogoff", 0 };
static const char *const at_sam[] = { "sAMAccountName", 0 };
static const char *const at_user[] = { "sAMAccountName", "displayName",
    "description", "userAccountControl", "accountExpires", "pwdLastSet",
    "lastLogon", "lastLogonTimestamp", "scriptPath", "profilePath",
    "homeDirectory", "userWorkstations", "primaryGroupID", "memberOf", 0 };
static const char *const at_grp[] = { "sAMAccountName", "description", "member", 0 };

static void domain(void)
{
    mode = M_DOM;
    search(base, 5, 0, at_dom);
}

static unsigned long dnum(const char *s)
{
    unsigned long v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        v = (v << 3) + (v << 1) + (unsigned)(*s - '0');
    return v;
}

/* ---- Befehle ------------------------------------------------------------- */

static void title(const char *what)
{
    out(what);
    out(" f\x81r \\\\");
    out(dchost);
    out("\r\n\r\n-------------------------------------------------------------------------------\r\n");
}

static void user_list(void)
{
    mode = M_LIST;
    search(base, 1, 0, at_sam);
    l_sort();
    title("Benutzerkonten");
    l_print(3, 25, 0, 0);
}

static void user_show(void)
{
    unsigned long uac, pls, t, e;
    int nev;
    mode = M_USER;
    if (!search(base, 2, 0, at_user))
        fail("Der Benutzername konnte nicht gefunden werden.\r\n\r\n"
             "Weitere Hilfe erhalten Sie, wenn Sie NET HELPMSG 2221 eingeben.", 2);
    if (a_pgid[0] && d_sidn >= 8) {     /* primaere Gruppe ueber die SID  */
        cr_cpy(g_sid, d_sid, d_sidn);
        t = dnum(a_pgid);
        g_sid[1]++;
        g_sid[d_sidn] = (u8)t;
        g_sid[d_sidn + 1] = (u8)(t >> 8);
        g_sid[d_sidn + 2] = (u8)(t >> 16);
        g_sid[d_sidn + 3] = (u8)(t >> 24);
        mode = M_LIST;
        search(base, 6, d_sidn + 4, at_sam);
    }
    uac = dnum(a_uac);
    field("Benutzername", a_sam);
    field("Vollst\x84ndiger Name", a_disp);
    field("Kommentar", a_desc);
    field("Konto aktiv", uac & 2 ? "Nein" : "Ja");
    e = ft_unix(a_exp);
    tfield("Konto abgelaufen", e);
    out("\r\n");
    pls = ft_unix(a_pls);
    tfield("Letztes Setzen des Kennworts", pls);
    t = span_s(d_maxa, &nev);
    tfield("Kennwort l\x84uft ab", (uac & 0x10000UL) || nev || !pls ? 0 : pls + t);
    t = span_s(d_mina, &nev);
    tfield("Kennwort \x84nderbar", pls ? pls + t : 0);
    field("Kennwort erforderlich", uac & 0x20 ? "Nein" : "Ja");
    out("\r\n");
    field("Arbeitsstationen zugelassen", a_ws[0] ? a_ws : "Alle");
    field("Anmeldeskript", a_script);
    field("Benutzerprofil", a_prof);
    field("Basisverzeichnis", a_home);
    t = ft_unix(a_ll);
    e = ft_unix(a_llt);
    tfield("Letzte Anmeldung", t > e ? t : e);
    out("\r\n");
    pad("Gruppenmitgliedschaften", 38);
    if (!ln)
        out("*Keine\r\n");
    else {
        l_sort();
        l_print(2, 22, 38, 1);
    }
}

static void group_list(void)
{
    mode = M_LIST;
    search(base, 3, 0, at_sam);
    l_sort();
    title("Gruppenkonten");
    l_print(3, 25, 0, 1);
}

static void group_show(void)
{
    mode = M_MEMB;
    if (!search(base, 4, 0, at_grp))
        fail("Der Gruppenname konnte nicht gefunden werden.\r\n\r\n"
             "Weitere Hilfe erhalten Sie, wenn Sie NET HELPMSG 2220 eingeben.", 2);
    pad("Gruppenname", 16);
    out(a_sam);
    out("\r\n");
    pad("Kommentar", 16);
    out(a_desc);
    out("\r\n\r\nMitglieder\r\n\r\n"
        "-------------------------------------------------------------------------------\r\n");
    l_sort();
    if (ln)
        l_print(3, 25, 0, 0);
}

static void accounts(void)
{
    unsigned long t;
    int nev;
    char *p;
    fw = 50;
    t = span_s(d_flog, &nev);
    nfield("Abmeldung erzwingen nach (Sekunden):", t, nev ? "Nie" : 0);
    t = span_s(d_mina, &nev);
    nfield("Minimales Kennwortalter (Tage):", au_div(t, 86400UL, 0), nev ? "0" : 0);
    t = span_s(d_maxa, &nev);
    nfield("Maximales Kennwortalter (Tage):", au_div(t, 86400UL, 0), nev ? "Unbegrenzt" : 0);
    field("Minimale Kennwortl\x84nge:", d_minl[0] ? d_minl : "0");
    field("L\x84nge der Kennwortchronik:", d_hist[0] && dnum(d_hist) ? d_hist : "Keine");
    field("Sperrschwelle:", d_lth[0] && dnum(d_lth) ? d_lth : "Nie");
    t = span_s(d_ldur, &nev);
    nfield("Sperrdauer (Minuten):", au_div(t, 60, 0), nev ? "Unbegrenzt" : 0);
    t = span_s(d_lwin, &nev);
    p = "Beobachtungsfenster der Sperre (Minuten):";
    nfield(p, au_div(t, 60, 0), 0);
    field("Computerrolle:", "ARBEITSSTATION");
}

void main(void)
{
    char *a;
    unsigned dom = 0, cmd = 0, bad = 0, k;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (!cmd) {
            cmd = same(w, "USER") || same(w, "USERS") ? 1 :
                  same(w, "GROUP") ? 2 : same(w, "ACCOUNTS") ? 3 : 0;
            if (!cmd)
                fail("NET1 USER|GROUP|ACCOUNTS ... /DOMAIN (Aufruf \x81ber NET)", 2);
        } else if (w[0] == '/') {
            for (k = 1; w[k] && w[k] != ':'; k++)
                ;
            w[k] = 0;
            if (same(w, "/DOMAIN") || same(w, "/DO") || same(w, "/DOM"))
                dom = 1;
            else
                bad = 1;
        } else if (!nm[0] && cmd != 3)
            scopy(nm, w, sizeof(nm));
        else
            bad = 1;
    }
    if (bad)
        fail("Konten \x84ndern geht unter AT-DOS nicht (NET USER/GROUP/ACCOUNTS\r\n"
             "zeigen nur an, mit /DOMAIN).", 2);
    if (!dom)
        fail("AT-DOS hat keine lokalen Konten. Mit /DOMAIN zeigt NET die Konten\r\n"
             "und Regeln der Dom\x84ne (Kerberos-Ticket von LOGON oder KINIT).", 2);
    if (krb_cc_open() <= 0 || krb_cc_get(0, &tgt))
        fail("Kein Kerberos-Ticket (erst LOGON oder KINIT).", 1);
    krb_bufs(kb1, kb2);
    krb_kdc_list(0, tgt.realm);
    lseg = DosAllocSeg(0);
    if (FAPI_FAILED(lseg))
        fail("Zu wenig Speicher.", 1);
    tz_init();
    ctry_init();
    lower(w, tgt.realm, sizeof(w));
    out("Die Anforderung wird auf einem Dom\x84nencontroller der Dom\x84ne ");
    out(w);
    out(" bearbeitet.\r\n\r\n");
    lconnect();
    rootdse();
    sasl_bind();
    domain();
    if (cmd == 1)
        nm[0] ? user_show() : user_list();
    else if (cmd == 2)
        nm[0] ? group_show() : group_list();
    else
        accounts();
    ldap_close();
    out(OK_MSG);
    DosExit(0);
}
