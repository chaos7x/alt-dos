/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TELNET.EXE - Terminal ueber das Netz (RFC 854, eigener Code):
 *   TELNET [-4|-6] [-b] [-e] Rechner [Port]
 *   -b  Rueckschritt sendet ^H statt DEL (7Fh)
 *   -e  Zeichen selbst anzeigen (auch wenn der Server ECHO anbietet)
 * Optionen: ECHO und SUPPRESS-GO-AHEAD werden angenommen, TERMINAL-TYPE
 * meldet "ANSI", NAWS die Groesse des Bildschirms; alles andere wird
 * abgelehnt. Ohne ECHO des Servers zeigt TELNET Getipptes selbst und
 * schickt ganze Zeilen (wie das Unix-telnet ohne Optionen, z. B. an
 * einen SMTP- oder POP3-Port). Die Ausgabe geht durch die eingebaute
 * ANSI-Auswertung der Konsole. Strg+] beendet die Verbindung.
 * ERRORLEVEL: 0 ok, 1 keine Verbindung, 2 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"

#define IAC     255
#define DONT    254
#define DO      253
#define WONT    252
#define WILL    251
#define SB      250
#define SE      240
#define O_ECHO  1
#define O_SGA   3
#define O_TTYPE 24
#define O_NAWS  31

static char args[FAPI_ARGLEN], host[128], ts[64];
static unsigned char rb[2048], ob[2048], sbuf[64], line[256];
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned char ad[16];
static unsigned h, srv_echo, loc_echo, bs = 0x7F, nl, sbl;
static unsigned char st, opt;           /* Zustand des Empfangsautomaten */

static void out(const char *s)
{
    VioWrtTTY(s, slen(s));
}

static void die(const char *m, unsigned rc)
{
    out(m);
    out("\r\n");
    DosExit(rc);
}

static unsigned sio(unsigned fn, void *b, unsigned len, unsigned ms)
{
    io.h = h;
    io.buf = (unsigned)b;
    io.len = len;
    io.flags = 0;
    io.timeout = ms;
    return DosNet(fn, &io, sizeof(io));
}

static void tx(const unsigned char *b, unsigned n)
{
    unsigned r;
    while (n) {
        r = sio(NET_SEND, (void *)b, n, 0);
        if (r == 0 || FAPI_FAILED(r))
            die("\r\n[Verbindung abgebrochen]", 0);
        b += r;
        n -= r;
    }
}

static void tx3(unsigned char cmd, unsigned char o)
{
    unsigned char b[3];
    b[0] = IAC;
    b[1] = cmd;
    b[2] = o;
    tx(b, 3);
}

static void naws(void)
{
    unsigned char b[9];
    unsigned rows = VioGetRows();
    b[0] = IAC; b[1] = SB; b[2] = O_NAWS;
    b[3] = 0; b[4] = 80;
    b[5] = (unsigned char)(rows >> 8); b[6] = (unsigned char)rows;
    tx(b, 7);
    b[0] = IAC; b[1] = SE;
    tx(b, 2);
}

/* Anfrage des Servers beantworten: geantwortet wird nur, wenn sich der
   Zustand einer Option aendert (RFC 854/1143) - keine Schleifen */
static unsigned char rem[64], lcl[64];  /* Optionen der Gegenseite/eigene */

static void negotiate(unsigned char cmd, unsigned char o)
{
    unsigned char i = o < 64 ? o : 63;
    int ok;
    if (cmd == WILL || cmd == WONT) {
        ok = o == O_ECHO || o == O_SGA;
        if (cmd == WILL && ok && !rem[i]) {
            rem[i] = 1;
            tx3(DO, o);
        } else if (cmd == WILL && !ok)
            tx3(DONT, o);
        else if (cmd == WONT && rem[i]) {
            rem[i] = 0;
            tx3(DONT, o);
        }
        if (o == O_ECHO)
            srv_echo = rem[i];
        return;
    }
    ok = o == O_TTYPE || o == O_NAWS || o == O_SGA;
    if (cmd == DO && ok) {
        if (!lcl[i]) {
            lcl[i] = 1;
            tx3(WILL, o);
        }
        if (o == O_NAWS)
            naws();
    } else if (cmd == DO)
        tx3(WONT, o);
    else if (lcl[i]) {
        lcl[i] = 0;
        tx3(WONT, o);
    }
}

static void subneg(void)
{
    static const unsigned char tt[] = { IAC, SB, O_TTYPE, 0, 'A', 'N', 'S', 'I', IAC, SE };
    if (sbl >= 2 && sbuf[0] == O_TTYPE && sbuf[1] == 1)   /* SEND        */
        tx(tt, sizeof(tt));
}

/* Empfangenes auswerten, Text nach ob -> Bildschirm */
static void rx(unsigned n)
{
    unsigned k, o = 0;
    unsigned char c;
    for (k = 0; k < n; k++) {
        c = rb[k];
        switch (st) {
        case 0:
            if (c == IAC)
                st = 1;
            else if (c)
                ob[o++] = c;
            break;
        case 1:
            if (c == IAC) {
                ob[o++] = c;
                st = 0;
            } else if (c >= WILL && c <= DONT) {
                opt = c;
                st = 2;
            } else if (c == SB) {
                sbl = 0;
                st = 3;
            } else
                st = 0;                 /* NOP, GA, DM ...                */
            break;
        case 2:
            if (o) {
                VioWrtTTY((char *)ob, o);
                o = 0;
            }
            negotiate(opt, c);
            st = 0;
            break;
        case 3:                         /* Unterverhandlung bis IAC SE   */
            if (c == IAC)
                st = 4;
            else if (sbl < sizeof(sbuf))
                sbuf[sbl++] = c;
            break;
        case 4:
            if (c == SE) {
                subneg();
                st = 0;
            } else {
                if (c == IAC && sbl < sizeof(sbuf))
                    sbuf[sbl++] = c;
                st = 3;
            }
            break;
        }
    }
    if (o)
        VioWrtTTY((char *)ob, o);
}

