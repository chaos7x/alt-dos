/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CURL.EXE - HTTP(S)-Anfragen von der Befehlszeile, Bedienung wie curl
 * (eigener Code, HTTP- und TLS-Teil wie WGET):
 *
 *   CURL [Schalter] URL
 *   -o Datei | -O   Antwort in eine Datei (sonst Standardausgabe)
 *   -X Methode      GET, POST, PUT, DELETE ... (Vorgabe GET, mit -d POST)
 *   -H "Name: Wert" eigene Kopfzeile (mehrfach)
 *   -d Daten        Rumpf (@Datei liest ihn aus einer Datei; mehrfach
 *                   mit & verbunden), Content-Type wie curl
 *   -I              nur die Kopfzeilen (HEAD)
 *   -i              Kopfzeilen der Antwort mit ausgeben
 *   -L              Weiterleitungen folgen (bis 10)
 *   -u Name:Kennwort  Basic-Anmeldung
 *   -A Kennung, -e Herkunft   User-Agent bzw. Referer
 *   -f              bei Fehlerantwort (>= 400) nichts ausgeben, ERRORLEVEL 22
 *   -s, -S          still (ohne Fortschritt), mit -S Fehler trotzdem zeigen
 *   -v              Anfrage und Antwortkopf zeigen (> und <)
 *   -w Format       danach ausgeben: %{http_code}, %{size_download},
 *                   %{url_effective}, \n
 *   -k              Server-Zertifikat nicht pruefen
 *   -m s            Frist (Vorgabe 30 s), -4 / -6
 * ERRORLEVEL wie curl: 0 ok, 3 URL, 6 Rechner unbekannt, 7 keine
 * Verbindung, 22 HTTP-Fehler (-f), 23 Schreibfehler, 28 Zeitueber-
 * schreitung, 35 TLS, 47 zu viele Weiterleitungen, 26 Datei fuer -d fehlt.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#include "tls.h"

static char args[FAPI_ARGLEN], url[FAPI_ARGLEN], host[128], path[FAPI_ARGLEN];
static char out[FAPI_PATHLEN], ts[48], method[16], ua[64], referer[128], auth[96];
static char buf[4096], hdr[4096], req[1400], loc[FAPI_ARGLEN], wfmt[64];
static char hdrs[8][96];
static unsigned nhdr;
static char data[2048];
static unsigned dlen, have_data, data_bin;
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned char ad[16];
static unsigned tmo = 30, port, https, nocheck, tlson, silent, showerr, verbose;
static unsigned incl, head_only, follow, failf, oname, fh, code;
static unsigned long got;

static void msg(const char *m)
{
    VioWrtTTY(m, slen(m));
}

static void die(const char *m, unsigned rc)
{
    if (!silent || showerr) {
        msg("curl: ");
        msg(m);
        msg("\r\n");
    }
    DosExit(rc);
}

static int lcmp(const char *a, const char *b, unsigned n)
{
    while (n--) {
        if (up(*a) != up(*b))
            return 1;
        a++, b++;
    }
    return 0;
}

static unsigned long udiv(unsigned long a, unsigned long b)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

static char *ldec(unsigned long v, char *p)
{
    char t[12];
    int n = 0;
    do {
        unsigned long q = udiv(v, 10);
        t[n++] = (char)('0' + (unsigned)(v - ((q << 3) + (q << 1))));
        v = q;
    } while (v);
    while (n)
        *p++ = t[--n];
    *p = 0;
    return p;
}

static int strchr_(const char *s, char c);

