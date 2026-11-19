/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FTP.EXE - Dateiuebertragung (Bedienung angelehnt an das ftp von
 * MS-DOS/Windows, eigener Code; RFC 959, RFC 2428 fuer IPv6):
 *   FTP [-n] [-v] [-i] [-d] [-A] [-4|-6] [-s:Datei] [Rechner [Port]]
 *   -n  nicht selbst anmelden     -v  Antworten des Servers nicht zeigen
 *   -i  bei MGET/MPUT nicht fragen -d  gesendete Befehle zeigen
 *   -A  anonym anmelden           -s:Datei  Befehle aus der Datei
 * Befehle: open close user ls dir cd cdup lcd pwd get recv put send mget
 * mput delete mdelete mkdir rmdir rename binary ascii type prompt hash
 * passive verbose debug status quote literal size bye quit help ?
 * (eindeutige Abkuerzungen gehen). Vorgabe passiv (PASV, bei IPv6 EPSV);
 * "passive" schaltet auf aktiv (PORT/EPRT) - dafuer braucht die Firewall
 * eine Regel fuer FTP.EXE. ERRORLEVEL 0, 1 bei einem Fehler im letzten
 * Befehl (Skripte), 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"

#define TMO     30000u                  /* ms je Empfang                 */

static char args[FAPI_ARGLEN], line[300], cmd[300], rl[600], host[128];
static char a1[FAPI_PATHLEN], a2[FAPI_PATHLEN], ts[64], loc[FAPI_PATHLEN];
static char buf[4096], names[8192];
static struct fapi_socket so;
static struct fapi_sockio io;
static struct fapi_dirent de;
static unsigned char ad[16], cad[16];
static unsigned ctl = 0xFFFF, verbose = 1, prompting = 1, hashing, passive = 1;
static unsigned binary = 1, dbg, autologin = 1, anon, failed, scr = 0xFFFF;
static unsigned conport;

/* ---- Ausgabe, Zahlen ---------------------------------------------- */

static void out(const char *s)          /* nach STDOUT (umleitbar)      */
{
    DosWrite(STDOUT, s, slen(s));
}

static void con(const char *s, unsigned n)  /* Tastatur-Echo: Bildschirm */
{
    VioWrtTTY(s, n);
}

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

static unsigned long mul10(unsigned long v) { return (v << 3) + (v << 1); }
static unsigned long mul100(unsigned long v) { return (v << 6) + (v << 5) + (v << 2); }

static char *ldec(unsigned long v, char *p)
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

static int lcmp(const char *a, const char *b, unsigned n)
{
    while (n--) {
        if (up(*a) != up(*b))
            return 1;
        a++;
        b++;
    }
    return 0;
}

/* ---- Eingabe: Tastatur, umgeleitete Eingabe oder -s:Datei ---------- */

static char ib[512];
static unsigned ipos, ilen, ieof;

static int in_byte(unsigned h)
{
    if (ipos >= ilen) {
        unsigned r = DosRead(h, ib, sizeof(ib));
        if (r == 0 || FAPI_FAILED(r))
            return -1;
        ilen = r;
        ipos = 0;
    }
    return (unsigned char)ib[ipos++];
}

/* Zeile holen; echo 0 = Kennwort; -> 0, -1 Ende der Eingabe */
static int get_line(const char *pr, char *b, unsigned max, int echo)
{
    unsigned n = 0, c;
    int k;
    unsigned h = scr != 0xFFFF ? scr : STDIN;
    if (pr)
        out(pr);
    if (h != STDIN || DosQHandType(STDIN) != HT_CONSOLE) {
        if (ieof)
            return -1;
        while ((k = in_byte(h)) >= 0 && k != '\n')
            if (k != '\r' && n < max - 1)
                b[n++] = (char)k;
        b[n] = 0;
        if (k < 0) {
            ieof = 1;
            if (!n)
                return -1;
        }
        if (echo) {
            out(b);
        }
        out("\r\n");
        return 0;
    }
    for (;;) {
        c = KbdCharIn();
        if ((c & 0xFF) == '\r') {
            b[n] = 0;
            con("\r\n", 2);
            if (DosQHandType(STDOUT) != HT_CONSOLE && echo) {
                out(b);                 /* Mitschrift in der Umleitung    */
                out("\r\n");
            }
            return 0;
        }
        if ((c & 0xFF) == '\b') {
            if (n) {
                n--;
                if (echo)
                    con("\b \b", 3);
            }
        } else if ((c & 0xFF) == 0x1B) {
            while (n) {
                n--;
                if (echo)
                    con("\b \b", 3);
            }
        } else if (!KEY_EXT(c & 0xFF, c >> 8) && (c & 0xFF) >= ' ' && n < max - 1) {
            b[n++] = (char)c;
            if (echo)
                con(b + n - 1, 1);
        }
    }
}

