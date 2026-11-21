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
 * ERRORLEVEL: 0 ok, 1 Fehler, 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"

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

void main(void)
{
    char *a;
    const char *e;
    unsigned drive = 0xFFFF, del = 0, rest = 0, pset = 0, t;

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (word(a, w, sizeof(w)), !same(w, "USE")))
        usage();
    lst_load();
    for (a = next_word(a); *a; a = next_word(a)) {
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
