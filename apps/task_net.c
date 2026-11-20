/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NET.EXE - Netzlaufwerke (AT-DOS 6.3), Bedienung wie NET USE von
 * MS-DOS/LAN Manager/Windows:
 *   NET USE                         verbundene Laufwerke zeigen
 *   NET USE X: \\Rechner\Freigabe [Kennwort|*] [/USER:[Domaene\]Name]
 *   NET USE X: /DELETE              trennen (auch /D)
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
static struct fapi_ufsreg rg;

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

static void usage(void)
{
    out("NET USE [X: [\\\\Rechner\\Freigabe [Kennwort|*] [/USER:[Dom\x84ne\\]Name]] | X: /DELETE]\r\n");
    DosExit(2);
}

static void fail(const char *m)
{
    out(m);
    out("\r\n");
    DosExit(1);
}

static void list(void)
{
    unsigned d, n = 0;
    char l[4];
    for (d = 2; d < 26; d++) {
        if (fapi_call(FAPI_UFS, UFS_INFO, d, (unsigned)&rg))
            continue;
        if (!n++)
            out("Lokal    Remote\r\n"
                "------------------------------------------------\r\n");
        l[0] = (char)('A' + d);
        l[1] = ':';
        l[2] = 0;
        out(l);
        out("       ");
        out(rg.name);
        out("\r\n");
    }
    if (!n)
        out("Keine Netzlaufwerke verbunden.\r\n");
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

void main(void)
{
    char *a, *p;
    unsigned drive = 0xFFFF, del = 0, star = 0, havepw = 0, kerb, h, n, t;

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (word(a, w, sizeof(w)), !same(w, "USE")))
        usage();
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
        else if (w[0] == '/' && up(w[1]) == 'U' && (p = w + 2, 1)) {
            while (*p && *p != ':')
                p++;
            if (!*p)
                usage();
            p++;
            while (*p == '\\')                 /* \\DOM\Name wie DOM\Name     */
                p++;
            if (!*p)
                usage();
            for (n = 0; p[n] && p[n] != '\\'; n++)
                ;
            if (p[n]) {
                p[n] = 0;
                scopy(dom, p, sizeof(dom));
                p += n + 1;
            }
            scopy(user, p, sizeof(user));
        } else if (w[0] == '*' && !w[1])
            star = 1;
        else if (w[0] != '/' && unc[0] && !havepw) {
            scopy(pass, w, sizeof(pass));
            havepw = 1;
        } else
            usage();
    }
    if (drive == 0xFFFF) {
        if (unc[0] || del)
            usage();
        list();
        DosExit(0);
    }
    if (del) {
        n = fapi_call(FAPI_UFS, UFS_DEL, drive, 0);
        if (n == FAPI_ERR_INUSE)
            fail("Laufwerk wird noch benutzt (offene Dateien oder aktuelles Verzeichnis).");
        if (n)
            fail("Das Laufwerk ist kein Netzlaufwerk.");
        for (t = 0; t < 50 && !fapi_call(FAPI_UFS, UFS_INFO, drive, (unsigned)&rg); t++)
            DosSleep(100);
        out("Netzlaufwerk getrennt.\r\n");
        DosExit(0);
    }
    if (!unc[0])
        usage();
    if (!fapi_call(FAPI_UFS, UFS_INFO, drive, (unsigned)&rg))
        fail("Das Laufwerk ist schon verbunden.");
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
        fail("Kann die Datei f\x81r das Kennwort nicht anlegen (%TEMP%).");
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
        fail("SMBCLI.EXE nicht gefunden (\\DOS).");
    }

    /* warten: Laufwerk da (Datei leer) oder Meldung "!..." */
    for (t = 0; t < 600; t++) {
        DosSleep(100);
        if (!fapi_call(FAPI_UFS, UFS_INFO, drive, (unsigned)&rg)) {
            DosDelete(pwf);
            out("Laufwerk ");
            msg[0] = (char)('A' + drive);
            msg[1] = ':';
            msg[2] = 0;
            out(msg);
            out(" ist mit ");
            out(rg.name);
            out(" verbunden.\r\n");
            DosExit(0);
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
        fail(msg + 1);
    }
    DosDelete(pwf);
    fail("Keine Antwort vom Server (Frist).");
}