/* ---- Steuerverbindung --------------------------------------------- */

static unsigned sio(unsigned fn, unsigned h, void *b, unsigned len, unsigned ms)
{
    io.h = h;
    io.buf = (unsigned)b;
    io.len = len;
    io.flags = 0;
    io.timeout = ms;
    return DosNet(fn, &io, sizeof(io));
}

static int sendall(unsigned h, const char *b, unsigned n)
{
    unsigned r;
    while (n) {
        r = sio(NET_SEND, h, (void *)b, n, 0);
        if (r == 0 || FAPI_FAILED(r))
            return 1;
        b += r;
        n -= r;
    }
    return 0;
}

static char cb[1024];
static unsigned cpos, clen;

static int ctl_byte(void)
{
    if (cpos >= clen) {
        unsigned r = sio(NET_RECV, ctl, cb, sizeof(cb), TMO);
        if (r == 0 || FAPI_FAILED(r))
            return -1;
        clen = r;
        cpos = 0;
    }
    return (unsigned char)cb[cpos++];
}

static void disconnect(void)
{
    if (ctl != 0xFFFF)
        DosClose(ctl);
    ctl = 0xFFFF;
    cpos = clen = 0;
}

/* Antwort lesen (auch mehrzeilig "123-..." bis "123 ..."); -> Code,
   0 = Verbindung weg. Letzte Zeile bleibt in rl. */
static unsigned reply(void)
{
    unsigned n, code = 0, first = 1, c0 = 0;
    int c;
    for (;;) {
        n = 0;
        while ((c = ctl_byte()) >= 0 && c != '\n')
            if (c != '\r' && n < sizeof(rl) - 1)
                rl[n++] = (char)c;
        rl[n] = 0;
        if (c < 0 && !n) {
            out("Verbindung vom Server geschlossen.\r\n");
            disconnect();
            return 0;
        }
        if (verbose) {
            out(rl);
            out("\r\n");
        }
        if (n >= 3 && rl[0] >= '1' && rl[0] <= '5' && rl[1] >= '0' && rl[1] <= '9' &&
            rl[2] >= '0' && rl[2] <= '9') {
            code = (rl[0] - '0') * 100 + (rl[1] - '0') * 10 + (rl[2] - '0');
            if (first) {
                c0 = code;
                first = 0;
                if (rl[3] != '-')
                    return code;
            } else if (code == c0 && rl[3] != '-')
                return code;
        } else if (first)
            return 0;
        if (c < 0)
            return code;
    }
}

/* Befehl senden (ohne CR LF), Antwort lesen -> Code */
static unsigned command(const char *c, const char *arg)
{
    unsigned n;
    if (ctl == 0xFFFF) {
        out("Nicht verbunden.\r\n");
        return 0;
    }
    scopy(cmd, c, sizeof(cmd));
    if (arg) {
        scat(cmd, " ", sizeof(cmd));
        scat(cmd, arg, sizeof(cmd));
    }
    if (dbg) {
        out("---> ");
        out(lcmp(c, "PASS", 4) ? cmd : "PASS xxxx");
        out("\r\n");
    }
    n = slen(cmd);
    cmd[n++] = '\r';
    cmd[n++] = '\n';
    if (sendall(ctl, cmd, n)) {
        out("Verbindung unterbrochen.\r\n");
        disconnect();
        return 0;
    }
    return reply();
}

/* ---- Verbinden, anmelden ------------------------------------------ */

static void login(void)
{
    unsigned r;
    char pw[64];
    if (anon) {
        scopy(a1, "anonymous", sizeof(a1));
    } else {
        scopy(ts, "Benutzer (", sizeof(ts));
        scat(ts, host, sizeof(ts));
        scat(ts, "): ", sizeof(ts));
        if (get_line(ts, a1, 64, 1) < 0)
            return;
    }
    r = command("USER", a1);
    if (r == 331 || r == 332) {
        if (anon)
            scopy(pw, "atdos@", sizeof(pw));
        else if (get_line("Kennwort: ", pw, sizeof(pw), 0) < 0)
            return;
        r = command("PASS", pw);
    }
    if (r != 230 && r != 202) {
        out("Anmeldung fehlgeschlagen.\r\n");
        failed = 1;
    }
}

