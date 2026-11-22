/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HOSTNAME.EXE - Rechnername (AT-DOS 6.3), wie /etc/hostname unter Linux
 *
 *   HOSTNAME              Namen zeigen
 *   HOSTNAME Name         Namen setzen und in der Datei ablegen
 *   HOSTNAME /LOAD [/Q]   Namen aus der Datei setzen (AUTOEXEC.CMD)
 *
 * Datei %ETC%\HOSTNAME, sonst X:\DOS\SYSTEM\HOSTNAME (X = Laufwerk von
 * COMSPEC), eine Zeile. Gesetzt wird der Name in TCPIP.SYS (NET_HOSTNAME:
 * DHCP meldet ihn als Option 12, gebunden gleich mit Erneuern) und als
 * COMPUTERNAME in der Umgebung des aufrufenden CMD (wie bei Windows).
 * Ohne TCPIP.SYS nur Datei und Umgebung. Vorgabe von TCPIP.SYS: /HOST:
 * oder "ATDOS". ERRORLEVEL: 0 ok, 1 Fehler, 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"

#define DosNet(fn, p, n)    fapi_call(FAPI_NET, (fn), (unsigned short)(p), (n))

static char args[FAPI_ARGLEN], w[64], path[FAPI_PATHLEN], ev[48];
static struct fapi_hostname hn;
static struct fapi_netif ni;

static void out(const char *s) { DosWrite(STDOUT, s, slen(s)); }

static void fail(const char *m, unsigned rc)
{
    out(m);
    out("\r\n");
    DosExit(rc);
}

static int fpath(void)
{
    unsigned n;
    if (FAPI_FAILED(DosGetEnv("ETC", path, sizeof(path) - 10)) || !path[0]) {
        if (FAPI_FAILED(DosGetEnv("COMSPEC", path, sizeof(path))) || path[1] != ':')
            return 1;
        path[2] = 0;
        scat(path, "\\DOS\\SYSTEM", sizeof(path));
    }
    n = slen(path);
    if (path[n - 1] != '\\')
        scat(path, "\\", sizeof(path));
    scat(path, "HOSTNAME", sizeof(path));
    return 0;
}

/* Name pruefen: A-Z, a-z, 0-9, '-' (nicht vorn), 1..31 Zeichen */
static int valid(const char *s)
{
    unsigned k;
    for (k = 0; s[k]; k++)
        if (!((s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= 'a' && s[k] <= 'z') ||
              (s[k] >= '0' && s[k] <= '9') || (s[k] == '-' && k)))
            return 0;
    return k > 0 && k <= 31;
}

static void apply(const char *s, unsigned quiet)
{
    unsigned r;
    scopy(hn.name, s, sizeof(hn.name));
    r = DosNet(NET_HOSTNAME, &hn, sizeof(hn));
    scopy(ev, "COMPUTERNAME=", sizeof(ev));
    scat(ev, s, sizeof(ev));
    DosSetEnvCaller(ev);
    if (quiet)
        return;
    out("Rechnername ");
    out(s);
    out(r == FAPI_ERR_NONET ? " (TCPIP.SYS nicht geladen: nur Datei und Umgebung).\r\n" : ".\r\n");
}

void main(void)
{
    char *a;
    unsigned load = 0, quiet = 0, h, n, i;

    w[0] = 0;
    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        word(a, w + 32, 32);
        if (w[32] == '/' && up(w[33]) == 'L')
            load = 1;
        else if (w[32] == '/' && up(w[33]) == 'Q')
            quiet = 1;
        else if (w[32] == '/' || w[32] == '-')
            fail("HOSTNAME [Name | /LOAD [/Q]]", 2);
        else
            scopy(w, w + 32, 32);
    }
    if (load) {
        if (fpath())
            fail("COMSPEC fehlt.", 1);
        h = DosOpen(path);
        if (FAPI_FAILED(h)) {
            if (!quiet)
                out("Keine Datei HOSTNAME - Name bleibt.\r\n");
            DosExit(0);
        }
        n = DosRead(h, w, 31);
        DosClose(h);
        if (FAPI_FAILED(n))
            n = 0;
        for (i = 0; i < n && w[i] != '\r' && w[i] != '\n' && w[i] != ' ' && w[i] != '\t'; i++)
            ;
        w[i] = 0;
        if (!valid(w))
            fail("HOSTNAME: Name in der Datei ung\x81ltig (A-Z, 0-9, -, bis 31 Zeichen).", 1);
        apply(w, quiet);
        DosExit(0);
    }
    if (w[0]) {
        if (!valid(w))
            fail("Name nur aus A-Z, 0-9 und - (nicht vorn), bis 31 Zeichen.", 2);
        if (fpath())
            fail("COMSPEC fehlt.", 1);
        h = DosCreate(path);
        if (FAPI_FAILED(h))
            fail("Kann die Datei HOSTNAME nicht schreiben.", 1);
        DosWrite(h, w, slen(w));
        DosWrite(h, "\r\n", 2);
        DosClose(h);
        apply(w, quiet);
        DosExit(0);
    }
    ni.ifc = 0;
    if (DosNet(NET_IFINFO, &ni, sizeof(ni)) == 0 && ni.host[0])
        out(ni.host);
    else if (!FAPI_FAILED(DosGetEnv("COMPUTERNAME", w, sizeof(w))) && w[0])
        out(w);
    else
        fail("Kein Rechnername (TCPIP.SYS nicht geladen).", 1);
    out("\r\n");
    DosExit(0);
}
