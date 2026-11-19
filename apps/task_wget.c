/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WGET.EXE - Dateien ueber HTTP holen (Bedienung angelehnt an GNU wget,
 * eigener Code):
 *   WGET [Schalter] URL
 *   -O Datei   Ziel (statt des letzten Teils der URL); -O - = STDOUT
 *   -P Verz    Zielverzeichnis
 *   -c         abgebrochene Datei fortsetzen (Range)
 *   -q         keine Meldungen      -S  Kopfzeilen des Servers zeigen
 *   -T s       Frist fuer Verbinden und je Empfang (Vorgabe 30 s)
 *   -4 / -6    nur IPv4 bzw. IPv6
 * HTTP/1.1 mit Connection: close, Content-Length oder chunked, bis zu
 * 5 Weiterleitungen (301/302/303/307/308). https:// kommt mit TLS (6.3).
 * Vorhandene Dateien werden nur mit -O oder -c ueberschrieben bzw.
 * fortgesetzt. ERRORLEVEL wie GNU wget: 0 ok, 1 Fehler, 2 Aufruf,
 * 3 Datei, 4 Netz, 8 Fehlerantwort des Servers.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"

static char args[FAPI_ARGLEN], url[FAPI_ARGLEN], host[128], path[FAPI_ARGLEN];
static char out[FAPI_PATHLEN], odir[FAPI_PATHLEN], tmp[FAPI_PATHLEN], ts[48];
static char buf[4096], hdr[4096], req[600], loc[FAPI_ARGLEN];
static struct fapi_socket so;
static struct fapi_sockio io;
static struct fapi_dirent de;
static unsigned char ad[16];
static unsigned quiet, show, cont, tmo = 30, port, tostd, given;

static void say(const char *m)
{
    if (!quiet)
        VioWrtTTY(m, slen(m));
}

static void die(const char *m, unsigned rc)
{
    VioWrtTTY("wget: ", 6);
    VioWrtTTY(m, slen(m));
    VioWrtTTY("\r\n", 2);
    DosExit(rc);
}

/* 32/32-Division ohne Laufzeitbibliothek (Bit fuer Bit) */
static unsigned long udiv(unsigned long a, unsigned long b)
{
    unsigned long q = 0, r = 0;
    int i;
    if (!b)
        return 0;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

static unsigned long mul10(unsigned long v)
{
    return (v << 3) + (v << 1);
}

static unsigned long mul100(unsigned long v)
{
    return (v << 6) + (v << 5) + (v << 2);
}

static char *ldec(unsigned long v, char *p)    /* -> Ende */
{
    char t[12];
    int n = 0;
    do {
        unsigned long q = udiv(v, 10);
        t[n++] = (char)('0' + (unsigned)(v - mul10(q)));
        v = q;
    } while (v);
    while (n)
        *p++ = t[--n];
    *p = 0;
    return p;
}

static int lcmp(const char *a, const char *b, unsigned n)  /* ohne Gross/klein */
{
    while (n--) {
        if (up(*a) != up(*b))
            return 1;
        a++;
        b++;
    }
    return 0;
}

/* URL zerlegen: http://host[:port]/pfad, [v6]:port; -> 0 ok */
static int parse_url(const char *u)
{
    unsigned k = 0, n;
    const char *p = u;
    if (!lcmp(p, "https://", 8))
        die("https:// folgt mit TLS in AT-DOS 6.3.", 1);
    if (!lcmp(p, "http://", 7))
        p += 7;
    else if (!lcmp(p, "ftp://", 6))
        die("ftp:// bitte mit FTP holen.", 1);
    else if (p[0] && p[1] == ':' && p[2] == '/')
        return 1;
    port = 80;
    if (*p == '[') {                    /* [IPv6]                         */
        p++;
        while (*p && *p != ']' && k < sizeof(host) - 1)
            host[k++] = *p++;
        if (*p == ']')
            p++;
    } else
        while (*p && *p != ':' && *p != '/' && *p != '?' && k < sizeof(host) - 1)
            host[k++] = *p++;
    host[k] = 0;
    if (!k)
        return 1;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9')
            port = port * 10 + (*p++ - '0');
        if (!port)
            return 1;
    }
    n = 0;
    if (*p != '/')
        path[n++] = '/';
    while (*p && n < sizeof(path) - 1)
        path[n++] = *p++;
    path[n] = 0;
    return 0;
}