static void do_open(const char *h, unsigned port)
{
    unsigned s, r, k;
    if (ctl != 0xFFFF) {
        out("Schon verbunden mit ");
        out(host);
        out(", erst CLOSE.\r\n");
        return;
    }
    scopy(host, h, sizeof(host));
    if (net_lookup(host, cad)) {
        out("Unbekannter Rechner ");
        out(host);
        out(".\r\n");
        failed = 1;
        return;
    }
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    s = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(s)) {
        out(s == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt).\r\n" : "Kein Socket frei.\r\n");
        failed = 1;
        return;
    }
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = cad[k];
    io.h = s;
    io.timeout = TMO;
    r = DosNet(NET_CONNECT, &io, sizeof(io));
    if (r) {
        DosClose(s);
        out(r == FAPI_ERR_TIMEOUT ? "Keine Antwort (Zeit\x81" "berschreitung).\r\n" :
            r == FAPI_ERR_RESET ? "Verbindung abgelehnt.\r\n" : "Verbinden fehlgeschlagen.\r\n");
        failed = 1;
        return;
    }
    ctl = s;
    conport = port;
    net_ntop(cad, ts);
    out("Verbunden mit ");
    out(host);
    if (!same(host, ts)) {
        out(" (");
        out(ts);
        out(")");
    }
    out(".\r\n");
    r = reply();
    if (r != 220) {
        if (r)
            disconnect();
        failed = 1;
        return;
    }
    if (autologin)
        login();
}

/* ---- Datenverbindung ---------------------------------------------- */

static unsigned lsock = 0xFFFF;         /* aktiv: Listener               */

static unsigned num_at(char **p)
{
    unsigned v = 0;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (*(*p)++ - '0');
    return v;
}

static unsigned data_connect(const unsigned char *a, unsigned port)
{
    unsigned s, k;
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    s = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(s))
        return 0xFFFF;
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = a[k];
    io.h = s;
    io.timeout = TMO;
    if (DosNet(NET_CONNECT, &io, sizeof(io))) {
        DosClose(s);
        return 0xFFFF;
    }
    return s;
}

/* Vor dem Befehl: passiv -> verbundenes Socket, aktiv -> Listener in
   lsock (0 zurueck); 0xFFFF = Fehler */
static unsigned data_open(void)
{
    unsigned r, k, port, h[6];
    char *p;
    if (passive) {
        if (!net_is4(cad)) {
            r = command("EPSV", 0);
            if (r != 229)
                return 0xFFFF;
            for (p = rl; *p && *p != '('; p++)
                ;
            if (!*p)
                return 0xFFFF;
            p++;                        /* (|||port|)                     */
            for (k = 0; k < 3 && *p && (*p < '0' || *p > '9'); k++)
                p++;
            port = num_at(&p);
            return port ? data_connect(cad, port) : 0xFFFF;
        }
        r = command("PASV", 0);
        if (r != 227)
            return 0xFFFF;
        for (p = rl + 4; *p && (*p < '0' || *p > '9'); p++)
            ;
        for (k = 0; k < 6; k++) {
            h[k] = num_at(&p);
            if (k < 5 && *p++ != ',')
                return 0xFFFF;
        }
        for (k = 0; k < 16; k++)
            ad[k] = 0;
        ad[10] = ad[11] = 0xFF;
        for (k = 0; k < 4; k++)
            ad[12 + k] = (unsigned char)h[k];
        /* Server hinter NAT melden oft ihre innere Adresse: liegt sie in
           einem privaten Netz und die Steuerverbindung nicht, nimm die
           Adresse der Steuerverbindung */
        if ((ad[12] == 10 || (ad[12] == 192 && ad[13] == 168) ||
             (ad[12] == 172 && (ad[13] & 0xF0) == 16) || ad[12] == 0) &&
            !(cad[12] == ad[12] && cad[13] == ad[13]))
            for (k = 12; k < 16; k++)
                ad[k] = cad[k];
        return data_connect(ad, (h[4] << 8) | h[5]);
    }
    /* aktiv: Listener auf der eigenen Adresse der Steuerverbindung */
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    lsock = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(lsock)) {
        lsock = 0xFFFF;
        return 0xFFFF;
    }
    io.h = ctl;
    DosNet(NET_GETNAME, &io, sizeof(io));
    for (k = 0; k < 16; k++)
        ad[k] = io.addr.a6[k];
    io.h = lsock;
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = 0;
    if (DosNet(NET_BIND, &io, sizeof(io)) || sio(NET_LISTEN, lsock, 0, 1, 0))
        goto bad;
    io.h = lsock;
    DosNet(NET_GETNAME, &io, sizeof(io));
    port = ntohs(io.addr.port);
    if (net_is4(cad)) {
        p = a1;
        for (k = 12; k < 16; k++) {
            p = ldec(ad[k], p);
            *p++ = ',';
        }
        p = ldec(port >> 8, p);
        *p++ = ',';
        ldec(port & 0xFF, p);
        r = command("PORT", a1);
    } else {
        scopy(a1, "|2|", sizeof(a1));
        net_ntop(ad, a1 + 3);
        p = a1 + slen(a1);
        *p++ = '|';
        p = ldec(port, p);
        *p++ = '|';
        *p = 0;
        r = command("EPRT", a1);
    }
    if (r == 200)
        return 0;
