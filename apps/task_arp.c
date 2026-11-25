/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ARP.EXE - Nachbar-Cache zeigen (IPv4 wie arp -a von Windows, mit -6
 * auch die IPv6-Nachbarn aus der Neighbor Discovery):
 *
 *   ARP [-a | -g] [-6] [Adresse]
 *
 * Eintraege setzen oder loeschen (-s, -d) kann TCPIP.SYS nicht - die
 * Nachbarn lernt es selbst und vergisst sie nach einiger Zeit.
 */
#include "fapi.h"
#include "util.h"
#include "net.h"

static char args[FAPI_ARGLEN], ts[48], mac[20];
static struct fapi_neigh nb;
static struct fapi_netif ni;
static unsigned char only[16];
static int want6, have_only;

static const char hx[] = "0123456789abcdef";

static void fmt_mac(const unsigned char *m)
{
    int i, o = 0;
    for (i = 0; i < 6; i++) {
        mac[o++] = hx[m[i] >> 4];
        mac[o++] = hx[m[i] & 15];
        if (i < 5)
            mac[o++] = '-';
    }
    mac[o] = 0;
}

static void pad(const char *s, unsigned w)
{
    unsigned n = slen(s);
    puts(s);
    while (n++ < w)
        putc(' ');
}

static int same16(const unsigned char *a, const unsigned char *b)
{
    int i;
    for (i = 0; i < 16; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

void main(void)
{
    char *a, w[64];
    unsigned i, ifc, any = 0, other = 0;
    int shown;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if ((w[0] == '/' || w[0] == '-') && w[1] == '?') {
            puts("Zeigt die Zuordnung von IP- zu Hardwareadressen (Nachbar-Cache).\r\n\r\n"
                 "ARP [-a | -g] [-6] [Adresse]\r\n\r\n"
                 "  -a, -g  Eintr\x84ge zeigen (Vorgabe)\r\n"
                 "  -6      IPv6-Nachbarn (Neighbor Discovery) zeigen\r\n\r\n"
                 "Setzen und L\x94schen (-s, -d) gibt es nicht: TCPIP.SYS lernt die\r\n"
                 "Nachbarn selbst.\r\n");
            return;
        }
        if (w[0] == '-' && (up(w[1]) == 'S' || up(w[1]) == 'D') && !w[2]) {
            puts("ARP: Eintr\x84ge setzen oder l\x94schen kann TCPIP.SYS nicht.\r\n");
            DosExit(1);
        }
        if (w[0] == '-' && w[1] == '6') { want6 = 1; continue; }
        if (w[0] == '-')
            continue;
        if (!net_pton(w, only))
            have_only = 1;
    }
    for (ifc = 0; ; ifc++) {
        ni.ifc = ifc;
        i = DosNet(NET_IFINFO, &ni, sizeof(ni));
        if (i == FAPI_ERR_NONET) {
            puts("Kein Netz: TCPIP.SYS fehlt.\r\n");
            DosExit(2);
        }
        if (i)
            break;
        shown = 0;
        for (i = 0; ; i++) {
            nb.idx = i;
            if (DosNet(NET_NEIGH, &nb, sizeof(nb)))
                break;
            if (!nb.state || nb.ifc != ifc)
                continue;
            if (net_is4(nb.ip) == want6) {      /* andere Familie: nur zaehlen */
                if (!have_only)
                    other++;
                continue;
            }
            if (have_only && !same16(nb.ip, only))
                continue;
            if (!shown) {
                unsigned k;
                puts("\r\nSchnittstelle: ");
                for (k = 0; k < ni.naddr; k++)
                    if (net_is4(ni.addr[k].a) != want6)
                        break;
                puts(k < ni.naddr ? net_ntop(ni.addr[k].a, ts) : ni.name);
                puts(" --- 0x");
                putc(hx[ifc + 1]);
                puts(want6 ? "\r\n  IPv6-Adresse                             Physische Adresse     Typ\r\n"
                           : "\r\n  Internetadresse       Physische Adresse     Typ\r\n");
                shown = 1;
            }
            puts("  ");
            pad(net_ntop(nb.ip, ts), want6 ? 41 : 22);
            if (nb.state == 2) {
                fmt_mac(nb.mac);
                pad(mac, 22);
                puts("dynamisch");
            } else {
                pad("(unvollst\x84ndig)", 22);
                puts("wird gefragt");
            }
            puts("\r\n");
            any++;
        }
    }
    if (!any)
        puts("Keine ARP-Eintr\x84ge gefunden.\r\n");
    if (other) {                        /* PING mit Namen nimmt oft IPv6 */
        char d[6];
        int k = 0;
        puts("\r\nDazu ");
        do d[k++] = (char)('0' + other % 10); while ((other /= 10) != 0);
        while (k)
            putc(d[--k]);
        puts(want6 ? " IPv4-Nachbar(n) - anzeigen mit ARP -a\r\n"
                   : " IPv6-Nachbar(n) - anzeigen mit ARP -a -6\r\n");
    }
}
