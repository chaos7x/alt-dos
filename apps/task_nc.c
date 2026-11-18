/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NC.EXE - Netz-Werkzeug (Bedienung angelehnt an netcat):
 *   NC Rechner Port [Text]      TCP verbinden; Tastatur -> Netz, Netz ->
 *                               Bildschirm; mit Text: senden, Antwort zeigen
 *   NC -l Port                  TCP: auf eine Verbindung warten
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
#define NET_NO_PTON
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

static void show_peer(void)
{
    unsigned k;
    for (k = 0; k < 10; k++)
        ad[k] = 0;
    ad[10] = ad[11] = 0xFF;
    for (k = 0; k < 4; k++)
        ad[12 + k] = io.addr.a4[k];
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
                VioWrtTTY("\r\n[Verbindung von der Gegenseite geschlossen]\r\n", 46);
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
    unsigned port = 0, wait = 0, udp = 0, lst = 0, h, r, got = 0, k, n = 0, c;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == 'u') udp = 1;
        else if (*a == '-' && a[1] == 'l') lst = 1;
        else if (*a == '-' && a[1] == 'w') { a = next_word(a); wait = num(a); }
        else if (*a == '-' || *a == '/')
            err("NC [-u] Rechner Port [Text]  |  NC [-u] -l Port  [-w s]", 2);
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
    so.family = AF_INET;
    so.type = udp ? SOCK_DGRAM : SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (h >= 0xFFF0)
        err(h == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.", 2);
    io.addr.len = 16;
    io.addr.family = AF_INET;
    io.addr.port = htons(port);
    if (lst) {
        io.h = h;
        if (DosNet(NET_BIND, &io, sizeof(io)))
            err("Port belegt.", 2);
    } else {
        if (net_lookup(host, ad))
            err("Rechner nicht gefunden.", 2);
        for (k = 0; k < 4; k++)
            io.addr.a4[k] = ad[12 + k];
    }

    if (!udp) {
        if (lst) {
            if (sio(NET_LISTEN, h, 0, 1, 0))
                err("Lauschen nicht m\x94glich.", 2);
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
