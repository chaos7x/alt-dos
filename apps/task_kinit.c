/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KINIT.EXE / KLIST.EXE / KDESTROY.EXE - Kerberos-Tickets (AT-DOS 6.3),
 * Bedienung wie bei MIT Kerberos:
 *   KINIT [Name[@REALM]] [/KDC:Adresse]   anmelden, Ticket ablegen
 *   KLIST                                 Tickets der Ablage zeigen
 *   KDESTROY                              Ablage loeschen
 * Das Kennwort kommt von der Tastatur, bei umgeleiteter Eingabe aus
 * deren erster Zeile (wie kinit von MIT). Realm ohne Angabe aus
 * %USERDNSDOMAIN%, sonst aus DOMAIN.KEY (DOMJOIN), sonst dem DNS-Suffix
 * der Netzkarte (DHCP); Name aus %USERNAME%. Der KDC ist
 * ohne /KDC: der Rechner mit dem Namen der Domaene (im AD hat der Name
 * der Domaene die Adressen der DCs). Bibliothek: apps/krb5.c.
 * task_klist.c / task_kdestroy.c binden diese Datei mit KLIST / KDESTROY
 * ein. ERRORLEVEL: 0 ok, 1 Fehler, 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#ifndef KDESTROY
#ifdef KLIST
#define NET_NO_PTON
#else
#define NET_RESOLVE_FN
#endif
#include "net.h"
#endif
#include "krb5.h"

#ifndef KDESTROY
static char w[128], ts[20];
#endif
#if defined(KLIST) || defined(KDESTROY)
static struct kcred cr;
#endif

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

static void fail(const char *m, unsigned rc)
{
    out(m);
    out("\r\n");
    DosExit(rc);
}

#if defined(KLIST)
void main(void)
{
    int i, n;
    char p[FAPI_PATHLEN];
    krb_cc_path(p, sizeof(p));
    if ((n = krb_cc_open()) <= 0 || krb_cc_get(0, &cr))
        fail("Keine Kerberos-Tickets.", 1);
    out("Ablage: ");
    out(p);
    out("\r\nKDC: ");
    out(net_ntop(krb_kdc, w));
    out("\r\nAnmeldung: ");
    out(cr.cname);
    out("@");
    out(cr.realm);
    out("\r\n\r\n  G\x81ltig bis           Dienst\r\n");
    for (i = 0; i < n; i++) {
        if (krb_cc_get(i, &cr))
            break;
        krb_tstr(cr.end, ts);
        out("  ");
        out(ts);
        out(krb_now() >= cr.end ? " (abgelaufen) " : "  ");
        out(cr.sname);
        out("\r\n");
    }
    DosExit(0);
}

#elif defined(KDESTROY)
void main(void)
{
    char p[FAPI_PATHLEN];
    unsigned h, n;
    krb_cc_path(p, sizeof(p));
    h = DosOpenRW(p);                   /* erst mit Nullen ueberschreiben */
    if (FAPI_FAILED(h))
        fail("Keine Kerberos-Tickets.", 1);
    cr_zero(&cr, sizeof(cr));
    for (n = 0; n < KNCRED + 1; n++)
        DosWrite(h, &cr, sizeof(cr));
    DosClose(h);
    DosDelete(p);
    out("Tickets gel\x94scht.\r\n");
    DosExit(0);
}

#else
static char args[FAPI_ARGLEN], user[64], realm[64], kdc[128], pass[128];
static struct kcred tgt;
static u8 kb1[KBUF], kb2[KBUF];

static void ask_pass(void)
{
    unsigned n = 0, c;
    if (DosQHandType(STDIN) != HT_CONSOLE) {    /* umgeleitet: eine Zeile */
        char ch;
        while (n < sizeof(pass) - 1 && DosRead(STDIN, &ch, 1) == 1 && ch != '\r' && ch != '\n')
            pass[n++] = ch;
        pass[n] = 0;
        return;
    }
    out("Kennwort f\x81r ");
    out(user);
    out("@");
    out(realm);
    out(": ");
    for (;;) {
        c = KbdCharIn();
        if ((c & 0xFF) == '\r' || FAPI_FAILED(c))
            break;
        if ((c & 0xFF) == '\b') {
            if (n)
                n--;
        } else if (!KEY_EXT(c & 0xFF, c >> 8) && (c & 0xFF) >= ' ' && n < sizeof(pass) - 1)
            pass[n++] = (char)c;
    }
    pass[n] = 0;
    out("\r\n");
}

