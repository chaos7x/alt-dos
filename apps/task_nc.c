/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NC.EXE - Netz-Werkzeug (Bedienung angelehnt an netcat):
 *   NC Rechner Port [Text]      TCP verbinden; Tastatur -> Netz, Netz ->
 *                               Bildschirm; mit Text: senden, Antwort zeigen
 *   NC -l Port                  TCP: auf eine Verbindung warten
 *   NC -l -k Port               TCP: weiter lauschen, mehrere Verbindungen
 *                               zugleich (NET_SELECT); Empfangenes mit
 *                               Absender, getippte Zeilen gehen an alle
 *   NC -u Rechner Port [Text]   UDP: Datagramm senden, Antworten zeigen
 *   NC -u -l Port               UDP: auf Datagramme warten
 *   -w s   Frist (Verbinden, Antworten); Vorgabe 10 s bzw. 3 s bei UDP
 * Mit umgeleiteter Eingabe (NC r p < DATEI) wird die Datei gesendet und
 * danach die Senderichtung geschlossen; Empfangenes geht nach STDOUT
 * (NC r p > DATEI). Strg+C beendet. Die Firewall laesst eingehende
 * Verbindungen erst mit TCPIP.SYS /NOFW durch.
 * ERRORLEVEL: 0 ok, 1 keine Antwort/abgelehnt, 2 Fehler.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"

static char args[FAPI_ARGLEN], ts[48], host[64], text[128];
static char buf[4096];
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned char ad[16];

static unsigned num(char *a)
{
    unsigned v = 0;
    while (*a >= '0' && *a <= '9')
        v = v * 10 + (*a++ - '0');
    return v;
}

static void err(const char *m, unsigned rc)
{
    VioWrtTTY(m, slen(m));
    VioWrtTTY("\r\n", 2);
    DosExit(rc);
}

static void show_peer(void)            /* Sockets sind AF_INET6: IPv4    */
{                                       /* kommt als ::ffff:a.b.c.d        */
    unsigned k;
    for (k = 0; k < 16; k++)
        ad[k] = io.addr.a6[k];
    net_ntop(ad, ts);
}

static unsigned sio(unsigned fn, unsigned h, void *b, unsigned len, unsigned ms)
{
    io.h = h;
    io.buf = (unsigned)b;
    io.len = len;
    io.flags = 0;
    io.timeout = ms;
    return DosNet(fn, &io, sizeof(io));
}

/* Daten ausgeben, solange etwas kommt (ms: Frist je Stueck) */
static unsigned drain(unsigned h, unsigned ms)
{
    unsigned r;
    for (;;) {
        r = sio(NET_RECV, h, buf, sizeof(buf), ms);
        if (r == 0 || r >= 0xFFF0)
            return r;
        DosWrite(STDOUT, buf, r);
    }
}

/* -k: Listener lh und bis zu NSEL_MAX-1 Verbindungen ueber NET_SELECT */
static struct fapi_select sel;
static unsigned cl[NSEL_MAX - 1];
static char cname[NSEL_MAX - 1][24];

static void say(const char *a, const char *b, const char *c)
{
    VioWrtTTY(a, slen(a));
    VioWrtTTY(b, slen(b));
    VioWrtTTY(c, slen(c));
}