bad:
    DosClose(lsock);
    lsock = 0xFFFF;
    return 0xFFFF;
}

/* Nach der vorlaeufigen Antwort (1xx): aktiv -> Verbindung annehmen */
static unsigned data_accept(unsigned d)
{
    unsigned s;
    if (lsock == 0xFFFF)
        return d;
    s = sio(NET_ACCEPT, lsock, 0, 0, TMO);
    DosClose(lsock);
    lsock = 0xFFFF;
    if (FAPI_FAILED(s)) {
        out("Keine Datenverbindung vom Server (Firewall-Regel f\x81r FTP.EXE?).\r\n");
        return 0xFFFF;
    }
    return s;
}

static void data_abort(unsigned d)
{
    if (d != 0xFFFF && d)
        DosClose(d);
    if (lsock != 0xFFFF) {
        DosClose(lsock);
        lsock = 0xFFFF;
    }
}

static unsigned long xbytes;
static unsigned xt0;

static void x_start(void)
{
    xbytes = 0;
    xt0 = (unsigned)DosQTicks();
}

static void x_count(unsigned n)
{
    unsigned long o = xbytes;
    xbytes += n;
    if (hashing)
        while ((o >> 11) != (xbytes >> 11)) {
            out("#");
            o += 2048;
        }
}

static void x_report(const char *what)
{
    char t[80], *p;
    unsigned long dt = (unsigned)((unsigned)DosQTicks() - xt0);   /* 10 ms */
    if (!verbose)
        return;
    p = t;
    scopy(p, "ftp: ", 6);
    p += 5;
    p = ldec(xbytes, p);
    scopy(p, " Bytes ", 8);
    p += 7;
    scopy(p, what, 20);
    p += slen(what);
    scopy(p, " in ", 5);
    p += 4;
    p = ldec(udiv(dt, 100), p);
    *p++ = ',';
    *p++ = (char)('0' + (unsigned)udiv(dt - mul100(udiv(dt, 100)), 10));
    *p++ = (char)('0' + (unsigned)(dt - mul10(udiv(dt, 10))));
    scopy(p, " Sekunden", 10);
    p += 9;
    if (dt) {
        unsigned long rate = xbytes < 0x28F5C28UL ? udiv(mul100(xbytes), dt) :
                             udiv(xbytes, dt < 100 ? 1 : udiv(dt, 100));   /* Byte/s */
        *p++ = ' ';
        p = ldec(rate >> 10, p);
        *p++ = ',';
        *p++ = (char)('0' + (unsigned)udiv(mul10(rate & 1023), 1024));
        scopy(p, " KB/s", 6);
        p += 5;
    }
    *p++ = '\r';
    *p++ = '\n';
    *p = 0;
    out(t);
}

/* Datenbefehl ausfuehren: Empfang nach Handle oh (0xFFFF = in names
   sammeln) bzw. Senden aus ih. -> 0 ok */
static unsigned nlen;

static int transfer(const char *c, const char *arg, unsigned oh, unsigned ih, const char *what)
{
    unsigned d, r, n;
    nlen = 0;
    d = data_open();
    if (d == 0xFFFF) {
        out("Datenverbindung fehlgeschlagen.\r\n");
        return 1;
    }
    r = command(c, arg);
    if (r / 100 != 1) {
        data_abort(d);
        return 1;
    }
    d = data_accept(d);
    if (d == 0xFFFF) {
        reply();
        return 1;
    }
    x_start();
    if (ih != 0xFFFF) {
        while ((n = DosRead(ih, buf, sizeof(buf))) != 0 && !FAPI_FAILED(n)) {
            if (sendall(d, buf, n))
                break;
            x_count(n);
        }
        sio(NET_SHUTDOWN, d, 0, 1, 0);
        while ((n = sio(NET_RECV, d, buf, sizeof(buf), TMO)) != 0 && !FAPI_FAILED(n))
            ;
    } else
        for (;;) {
            n = sio(NET_RECV, d, buf, sizeof(buf), TMO);
            if (n == 0 || FAPI_FAILED(n))
                break;
            if (oh == 0xFFFF) {
                unsigned k;
                for (k = 0; k < n && nlen < sizeof(names) - 1; k++)
                    names[nlen++] = buf[k];
            } else if (DosWrite(oh, buf, n) != n) {
                out("Schreiben fehlgeschlagen (Datentr\x84ger voll?).\r\n");
                DosClose(d);
                reply();
                return 1;
            }
            x_count(n);
        }
    DosClose(d);
    if (hashing && xbytes >= 2048)
        out("\r\n");
    r = reply();
    names[nlen] = 0;
    if (r / 100 != 2)
        return 1;
    if (what)
        x_report(what);
    return 0;
}

/* ---- Befehle ------------------------------------------------------ */

