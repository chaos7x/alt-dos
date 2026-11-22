/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NET.EXE - Netzlaufwerke (AT-DOS 6.3), Bedienung wie NET USE von
 * MS-DOS/LAN Manager/Windows:
 *   NET USE                         verbundene Laufwerke zeigen
 *   NET USE X: \\Rechner\Freigabe [Kennwort|*] [/USER:[Domaene\]Name]
 *   NET USE X: /DELETE              trennen (auch /D)
 *   NET USE ... /PERSISTENT:YES|NO  Verbindungen merken (Vorgabe ja, wie
 *                                   Windows); allein: nur die Vorgabe
 *   NET USE /RESTORE                gemerkte Verbindungen herstellen
 *                                   (LOGON ruft es nach dem Heimlaufwerk)
 * Gemerkt wird in X:\DOS\SYSTEM\NETUSE.LST (Startlaufwerk) - ohne
 * Kennwort: mit Kerberos (LOGON/KINIT) geht es ohne, mit /USER: fragt
 * /RESTORE nach dem Kennwort (wie WfW ohne Kennwortliste).
 * Das Laufwerk bedient ein losgeloestes SMBCLI.EXE (--serve X:) ueber den
 * Ring-3-Dienst des Kernels (FAPI_UFS, kernel/ufs.c). Das Kennwort geht
 * ueber eine kurzlebige Datei in %TEMP% (nicht in der Befehlszeile, die
 * TASKMAN zeigen wuerde); SMBCLI liest und loescht sie, legt sie leer neu
 * an, wenn das Laufwerk steht, oder schreibt "!Meldung", wenn nicht.
 * Ohne Kennwort und /USER: mit Kerberos (SMBCLI -k), wenn KINIT Tickets
 * abgelegt hat und der Rechner mit Namen angegeben ist (wie Windows).
 * Weitere Unterbefehle wie Windows (Wunsch des Nutzers, 22.11.2026), nur
 * was zu einem Arbeitsplatz passt (kein Serverdienst):
 *   NET                       Syntax
 *   NET HELP [Befehl]         Hilfe, NET Befehl /? ebenso
 *   NET HELPMSG Nummer        Text zu einer Windows-/LAN-Manager-Fehlernummer
 *   NET CONFIG [WORKSTATION]  Rechner, Benutzer, Domaene, Karte, Version
 *   NET STATISTICS [WORKSTATION]  Zaehler der Netzkarten, Verbindungen
 *   NET TIME [\\Rechner | /DOMAIN[:Name]] [/SET [/Y]]
 *                             Uhrzeit eines SMB-Servers (aus der SMB2-
 *                             Aushandlung, ohne Anmeldung), Zeitzone aus TZ
 *   NET VIEW \\Rechner [/ALL]  Freigaben (SMBCLI --view, srvsvc ueber RPC)
 *   NET USER/GROUP/ACCOUNTS ... /DOMAIN   Domaene lesen (NET1.EXE, LDAP)
 * ERRORLEVEL: 0 ok, 1 Fehler, 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#include "arcutil.h"
#include "tz.h"
#define NET_RESOLVE_FN
#include "net.h"
#define CTRY_NO_DPAT
#include "country.h"

static char args[FAPI_ARGLEN], cl[FAPI_ARGLEN + FAPI_PATHLEN], pwf[FAPI_PATHLEN];
static char unc[128], user[64], dom[64], pass[64], msg[100], w[128];
static char lst[FAPI_PATHLEN], lb[2048], ub[80];
static unsigned lbn, persist = 1, nosave, havepw, star;
static struct fapi_ufsreg rg;

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

static void usage(void)
{
    out("NET USE [X: [\\\\Rechner\\Freigabe [Kennwort|*] [/USER:[Dom\x84ne\\]Name]] | X: /DELETE]\r\n"
        "        [/PERSISTENT:YES|NO]\r\n"
        "NET USE /RESTORE   gemerkte Verbindungen herstellen\r\n");
    DosExit(2);
}

static void fail(const char *m)
{
    out(m);
    out("\r\n");
    DosExit(1);
}