static void serve_multi(unsigned lh)
{
    unsigned ncl = 0, k, j, r, c, n = 0;
    char line[130];
    VioWrtTTY("Warte auf Verbindungen (-k, Strg+C beendet) ...\r\n", 49);
    for (;;) {
        sel.e[0].h = lh;
        sel.e[0].want = SEL_READ;
        for (k = 0; k < ncl; k++) {
            sel.e[k + 1].h = cl[k];
            sel.e[k + 1].want = SEL_READ;
        }
        sel.n = ncl + 1;
        sel.timeout = 50;               /* dazwischen die Tastatur        */
        r = DosNet(NET_SELECT, &sel, sizeof(sel));
        if (r >= 0xFFF0)
            err("SELECT fehlgeschlagen.", 2);
        if (sel.e[0].ready & SEL_READ) {
            c = sio(NET_ACCEPT, lh, 0, 0, 0);
            if (c < 0xFFF0) {
                show_peer();
                if (ncl < NSEL_MAX - 1) {
                    cl[ncl] = c;
                    scopy(cname[ncl], ts, sizeof(cname[0]));
                    ncl++;
                    say("[Verbunden mit ", ts, "]\r\n");
                } else {
                    DosClose(c);
                    say("[Abgewiesen (voll): ", ts, "]\r\n");
                }
            }
        }
        for (k = 0; k < ncl; k++) {
            if (!sel.e[k + 1].ready)
                continue;
            r = sio(NET_RECV, cl[k], buf, sizeof(buf), 0);
            if (r == 0 || r >= 0xFFF0) {
                say("[Getrennt: ", cname[k], "]\r\n");
                DosClose(cl[k]);
                for (j = k; j + 1 < ncl; j++) {
                    cl[j] = cl[j + 1];
                    scopy(cname[j], cname[j + 1], sizeof(cname[0]));
                    sel.e[j + 1].ready = sel.e[j + 2].ready;
                }
                ncl--;
                k--;
                continue;
            }
            say("[", cname[k], "] ");
            DosWrite(STDOUT, buf, r);
        }
        while ((c = KbdPeek()) != 0) {  /* Zeile an alle                  */
            c = KbdCharIn();
            if ((c & 0xFF) == '\r') {
                VioWrtTTY("\r\n", 2);
                line[n++] = '\r';
                line[n++] = '\n';
                for (k = 0; k < ncl; k++)
                    sio(NET_SEND, cl[k], line, n, 0);
                n = 0;
            } else if ((c & 0xFF) == '\b') {
                if (n) {
                    n--;
                    VioWrtTTY("\b \b", 3);
                }
            } else if ((c & 0xFF) >= ' ' && n < sizeof(line) - 3) {
                line[n++] = (char)c;
                VioWrtTTY(line + n - 1, 1);
            }
        }
    }
}

static void tcp_session(unsigned h, int interactive)
{
    unsigned r, n = 0, c;
    char line[130];

    if (text[0]) {                      /* Text senden, Antwort zeigen     */
        r = slen(text);
        text[r++] = '\r';
        text[r++] = '\n';
        sio(NET_SEND, h, text, r, 0);
        sio(NET_SHUTDOWN, h, 0, 1, 0);
        drain(h, 0);
        return;
    }
    if (!interactive) {                 /* Datei senden, dann lesen        */
        while ((r = DosRead(STDIN, buf, sizeof(buf))) != 0 && r < 0xFFF0)
            if (sio(NET_SEND, h, buf, r, 0) != r)
                err("Verbindung abgebrochen.", 1);
        sio(NET_SHUTDOWN, h, 0, 1, 0);
        drain(h, 0);
        return;
    }
    for (;;) {                          /* Tastatur und Netz abwechselnd   */
        r = sio(NET_RECV, h, buf, sizeof(buf), 50);
        if (r == 0) {
            if (DosQHandType(STDOUT) == HT_CONSOLE)
                VioWrtTTY("\r\n[Verbindung von der Gegenseite geschlossen]\r\n", 47);
            return;
        }
        if (r < 0xFFF0)
            DosWrite(STDOUT, buf, r);
        else if (r != FAPI_ERR_TIMEOUT && r != FAPI_ERR_AGAIN)
            err("Verbindung abgebrochen.", 1);
        while ((c = KbdPeek()) != 0) {
            c = KbdCharIn();
            if ((c & 0xFF) == '\r') {
                VioWrtTTY("\r\n", 2);
                line[n++] = '\r';
                line[n++] = '\n';
                sio(NET_SEND, h, line, n, 0);
                n = 0;
            } else if ((c & 0xFF) == '\b') {
                if (n) {
                    n--;
                    VioWrtTTY("\b \b", 3);
                }
            } else if ((c & 0xFF) >= ' ' && n < sizeof(line) - 3) {
                line[n++] = (char)c;
                VioWrtTTY(line + n - 1, 1);
            }
        }
    }
}