static char *ap;                         /* Rest der Befehlszeile        */

static int arg(char *b, unsigned max, const char *ask)
{
    ap = skip(ap);
    if (!*ap && ask) {
        if (get_line(ask, b, max, 1) < 0)
            return 0;
        return b[0] != 0;
    }
    ap = word(ap, b, max);
    return b[0] != 0;
}

/* lokaler Name aus einem fernen Pfad (letzter Teil, ungueltige Zeichen -> _) */
static void local_name(const char *r, char *l)
{
    const char *b = r;
    unsigned n = 0;
    char c;
    for (; *r; r++)
        if (*r == '/' || *r == '\\')
            b = r + 1;
    while (*b && n < FAPI_PATHLEN - 1) {
        c = *b++;
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || (unsigned char)c < ' ')
            c = '_';
        l[n++] = c;
    }
    l[n] = 0;
}

static int get_file(const char *r, const char *l)
{
    unsigned h;
    int rc;
    h = DosCreate(l);
    if (FAPI_FAILED(h)) {
        out("Datei l\x84\xE1t sich nicht anlegen: ");
        out(l);
        out("\r\n");
        return 1;
    }
    rc = transfer("RETR", r, h, 0xFFFF, "empfangen");
    DosClose(h);
    if (rc)
        DosDelete(l);
    return rc;
}

static int put_file(const char *l, const char *r)
{
    unsigned h;
    int rc;
    h = DosOpen(l);
    if (FAPI_FAILED(h)) {
        out("Datei nicht gefunden: ");
        out(l);
        out("\r\n");
        return 1;
    }
    rc = transfer("STOR", r, 0xFFFF, h, "gesendet");
    DosClose(h);
    return rc;
}

static int confirm(const char *what, const char *name)
{
    unsigned c;
    if (!prompting || scr != 0xFFFF || DosQHandType(STDIN) != HT_CONSOLE)
        return 1;
    out(what);
    out(" ");
    out(name);
    out("? (J/N/A) ");
    for (;;) {
        c = (unsigned)up((char)KbdCharIn());
        if (c == 'J' || c == 'Y' || c == '\r') { out("J\r\n"); return 1; }
        if (c == 'N') { out("N\r\n"); return 0; }
        if (c == 'A') { out("A\r\n"); prompting = 0; return 1; }
        if (c == 0x1B || c == 0) { out("\r\n"); return -1; }
    }
}

/* NLST in names sammeln; -> Zahl der Namen (je Zeile, CR LF -> 0).
   Platzhalter wertet FTP selbst aus (viele Server kennen sie bei NLST
   nicht): NLST des Verzeichnisteils, dann Namen nach Muster filtern. */
static char npat[FAPI_PATHLEN], ndir[FAPI_PATHLEN];

/* Platzhalter wie Unix ueber den ganzen Namen (* und ?, ohne
   Gross/klein) - ferne Namen haben keine DOS-Endungsregel */
static int gmatch(const char *n, const char *p)
{
    const char *sn = 0, *sp = 0;
    while (*n) {
        if (*p == '*') {
            sp = ++p;
            sn = n;
        } else if (*p == '?' || up(*p) == up(*n)) {
            p++;
            n++;
        } else if (sp) {
            p = sp;
            n = ++sn;
        } else
            return 0;
    }
    while (*p == '*')
        p++;
    return !*p;
}

/* ferner Name eines NLST-Treffers: liefert der Server nur den Namen,
   kommt der Verzeichnisteil des Musters davor */
static const char *rname(const char *n)
{
    const char *q;
    if (!ndir[0])
        return n;
    for (q = n; *q; q++)
        if (*q == '/')
            return n;
    scopy(cmd, ndir, sizeof(cmd));
    if (ndir[slen(ndir) - 1] != '/')
        scat(cmd, "/", sizeof(cmd));
    scat(cmd, n, sizeof(cmd));
    return cmd;
}

static unsigned nlst(const char *pat)
{
    unsigned k, n = 0, sv = verbose, w = 0, i;
    const char *b = pat, *q;
    char *p, *e;
    for (q = pat; *q; q++)
        if (*q == '/')
            b = q + 1;
    if (has_wild(b)) {
        scopy(npat, b, sizeof(npat));
        scopy(a2, pat, sizeof(a2));
        a2[b - pat > 1 ? b - pat - 1 : b - pat] = 0;    /* "/" bleibt     */
        w = 1;
    }
    ndir[0] = 0;
    if (w)
        scopy(ndir, a2, sizeof(ndir));
    verbose = 0;
    k = transfer("NLST", w ? (a2[0] ? a2 : 0) : pat, 0xFFFF, 0xFFFF, 0);
    verbose = sv;
    if (k)
        return 0;
    for (k = 0; k < nlen; k++)
        if (names[k] == '\r' || names[k] == '\n')
            names[k] = 0;
    for (p = names, e = names + nlen, i = 0; p < e; p += slen(p) + 1) {
        const char *bn = p;
        unsigned l = slen(p);
        if (!l)
            continue;
        for (q = p; *q; q++)
            if (*q == '/')
                bn = q + 1;
        if (w && !gmatch(bn, npat))
            continue;
        for (k = 0; k <= l; k++)
            names[i + k] = p[k];
        i += l + 1;
        n++;
    }
    nlen = i;
    return n;
}

