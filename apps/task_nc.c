/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NC.EXE - Netz-Werkzeug zum Testen (Bedienung angelehnt an netcat):
 *   NC -u Rechner Port [Text]   Datagramm senden, Antworten zeigen
 *   NC -u -l Port               auf Datagramme warten (Firewall beachten)
 *   -w s   Wartezeit auf Antworten (Vorgabe 3 s)
 * TCP folgt mit Stufe 2b. ERRORLEVEL: 0 Antwort da, 1 keine, 2 Fehler.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#define NET_NO_PTON
#include "net.h"

static char args[FAPI_ARGLEN], ts[48], host[64], text[128];
static char buf[1500];
static struct fapi_socket so;
static struct fapi_sockio io;

static unsigned num(char *a)
{
    unsigned v = 0;
    while (*a >= '0' && *a <= '9')
        v = v * 10 + (*a++ - '0');
    return v;
}

static void err(const char *m, unsigned rc)
{
    puts(m);
    puts("\r\n");
    DosExit(rc);
}

void main(void)
{
    char *a;
    unsigned port = 0, wait = 3, udp = 0, lst = 0, h, r, got = 0, k, n = 0;
    unsigned char ad[16];

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' && a[1] == 'u') udp = 1;
        else if (*a == '-' && a[1] == 'l') lst = 1;
        else if (*a == '-' && a[1] == 'w') { a = next_word(a); wait = num(a); }
        else if (*a == '-' || *a == '/')
            err("NC -u Rechner Port [Text]  |  NC -u -l Port  [-w s]", 2);
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
    if (!udp)
        err("TCP kommt mit der n\x84" "chsten Stufe; bis dahin nur -u.", 2);
    if (!port)
        err("Port fehlt.", 2);
    so.family = AF_INET;
    so.type = SOCK_DGRAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (h >= 0xFFF0)
        err(h == FAPI_ERR_NONET ? "Kein Netz (TCPIP.SYS fehlt)." : "Kein Socket frei.", 2);
    io.h = h;
    io.addr.len = 16;
    io.addr.family = AF_INET;
    io.addr.port = htons(port);
    if (lst) {
        if (DosNet(NET_BIND, &io, sizeof(io)))
            err("Port belegt.", 2);
        puts("Warte auf Datagramme (Strg+C beendet) ...\r\n");
        wait = 0;
    } else {
        if (net_lookup(host, ad))
            err("Rechner nicht gefunden.", 2);
        for (k = 0; k < 4; k++)
            io.addr.a4[k] = ad[12 + k];
        if (!n) {
            text[0] = 'h'; text[1] = 'a'; text[2] = 'l'; text[3] = 'l'; text[4] = 'o';
            n = 5;
        }
        text[n++] = '\r';
        text[n++] = '\n';
        io.buf = (unsigned)text;
        io.len = n;
        r = DosNet(NET_SENDTO, &io, sizeof(io));
        if (r != n)
            err("Senden fehlgeschlagen.", 2);
    }
    for (;;) {
        io.buf = (unsigned)buf;
        io.len = sizeof(buf) - 1;
        io.flags = 0;
        io.timeout = wait ? wait * 1000 : 0;
        r = DosNet(NET_RECVFROM, &io, sizeof(io));
        if (r >= 0xFFF0)
            break;
        got = 1;
        puts("[");
        for (k = 0; k < 4; k++)
            ad[12 + k] = io.addr.a4[k];
        for (k = 0; k < 10; k++)
            ad[k] = 0;
        ad[10] = ad[11] = 0xFF;
        puts(net_ntop(ad, ts));
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
