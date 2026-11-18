/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FIREWALL.EXE - Firewall von TCPIP.SYS zeigen und einstellen:
 *   FIREWALL                          Zustand, Zaehler, Regeln
 *   FIREWALL ON | OFF                 an/aus (ab Werk an)
 *   FIREWALL LOG ON | OFF             Abgewiesenes ins DMESG
 *   FIREWALL ALLOW Programm Protokoll [Port[-Port]] [FROM Netz[/n] | LOCAL | ANY]
 *                                     eingehend erlauben (Programm "*" =
 *                                     jedes; Protokoll TCP, UDP, ICMP, ANY)
 *   FIREWALL DELETE n | CLEAR         Regel n bzw. alle loeschen
 *   FIREWALL /LOAD [Datei] [/Q]       Regeln neu aus der Datei (Vorgabe
 *                                     X:\DOS\SYSTEM\FIREWALL.INI, X =
 *                                     Startlaufwerk); /Q still, auch ohne Netz
 * Die Datei hat dieselben Befehle je Zeile (ohne FIREWALL), ';' oder REM
 * leitet Kommentare ein. Ohne Regel ist eingehend alles zu ausser
 * Antworten auf eigene Anfragen; PING aus dem eigenen Netz geht immer.
 * ERRORLEVEL: 0 ok, 1 Fehler, 2 kein Netz.
 */
#include "fapi.h"
#include "util.h"
#include "net.h"

static char args[FAPI_ARGLEN], ts[64], file[FAPI_PATHLEN], ev[FAPI_PATHLEN];
static char fbuf[4096];
static struct fapi_fwctl fc;
static int quiet;
static unsigned lineno;

static void msg(const char *m)
{
    if (!quiet)
        puts(m);
}

static void err(const char *m)
{
    if (lineno) {
        puts("Zeile ");
        put_num(lineno, 0, 0);
        puts(": ");
    }
    puts(m);
    puts("\r\n");
}

static unsigned ctl(unsigned char op)
{
    unsigned r;
    fc.op = op;
    r = DosNet(NET_FWCTL, &fc, sizeof(fc));
    if (r == FAPI_ERR_NONET) {
        if (!quiet)
            puts("Kein Netz: TCPIP.SYS ist nicht geladen.\r\n");
        DosExit(quiet ? 0 : 2);
    }
    return r;
}

static int kw(const char *a, const char *k)     /* Wort a == k?        */
{
    while (*k)
        if (up(*a++) != *k++)
            return 0;
    return *a == 0 || *a == ' ' || *a == '\t';
}

static unsigned pnum(const char **pp, int *ok)
{
    const char *p = *pp;
    unsigned v = 0, d;
    *ok = *p >= '0' && *p <= '9';
    for (; *p >= '0' && *p <= '9'; p++) {
        d = *p - '0';
        if (v > 6553 || (v == 6553 && d > 5))
            *ok = 0;                    /* ueber 65535                     */
        v = v * 10 + d;
    }
    *pp = p;
    return v;
}

static char *wnext(char *a)                     /* naechstes Wort        */
{
    while (*a && *a != ' ' && *a != '\t')
        a++;
    while (*a == ' ' || *a == '\t')
        a++;
    return a;
}