static char keep[sizeof(names)];

static void c_mget(void)
{
    unsigned r, i, kl, any = 0;
    char *p;
    while (arg(a1, sizeof(a1), any ? 0 : "Ferne Dateien: ")) {
        any = 1;
        if (!nlst(a1)) {
            out("Keine Dateien f\x81r ");
            out(a1);
            out(".\r\n");
            failed = 1;
            continue;
        }
        for (i = 0; i < nlen; i++)      /* transfer() ueberschreibt names */
            keep[i] = names[i];
        kl = nlen;
        for (p = keep; p < keep + kl; p += slen(p) + 1) {
            if (!*p)
                continue;
            r = (unsigned)confirm("mget", p);
            if (r == 0xFFFF)
                return;
            if (!r)
                continue;
            local_name(p, loc);
            scopy(a2, rname(p), sizeof(a2));
            if (get_file(a2, loc))
                failed = 1;
        }
    }
}

static void c_mput(void)
{
    unsigned n, k, any = 0, r;
    char *b;
    while (arg(a1, sizeof(a1), any ? 0 : "Lokale Dateien: ")) {
        any = 1;
        scopy(a2, a1, sizeof(a2));
        b = base_name(a2);
        if (b == a2)
            scopy(a2, ".", sizeof(a2));
        else if (b - a2 > 1 && b[-2] != ':')
            b[-1] = 0;
        else
            *b = 0;
        b = base_name(a1);
        for (n = 0, k = 0; DosFindEntry(&de, n, a2) != FAPI_ERROR; n++) {
            if (de.attr & 0x18 || !match(de.name, b))
                continue;
            k++;
            r = (unsigned)confirm("mput", de.name);
            if (r == 0xFFFF)
                return;
            if (!r)
                continue;
            if (a2[0] == '.' && !a2[1])
                scopy(loc, de.name, sizeof(loc));
            else
                join(loc, a2, de.name, sizeof(loc));
            if (put_file(loc, de.name))
                failed = 1;
        }
        if (!k) {
            out("Keine Dateien f\x81r ");
            out(a1);
            out(".\r\n");
            failed = 1;
        }
    }
}

static void c_list(int full)
{
    unsigned h = STDOUT;
    a1[0] = a2[0] = 0;
    arg(a1, sizeof(a1), 0);
    arg(a2, sizeof(a2), 0);
    if (a2[0] && !(a2[0] == '-' && !a2[1])) {
        h = DosCreate(a2);
        if (FAPI_FAILED(h)) {
            out("Datei l\x84\xE1t sich nicht anlegen.\r\n");
            failed = 1;
            return;
        }
    }
    if (transfer(full ? "LIST" : "NLST", a1[0] ? a1 : 0, h, 0xFFFF, "empfangen"))
        failed = 1;
    if (h != STDOUT)
        DosClose(h);
}

static void c_lcd(void)
{
    char cur[FAPI_PATHLEN + 4];
    if (arg(a1, sizeof(a1), 0)) {
        if (a1[1] == ':' && !a1[2]) {
            if (DosSelectDisk(up(a1[0]) - 'A')) {
                out("Ung\x81ltiges Laufwerk.\r\n");
                failed = 1;
                return;
            }
        } else {
            if (a1[1] == ':')
                DosSelectDisk(up(a1[0]) - 'A');
            if (DosChDir(a1)) {
                out("Verzeichnis nicht gefunden.\r\n");
                failed = 1;
                return;
            }
        }
    }
    DosQCurDir(cur);
    out("Lokales Verzeichnis ist jetzt ");
    out(cur);
    out(".\r\n");
}

static unsigned code2ok(unsigned r)
{
    if (r / 100 != 2)
        failed = 1;
    return r;
}

static const char *const help_txt =
    "Befehle (eindeutig abk\x81rzbar):\r\n"
    "  open Rechner [Port]  close  user [Name]  bye/quit\r\n"
    "  ls [Fern [Lokal]]  dir [Fern [Lokal]]  cd Verz  cdup  pwd  lcd [Verz]\r\n"
    "  get/recv Fern [Lokal]  put/send Lokal [Fern]  mget Muster...  mput Muster...\r\n"
    "  delete Datei  mdelete Muster...  mkdir Verz  rmdir Verz  rename Alt Neu\r\n"
    "  binary  ascii  type [binary|ascii]  size Datei  quote/literal Text\r\n"
    "  prompt  hash  passive  verbose  debug  status  help/?\r\n";