static void key(unsigned c)
{
    unsigned char b[8], ch = (unsigned char)c, sc = (unsigned char)(c >> 8);
    unsigned n = 0;
    if (KEY_EXT(ch, sc)) {
        static const unsigned char scs[] = { 0x48, 0x50, 0x4D, 0x4B, 0x47, 0x4F };
        static const char seq[] = "ABCDHF";
        unsigned k;
        for (k = 0; k < sizeof(scs); k++)
            if (sc == scs[k]) {
                b[0] = 27; b[1] = '['; b[2] = (unsigned char)seq[k];
                n = 3;
            }
        if (!n && (sc == 0x53 || sc == 0x52 || sc == 0x49 || sc == 0x51)) {
            b[0] = 27; b[1] = '[';
            b[2] = sc == 0x52 ? '2' : sc == 0x53 ? '3' : sc == 0x49 ? '5' : '6';
            b[3] = '~';
            n = 4;
        }
        if (!n && sc >= 0x3B && sc <= 0x3E) {   /* F1-F4                */
            b[0] = 27; b[1] = 'O'; b[2] = (unsigned char)('P' + sc - 0x3B);
            n = 3;
        }
        if (n)
            tx(b, n);
        return;
    }
    if (ch == 0x1D) {                   /* Strg+]                        */
        out("\r\n[Verbindung beendet]\r\n");
        DosClose(h);
        DosExit(0);
    }
    if (!srv_echo || loc_echo) {        /* Zeilenbetrieb                 */
        if (ch == '\r') {
            out("\r\n");
            line[nl++] = '\r';
            line[nl++] = '\n';
            tx(line, nl);
            nl = 0;
        } else if (ch == '\b') {
            if (nl) {
                nl--;
                out("\b \b");
            }
        } else if (nl < sizeof(line) - 3) {
            if (ch == IAC)
                return;
            line[nl++] = ch;
            if (ch >= ' ')
                VioWrtTTY((char *)line + nl - 1, 1);
            else if (ch == 3 || ch == 4 || ch == 26) {   /* sofort senden */
                tx(line, nl);
                nl = 0;
            }
        }
        return;
    }
    if (ch == '\r') {
        b[0] = '\r'; b[1] = 0;
        n = 2;
    } else if (ch == '\b') {
        b[0] = (unsigned char)bs;
        n = 1;
    } else if (ch == IAC) {
        b[0] = b[1] = IAC;
        n = 2;
    } else {
        b[0] = ch;
        n = 1;
    }
    tx(b, n);
}

void main(void)
{
    char *a;
    unsigned port = 23, k, r, c;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == '4' && a[2] <= ' ') net_fam = AF_INET;
        else if (*a == '-' && a[1] == '6' && a[2] <= ' ') net_fam = AF_INET6;
        else if (*a == '-' && (a[1] | 32) == 'b' && a[2] <= ' ') bs = 8;
        else if (*a == '-' && (a[1] | 32) == 'e' && a[2] <= ' ') loc_echo = 1;
        else if (*a == '-' || *a == '/')
            die("TELNET [-4|-6] [-b] [-e] Rechner [Port]   (Strg+] beendet)", 2);
        else if (!host[0])
            word(a, host, sizeof(host));
        else {
            port = 0;
            while (*a >= '0' && *a <= '9')
                port = port * 10 + (*a++ - '0');
            if (!port)
                die("Port nicht verstanden.", 2);
        }
    }
    if (!host[0])
        die("TELNET [-4|-6] [-b] [-e] Rechner [Port]   (Strg+] beendet)", 2);
    if (net_lookup(host, ad))
        die("Rechner nicht gefunden.", 1);
    net_ntop(ad, ts);
    out("Verbinde mit ");
    out(host);
    if (!same(host, ts)) {
        out(" (");
        out(ts);
        out(")");
    }
    out(" ...\r\n");
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (FAPI_FAILED(h))
        die(h == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.", 1);
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.h = h;
    io.timeout = 20000;
    r = DosNet(NET_CONNECT, &io, sizeof(io));
    if (r)
        die(r == FAPI_ERR_TIMEOUT ? "Keine Antwort (Zeit\x81" "berschreitung)." :
            r == FAPI_ERR_RESET ? "Verbindung abgelehnt." : "Verbinden fehlgeschlagen.", 1);
    out("Verbunden. Strg+] beendet.\r\n");
    if (port == 23) {                   /* wie das Unix-telnet: anbieten */
        tx3(DO, O_SGA);
        rem[O_SGA] = 1;
        tx3(WILL, O_TTYPE);
        lcl[O_TTYPE] = 1;
        tx3(WILL, O_NAWS);
        lcl[O_NAWS] = 1;
    }
    for (;;) {
        r = sio(NET_RECV, rb, sizeof(rb), 30);
        if (r == 0)
            die("\r\n[Verbindung vom Server geschlossen]", 0);
        if (!FAPI_FAILED(r))
            rx(r);
        else if (r != FAPI_ERR_TIMEOUT && r != FAPI_ERR_AGAIN)
            die("\r\n[Verbindung abgebrochen]", 0);
        while ((c = KbdPeek()) != 0)
            key(KbdCharIn());
    }
}