/* Ziel aus dem letzten Pfadteil (ohne ?..., %20 -> Leerzeichen) */
static void name_from_url(void)
{
    const char *p = path, *b = path;
    unsigned n = 0;
    char c;
    while (*p && *p != '?' && *p != '#') {
        if (*p == '/')
            b = p + 1;
        p++;
    }
    while (b < p && n < sizeof(tmp) - 1) {
        c = *b++;
        if (c == '%' && b + 1 < p) {
            unsigned v = 0, i;
            for (i = 0; i < 2; i++) {
                char h = up(*b++);
                v = v * 16 + (h >= 'A' ? h - 'A' + 10 : h - '0');
            }
            c = (char)v;
        }
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || (unsigned char)c < ' ')
            c = '_';
        tmp[n++] = c;
    }
    tmp[n] = 0;
    if (!n)
        scopy(tmp, "index.htm", sizeof(tmp));
    if (odir[0])
        join(out, odir, tmp, sizeof(out));
    else
        scopy(out, tmp, sizeof(out));
}

/* Groesse einer vorhandenen Datei, 0xFFFFFFFF = fehlt */
static unsigned long file_size(const char *f)
{
    char dir[FAPI_PATHLEN];
    char *b;
    unsigned n;
    scopy(dir, f, sizeof(dir));
    b = base_name(dir);
    if (b == dir) {
        scopy(dir, ".", sizeof(dir));
        b = (char *)f;
    } else {
        b = (char *)f + (b - dir);
        if (b - f > 1 && (b[-1] == '\\' || b[-1] == '/') && b[-2] != ':')
            dir[b - f - 1] = 0;
        else
            dir[b - f] = 0;
    }
    for (n = 0; DosFindEntry(&de, n, dir) != FAPI_ERROR; n++)
        if (!(de.attr & 0x18) && same(de.name, b))
            return de.size;
    return 0xFFFFFFFFUL;
}

static unsigned sio(unsigned fn, unsigned h, void *b, unsigned len)
{
    io.h = h;
    io.buf = (unsigned)b;
    io.len = len;
    io.flags = 0;
    io.timeout = tmo * 1000;
    return DosNet(fn, &io, sizeof(io));
}