static const char *const cmds[] = {
    "open", "close", "disconnect", "user", "ls", "dir", "cd", "cdup", "lcd", "pwd",
    "get", "recv", "put", "send", "mget", "mput", "delete", "mdelete", "mkdir",
    "rmdir", "rename", "binary", "ascii", "type", "prompt", "hash", "passive",
    "verbose", "debug", "status", "quote", "literal", "size", "bye", "quit",
    "help", "?", 0
};

static int lookup(const char *w)
{
    unsigned k, n = slen(w), hit = 0xFFFF, cnt = 0;
    for (k = 0; cmds[k]; k++) {
        if (same(cmds[k], w))
            return (int)k;
        if (n && !lcmp(cmds[k], w, n) && slen(cmds[k]) > n) {
            hit = k;
            cnt++;
        }
    }
    if (cnt == 1)
        return (int)hit;
    return cnt ? -2 : -1;
}

static void onoff(const char *what, unsigned v)
{
    out(what);
    out(v ? " an.\r\n" : " aus.\r\n");
}

/* -> 1 = beenden */
static int run(char *l)
{
    char w[16];
    int c;
    unsigned r;
    ap = word(skip(l), w, sizeof(w));
    if (!w[0])
        return 0;
    c = lookup(w);
    if (c == -1) {
        out("Ung\x81ltiger Befehl.\r\n");
        failed = 1;
        return 0;
    }
    if (c == -2) {
        out("Mehrdeutiger Befehl.\r\n");
        failed = 1;
        return 0;
    }
    failed = 0;
    switch (c) {
    case 0:                             /* open                         */
        if (!arg(a1, sizeof(a1), "Zu: "))
            break;
        r = 21;
        if (arg(a2, sizeof(a2), 0)) {
            char *p = a2;
            r = num_at(&p);
        }
        do_open(a1, r ? r : 21);
        break;
    case 1: case 2:                     /* close                        */
        if (ctl != 0xFFFF) {
            command("QUIT", 0);
            disconnect();
        }
        break;
    case 3:                             /* user                         */
        if (ctl == 0xFFFF) {
            out("Nicht verbunden.\r\n");
            failed = 1;
            break;
        }
        if (arg(a1, sizeof(a1), "Benutzer: ")) {
            char pw[64];
            r = command("USER", a1);
            if (r == 331 || r == 332) {
                if (!arg(pw, sizeof(pw), 0) && get_line("Kennwort: ", pw, sizeof(pw), 0) < 0)
                    break;
                r = command("PASS", pw);
            }
            code2ok(r);
        }
        break;
    case 4: c_list(0); break;
    case 5: c_list(1); break;
    case 6:                             /* cd                           */
        if (arg(a1, sizeof(a1), "Fernes Verzeichnis: "))
            code2ok(command("CWD", a1));
        break;
    case 7: code2ok(command("CDUP", 0)); break;
    case 8: c_lcd(); break;
    case 9: code2ok(command("PWD", 0)); break;
    case 10: case 11:                   /* get                          */
        if (!arg(a1, sizeof(a1), "Ferne Datei: "))
            break;
        if (!arg(a2, sizeof(a2), 0))
            local_name(a1, a2);
        failed = get_file(a1, a2);
        break;
    case 12: case 13:                   /* put                          */
        if (!arg(a1, sizeof(a1), "Lokale Datei: "))
            break;
        if (!arg(a2, sizeof(a2), 0))
            scopy(a2, base_name(a1), sizeof(a2));
        failed = put_file(a1, a2);
        break;
    case 14: c_mget(); break;
    case 15: c_mput(); break;
    case 16:                            /* delete                       */
        if (arg(a1, sizeof(a1), "Ferne Datei: "))
            code2ok(command("DELE", a1));
        break;
    case 17:                            /* mdelete                      */
        while (arg(a1, sizeof(a1), 0)) {
            unsigned n = nlst(a1);
            char *p, *e;
            unsigned i;
            if (!n) {
                failed = 1;
                continue;
            }
            for (i = 0; i < nlen; i++)
                keep[i] = names[i];
            for (p = keep, e = keep + nlen; p < e; p += slen(p) + 1) {
                if (!*p)
                    continue;
                r = (unsigned)confirm("mdelete", p);
                if (r == 0xFFFF)
                    break;
                if (r) {
                    scopy(a2, rname(p), sizeof(a2));
                    code2ok(command("DELE", a2));
                }
            }
        }
        break;
    case 18:
        if (arg(a1, sizeof(a1), "Verzeichnis: "))
            code2ok(command("MKD", a1));
        break;
    case 19:
        if (arg(a1, sizeof(a1), "Verzeichnis: "))
            code2ok(command("RMD", a1));
        break;
    case 20:                            /* rename                       */
        if (!arg(a1, sizeof(a1), "Von: ") || !arg(a2, sizeof(a2), "Nach: "))
            break;
        r = command("RNFR", a1);
        if (r == 350)
            r = command("RNTO", a2);
        code2ok(r);
        break;
    case 21:
        binary = 1;
        code2ok(command("TYPE", "I"));
        break;
    case 22:
        binary = 0;
        code2ok(command("TYPE", "A"));
        break;
    case 23:                            /* type                         */
        if (arg(a1, sizeof(a1), 0)) {
            if (!lcmp(a1, "bin", 3) || !lcmp(a1, "image", 5))
                binary = 1;
            else if (!lcmp(a1, "asc", 3))
                binary = 0;
            else {
                out("Unbekannte \x9C" "bertragungsart.\r\n");
                break;
            }
            code2ok(command("TYPE", binary ? "I" : "A"));
        } else {
            out(binary ? "\x9C" "bertragungsart: bin\x84r.\r\n" : "\x9C" "bertragungsart: ASCII.\r\n");
        }
        break;
    case 24: prompting = !prompting; onoff("Interaktiver Modus", prompting); break;
    case 25: hashing = !hashing; onoff("Hash-Zeichen (je 2 KB)", hashing); break;
    case 26: passive = !passive; onoff("Passiver Modus", passive); break;
    case 27: verbose = !verbose; onoff("Ausf\x81hrlich", verbose); break;
    case 28: dbg = !dbg; onoff("Befehle zeigen", dbg); break;
    case 29:                            /* status                       */
        if (ctl != 0xFFFF) {
            out("Verbunden mit ");
            out(host);
            out(".\r\n");
        } else
            out("Nicht verbunden.\r\n");
        out(binary ? "\x9C" "bertragungsart bin\x84r" : "\x9C" "bertragungsart ASCII");
        out(passive ? ", passiv" : ", aktiv");
        out(prompting ? ", fragt bei mget/mput" : ", fragt nicht");
        out(verbose ? ", ausf\x81hrlich" : "");
        out(hashing ? ", Hash" : "");
        out(dbg ? ", Debug" : "");
        out(".\r\n");
        break;
    case 30: case 31:                   /* quote                        */
        ap = skip(ap);
        if (!*ap && get_line("Befehl: ", a1, sizeof(a1), 1) >= 0)
            ap = a1;
        if (*ap)
            r = command(ap, 0), failed = r == 0 || r >= 400;
        break;
    case 32:                            /* size                         */
        if (arg(a1, sizeof(a1), "Ferne Datei: "))
            code2ok(command("SIZE", a1));
        break;
    case 33: case 34:                   /* bye                          */
        if (ctl != 0xFFFF) {
            command("QUIT", 0);
            disconnect();
        }
        return 1;
    default:
        out(help_txt);
    }
    return 0;
}

