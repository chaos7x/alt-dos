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
 * userAccountControl 4096 (Arbeitsstation), dNSHostName, SPN HOST/...,
 * msDS-SupportedEncryptionTypes 24 (Tickets fuer den Rechner mit AES, wie
 * Windows; sonst nimmt Samba RC4) und zufaelligem Kennwort. Gibt es das Konto schon, wird nur das Kennwort
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
#include "ldapc.h"


static char args[FAPI_ARGLEN], w[256], ou[192], kpath[FAPI_PATHLEN];
static char ts[20];
static struct djfile df;
static struct kcred mc;
static u8 kb1[KBUF], kb2[KBUF];
static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

void fail(const char *m, unsigned rc)
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

static void setattr(unsigned a)
{
    struct fapi_finfo fi;
    fi.mask = 1;
    fi.attr = a;
    DosSetFInfo(kpath, &fi);
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
    attr1("msDS-SupportedEncryptionTypes", "24", 2);   /* AES128 + AES256 */
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
        der_open(0x30);
        der_num(0x0A, 2);               /* Tickets fuer den Rechner: AES  */
        attr1("msDS-SupportedEncryptionTypes", "24", 2);
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
