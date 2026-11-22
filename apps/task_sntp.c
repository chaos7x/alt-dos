/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SNTP.EXE - Uhrzeit aus dem Netz (SNTP nach RFC 4330, eigener Code):
 *   SNTP [Server] [/N] [/Q] [/W:s] [-4|-6]
 *   Server  Vorgabe %NTPSERVER%, sonst pool.ntp.org
 *   /N      nur anzeigen, die Uhr nicht stellen
 *   /Q      still (fuer AUTOEXEC.CMD)
 *   /W:s    Frist je Versuch (Vorgabe 3 s, drei Versuche)
 * Die Echtzeituhr haelt wie bei DOS die Ortszeit. Die Zeitzone kommt aus
 * der Variablen TZ im POSIX-Format (wie Unix und DJGPP), z. B.
 *   SET TZ=CET-1CEST,M3.5.0,M10.5.0/3      (Vorgabe ohne TZ: Mitteleuropa)
 *   SET TZ=EST5EDT,M3.2.0,M11.1.0          SET TZ=UTC0
 * Gestellt wird auf die volle (gerade) Sekunde: die Uhr kennt nur 2-s-
 * Schritte im FAT-Format, SNTP wartet bis dahin.
 * ERRORLEVEL: 0 ok, 1 keine Antwort/kein Netz, 2 Aufruf, 3 Uhr nicht stellbar.
 */
#include "fapi.h"
#include "util.h"
#include "arcutil.h"
#include "tz.h"
#define NET_RESOLVE_FN
#include "net.h"
#define CTRY_NO_DPAT
#include "country.h"

unsigned long au_mul16(unsigned a, unsigned b);
#pragma aux au_mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];

#define NTP_UNIX 2208988800UL           /* 1900 -> 1970                   */

static char args[FAPI_ARGLEN], host[128], ts[64], line[100];
static unsigned char pkt[48], ad[16];
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned quiet, noset, wait = 3;

static void out(const char *s)
{
    if (!quiet)
        DosWrite(STDOUT, s, slen(s));
}

static void die(const char *m, unsigned rc)
{
    DosWrite(STDOUT, "sntp: ", 6);
    DosWrite(STDOUT, m, slen(m));
    DosWrite(STDOUT, "\r\n", 2);
    DosExit(rc);
}

static char *cat(char *d, const char *s)    /* -> Ende */
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static char *ldec(unsigned long v, char *p)
{
    char t[12];
    int n = 0;
    unsigned long r;
    do {
        v = au_div(v, 10, &r);
        t[n++] = (char)('0' + (unsigned)r);
    } while (v);
    while (n)
        *p++ = t[--n];
    *p = 0;
    return p;
}

/* ---- Zeitzone (POSIX-TZ) ------------------------------------------ */

/* ---- Netz ---------------------------------------------------------- */