/* ALLOW Programm Protokoll [Ports] [FROM ...] -> 0 ok */
static int allow(char *a)
{
    struct fapi_fwrule *r = &fc.r;
    const char *q;
    unsigned k, n;
    int ok;
    char nb[48], *s;

    for (k = 0; k < sizeof(*r); k++)
        ((char *)r)[k] = 0;
    for (k = 0; a[k] && a[k] != ' ' && a[k] != '\t' && k < 12; k++)
        r->prog[k] = up(a[k]);
    if (!k) {
        err("ALLOW Programm Protokoll [Port[-Port]] [FROM Netz/n | LOCAL | ANY]");
        return 1;
    }
    a = wnext(a);
    if (kw(a, "TCP")) r->proto = FWP_TCP;
    else if (kw(a, "UDP")) r->proto = FWP_UDP;
    else if (kw(a, "ICMP")) r->proto = FWP_ICMP;
    else if (kw(a, "ANY")) r->proto = FWP_ANY;
    else {
        err("Protokoll: TCP, UDP, ICMP oder ANY");
        return 1;
    }
    a = wnext(a);
    r->lo = 1;
    r->hi = 65535;
    if (*a >= '0' && *a <= '9') {
        q = a;
        r->lo = r->hi = pnum(&q, &ok);
        if (ok && *q == '-') {
            q++;
            r->hi = pnum(&q, &ok);
        }
        if (!ok || !r->lo || r->hi < r->lo || (*q && *q != ' ' && *q != '\t')) {
            err("Ung\x81ltiger Port (1-65535 oder von-bis)");
            return 1;
        }
        a = wnext(a);
    } else if (r->proto == FWP_TCP || r->proto == FWP_UDP) {
        err("Port fehlt");
        return 1;
    }
    if (kw(a, "FROM") || kw(a, "VON")) {
        a = wnext(a);
        if (kw(a, "LOCAL") || kw(a, "LOKAL"))
            r->flags = FWR_LOCAL;
        else if (!kw(a, "ANY") && !kw(a, "ALLE")) {
            for (n = 0; a[n] && a[n] != ' ' && a[n] != '/' && n < sizeof(nb) - 1; n++)
                nb[n] = a[n];
            nb[n] = 0;
            if (net_pton(nb, r->net)) {
                err("Ung\x81ltiges Netz (a.b.c.d/n, LOCAL oder ANY)");
                return 1;
            }
            s = a + n;
            k = net_is4(r->net) ? 32 : 128;
            if (*s == '/') {
                q = s + 1;
                k = pnum(&q, &ok);
                if (!ok || k > (net_is4(r->net) ? 32 : 128)) {
                    err("Ung\x81ltige Pr\x84" "fixl\x84nge");
                    return 1;
                }
            }
            r->plen = (unsigned char)(net_is4(r->net) ? 96 + k : k);
        }
        a = wnext(a);
    }
    if (*a) {
        err("Unerwartet am Ende der Regel");
        return 1;
    }
    if (ctl(FW_ADD) >= 0xFFF0) {
        err("Kein Platz f\x81r weitere Regeln");
        return 1;
    }
    return 0;
}

/* ein Befehl (Befehlszeile oder Zeile der Datei) -> 0 ok */
static int command(char *a)
{
    a = skip(a);
    if (kw(a, "ON") || kw(a, "OFF")) {
        fc.on = kw(a, "ON");
        ctl(FW_SET);
        return 0;
    }
    if (kw(a, "LOG")) {
        a = wnext(a);
        if (!kw(a, "ON") && !kw(a, "OFF")) {
            err("LOG ON oder LOG OFF");
            return 1;
        }
        fc.on = kw(a, "ON");
        ctl(FW_LOG);
        return 0;
    }
    if (kw(a, "ALLOW") || kw(a, "ERLAUBE"))
        return allow(wnext(a));
    if (kw(a, "CLEAR")) {
        ctl(FW_CLEAR);
        return 0;
    }
    if (kw(a, "DELETE") || kw(a, "DEL")) {
        const char *q = wnext(a);
        int ok;
        unsigned n = pnum(&q, &ok);
        fc.idx = n - 1;
        if (!ok || !n || ctl(FW_DEL)) {
            err("Keine solche Regel");
            return 1;
        }
        return 0;
    }
    err("Unbekannter Befehl (ON, OFF, LOG, ALLOW, DELETE, CLEAR, /LOAD)");
    return 1;
}

static int load(char *a)
{
    unsigned h, n, i, k, bad = 0;
    char line[128];

    a = skip(a);
    if (*a && *a != '/') {
        word(a, file, sizeof(file));
    } else {
        char d = 'C';
        if (!FAPI_FAILED(DosGetEnv("COMSPEC", ev, sizeof(ev))) && ev[1] == ':')
            d = up(ev[0]);
        file[0] = d;
        file[1] = 0;
        scat(file, ":\\DOS\\SYSTEM\\FIREWALL.INI", sizeof(file));
    }
    ctl(FW_GET);                        /* Netz da? (sonst Ende mit /Q)  */
    h = DosOpen(file);
    if (FAPI_FAILED(h)) {
        if (!quiet) {
            puts(file);
            puts(" nicht gefunden.\r\n");
        }
        return quiet ? 0 : 1;
    }
    n = DosRead(h, fbuf, sizeof(fbuf) - 1);
    DosClose(h);
    if (FAPI_FAILED(n))
        n = 0;
    fbuf[n] = 0;
    ctl(FW_CLEAR);
    for (i = 0; fbuf[i];) {
        lineno++;
        for (k = 0; fbuf[i] && fbuf[i] != '\r' && fbuf[i] != '\n'; i++)
            if (k < sizeof(line) - 1)
                line[k++] = fbuf[i] == '\t' ? ' ' : fbuf[i];
        line[k] = 0;
        while (fbuf[i] == '\r' || fbuf[i] == '\n')
            i++;
        a = skip(line);
        if (!*a || *a == ';' || *a == '#' || kw(a, "REM"))
            continue;
        if (kw(a, "FIREWALL"))
            a = wnext(a);
        bad |= command(a);
    }
    lineno = 0;
    return bad;
}