void main(void)
{
    char *a, w[FAPI_PATHLEN];
    unsigned port = 21;
    char *p;

    DosGetArgs(args);
    host[0] = 0;
    for (a = skip(args); *a; a = next_word(a)) {
        if ((*a == '-' || *a == '/') && a[1] && a[2] <= ' ') {
            switch (up(a[1])) {
            case 'N': autologin = 0; continue;
            case 'V': verbose = 0; continue;
            case 'I': prompting = 0; continue;
            case 'D': dbg = 1; continue;
            case 'A': anon = 1; continue;
            case '4': net_fam = AF_INET; continue;
            case '6': net_fam = AF_INET6; continue;
            }
        }
        if ((*a == '-' || *a == '/') && up(a[1]) == 'S' && a[2] == ':') {
            word(a + 3, w, sizeof(w));
            scr = DosOpen(w);
            if (FAPI_FAILED(scr)) {
                out("Skript nicht gefunden.\r\n");
                DosExit(2);
            }
            continue;
        }
        if (*a == '-' || *a == '/') {
            out("FTP [-n] [-v] [-i] [-d] [-A] [-4|-6] [-s:Datei] [Rechner [Port]]\r\n");
            DosExit(2);
        }
        if (!host[0])
            word(a, host, sizeof(host));
        else {
            p = a;
            port = num_at(&p);
            if (!port)
                port = 21;
        }
    }
    if (host[0]) {
        scopy(a1, host, sizeof(a1));
        host[0] = 0;
        do_open(a1, port);
    }
    for (;;) {
        if (get_line("ftp> ", line, sizeof(line), 1) < 0)
            break;
        if (run(line))
            break;
    }
    if (ctl != 0xFFFF) {
        command("QUIT", 0);
        disconnect();
    }
    DosExit(failed ? 1 : 0);
}