/* Kennwort ohne Echo von der Tastatur */
static void ask_pass(void)
{
    unsigned n = 0, c;
    out("Kennwort f\x81r ");
    out(unc);
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

static void cat(char *d, const char *s, unsigned max)
{
    unsigned n = slen(d);
    scopy(d + n, s, max - n);
}

/* Kerberos-Ablage vorhanden (Pfad wie krb5.c krb_cc_path)? */
static int have_tickets(void)
{
    unsigned n, h;
    if (FAPI_FAILED(DosGetEnv("KRB5CCNAME", w, sizeof(w))) || !w[0]) {
        if (FAPI_FAILED(DosGetEnv("TEMP", w, sizeof(w) - 14)) || !w[0])
            scopy(w, "C:", sizeof(w));
        n = slen(w);
        if (w[n - 1] != '\\')
            w[n++] = '\\';
        scopy(w + n, "KRB5CC.DAT", sizeof(w) - n);
    }
    h = DosOpen(w);
    if (FAPI_FAILED(h))
        return 0;
    DosClose(h);
    return 1;
}

/* Rechner im UNC als Zahl (IPv4/IPv6)? Dann geht Kerberos nicht */
static int unc_numeric(void)
{
    const char *p = unc + 2;
    unsigned dig = 1;
    for (; *p && *p != '\\'; p++)
        if (*p == ':')
            return 1;
        else if (!(*p >= '0' && *p <= '9') && *p != '.')
            dig = 0;
    return dig;
}

/* /USER:[Dom\]Name auswerten (p zeigt hinter "/U"); 0 = ok */
static int take_user(char *p)
{
    unsigned n;
    while (*p && *p != ':')
        p++;
    if (!*p)
        return 1;
    p++;
    while (*p == '\\')                         /* \\DOM\Name wie DOM\Name     */
        p++;
    if (!*p)
        return 1;
    for (n = 0; p[n] && p[n] != '\\'; n++)
        ;
    if (p[n]) {
        p[n] = 0;
        scopy(dom, p, sizeof(dom));
        p += n + 1;
    }
    scopy(user, p, sizeof(user));
    return 0;
}

/* ---- gemerkte Verbindungen (X:\DOS\SYSTEM\NETUSE.LST) ------------------ */

static void lst_load(void)
{
    unsigned h, n;
    if (FAPI_FAILED(DosGetEnv("COMSPEC", lst, sizeof(lst))) || !lst[0])
        scopy(lst, "C:", sizeof(lst));
    lst[2] = 0;
    cat(lst, "\\DOS\\SYSTEM\\NETUSE.LST", sizeof(lst));
    lbn = 0;
    h = DosOpen(lst);
    if (FAPI_FAILED(h))
        return;
    n = DosRead(h, lb, sizeof(lb) - 1);
    DosClose(h);
    if (!FAPI_FAILED(n))
        lbn = n;
    lb[lbn] = 0;
    if (lbn >= 12 && lb[0] == 'P' && lb[10] == '=')   /* PERSISTENT=YES/NO */
        persist = up(lb[11]) != 'N';
}

/* Zeile i (ab 0, ohne die Kopfzeile) -> Zeiger, Laenge; 0 = keine */
static char *lst_line(unsigned i, unsigned *len)
{
    unsigned o = 0, k = 0, e;
    while (o < lbn) {
        for (e = o; e < lbn && lb[e] != '\r' && lb[e] != '\n'; e++)
            ;
        if (e > o && lb[o] != ';' && !(lb[o] == 'P' && e > o + 1 && lb[o + 1] != ':')) {
            if (k++ == i) {
                *len = e - o;
                return lb + o;
            }
        }
        o = e;
        while (o < lbn && (lb[o] == '\r' || lb[o] == '\n'))
            o++;
    }
    return 0;
}

/* Liste neu schreiben: Kopf, alle Zeilen ausser Laufwerk skip, dazu add */
static void lst_save(unsigned skip, const char *add)
{
    unsigned h, i, n;
    char *l;
    h = DosCreate(lst);
    if (FAPI_FAILED(h))
        return;
    DosWrite(h, persist ? "PERSISTENT=YES\r\n" : "PERSISTENT=NO\r\n",
             persist ? 16 : 15);
    for (i = 0; (l = lst_line(i, &n)) != 0; i++)
        if (n > 2 && (unsigned)(up(l[0]) - 'A') != skip) {
            DosWrite(h, l, n);
            DosWrite(h, "\r\n", 2);
        }
    if (add) {
        DosWrite(h, add, slen(add));
        DosWrite(h, "\r\n", 2);
    }
    DosClose(h);
}

/* steht Laufwerk d in der Liste? */
static unsigned lst_has(unsigned d)
{
    unsigned i, n;
    char *l;
    for (i = 0; (l = lst_line(i, &n)) != 0; i++)
        if (n > 2 && (unsigned)(up(l[0]) - 'A') == d)
            return 1;
    return 0;
}

static void list(void)
{
    unsigned d, n = 0, i, k;
    char l[4], *e;
    out(persist ? "Neue Verbindungen werden gespeichert.\r\n\r\n"
                : "Neue Verbindungen werden nicht gespeichert.\r\n\r\n");
    l[1] = ':';
    l[2] = 0;
    for (d = 2; d < 26; d++) {
        e = 0;
        if (fapi_call(FAPI_UFS, UFS_INFO, d, (unsigned)&rg)) {
            for (i = 0; (e = lst_line(i, &k)) != 0; i++)   /* nur gemerkt?  */
                if (k > 2 && (unsigned)(up(e[0]) - 'A') == d)
                    break;
            if (!e)
                continue;
        }
        if (!n++)
            out("Status         Lokal    Remote\r\n"
                "------------------------------------------------------\r\n");
        out(e ? "Getrennt       " : "OK             ");
        l[0] = (char)('A' + d);
        out(l);
        out("       ");
        if (e) {
            for (i = 3; i < k && e[i] != ' '; i++)
                ;
            DosWrite(STDOUT, e + 3, i - 3);
        } else
            out(rg.name);
        out("\r\n");
    }
    if (!n)
        out("Keine Netzlaufwerke verbunden.\r\n");
}

/* Verbindung merken (nach Erfolg) */
static void remember(unsigned drive)
{
    ub[0] = (char)('A' + drive);
    ub[1] = ':';
    ub[2] = ' ';
    scopy(ub + 3, unc, sizeof(ub) - 3);
    if (user[0]) {
        cat(ub, " /USER:", sizeof(ub));
        if (dom[0]) {
            cat(ub, dom, sizeof(ub));
            cat(ub, "\\", sizeof(ub));
        }
        cat(ub, user, sizeof(ub));
    }
    lst_save(drive, ub);
}

static void drv_out(unsigned drive)
{
    msg[0] = (char)('A' + drive);
    msg[1] = ':';
    msg[2] = 0;
    out(msg);
}

/* Laufwerk mit unc/user/dom/pass verbinden; 0 = ok, sonst Meldung */
static const char *connect(unsigned drive)
{
    unsigned kerb, h, n, t;

    if (!fapi_call(FAPI_UFS, UFS_INFO, drive, (unsigned)&rg)) {
        if (same(rg.name, unc))
            return 0;                           /* schon so verbunden      */
        return "Das Laufwerk ist schon verbunden.";
    }
    kerb = !user[0] && !havepw && !star && !unc_numeric() && have_tickets();
    if (star || (user[0] && !havepw))
        ask_pass();

    /* Kennwortdatei in %TEMP% */
    if (FAPI_FAILED(DosGetEnv("TEMP", pwf, sizeof(pwf) - 16)) || !pwf[0])
        scopy(pwf, "C:\\", sizeof(pwf));
    n = slen(pwf);
    if (pwf[n - 1] != '\\')
        pwf[n++] = '\\';
    pwf[n] = 0;
    {
        unsigned tk = DosQTicks();
        char nm[13];
        nm[0] = 'N'; nm[1] = 'U';
        for (h = 0; h < 4; h++)
            nm[2 + h] = "0123456789ABCDEF"[(tk >> (12 - 4 * h)) & 15];
        nm[6] = '.'; nm[7] = 'P'; nm[8] = 'W'; nm[9] = 'D'; nm[10] = 0;
        cat(pwf, nm, sizeof(pwf));
    }
    h = DosCreate(pwf);
    if (FAPI_FAILED(h))
        return "Kann die Datei f\x81r das Kennwort nicht anlegen (%TEMP%).";
    DosWrite(h, pass, slen(pass));
    DosClose(h);
    for (n = 0; n < sizeof(pass); n++)
        pass[n] = 0;

    /* SMBCLI losgeloest starten: Laufwerk von COMSPEC, \DOS\SMBCLI.EXE */
    if (FAPI_FAILED(DosGetEnv("COMSPEC", cl, sizeof(cl))) || !cl[0])
        scopy(cl, "C:", sizeof(cl));
    cl[2] = 0;
    cat(cl, "\\DOS\\SMBCLI.EXE ", sizeof(cl));
    cat(cl, unc, sizeof(cl));
    if (user[0]) {
        cat(cl, " -U ", sizeof(cl));
        cat(cl, user, sizeof(cl));
    }
    if (dom[0]) {
        cat(cl, " -W ", sizeof(cl));
        cat(cl, dom, sizeof(cl));
    }
    if (kerb)
        cat(cl, " -k", sizeof(cl));
    cat(cl, " -P ", sizeof(cl));
    cat(cl, pwf, sizeof(cl));
    cat(cl, " --serve ", sizeof(cl));
    n = slen(cl);
    cl[n] = (char)('A' + drive);
    cl[n + 1] = ':';
    cl[n + 2] = 0;
    if (DosExecPgm(cl, EXEC_DETACH)) {
        DosDelete(pwf);
        return "SMBCLI.EXE nicht gefunden (\\DOS).";
    }

    /* warten: Laufwerk da (Datei leer) oder Meldung "!..." */
    for (t = 0; t < 600; t++) {
        DosSleep(100);
        if (!fapi_call(FAPI_UFS, UFS_INFO, drive, (unsigned)&rg)) {
            DosDelete(pwf);
            out("Laufwerk ");
            drv_out(drive);
            out(" ist mit ");
            out(rg.name);
            out(" verbunden.\r\n");
            return 0;
        }
        h = DosOpen(pwf);
        if (FAPI_FAILED(h))
            continue;
        n = DosRead(h, msg, sizeof(msg) - 1);
        DosClose(h);
        if (FAPI_FAILED(n) || !n || msg[0] != '!')
            continue;
        msg[n] = 0;
        DosDelete(pwf);
        return msg + 1;
    }
    DosDelete(pwf);
    return "Keine Antwort vom Server (Frist).";
}

/* NET USE /RESTORE: gemerkte, nicht verbundene Laufwerke herstellen */
static unsigned restore(void)
{
    unsigned i, n, d, bad = 0;
    const char *e;
    char *l, *q;
    for (i = 0; (l = lst_line(i, &n)) != 0; i++) {
        if (n < 6 || n >= sizeof(ub) || l[1] != ':')
            continue;
        d = (unsigned)(up(l[0]) - 'A');
        if (d < 2 || d > 25 || !fapi_call(FAPI_UFS, UFS_INFO, d, (unsigned)&rg))
            continue;                           /* z. B. Heimlaufwerk      */
        for (d = 0; d < n; d++)
            ub[d] = l[d];
        d = (unsigned)(up(l[0]) - 'A');
        ub[n] = 0;
        user[0] = dom[0] = pass[0] = 0;
        havepw = star = 0;
        q = next_word(ub);
        q = word(q, unc, sizeof(unc));
        q = skip(q);
        if (q[0] == '/' && up(q[1]) == 'U')
            take_user(q + 2);
        e = connect(d);
        if (e) {
            drv_out(d);
            out(" ");
            out(unc);
            out(": ");
            out(e);
            out("\r\n");
            bad = 1;
        }
    }
    return bad;
}

static void net_use(char *a)
{
    const char *e;
    unsigned drive = 0xFFFF, del = 0, rest = 0, pset = 0, t;

    lst_load();
    for (; *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (w[0] && w[1] == ':' && !w[2] && drive == 0xFFFF) {
            drive = (unsigned)(up(w[0]) - 'A');
            if (drive < 2 || drive > 25)
                fail("Ung\x81ltiges Laufwerk (C: bis Z:).");
        } else if (w[0] == '\\' && w[1] == '\\')
            scopy(unc, w, sizeof(unc));
        else if (w[0] == '/' && (up(w[1]) == 'D') && (!w[2] || same(w, "/DELETE")))
            del = 1;
        else if (w[0] == '/' && up(w[1]) == 'P') {      /* /PERSISTENT:x  */
            for (t = 0; w[t] && w[t] != ':'; t++)
                ;
            if (!w[t])
                usage();
            persist = up(w[t + 1]) == 'Y' || up(w[t + 1]) == 'J';
            pset = 1;
        } else if (w[0] == '/' && same(w, "/RESTORE"))
            rest = 1;
        else if (w[0] == '/' && same(w, "/NOSAVE"))   /* LOGON: Heimlaufwerk */
            nosave = 1;
        else if (w[0] == '/' && up(w[1]) == 'U') {
            if (take_user(w + 2))
                usage();
        } else if (w[0] == '*' && !w[1])
            star = 1;
        else if (w[0] != '/' && unc[0] && !havepw) {
            scopy(pass, w, sizeof(pass));
            havepw = 1;
        } else
            usage();
    }
    if (rest)
        DosExit(restore());
    if (drive == 0xFFFF) {
        if (unc[0] || del)
            usage();
        if (pset) {
            lst_save(0xFFFF, 0);
            out(persist ? "Neue Verbindungen werden gespeichert.\r\n"
                        : "Neue Verbindungen werden nicht gespeichert.\r\n");
            DosExit(0);
        }
        list();
        DosExit(0);
    }
    if (del) {
        unsigned had = lst_has(drive);
        t = fapi_call(FAPI_UFS, UFS_DEL, drive, 0);
        if (t == FAPI_ERR_INUSE)
            fail("Laufwerk wird noch benutzt (offene Dateien oder aktuelles Verzeichnis).");
        if (had)
            lst_save(drive, 0);                 /* auch nicht mehr merken  */
        if (t) {
            if (!had)
                fail("Das Laufwerk ist kein Netzlaufwerk.");
            out("Gespeicherte Verbindung entfernt.\r\n");
            DosExit(0);
        }
        for (t = 0; t < 50 && !fapi_call(FAPI_UFS, UFS_INFO, drive, (unsigned)&rg); t++)
            DosSleep(100);
        out("Netzlaufwerk getrennt.\r\n");
        DosExit(0);
    }
    if (!unc[0])
        usage();
    e = connect(drive);
    if (e)
        fail(e);
    if (pset || !nosave) {
        if (persist && !nosave)
            remember(drive);
        else if (pset)
            lst_save(drive, 0);                 /* Vorgabe merken, Eintrag weg */
    }
    DosExit(0);
}

/* ======================================================================
 * Weitere Unterbefehle (wie NET von Windows, nur Arbeitsplatz)
 * ====================================================================== */

#define OK_MSG "Der Befehl wurde erfolgreich ausgef\x81hrt.\r\n"

static struct fapi_netif nf;
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned char ad[16], sb[512];

/* 64-Bit-Zahl (4 Worte, w[3] oben) / d -> Rest; ohne Watcom-Hilfen */
unsigned long nt_dv(unsigned long n, unsigned d);
#pragma aux nt_dv = "div bx" __parm [__dx __ax] [__bx] __value [__dx __ax];
static unsigned dv64(unsigned *w, unsigned d)
{
    unsigned i = 4, r = 0;
    unsigned long q;
    while (i--) {
        q = nt_dv((unsigned long)r << 16 | w[i], d);
        w[i] = (unsigned)q;
        r = (unsigned)(q >> 16);
    }
    return r;
}

static void sys_path(char *d, const char *name, unsigned max)
{
    if (FAPI_FAILED(DosGetEnv("COMSPEC", d, max)) || !d[0])
        scopy(d, "C:", max);
    d[2] = 0;
    cat(d, name, max);
}

/* Programm aus \DOS starten und auf das Ende warten -> ERRORLEVEL */
static unsigned run_sub(const char *prog, const char *tail)
{
    sys_path(cl, "\\DOS\\", sizeof(cl));
    cat(cl, prog, sizeof(cl));
    cat(cl, " ", sizeof(cl));
    cat(cl, tail, sizeof(cl));
    if (DosExecPgm(cl, EXEC_SYNC)) {
        out(prog);
        fail(" nicht gefunden (\\DOS).");
    }
    return (unsigned)DosQChildRC();
}

static void pad(const char *s, unsigned wd)
{
    unsigned n = slen(s);
    out(s);
    while (n++ < wd)
        out(" ");
}

static void field(const char *name, const char *val)
{
    pad(name, 37);
    out(val);
    out("\r\n");
}

static void num_field(const char *name, unsigned long v)
{
    pad(name, 30);
    put_num(v, 12, 0);
    out("\r\n");
}

/* ---- Syntax und Hilfe ------------------------------------------------- */

static void syntax(void)
{
    out("Die Syntax dieses Befehls lautet:\r\n\r\nNET\r\n"
        "    [ ACCOUNTS | CONFIG | GROUP | HELP | HELPMSG | STATISTICS |\r\n"
        "      TIME | USE | USER | VIEW ]\r\n");
    DosExit(2);
}

static const char *const hlp[] = {
    "ACCOUNTS", "NET ACCOUNTS /DOMAIN\r\n\r\nZeigt die Kennwort- und Sperrregeln der Dom\x84ne (LDAP, braucht\r\nein Kerberos-Ticket von LOGON oder KINIT).\r\n",
    "CONFIG", "NET CONFIG [WORKSTATION]\r\n\r\nZeigt Rechnername, Benutzer, Dom\x84ne, Netzkarten und die Version.\r\nAT-DOS hat keinen Serverdienst.\r\n",
    "GROUP", "NET GROUP [Gruppe] /DOMAIN\r\n\r\nListet die Gruppen der Dom\x84ne bzw. die Mitglieder einer Gruppe (LDAP).\r\n",
    "HELP", "NET HELP [Befehl]\r\nNET Befehl /?\r\n\r\nZeigt die Hilfe zu einem NET-Befehl.\r\n",
    "HELPMSG", "NET HELPMSG Nummer\r\n\r\nErkl\x84rt eine Windows- bzw. LAN-Manager-Fehlernummer (z. B. 53, 86, 2250).\r\n",
    "STATISTICS", "NET STATISTICS [WORKSTATION]\r\n\r\nZeigt die Z\x84hler der Netzkarten und die verbundenen Netzlaufwerke.\r\n",
    "TIME", "NET TIME [\\\\Rechner | /DOMAIN[:Name]] [/SET [/Y]]\r\n\r\nZeigt die Uhrzeit eines SMB-Servers (ohne Anmeldung) und stellt mit\r\n/SET die eigene Uhr danach. Ohne Rechner: der Anmeldeserver (LOGON).\r\nZeitzone aus TZ wie bei SNTP.\r\n",
    "USE", "NET USE [X: [\\\\Rechner\\Freigabe [Kennwort|*] [/USER:[Dom\x84ne\\]Name]] | X: /DELETE]\r\n        [/PERSISTENT:YES|NO]\r\nNET USE /RESTORE\r\n\r\nVerbindet Netzlaufwerke (SMB 2/3) und zeigt sie an.\r\n",
    "USER", "NET USER [Name] /DOMAIN\r\n\r\nListet die Benutzer der Dom\x84ne bzw. zeigt ein Konto (LDAP).\r\nAT-DOS hat keine lokalen Konten.\r\n",
    "VIEW", "NET VIEW \\\\Rechner [/ALL] [/USER:[Dom\x84ne\\]Name]\r\n\r\nListet die Freigaben eines Servers (mit /ALL auch die versteckten).\r\nAngemeldet wird mit Kerberos (LOGON/KINIT), mit /USER: per Kennwort,\r\nsonst als Gast.\r\n",
    0
};

static void help(const char *c)
{
    unsigned i;
    if (c && *c) {
        for (i = 0; hlp[i]; i += 2)
            if (same(hlp[i], c)) {
                out("Die Syntax dieses Befehls lautet:\r\n\r\n");
                out(hlp[i + 1]);
                DosExit(0);
            }
    }
    out("NET-Befehle unter AT-DOS (NET HELP Befehl zeigt Einzelheiten):\r\n\r\n"
        "  NET ACCOUNTS /DOMAIN   Kennwortregeln der Dom\x84ne\r\n"
        "  NET CONFIG             Einstellungen der Arbeitsstation\r\n"
        "  NET GROUP /DOMAIN      Gruppen der Dom\x84ne\r\n"
        "  NET HELPMSG Nummer     Fehlernummer erkl\x84ren\r\n"
        "  NET STATISTICS         Z\x84hler der Netzkarten\r\n"
        "  NET TIME               Uhrzeit eines Servers, Uhr stellen\r\n"
        "  NET USE                Netzlaufwerke\r\n"
        "  NET USER /DOMAIN       Benutzer der Dom\x84ne\r\n"
        "  NET VIEW \\\\Rechner     Freigaben eines Servers\r\n");
    DosExit(0);
}

static const struct { unsigned n; const char *t; } msgs[] = {
    { 2, "Das System kann die angegebene Datei nicht finden." },
    { 3, "Das System kann den angegebenen Pfad nicht finden." },
    { 5, "Zugriff verweigert." },
    { 32, "Die Datei wird von einem anderen Prozess verwendet." },
    { 51, "Der Remotecomputer ist nicht verf\x81gbar." },
    { 53, "Der Netzwerkpfad wurde nicht gefunden." },
    { 59, "Unerwarteter Netzwerkfehler." },
    { 64, "Der angegebene Netzwerkname ist nicht mehr verf\x81gbar." },
    { 65, "Der Netzwerkzugriff wurde verweigert." },
    { 67, "Der Netzwerkname wurde nicht gefunden." },
    { 85, "Der lokale Ger\x84tename wird bereits verwendet." },
    { 86, "Das angegebene Netzwerkkennwort ist falsch." },
    { 1219, "Mehrere Verbindungen zu einem Server mit verschiedenen Benutzernamen sind nicht zul\x84ssig." },
    { 1311, "Es ist kein Anmeldeserver verf\x81gbar." },
    { 1326, "Der Benutzername oder das Kennwort ist falsch." },
    { 1327, "Kontoeinschr\x84nkungen verhindern die Anmeldung." },
    { 1330, "Das Kennwort ist abgelaufen." },
    { 1331, "Das Konto ist deaktiviert." },
    { 1909, "Das Konto ist gesperrt." },
    { 2102, "Der Arbeitsstationsdienst ist nicht gestartet." },
    { 2221, "Der Benutzername wurde nicht gefunden." },
    { 2220, "Der Gruppenname wurde nicht gefunden." },
    { 2250, "Die Netzwerkverbindung wurde nicht gefunden." },
    { 2310, "Die Freigabe ist nicht vorhanden." },
    { 2404, "Das Ger\x84t wird von einem aktiven Prozess verwendet und kann nicht getrennt werden." },
    { 0, 0 }
};

static void helpmsg(const char *a)
{
    unsigned n = 0, i;
    if (*a < '0' || *a > '9')
        syntax();
    for (; *a >= '0' && *a <= '9'; a++)
        n = n * 10 + (*a - '0');
    for (i = 0; msgs[i].t; i++)
        if (msgs[i].n == n) {
            out("\r\n");
            out(msgs[i].t);
            out("\r\n");
            DosExit(0);
        }
    put_num(n, 0, 0);
    fail(" ist keine Meldungsnummer, die AT-DOS kennt.");
}

/* ---- Netzkarten, Domaene ------------------------------------------------ */

static int card(unsigned i)
{
    nf.ifc = i;
    return !DosNet(NET_IFINFO, &nf, sizeof(nf));
}

static void comp_name(char *d, unsigned max)
{
    if (FAPI_FAILED(DosGetEnv("COMPUTERNAME", d, max)) || !d[0]) {
        d[0] = 0;
        if (card(0))
            scopy(d, nf.host, max);
    }
    if (!d[0])
        scopy(d, "ATDOS", max);
}

/* DOMAIN.KEY (DOMJOIN): Realm ab 8, DNS-Name ab 72 */
static int dj_load(void)
{
    unsigned h, n;
    sys_path(lst, "\\DOS\\SYSTEM\\DOMAIN.KEY", sizeof(lst));
    h = DosOpen(lst);
    if (FAPI_FAILED(h))
        return 0;
    n = DosRead(h, lb, 152);
    DosClose(h);
    return !FAPI_FAILED(n) && n == 152 && lb[0] == 'A' && lb[1] == 'T';
}

static void config(char *a)
{
    unsigned v, i, k;
    char t[8];
    word(a, w, sizeof(w));
    if (!w[0]) {
        out("Folgende Dienste werden ausgef\x81hrt und k\x94nnen ge\x84ndert werden:\r\n\r\n"
            "   Arbeitsstation\r\n\r\n" OK_MSG);
        DosExit(0);
    }
    if (!same(w, "WORKSTATION") && !same(w, "ARBEITSSTATION"))
        fail("Der Serverdienst ist unter AT-DOS nicht vorhanden.");
    comp_name(msg, sizeof(msg));
    scopy(w, "\\\\", sizeof(w));
    cat(w, msg, sizeof(w));
    field("Computername", w);
    if (card(0) && nf.domain[0]) {
        cat(msg, ".", sizeof(msg));
        cat(msg, nf.domain, sizeof(msg));
    }
    field("Vollst\x84ndiger Computername", msg);
    if (FAPI_FAILED(DosGetEnv("USERNAME", w, sizeof(w))) || !w[0])
        scopy(w, "(nicht angemeldet)", sizeof(w));
    field("Benutzername", w);
    out("\r\nArbeitsstation ist aktiv auf\r\n");
    for (i = 0; card(i); i++) {
        out("        ");
        out(nf.name);
        out(" (");
        for (k = 0; k < 6; k++) {
            t[0] = "0123456789ABCDEF"[nf.mac[k] >> 4];
            t[1] = "0123456789ABCDEF"[nf.mac[k] & 15];
            t[2] = 0;
            out(t);
        }
        out(")");
        for (k = 0; k < nf.naddr && k < NET_MAXADDR; k++)
            if (nf.addr[k].kind != NA_LINK) {
                out(" ");
                out(net_ntop(nf.addr[k].a, w));
            }
        out("\r\n");
    }
    if (!i)
        out("        (keine Netzkarte)\r\n");
    v = DosGetVersion();
    scopy(w, "AT-DOS ", sizeof(w));
    k = slen(w);
    w[k++] = (char)('0' + (v >> 8));
    w[k++] = '.';
    w[k++] = (char)('0' + (v & 0xFF) / 10);
    w[k++] = (char)('0' + (v & 0xFF) % 10);
    w[k] = 0;
    out("\r\n");
    field("Softwareversion", w);
    out("\r\n");
    if (dj_load()) {
        for (k = 0; lb[8 + k] && lb[8 + k] != '.' && k < sizeof(w) - 1; k++)
            w[k] = up(lb[8 + k]);
        w[k] = 0;
        field("Arbeitsstationsdom\x84ne", w);
        field("DNS-Name der Arbeitsstationsdom\x84ne", lb + 72);
    } else
        field("Arbeitsstationsdom\x84ne", "(keine - DOMJOIN nicht ausgef\x81hrt)");
    if (FAPI_FAILED(DosGetEnv("USERDOMAIN", w, sizeof(w))) || !w[0])
        scopy(w, "(nicht angemeldet)", sizeof(w));
    field("Anmeldedom\x84ne", w);
    if (!FAPI_FAILED(DosGetEnv("LOGONSERVER", w, sizeof(w))) && w[0])
        field("Anmeldeserver", w);
    out(OK_MSG);
    DosExit(0);
}

/* jetzt als Unix-Zeit (Ortszeit) */
static unsigned long now_loc(void)
{
    unsigned char dt[4];
    DosGetDateTime(dt);
    return fat2unix(dt[0] | (unsigned)dt[1] << 8, dt[2] | (unsigned)dt[3] << 8);
}

static void stamp(unsigned long t)
{
    unsigned date, time;
    unix2fat(t, &date, &time);
    ctry_date(w, date);
    out(w);
    out(" ");
    ctry_time(w, time, 0);
    out(w);
}

static void statistics(char *a)
{
    unsigned i, d, n = 0;
    unsigned long upt;
    word(a, w, sizeof(w));
    if (!w[0]) {
        out("Statistik ist f\x81r die folgenden ausgef\x81hrten Dienste verf\x81gbar:\r\n\r\n"
            "   Arbeitsstation\r\n\r\n" OK_MSG);
        DosExit(0);
    }
    if (!same(w, "WORKSTATION") && !same(w, "ARBEITSSTATION"))
        fail("Der Serverdienst ist unter AT-DOS nicht vorhanden.");
    comp_name(msg, sizeof(msg));
    out("Statistik f\x81r Arbeitsstation \\\\");
    out(msg);
    out("\r\n\r\n\r\nStatistik seit ");
    upt = au_div(DosQTicks(), 100, 0);
    stamp(now_loc() - upt);
    out("\r\n\r\n");
    for (i = 0; card(i); i++) {
        out("  Netzkarte ");
        out(nf.name);
        out("\r\n");
        num_field("  Empfangene Rahmen", nf.rx_ok);
        num_field("  Gesendete Rahmen", nf.tx_ok);
        num_field("  Empfangsfehler", nf.rx_err);
        num_field("  Sendefehler", nf.tx_err);
        num_field("  Verworfene Rahmen", nf.rx_drop);
        out("\r\n");
    }
    if (!i)
        out("  (keine Netzkarte)\r\n\r\n");
    for (d = 2; d < 26; d++)
        if (!fapi_call(FAPI_UFS, UFS_INFO, d, (unsigned)&rg))
            n++;
    num_field("Verbundene Netzlaufwerke", n);
    out(OK_MSG);
    DosExit(0);
}

/* ---- NET TIME: Systemzeit aus der SMB2-Aushandlung ---------------------- */

static int rd(unsigned char *d, unsigned n)
{
    unsigned r;
    while (n) {
        io.buf = (unsigned)d;
        io.len = n;
        io.flags = 0;
        io.timeout = 5000;
        r = DosNet(NET_RECV, &io, sizeof(io));
        if (!r || FAPI_FAILED(r))
            return 1;
        d += r;
        n -= r;
    }
    return 0;
}

/* -> Fehlermeldung oder 0; utc, ms (Restmillisekunden) */
static const char *smb_time(const char *h, unsigned long *utc, unsigned *ms)
{
    unsigned s, k, n, wd[4];
    unsigned long l;
    if (net_lookup(h, ad))
        return "Der Rechner wurde nicht gefunden.";
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    s = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(s))
        return "Kein Netz (TCPIP.SYS fehlt).";
    io.h = s;
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(445);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.timeout = 5000;
    if (DosNet(NET_CONNECT, &io, sizeof(io))) {
        DosClose(s);
        return "Der Rechner antwortet nicht (Port 445).";
    }
    for (k = 0; k < 112; k++)
        sb[k] = 0;
    sb[3] = 108;                        /* NetBIOS-Laenge                */
    sb[4] = 0xFE; sb[5] = 'S'; sb[6] = 'M'; sb[7] = 'B';
    sb[8] = 64;
    sb[18] = 1;                         /* CreditRequest                 */
    sb[68] = 36;                        /* NEGOTIATE                     */
    sb[70] = 4;                         /* vier Dialekte                 */
    sb[72] = 1;                         /* signieren moeglich            */
    sb[104] = 0x02; sb[105] = 0x02;     /* 2.0.2, 2.1, 3.0, 3.0.2        */
    sb[106] = 0x10; sb[107] = 0x02;
    sb[108] = 0x00; sb[109] = 0x03;
    sb[110] = 0x02; sb[111] = 0x03;
    io.buf = (unsigned)sb;
    io.len = 112;
    io.flags = 0;
    if (DosNet(NET_SEND, &io, sizeof(io)) != 112 || rd(sb, 4)) {
        DosClose(s);
        return "Keine Antwort vom Server.";
    }
    l = (unsigned long)sb[1] << 16 | (unsigned)sb[2] << 8 | sb[3];
    n = l > sizeof(sb) ? sizeof(sb) : (unsigned)l;
    if (l < 64 + 48 || rd(sb, n)) {
        DosClose(s);
        return "Keine g\x81ltige SMB-Antwort.";
    }
    for (l -= n; l; l -= k) {           /* Rest (Sicherheitsblob) weg    */
        k = l > 64 ? 64 : (unsigned)l;
        if (rd((unsigned char *)lb, k))
            break;
    }
    DosClose(s);
    if (sb[0] != 0xFE || sb[1] != 'S' || sb[8] | sb[9] | sb[10] | sb[11])
        return "Keine g\x81ltige SMB-Antwort.";
    for (k = 0; k < 4; k++)             /* FILETIME (100 ns seit 1601)   */
        wd[k] = sb[104 + 2 * k] | (unsigned)sb[105 + 2 * k] << 8;
    if (!(wd[0] | wd[1] | wd[2] | wd[3]))
        return "Der Server nennt keine Uhrzeit.";
    dv64(wd, 10000);                    /* -> ms                         */
    *ms = dv64(wd, 1000);               /* -> s, Rest ms                 */
    k = wd[0] < 0x9100;                 /* - 11644473600 (1601 -> 1970)  */
    wd[0] -= 0x9100;
    n = (unsigned long)wd[1] < 0xB610UL + k;
    wd[1] -= 0xB610 + k;
    k = (unsigned long)wd[2] < 2UL + n;
    wd[2] -= 2 + n;
    wd[3] -= k;
    if (wd[3] || wd[2])
        return "Unsinnige Uhrzeit vom Server.";
    *utc = (unsigned long)wd[1] << 16 | wd[0];
    return 0;
}

