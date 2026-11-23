/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WHOIS.EXE - Eintraege von Domains, Adressen und Netzen abfragen
 * (RFC 3912), Bedienung wie whois:
 *
 *   WHOIS [-h Server] [-p Port] [-4|-6] Abfrage
 *
 * Ohne -h fragt WHOIS zuerst whois.iana.org und folgt dort der Zeile
 * "refer:" bzw. "whois:" zum zustaendigen Server (eine Stufe, wie das
 * whois von Linux). Die Antwort geht auf die Standardausgabe.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"

static char args[FAPI_ARGLEN], query[128], server[128], refer[128];
static char buf[2048], line[256];
static struct fapi_socket so;
static struct fapi_sockio io;
static unsigned char ad[16];
static unsigned port = 43, ln_n;

static int lcmp(const char *a, const char *b, unsigned n)
{
    while (n--) {
        if (up(*a) != up(*b))
            return 1;
        a++, b++;
    }
    return 0;
}

static void fail(const char *m, const char *x, unsigned rc)
{
    puts("whois: ");
    puts(m);
    if (x)
        puts(x);
    puts("\r\n");
    DosExit(rc);
}

/* eine Zeile der Antwort ansehen: "refer:" merken */
static void look(void)
{
    char *p = line;
    unsigned k;
    while (*p == ' ')
        p++;
    if (!lcmp(p, "refer:", 6))
        p += 6;
    else if (!lcmp(p, "whois:", 6))
        p += 6;
    else
        return;
    while (*p == ' ' || *p == '\t')
        p++;
    for (k = 0; p[k] && p[k] != ' ' && p[k] != '\r' && k < sizeof(refer) - 1; k++)
        refer[k] = p[k];
    refer[k] = 0;
}

static void ask(const char *srv, int print)
{
    unsigned h, r, k;
    if (net_lookup(srv, ad))
        fail("Server nicht gefunden: ", srv, 2);
    so.family = AF_INET6;
    so.type = SOCK_STREAM;
    h = DosNet(NET_SOCKET, &so, sizeof(so));
    if (h >= 0xFFF0)
        fail(h == FAPI_ERR_NONET ? "kein Netz (TCPIP.SYS fehlt)" : "kein Socket frei", 0, 2);
    io.addr.len = 28;
    io.addr.family = AF_INET6;
    io.addr.port = htons(port);
    for (k = 0; k < 16; k++)
        io.addr.a6[k] = ad[k];
    io.h = h;
    io.timeout = 15000;
    if (DosNet(NET_CONNECT, &io, sizeof(io))) {
        DosClose(h);
        fail("keine Verbindung zu ", srv, 2);
    }
    scopy(buf, query, sizeof(buf));
    scat(buf, "\r\n", sizeof(buf));
    io.h = h;
    io.buf = (unsigned)buf;
    io.len = slen(buf);
    io.flags = 0;
    DosNet(NET_SEND, &io, sizeof(io));
    ln_n = 0;
    for (;;) {
        io.h = h;
        io.buf = (unsigned)buf;
        io.len = sizeof(buf);
        io.flags = 0;
        io.timeout = 15000;
        r = DosNet(NET_RECV, &io, sizeof(io));
        if (r == 0 || r >= 0xFFF0)
            break;
        if (print)
            DosWrite(STDOUT, buf, r);
        for (k = 0; k < r; k++) {               /* Zeilen fuer refer:   */
            if (buf[k] == '\n' || ln_n == sizeof(line) - 1) {
                line[ln_n] = 0;
                look();
                ln_n = 0;
            } else if (buf[k] != '\r')
                line[ln_n++] = buf[k];
        }
    }
    DosClose(h);
}

void main(void)
{
    char *a, w[128];
    int want = 0;

    (void)net_ntop;                     /* aus net.h, hier ungenutzt    */
    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want == 'h') { scopy(server, w, sizeof(server)); want = 0; continue; }
        if (want == 'p') {
            unsigned v = 0, i;
            for (i = 0; w[i] >= '0' && w[i] <= '9'; i++)
                v = v * 10 + (unsigned)(w[i] - '0');
            port = v ? v : 43;
            want = 0;
            continue;
        }
        if ((w[0] == '/' || w[0] == '-') && w[1] == '?') {
            puts("Fragt den Whois-Eintrag einer Domain, Adresse oder eines Netzes ab.\r\n\r\n"
                 "WHOIS [-h Server] [-p Port] [-4|-6] Abfrage\r\n\r\n"
                 "Ohne -h: zuerst whois.iana.org, dann der dort genannte Server.\r\n");
            return;
        }
        if (w[0] == '-' && (w[1] == 'h' || w[1] == 'p') && !w[2]) { want = w[1]; continue; }
        if (w[0] == '-' && w[1] == '4' && !w[2]) { net_fam = AF_INET; continue; }
        if (w[0] == '-' && w[1] == '6' && !w[2]) { net_fam = AF_INET6; continue; }
        if (query[0])
            scat(query, " ", sizeof(query));
        scat(query, w, sizeof(query));
    }
    if (!query[0])
        fail("Abfrage fehlt (WHOIS /? zeigt die Bedienung)", 0, 1);
    if (server[0]) {
        ask(server, 1);
        return;
    }
    ask("whois.iana.org", 0);
    if (refer[0] && !same(refer, "whois.iana.org")) {
        scopy(server, refer, sizeof(server));
        refer[0] = 0;
        ask(server, 1);
    } else
        ask("whois.iana.org", 1);
}