/* Kopfzeile suchen: Wert nach "Name:" in hdr (ohne Gross/klein) */
static char *hval(const char *name)
{
    char *p = hdr;
    unsigned n = slen(name);
    while (*p) {
        if (!lcmp(p, name, n) && p[n] == ':') {
            p += n + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return 0;
}

static unsigned long dval(const char *p)
{
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9')
        v = mul10(v) + (*p++ - '0');
    return v;
}

/* Empfang ueber einen Puffer (hdr-Rest + Netz) */
static unsigned bpos, blen, sock;
static int rd(void)                    /* -> Byte oder -1               */
{
    if (bpos >= blen) {
        unsigned r = sio(NET_RECV, sock, buf, sizeof(buf));
        if (r == 0 || r >= 0xFFF0)
            return -1;
        blen = r;
        bpos = 0;
    }
    return (unsigned char)buf[bpos++];
}

static unsigned long got, total, t0, tl;
static int fh;

static void progress(int last)
{
    char line[80], *p;
    unsigned long t = (unsigned)DosQTicks();
    if (quiet || tostd && !last)
        return;
    if (!last && (unsigned)(t - tl) < 25)
        return;
    tl = t;
    p = line;
    *p++ = '\r';
    p = ldec(got, p);
    scopy(p, " Byte", 8);
    p += 5;
    if (total != 0xFFFFFFFFUL && total) {
        *p++ = ' ';
        *p++ = '(';
        p = ldec(total < 0x28F5C28UL ? udiv(mul100(got), total) : udiv(got, udiv(total, 100)), p);
        *p++ = '%';
        *p++ = ')';
    }
    {
        unsigned long dt = (unsigned)(t - t0);    /* 10-ms-Schritte     */
        if (dt) {
            scopy(p, ", ", 3);
            p += 2;
            p = ldec(mul100(udiv(got, dt)) >> 10, p);  /* KB/s          */
            scopy(p, " KB/s   ", 9);
            p += 8;
        }
    }
    if (last) {
        *p++ = '\r';
        *p++ = '\n';
    }
    VioWrtTTY(line, (unsigned)(p - line));
}

static void put(const char *b, unsigned n)
{
    if (n && DosWrite(fh, b, n) != n)
        die("Schreiben fehlgeschlagen (Datentr\x84ger voll?).", 3);
    got += n;
    progress(0);
}

/* Rumpf: chunked oder bis Ende/Laenge */
static int body(int chunked, unsigned long len)
{
    int c;
    if (chunked) {
        for (;;) {
            unsigned long n = 0;
            int any = 0;
            while ((c = rd()) >= 0) {
                if (c >= '0' && c <= '9') n = (n << 4) | (c - '0');
                else if (up((char)c) >= 'A' && up((char)c) <= 'F') n = (n << 4) | (up((char)c) - 'A' + 10);
                else break;
                any = 1;
            }
            while (c >= 0 && c != '\n')    /* Erweiterungen, CR LF       */
                c = rd();
            if (c < 0 || !any)
                return 1;
            if (!n)
                return 0;               /* Schluss (Trailer egal)         */
            while (n) {
                unsigned k;
                if (bpos >= blen && rd() >= 0)
                    bpos--;
                if (bpos >= blen)
                    return 1;
                k = blen - bpos;
                if (k > n)
                    k = (unsigned)n;
                put(buf + bpos, k);
                bpos += k;
                n -= k;
            }
            if (rd() == '\r')
                rd();
        }
    }
    for (;;) {
        unsigned k;
        if (len != 0xFFFFFFFFUL && !len)
            return 0;
        if (bpos >= blen) {
            if (rd() < 0)
                return len != 0xFFFFFFFFUL;
            bpos--;
        }
        k = blen - bpos;
        if (len != 0xFFFFFFFFUL && k > len)
            k = (unsigned)len;
        put(buf + bpos, k);
        bpos += k;
        if (len != 0xFFFFFFFFUL)
            len -= k;
    }
}

/* Antwort bis zur Leerzeile in hdr; -> Statuscode */
static unsigned read_head(void)
{
    unsigned n = 0, code = 0;
    int c, nl = 0;
    char *p;
    while ((c = rd()) >= 0) {
        if (c == '\r')
            continue;
        if (n < sizeof(hdr) - 1)
            hdr[n++] = (char)c;
        if (c == '\n') {
            if (nl)
                break;
            nl = 1;
        } else
            nl = 0;
    }
    hdr[n] = 0;
    if (c < 0)
        return 0;
    if (show) {
        p = hdr;
        while (*p) {
            char *e = p;
            while (*e && *e != '\n')
                e++;
            VioWrtTTY("  ", 2);
            VioWrtTTY(p, (unsigned)(e - p));
            VioWrtTTY("\r\n", 2);
            p = *e ? e + 1 : e;
        }
    }
    if (lcmp(hdr, "HTTP/", 5))
        return 0;
    p = hdr + 5;
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9')
        code = code * 10 + (*p++ - '0');
    return code;
}

static void add(char **p, const char *s)
{
    while (*s && *p < req + sizeof(req) - 3)
        *(*p)++ = *s++;
}

void main(void)
{
    char *a, *p;
    unsigned h, r, code, k, hops = 0;
    unsigned long have = 0xFFFFFFFFUL, len;
    int chunked;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == 'O' && a[2] <= ' ') {
            a = next_word(a);
            word(a, out, sizeof(out));
            given = 1;
            tostd = out[0] == '-' && !out[1];
        } else if (*a == '-' && a[1] == 'P' && a[2] <= ' ') {
            a = next_word(a);
            word(a, odir, sizeof(odir));
        } else if (*a == '-' && a[1] == 'T' && a[2] <= ' ') {
            a = next_word(a);
            tmo = 0;
            while (*a >= '0' && *a <= '9')
                tmo = tmo * 10 + (*a++ - '0');
            if (!tmo)
                tmo = 30;
        } else if (*a == '-' && a[1] == 'c' && a[2] <= ' ') cont = 1;
        else if (*a == '-' && a[1] == 'q' && a[2] <= ' ') quiet = 1;
        else if (*a == '-' && a[1] == 'S' && a[2] <= ' ') show = 1;
        else if (*a == '-' && a[1] == '4' && a[2] <= ' ') net_fam = AF_INET;
        else if (*a == '-' && a[1] == '6' && a[2] <= ' ') net_fam = AF_INET6;
        else if (*a == '-' || *a == '/' && a[1] == '?') {
            VioWrtTTY("WGET [-O Datei|-] [-P Verz] [-c] [-q] [-S] [-T s] [-4|-6] http://Rechner[:Port]/Pfad\r\n", 85);
            DosExit(2);
        } else
            word(a, url, sizeof(url));
    }
    if (!url[0])
        die("URL fehlt (WGET /? zeigt die Schalter).", 2);
    if (parse_url(url))
        die("URL nicht verstanden.", 2);
    if (!given)
        name_from_url();
    if (cont && !tostd)
        have = file_size(out);
    else if (!tostd && !given && file_size(out) != 0xFFFFFFFFUL)
        die("Datei gibt es schon (-O oder -c nehmen).", 3);

again:
    if (net_lookup(host, ad))
        die("Rechner nicht gefunden.", 4);
    net_ntop(ad, ts);
    say("Verbinde mit ");
    say(host);
    say(" (");
    say(ts);
    say(") ... ");
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (h >= 0xFFF0)
        die(h == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.", 4);
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.h = h;
    io.timeout = tmo * 1000;
    r = DosNet(NET_CONNECT, &io, sizeof(io));
    if (r) {
        say("\r\n");
        die(r == FAPI_ERR_TIMEOUT ? "Keine Antwort (Zeit\x81" "berschreitung)." :
            r == FAPI_ERR_RESET ? "Verbindung abgelehnt." : "Verbinden fehlgeschlagen.", 4);
    }
    say("verbunden.\r\n");
    sock = h;
    bpos = blen = 0;

    p = req;
    add(&p, "GET ");
    add(&p, path);
    add(&p, " HTTP/1.1\r\nHost: ");
    for (k = 0; host[k]; k++)
        if (host[k] == ':')
            break;
    if (host[k]) add(&p, "[");
    add(&p, host);
    if (host[k]) add(&p, "]");
    if (port != 80) {
        char pn[8];
        add(&p, ":");
        ldec(port, pn);
        add(&p, pn);
    }
    add(&p, "\r\nUser-Agent: Wget/1.0 (AT-DOS)\r\nAccept: */*\r\nConnection: close\r\n");
    if (have != 0xFFFFFFFFUL && have) {
        char pn[12];
        add(&p, "Range: bytes=");
        ldec(have, pn);
        add(&p, pn);
        add(&p, "-\r\n");
    }
    add(&p, "\r\n");
    if (sio(NET_SEND, h, req, (unsigned)(p - req)) != (unsigned)(p - req))
        die("Senden fehlgeschlagen.", 4);
    say("HTTP-Anfrage gesendet, warte auf Antwort ... ");
    code = read_head();
    if (!code) {
        say("\r\n");
        die("Keine g\x81ltige HTTP-Antwort.", 4);
    }
    {
        char cn[8];
        ldec(code, cn);
        say(cn);
        p = hdr;
        while (*p && *p != ' ')
            p++;
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            p++;
        if (*p == ' ') {
            char *e = p;
            while (*e && *e != '\n')
                e++;
            if (!quiet)
                VioWrtTTY(p, (unsigned)(e - p));
        }
        say("\r\n");
    }
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
        p = hval("Location");
        DosClose(h);
        if (!p || ++hops > 5)
            die("Zu viele oder ung\x81ltige Weiterleitungen.", 8);
        for (k = 0; p[k] && p[k] != '\n' && p[k] != '\r' && k < sizeof(loc) - 1; k++)
            loc[k] = p[k];
        loc[k] = 0;
        say("Weitergeleitet nach ");
        say(loc);
        say("\r\n");
        if (loc[0] == '/') {            /* gleicher Rechner                */
            scopy(path, loc, sizeof(path));
        } else if (parse_url(loc))
            die("Ziel der Weiterleitung nicht verstanden.", 8);
        if (!given && !cont) {
            name_from_url();
            if (!tostd && file_size(out) != 0xFFFFFFFFUL)
                die("Datei gibt es schon (-O oder -c nehmen).", 3);
        }
        goto again;
    }
    if (code == 416 && have != 0xFFFFFFFFUL) {
        say("Datei ist schon vollst\x84ndig.\r\n");
        DosClose(h);
        DosExit(0);
    }
    if (code != 200 && code != 206) {
        DosClose(h);
        die("Fehlerantwort des Servers.", 8);
    }
    p = hval("Transfer-Encoding");
    chunked = p && !lcmp(p, "chunked", 7);
    p = hval("Content-Length");
    len = p && !chunked ? dval(p) : 0xFFFFFFFFUL;
    total = len;

    if (tostd)
        fh = STDOUT;
    else if (code == 206 && have != 0xFFFFFFFFUL) {
        unsigned long pos = have;
        fh = DosOpenRW(out);
        if (FAPI_FAILED(fh))
            die("Datei l\x84\xE1t sich nicht \x94" "ffnen.", 3);
        DosSeek(fh, &pos);
        got = have;
        if (total != 0xFFFFFFFFUL)
            total += have;
        say("Setze fort ab Byte ");
        ldec(have, ts);
        say(ts);
        say("\r\n");
    } else {
        fh = DosCreate(out);
        if (FAPI_FAILED(fh))
            die("Datei l\x84\xE1t sich nicht anlegen.", 3);
    }
    if (!tostd) {
        say("Speichere in: ");
        say(out);
        if (total != 0xFFFFFFFFUL) {
            say(" (");
            ldec(total, ts);
            say(ts);
            say(" Byte)");
        }
        say("\r\n");
    }
    t0 = tl = (unsigned)DosQTicks();
    r = body(chunked, len);
    DosClose(h);
    progress(1);
    if (!tostd)
        DosClose(fh);
    if (r)
        die("Verbindung vorzeitig beendet (mit -c fortsetzen).", 4);
    DosExit(0);
}