static void ntime(char *a)
{
    unsigned set = 0, yes = 0, isdom = 0, ms, date, time, k;
    unsigned long utc, loc, t0;
    long off;
    int dst;
    const char *e;
    unsigned char dt[4];

    unc[0] = 0;
    for (; *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (w[0] == '\\' && w[1] == '\\')
            scopy(unc, w, sizeof(unc));
        else if (same(w, "/SET"))
            set = 1;
        else if (same(w, "/Y") || same(w, "/YES") || same(w, "/J"))
            yes = 1;
        else if (w[0] == '/' && up(w[1]) == 'D') {      /* /DOMAIN[:x]    */
            isdom = 1;
            for (k = 0; w[k] && w[k] != ':'; k++)
                ;
            if (w[k] && w[k + 1]) {
                scopy(unc, "\\\\", sizeof(unc));
                cat(unc, w + k + 1, sizeof(unc));
            }
        } else
            syntax();
    }
    if (!unc[0] && isdom) {
        if (FAPI_FAILED(DosGetEnv("USERDNSDOMAIN", w, sizeof(w))) || !w[0]) {
            if (!dj_load())
                fail("Es wurde kein Zeitserver gefunden (keine Dom\x84ne).");
            scopy(w, lb + 72, sizeof(w));
        }
        scopy(unc, "\\\\", sizeof(unc));
        cat(unc, w, sizeof(unc));
    }
    if (!unc[0]) {
        if (FAPI_FAILED(DosGetEnv("LOGONSERVER", w, sizeof(w))) || !w[0])
            fail("Es wurde kein Zeitserver gefunden (NET TIME \\\\Rechner angeben).");
        scopy(unc, w[0] == '\\' ? "" : "\\\\", sizeof(unc));
        cat(unc, w, sizeof(unc));
    }
    e = smb_time(unc + 2, &utc, &ms);
    if (e)
        fail(e);
    t0 = DosQTicks();
    tz_init();
    ctry_init();
    off = tz_off(utc, &dst);
    out("Aktuelle Zeit auf ");
    out(unc);
    out(" ist ");
    loc = utc + off;
    unix2fat(loc, &date, &time);
    ctry_date(w, date);
    out(w);
    out(" ");
    ctry_time(w, time, 1);
    out(w);
    out("\r\n\r\n");
    if (set) {
        if (!yes) {
            out("Die aktuelle lokale Uhrzeit ist ");
            loc = now_loc();
            unix2fat(loc, &date, &time);
            ctry_date(w, date);
            out(w);
            out(" ");
            ctry_time(w, time, 1);
            out(w);
            out(". M\x94" "chten Sie die lokale\r\nComputerzeit mit der von ");
            out(unc);
            out(" abgleichen? (J/N) [J]: ");
            k = KbdCharIn() & 0xFF;
            out("\r\n");
            if (k == 'n' || k == 'N') {
                out(OK_MSG);
                DosExit(0);
            }
        }
        /* Zeit seit der Antwort dazu, dann auf die naechste gerade Sekunde */
        ms += (unsigned)(DosQTicks() - t0) * 10;
        utc += ms / 1000;
        ms %= 1000;
        loc = utc + tz_off(utc, &dst) + 1;
        k = 1000 - ms;
        if (loc & 1) {
            k += 1000;
            loc++;
        }
        DosSleep(k);
        unix2fat(loc, &date, &time);
        dt[0] = (unsigned char)date;
        dt[1] = (unsigned char)(date >> 8);
        dt[2] = (unsigned char)time;
        dt[3] = (unsigned char)(time >> 8);
        if (DosSetDateTime(dt))
            fail("Die Uhr l\x84\xE1t sich nicht stellen.");
    }
    out(OK_MSG);
    DosExit(0);
}