static int parse_url(const char *u)
{
    unsigned k = 0, n;
    const char *p = u;
    https = 0;
    if (!lcmp(p, "https://", 8)) {
        p += 8;
        https = 1;
    } else if (!lcmp(p, "http://", 7))
        p += 7;
    else if (strchr_(p, ':'))           /* anderes Schema               */
        return 1;
    port = https ? 443 : 80;
    if (*p == '[') {
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

static int strchr_(const char *s, char c)
{
    for (; *s && *s != '/'; s++)
        if (*s == c && s[1] == '/' && s[2] == '/')
            return 1;
    return 0;
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

static unsigned bpos, blen, sock;
static int rd(void)
{
    if (bpos >= blen) {
        unsigned r;
        if (tlson) {
            int k = tls_read(buf, sizeof(buf));
            if (k <= 0)
                return -1;
            r = (unsigned)k;
        } else
            r = sio(NET_RECV, sock, buf, sizeof(buf));
        if (r == 0 || r >= 0xFFF0)
            return -1;
        blen = r;
        bpos = 0;
    }
    return (unsigned char)buf[bpos++];
}

static void put(const char *b, unsigned n)
{
    if (n && DosWrite(fh, b, n) != n)
        die("Schreiben fehlgeschlagen", 23);
    got += n;
}

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
            while (c >= 0 && c != '\n')
                c = rd();
            if (c < 0 || !any)
                return 1;
            if (!n)
                return 0;
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

static unsigned read_head(void)
{
    unsigned n = 0, c2 = 0;
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
    if (c < 0 && !n)
        return 0;
    if (lcmp(hdr, "HTTP/", 5))
        return 0;
    p = hdr + 5;
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9')
        c2 = c2 * 10 + (*p++ - '0');
    return c2;
}

/* Kopfzeilen (hdr) mit Vorsatz ausgeben: nach fh (-i) bzw. "< " (-v) */
static void show_head(int to_out)
{
    char *p = hdr, *e;
    while (*p) {
        e = p;
        while (*e && *e != '\n')
            e++;
        if (to_out) {
            put(p, (unsigned)(e - p));
            put("\r\n", 2);
        } else {
            msg("< ");
            VioWrtTTY(p, (unsigned)(e - p));
            msg("\r\n");
        }
        p = *e ? e + 1 : e;
    }
}

static void add(char **p, const char *s)
{
    while (*s && *p < req + sizeof(req) - 3)
        *(*p)++ = *s++;
}

static void b64(const char *s, char *o)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned n = slen(s), i, v;
    for (i = 0; i + 2 < n; i += 3) {
        v = ((unsigned char)s[i] << 8 | (unsigned char)s[i + 1]);
        *o++ = t[(unsigned char)s[i] >> 2];
        *o++ = t[((unsigned char)s[i] & 3) << 4 | (unsigned char)s[i + 1] >> 4];
        *o++ = t[((unsigned char)s[i + 1] & 15) << 2 | (unsigned char)s[i + 2] >> 6];
        *o++ = t[(unsigned char)s[i + 2] & 63];
        (void)v;
    }
    if (n - i == 1) {
        *o++ = t[(unsigned char)s[i] >> 2];
        *o++ = t[((unsigned char)s[i] & 3) << 4];
        *o++ = '=';
        *o++ = '=';
    } else if (n - i == 2) {
        *o++ = t[(unsigned char)s[i] >> 2];
        *o++ = t[((unsigned char)s[i] & 3) << 4 | (unsigned char)s[i + 1] >> 4];
        *o++ = t[((unsigned char)s[i + 1] & 15) << 2];
        *o++ = '=';
    }
    *o = 0;
}

/* -d: Text oder @Datei an data anhaengen (mehrere mit &) */
static void add_data(const char *w)
{
    if (have_data && dlen < sizeof(data))
        data[dlen++] = '&';
    have_data = 1;
    if (w[0] == '@') {
        unsigned h = w[1] == '-' && !w[2] ? STDIN : DosOpen(w + 1), r;
        if (FAPI_FAILED(h))
            die("Datei f\x81r -d nicht gefunden", 26);
        while (dlen < sizeof(data) &&
               (r = DosRead(h, data + dlen, sizeof(data) - dlen)) != 0 && !FAPI_FAILED(r))
            dlen += r;
        if (h != STDIN)
            DosClose(h);
        if (!data_bin) {                /* wie curl: Zeilenenden weg    */
            unsigned i, o = 0;
            for (i = 0; i < dlen; i++)
                if (data[i] != '\r' && data[i] != '\n')
                    data[o++] = data[i];
            dlen = o;
        }
        return;
    }
    while (*w && dlen < sizeof(data))
        data[dlen++] = *w++;
}

/* Wert eines Schalters: Rest des Worts oder das naechste Wort */
static char *val(char **a, char *w, unsigned max)
{
    *a = next_word(*a);
    if (**a == '"') {                   /* "mit Leerzeichen"            */
        unsigned n = 0;
        (*a)++;
        while (**a && **a != '"' && n < max - 1)
            w[n++] = *(*a)++;
        w[n] = 0;
        if (**a == '"')
            (*a)++;
        return w;
    }
    word(*a, w, max);
    return w;
}

static void wput(const char *s, unsigned n)     /* -w: Standardausgabe */
{
    DosWrite(STDOUT, s, n);
}

static void wout(void)
{
    const char *p = wfmt;
    char b[16];
    for (; *p; p++) {
        if (*p == '\\' && p[1] == 'n') {
            wput("\r\n", 2);
            p++;
        } else if (*p == '%' && p[1] == '{') {
            if (!lcmp(p, "%{http_code}", 12)) {
                ldec(code, b);
                wput(b, slen(b));
                p += 11;
            } else if (!lcmp(p, "%{size_download}", 16)) {
                ldec(got, b);
                wput(b, slen(b));
                p += 15;
            } else if (!lcmp(p, "%{url_effective}", 16)) {
                wput(url, slen(url));
                p += 15;
            } else
                wput(p, 1);
        } else
            wput(p, 1);
    }
}

void main(void)
{
    char *a, *p, w[128];
    unsigned h, r, k, hops = 0, i;
    unsigned long len;
    int chunked;

    scopy(ua, "curl/8.0 (AT-DOS)", sizeof(ua));
    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a != '-' || a[1] <= ' ') {
            word(a, url, sizeof(url));
            continue;
        }
        if (a[1] == '-') {                      /* lange Formen          */
            word(a, w, sizeof(w));
            if (same(w, "--insecure")) nocheck = 1;
            else if (same(w, "--silent")) silent = 1;
            else if (same(w, "--location")) follow = 1;
            else if (same(w, "--include")) incl = 1;
            else if (same(w, "--head")) head_only = 1;
            else if (same(w, "--fail")) failf = 1;
            else if (same(w, "--verbose")) verbose = 1;
            else if (same(w, "--remote-name")) oname = 1;
            else if (same(w, "--data-binary")) { data_bin = 1; add_data(val(&a, w, sizeof(w))); }
            else if (same(w, "--data")) add_data(val(&a, w, sizeof(w)));
            else if (same(w, "--output")) val(&a, out, sizeof(out));
            else if (same(w, "--request")) val(&a, method, sizeof(method));
            else if (same(w, "--header") && nhdr < 8) val(&a, hdrs[nhdr++], sizeof(hdrs[0]));
            else if (same(w, "--user")) val(&a, w, sizeof(w)), b64(w, auth);
            else if (same(w, "--user-agent")) val(&a, ua, sizeof(ua));
            else if (same(w, "--help")) a[0] = '-', a[1] = '?';
            else {
                msg("curl: unbekannter Schalter ");
                msg(w);
                msg("\r\n");
                DosExit(2);
            }
            continue;
        }
        for (p = a + 1; *p > ' '; p++) {
            switch (*p) {
            case 'k': nocheck = 1; break;
            case 's': silent = 1; break;
            case 'S': showerr = 1; break;
            case 'L': follow = 1; break;
            case 'i': incl = 1; break;
            case 'I': head_only = 1; break;
            case 'f': failf = 1; break;
            case 'v': verbose = 1; break;
            case 'O': oname = 1; break;
            case '4': net_fam = AF_INET; break;
            case '6': net_fam = AF_INET6; break;
            case 'o': val(&a, out, sizeof(out)); goto next;
            case 'X': val(&a, method, sizeof(method)); goto next;
            case 'H':
                if (nhdr < 8) val(&a, hdrs[nhdr++], sizeof(hdrs[0]));
                else val(&a, w, sizeof(w));
                goto next;
            case 'd': add_data(val(&a, w, sizeof(w))); goto next;
            case 'u': val(&a, w, sizeof(w)); b64(w, auth); goto next;
            case 'A': val(&a, ua, sizeof(ua)); goto next;
            case 'e': val(&a, referer, sizeof(referer)); goto next;
            case 'w': val(&a, wfmt, sizeof(wfmt)); goto next;
            case 'm':
                val(&a, w, sizeof(w));
                tmo = 0;
                for (i = 0; w[i] >= '0' && w[i] <= '9'; i++)
                    tmo = tmo * 10 + (w[i] - '0');
                if (!tmo)
                    tmo = 30;
                goto next;
            case '?':
            case 'h':
                msg("CURL [-o Datei|-O] [-X Methode] [-H Kopf] [-d Daten|@Datei] [-I] [-i] [-L]\r\n"
                    "     [-u Name:Kennwort] [-A Kennung] [-e Herkunft] [-f] [-s] [-S] [-v]\r\n"
                    "     [-w Format] [-k] [-m s] [-4|-6] http[s]://Rechner[:Port]/Pfad\r\n");
                DosExit(0);
            default:
                msg("curl: unbekannter Schalter -");
                VioWrtTTY(p, 1);
                msg("\r\n");
                DosExit(2);
            }
        }
next:   ;
    }
    if (!url[0])
        die("URL fehlt (CURL -h zeigt die Schalter)", 2);
    if (parse_url(url))
        die("URL nicht verstanden (nur http:// und https://)", 3);
    if (!method[0])
        scopy(method, head_only ? "HEAD" : have_data ? "POST" : "GET", sizeof(method));
    if (oname) {                                /* -O: letzter Pfadteil  */
        const char *b = path, *q;
        unsigned n = 0;
        for (q = path; *q && *q != '?'; q++)
            if (*q == '/')
                b = q + 1;
        while (b < q && n < sizeof(out) - 1)
            out[n++] = *b++;
        out[n] = 0;
        if (!n)
            die("kein Dateiname in der URL f\x81r -O", 23);
    }

again:
    if (net_lookup(host, ad))
        die("Rechner nicht gefunden", 6);
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (h >= 0xFFF0)
        die(h == FAPI_ERR_NONET ? "kein Netz (TCPIP.SYS fehlt)" : "kein Socket frei", 7);
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.h = h;
    io.timeout = tmo * 1000;
    if (verbose) {
        net_ntop(ad, ts);
        msg("*   Trying ");
        msg(ts);
        msg(" ...\r\n");
    }
    r = DosNet(NET_CONNECT, &io, sizeof(io));
    if (r)
        die(r == FAPI_ERR_TIMEOUT ? "Zeit\x81" "berschreitung beim Verbinden" :
            "Verbindung abgelehnt", r == FAPI_ERR_TIMEOUT ? 28 : 7);
    sock = h;
    bpos = blen = 0;
    tlson = 0;
    if (https) {
        int e = tls_open(h, host, tmo, nocheck ? TLS_NOVERIFY : 0);
        if (e) {
            DosClose(h);
            if (tls_why[0] && (!silent || showerr)) {
                msg("  ");
                msg(tls_why);
                msg("\r\n");
            }
            die(tls_msg(e), e == TLSE_NET ? 7 : 35);
        }
        tlson = 1;
    }
    p = req;
    add(&p, method);
    add(&p, " ");
    add(&p, path);
    add(&p, " HTTP/1.1\r\nHost: ");
    for (k = 0; host[k] && host[k] != ':'; k++)
        ;
    if (host[k]) add(&p, "[");
    add(&p, host);
    if (host[k]) add(&p, "]");
    if (port != (https ? 443 : 80)) {
        char pn[8];
        add(&p, ":");
        ldec(port, pn);
        add(&p, pn);
    }
    add(&p, "\r\nUser-Agent: ");
    add(&p, ua);
    add(&p, "\r\nAccept: */*\r\nConnection: close\r\n");
    if (referer[0]) {
        add(&p, "Referer: ");
        add(&p, referer);
        add(&p, "\r\n");
    }
    if (auth[0]) {
        add(&p, "Authorization: Basic ");
        add(&p, auth);
        add(&p, "\r\n");
    }
    for (i = 0; i < nhdr; i++) {
        add(&p, hdrs[i]);
        add(&p, "\r\n");
    }
    if (have_data) {
        int ct = 0;
        char pn[8];
        for (i = 0; i < nhdr; i++)
            if (!lcmp(hdrs[i], "Content-Type:", 13))
                ct = 1;
        if (!ct)
            add(&p, "Content-Type: application/x-www-form-urlencoded\r\n");
        add(&p, "Content-Length: ");
        ldec(dlen, pn);
        add(&p, pn);
        add(&p, "\r\n");
    }
    add(&p, "\r\n");
    if (verbose) {                              /* Anfrage zeigen        */
        char *q = req, *e;
        while (q < p) {
            e = q;
            while (e < p && *e != '\r')
                e++;
            msg("> ");
            VioWrtTTY(q, (unsigned)(e - q));
            msg("\r\n");
            q = e + 2;
        }
    }
    if (tlson ? tls_write(req, (unsigned)(p - req)) != (int)(p - req) :
        sio(NET_SEND, h, req, (unsigned)(p - req)) != (unsigned)(p - req))
        die("Senden fehlgeschlagen", 55);
    if (have_data && dlen) {
        if (tlson ? tls_write(data, dlen) != (int)dlen :
            sio(NET_SEND, h, data, dlen) != dlen)
            die("Senden fehlgeschlagen", 55);
    }
    code = read_head();
    if (!code)
        die("keine g\x81ltige HTTP-Antwort", 8);
    if (verbose)
        show_head(0);
    if (follow && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
        p = hval("Location");
        if (tlson) tls_close();
        DosClose(h);
        if (!p || ++hops > 10)
            die("zu viele Weiterleitungen", 47);
        for (k = 0; p[k] && p[k] != '\n' && p[k] != '\r' && k < sizeof(loc) - 1; k++)
            loc[k] = p[k];
        loc[k] = 0;
        if (loc[0] == '/')
            scopy(path, loc, sizeof(path));
        else if (parse_url(loc))
            die("Ziel der Weiterleitung nicht verstanden", 3);
        else
            scopy(url, loc, sizeof(url));
        if (code == 303 || ((code == 301 || code == 302) && same(method, "POST"))) {
            scopy(method, "GET", sizeof(method));
            have_data = 0;
            dlen = 0;
        }
        goto again;
    }
    if (failf && code >= 400) {
        if (tlson) tls_close();
        DosClose(h);
        if (wfmt[0])
            wout();
        if (!silent || showerr) {
            char b[8];
            ldec(code, b);
            msg("curl: (22) Der Server meldet den Fehler ");
            msg(b);
            msg("\r\n");
        }
        DosExit(22);
    }
    if (out[0]) {
        fh = DosCreate(out);
        if (FAPI_FAILED(fh))
            die("Datei l\x84\xE1t sich nicht anlegen", 23);
    } else
        fh = STDOUT;
    if (incl || head_only)
        show_head(1);
    r = 0;
    if (!head_only && !same(method, "HEAD") && code != 204 && code != 304) {
        p = hval("Transfer-Encoding");
        chunked = p && !lcmp(p, "chunked", 7);
        p = hval("Content-Length");
        len = 0xFFFFFFFFUL;
        if (p && !chunked) {
            len = 0;
            while (*p >= '0' && *p <= '9')
                len = (len << 3) + (len << 1) + (*p++ - '0');
        }
        r = body(chunked, len);
    }
    if (tlson) tls_close();
    DosClose(h);
    if (fh != STDOUT)
        DosClose(fh);
    if (wfmt[0])
        wout();
    if (r)
        die("Verbindung vorzeitig beendet", 18);
    DosExit(0);
}