static unsigned long be32(const unsigned char *b)
{
    return (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
}

void main(void)
{
    char *a, *p;
    unsigned h, r, k, t0, t1, rtt, ms, stratum, try, date, time;
    unsigned long sec, fr, utc, loc, cur;
    unsigned char dt[4];
    long off, diff;
    int dst;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if ((*a == '/' || *a == '-') && up(a[1]) == 'N' && a[2] <= ' ') noset = 1;
        else if ((*a == '/' || *a == '-') && up(a[1]) == 'Q' && a[2] <= ' ') quiet = 1;
        else if ((*a == '/' || *a == '-') && up(a[1]) == 'W' && a[2] == ':') {
            for (wait = 0, p = a + 3; *p >= '0' && *p <= '9'; p++)
                wait = wait * 10 + (*p - '0');
            if (!wait || wait > 60)
                wait = 3;
        } else if (*a == '-' && a[1] == '4' && a[2] <= ' ') net_fam = AF_INET;
        else if (*a == '-' && a[1] == '6' && a[2] <= ' ') net_fam = AF_INET6;
        else if (*a == '/' || *a == '-') {
            DosWrite(STDOUT, "SNTP [Server] [/N] [/Q] [/W:s] [-4|-6]   (Zeitzone: SET TZ=CET-1CEST,M3.5.0,M10.5.0/3)\r\n", 89);
            DosExit(2);
        } else
            word(a, host, sizeof(host));
    }
    if (!host[0] && DosGetEnv("NTPSERVER", host, sizeof(host)) == FAPI_ERROR)
        host[0] = 0;
    if (!host[0])
        scopy(host, "pool.ntp.org", sizeof(host));
    tz_init();
    ctry_init();

    if (net_lookup(host, ad))
        die("Server nicht gefunden (Name oder Netz).", 1);
    so.family = AF_INET6;
    so.type = SOCK_DGRAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(h))
        die(h == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.", 1);
    for (try = 0; try < 3; try++) {
        for (k = 0; k < 48; k++)
            pkt[k] = 0;
        pkt[0] = 0x23;                  /* LI 0, Version 4, Client         */
        io.h = h;
        io.buf = (unsigned)pkt;
        io.len = 48;
        io.flags = 0;
        io.addr.len = 28;
        io.addr.family = AF_INET6;
        io.addr.port = htons(123);
        for (k = 0; k < 16; k++)
            io.addr.a6[k] = ad[k];
        t0 = (unsigned)DosQTicks();
        if (DosNet(NET_SENDTO, &io, sizeof(io)) != 48)
            die("Senden fehlgeschlagen.", 1);
        for (;;) {
            io.h = h;
            io.buf = (unsigned)pkt;
            io.len = 48;
            io.timeout = wait * 1000;
            r = DosNet(NET_RECVFROM, &io, sizeof(io));
            if (FAPI_FAILED(r))
                break;
            if (r >= 48 && (pkt[0] & 7) == 4 && ntohs(io.addr.port) == 123)
                break;                  /* Antwort des Servers             */
        }
        t1 = (unsigned)DosQTicks();
        if (!FAPI_FAILED(r))
            break;
    }
    DosClose(h);
    if (try == 3)
        die("Keine Antwort vom Zeitserver.", 1);
    stratum = pkt[1];
    if (!stratum || (pkt[0] >> 6) == 3)
        die("Der Server hat keine g\x81ltige Zeit (nicht synchronisiert).", 1);
    sec = be32(pkt + 40);               /* Sendezeit des Servers           */
    fr = be32(pkt + 44);
    if (sec < NTP_UNIX)
        die("Unsinnige Zeit vom Server.", 1);
    utc = sec - NTP_UNIX;
    rtt = t1 - t0;                      /* 10-ms-Schritte                  */
    ms = (unsigned)(au_mul16((unsigned)(fr >> 16), 1000) >> 16) + rtt * 5;
    utc += ms / 1000;
    ms %= 1000;

    off = tz_off(utc, &dst);
    loc = utc + off;
    unix2fat(loc, &date, &time);

    DosGetDateTime(dt);
    cur = fat2unix(dt[0] | (unsigned)dt[1] << 8, dt[2] | (unsigned)dt[3] << 8);
    diff = (long)(loc - cur);

    p = cat(line, "Zeit von ");
    out(line);
    out(host);
    net_ntop(ad, ts);
    if (!same(host, ts)) {
        out(" (");
        out(ts);
        out(")");
    }
    p = cat(line, ", Stratum ");
    ldec(stratum, p);
    out(line);
    out(":\r\n  ");
    ctry_date(line, date);
    out(line);
    out(" ");
    ctry_time(line, time, 1);
    out(line);
    p = line;
    *p++ = ' ';
    p = cat(p, dst ? n_dst : n_std);
    if (off || !same(dst ? n_dst : n_std, "UTC")) {
        *p++ = ' ';
        *p++ = '(';
        p = tz_put_off(p, off);
        *p++ = ')';
    }
    *p = 0;
    out(line);
    p = cat(line, "\r\n  Abweichung der Uhr: ");
    *p++ = diff < 0 ? '-' : '+';
    p = ldec((unsigned long)(diff < 0 ? -diff : diff), p);
    p = cat(p, " s (Uhr hat 2-s-Schritte), Laufzeit ");
    p = ldec(au_mul16(rtt, 10), p);
    cat(p, " ms\r\n");
    out(line);
    if (noset)
        DosExit(0);

    /* auf die naechste gerade Sekunde warten, dann stellen */
    k = 1000 - ms;
    loc++;
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
        die("Die Uhr l\x84\xE1t sich nicht stellen.", 3);
    out("  Uhr gestellt.\r\n");
    DosExit(0);
}