/* ---- NET VIEW: SMBCLI --view ------------------------------------------- */

static void view(char *a)
{
    static char vt[220];
    unsigned all = 0;
    unc[0] = user[0] = dom[0] = 0;
    for (; *a; a = next_word(a)) {
        word(a, w, sizeof(w));
        if (w[0] == '\\' && w[1] == '\\')
            scopy(unc, w, sizeof(unc));
        else if (same(w, "/ALL"))
            all = 1;
        else if (w[0] == '/' && up(w[1]) == 'U') {
            if (take_user(w + 2))
                syntax();
        } else if (w[0] == '/' && up(w[1]) == 'D')
            fail("Dom\x84nenlisten gibt es unter AT-DOS nicht (kein Suchdienst).");
        else
            syntax();
    }
    if (!unc[0])
        fail("Eine Liste der Rechner im Netz gibt es unter AT-DOS nicht (kein\r\n"
             "Suchdienst) - NET VIEW \\\\Rechner angeben.");
    scopy(vt, unc, sizeof(vt));
    cat(vt, " --view", sizeof(vt));
    if (all)
        cat(vt, " /ALL", sizeof(vt));
    if (user[0]) {
        cat(vt, " -U ", sizeof(vt));
        cat(vt, user, sizeof(vt));
        if (dom[0]) {
            cat(vt, " -W ", sizeof(vt));
            cat(vt, dom, sizeof(vt));
        }
    } else if (!unc_numeric() && have_tickets())
        cat(vt, " -k", sizeof(vt));
    DosExit(run_sub("SMBCLI.EXE", vt));
}