static char *dec(char *p, unsigned v)    /* Zahl anhaengen, -> Ende       */
{
    char t[6];
    int m = 0;
    do {
        t[m++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (m)
        *p++ = t[--m];
    *p = 0;
    return p;
}

static void show(void)
{
    unsigned i;
    struct fapi_fwrule *r = &fc.r;
    fc.idx = 0xFFFF;
    ctl(FW_GET);
    puts("Firewall: ");
    puts(fc.on ? "an" : "AUS");
    puts(fc.log ? " (Abgewiesenes ins DMESG)\r\n" : "\r\n");
    puts("Abgewiesen: ");
    put_num(fc.drop, 0, 1);
    puts(", per Regel erlaubt: ");
    put_num(fc.pass, 0, 1);
    if (!fc.on) {
        puts("\r\n\r\nEingehend ist alles offen (FIREWALL ON schaltet sie wieder ein).\r\n");
        return;
    }
    puts("\r\n\r\nEingehend erlaubt: Antworten auf eigene Anfragen, PING aus dem eigenen\r\nNetz");
    if (!fc.nrules) {
        puts("\r\nKeine Regeln (FIREWALL ALLOW ...).\r\n");
        return;
    }
    puts(" und\r\n\r\n  Nr  Programm      Proto  Ports        Herkunft             Treffer\r\n");
    for (i = 0; i < fc.nrules; i++) {
        fc.idx = i;
        if (ctl(FW_GET))
            break;
        puts("  ");
        put_num(i + 1, 2, 0);
        puts("  ");
        puts(r->prog);
        for (fc.op = (unsigned char)slen(r->prog); fc.op < 14; fc.op++)
            putc(' ');
        puts(r->proto == FWP_TCP ? "TCP    " : r->proto == FWP_UDP ? "UDP    " :
             r->proto == FWP_ICMP ? "ICMP   " : "ANY    ");
        ts[0] = 0;
        if (r->proto != FWP_ICMP) {
            if (r->lo == 1 && r->hi == 65535)
                scopy(ts, "alle", sizeof(ts));
            else {
                char *p = dec(ts, r->lo);
                if (r->hi != r->lo) {
                    *p++ = '-';
                    dec(p, r->hi);
                }
            }
        }
        puts(ts);
        for (fc.op = (unsigned char)slen(ts); fc.op < 13; fc.op++)
            putc(' ');
        if (r->flags & FWR_LOCAL)
            scopy(ts, "eigenes Netz", sizeof(ts));
        else if (!r->plen || r->plen == 96)
            scopy(ts, "alle", sizeof(ts));
        else {
            net_ntop(r->net, ts);
            scat(ts, "/", sizeof(ts));
            dec(ts + slen(ts), net_is4(r->net) ? r->plen - 96 : r->plen);
        }
        puts(ts);
        for (fc.op = (unsigned char)slen(ts); fc.op < 21; fc.op++)
            putc(' ');
        put_num(r->hits, 0, 1);
        puts("\r\n");
    }
}

void main(void)
{
    char *a;
    DosGetArgs(args);
    a = skip(args);
    if (*a == '/' && a[1] == '?') {
        puts("Zeigt und stellt die Firewall von TCPIP.SYS ein.\r\n\r\n"
             "FIREWALL [ON | OFF | LOG ON|OFF | CLEAR | DELETE n]\r\n"
             "FIREWALL ALLOW Programm Protokoll [Port[-Port]] [FROM Netz/n | LOCAL | ANY]\r\n"
             "FIREWALL /LOAD [Datei] [/Q]\r\n\r\n"
             "  Programm  z. B. FTPD.EXE, * = jedes\r\n"
             "  Protokoll TCP, UDP, ICMP oder ANY\r\n"
             "  /LOAD     Regeln neu aus der Datei (Vorgabe \\DOS\\SYSTEM\\FIREWALL.INI)\r\n"
             "  /Q        still, auch ohne Netz\r\n\r\n"
             "Ohne Regel ist eingehend alles zu ausser Antworten auf eigene Anfragen.\r\n");
        return;
    }
    if (*a == '/' && up(a[1]) == 'L') {
        char *q;
        for (q = a; *q; q = next_word(q))
            if (*q == '/' && up(q[1]) == 'Q')
                quiet = 1;
        if (load(wnext(a)))
            DosExit(1);
        msg("Firewall-Regeln geladen.\r\n");
        return;
    }
    if (!*a) {
        show();
        return;
    }
    if (command(a))
        DosExit(1);
    show();
}