void main(void)
{
    char *a;
    unsigned port = 0, wait = 0, udp = 0, lst = 0, keep = 0, h, r, got = 0, k, n = 0, c;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == 'u') udp = 1;
        else if (*a == '-' && a[1] == 'l') lst = 1;
        else if (*a == '-' && a[1] == 'k') keep = 1;
        else if (*a == '-' && a[1] == 'w') { a = next_word(a); wait = num(a); }
        else if (*a == '-' && a[1] == '4') net_fam = AF_INET;
        else if (*a == '-' && a[1] == '6') net_fam = AF_INET6;
        else if (*a == '-' || *a == '/')
            err("NC [-u] [-4|-6] Rechner Port [Text]  |  NC [-u] -l [-k] Port  [-w s]", 2);
        else if (lst && !port) port = num(a);
        else if (!lst && !host[0]) word(a, host, sizeof(host));
        else if (!lst && !port) port = num(a);
        else {
            for (k = 0; a[k] && n < sizeof(text) - 3; k++)
                text[n++] = a[k];
            text[n] = 0;
            break;
        }
    }
    if (!port)
        err("Port fehlt.", 2);
    so.family = AF_INET6;               /* beide Familien (::ffff:a.b.c.d) */
    so.type = udp ? SOCK_DGRAM : SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (h >= 0xFFF0)
        err(h == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.", 2);
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    if (lst) {
        io.h = h;
        if (DosNet(NET_BIND, &io, sizeof(io)))
            err("Port belegt.", 2);
    } else {
        if (net_lookup(host, ad))
            err("Rechner nicht gefunden.", 2);
        for (k = 0; k < 16; k++)
            io.addr.a6[k] = ad[k];
    }

    if (!udp) {
        if (lst) {
            if (sio(NET_LISTEN, h, 0, keep ? 7 : 1, 0))
                err("Lauschen nicht m\x94glich.", 2);
            if (keep)
                serve_multi(h);
            VioWrtTTY("Warte auf eine Verbindung ...\r\n", 31);
            c = sio(NET_ACCEPT, h, 0, 0, wait * 1000);
            if (c >= 0xFFF0)
                err("Keine Verbindung.", 1);
            show_peer();
            VioWrtTTY("[Verbunden mit ", 15);
            VioWrtTTY(ts, slen(ts));
            VioWrtTTY("]\r\n", 3);
            DosClose(h);
            h = c;
        } else {
            io.h = h;
            io.timeout = (wait ? wait : 10) * 1000;
            r = DosNet(NET_CONNECT, &io, sizeof(io));
            if (r)
                err(r == FAPI_ERR_TIMEOUT ? "Keine Antwort (Zeit\x81" "berschreitung)." :
                    r == FAPI_ERR_RESET ? "Verbindung abgelehnt." : "Verbinden fehlgeschlagen.",
                    1);
        }
        tcp_session(h, DosQHandType(STDIN) == HT_CONSOLE);
        DosClose(h);
        return;
    }

    if (lst) {
        VioWrtTTY("Warte auf Datagramme (Strg+C beendet) ...\r\n", 43);
        wait = 0;
    } else {
        if (!wait)
            wait = 3;
        if (!n) {
            text[0] = 'h'; text[1] = 'a'; text[2] = 'l'; text[3] = 'l'; text[4] = 'o';
            n = 5;
        }
        text[n++] = '\r';
        text[n++] = '\n';
        io.h = h;
        io.buf = (unsigned)text;
        io.len = n;
        if (DosNet(NET_SENDTO, &io, sizeof(io)) != n)
            err("Senden fehlgeschlagen.", 2);
    }
    for (;;) {
        r = sio(NET_RECVFROM, h, buf, sizeof(buf) - 1, wait * 1000);
        if (r >= 0xFFF0)
            break;
        got = 1;
        show_peer();
        puts("[");
        puts(ts);
        puts(":");
        put_num(ntohs(io.addr.port), 0, 0);
        puts("] ");
        DosWrite(STDOUT, buf, r);
        if (r && buf[r - 1] != '\n')
            puts("\r\n");
    }
    DosClose(h);
    if (!got)
        puts("Keine Antwort.\r\n");
    DosExit(got ? 0 : 1);
}