void main(void)
{
    char *a, *r;
    DosGetArgs(args);
    ctry_init();
    a = skip(args);
    if (!*a)
        syntax();
    r = word(a, w, sizeof(w));
    r = skip(r);
    if (w[0] == '/' && w[1] == '?')
        help(0);
    if (!same(w, "USE") && !same(w, "HELP")) {     /* NET x /? = Hilfe   */
        char *q;
        for (q = r; *q; q = next_word(q))
            if (q[0] == '/' && (q[1] == '?' || same(q, "/HELP")))
                help(w);
    }
    if (same(w, "USE"))
        net_use(r);
    else if (same(w, "HELP")) {
        word(r, ub, sizeof(ub));
        help(ub);
    } else if (same(w, "HELPMSG"))
        helpmsg(r);
    else if (same(w, "CONFIG"))
        config(r);
    else if (same(w, "STATISTICS") || same(w, "STATS"))
        statistics(r);
    else if (same(w, "TIME"))
        ntime(r);
    else if (same(w, "VIEW"))
        view(r);
    else if (same(w, "USER") || same(w, "USERS") || same(w, "GROUP") || same(w, "ACCOUNTS"))
        DosExit(run_sub("NET1.EXE", a));
    else if (same(w, "COMPUTER") || same(w, "CONTINUE") || same(w, "FILE") || same(w, "PAUSE") ||
             same(w, "SESSION") || same(w, "SHARE") || same(w, "START") || same(w, "STOP"))
        fail("Dieser Befehl betrifft Serverdienste - die hat AT-DOS nicht.");
    else if (same(w, "LOCALGROUP"))
        fail("AT-DOS hat keine lokalen Gruppen (NET GROUP /DOMAIN zeigt die der Dom\x84ne).");
    else
        syntax();
}