/* Realm ohne Angabe: DOMAIN.KEY (DOMJOIN), sonst DNS-Suffix der Karte */
static void def_realm(void)
{
    static struct fapi_netif ni;
    char k[FAPI_PATHLEN], b[8 + 64];
    unsigned h, i;
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", k, sizeof(k))) && k[1] == ':') {
        k[2] = 0;
        scat(k, "\\DOS\\SYSTEM\\DOMAIN.KEY", sizeof(k));
        h = DosOpen(k);
        if (!FAPI_FAILED(h)) {
            if (DosRead(h, b, sizeof(b)) == sizeof(b) && cr_eq(b, "ATDOMJ1", 8)) {
                b[sizeof(b) - 1] = 0;
                scopy(realm, b + 8, sizeof(realm));
            }
            DosClose(h);
            if (realm[0])
                return;
        }
    }
    for (i = 0; i < 4; i++) {
        ni.ifc = i;
        if (DosNet(NET_IFINFO, &ni, sizeof(ni)) == 0 && ni.domain[0]) {
            scopy(realm, ni.domain, sizeof(realm));
            return;
        }
    }
}

void main(void)
{
    char *a, *p;
    const char *e;
    unsigned i;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (w[0] == '/' && up(w[1]) == 'K' && (p = w + 2, 1)) {
            while (*p && *p != ':')
                p++;
            if (!*p)
                fail("KINIT [Name[@REALM]] [/KDC:Adresse]", 2);
            scopy(kdc, p + 1, sizeof(kdc));
        } else if (w[0] == '/' || w[0] == '-')
            fail("KINIT [Name[@REALM]] [/KDC:Adresse]", 2);
        else {
            for (i = 0; w[i] && w[i] != '@'; i++)
                ;
            if (w[i]) {
                w[i] = 0;
                scopy(realm, w + i + 1, sizeof(realm));
            }
            scopy(user, w, sizeof(user));
        }
    }
    if (!user[0] && (FAPI_FAILED(DosGetEnv("USERNAME", user, sizeof(user))) || !user[0]))
        fail("Kein Name angegeben (KINIT Name@REALM).", 2);
    if (!realm[0] && (FAPI_FAILED(DosGetEnv("USERDNSDOMAIN", realm, sizeof(realm))) || !realm[0]))
        def_realm();
    if (!realm[0])
        fail("Keine Dom\x84ne angegeben (KINIT Name@REALM).", 2);
    for (i = 0; realm[i]; i++)
        realm[i] = (char)up(realm[i]);
    if (!kdc[0]) {
        for (i = 0; realm[i] && i < sizeof(kdc) - 1; i++)
            kdc[i] = (char)(realm[i] >= 'A' && realm[i] <= 'Z' ? realm[i] + 32 : realm[i]);
        kdc[i] = 0;
    }
    if (net_lookup(kdc, krb_kdc))
        fail("KDC nicht gefunden (Name der Dom\x84ne oder /KDC:).", 1);
    ask_pass();
    krb_bufs(kb1, kb2);
    e = krb_kinit(user, realm, pass, &tgt);
    cr_zero(pass, sizeof(pass));
    if (e)
        fail(e, 1);
    if (krb_cc_init(&tgt))
        fail("Kann die Ablage nicht schreiben (%TEMP%).", 1);
    krb_tstr(tgt.end, ts);
    out("Angemeldet als ");
    out(tgt.cname);
    out("@");
    out(tgt.realm);
    out(" (KDC ");
    out(net_ntop(krb_kdc, w));
    out("), Ticket g\x81ltig bis ");
    out(ts);
    out(".\r\n");
    DosExit(0);
}
#endif
