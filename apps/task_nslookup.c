/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NSLOOKUP.EXE - Namen aufloesen:  NSLOOKUP Name
 * Ueber FAPI_NET NET_RESOLVE (DNS-Aufloeser in TCPIP.SYS, Server aus DHCP
 * oder /DNS:). ERRORLEVEL: 0 gefunden, 1 unbekannt, 2 kein Netz/keine Antwort.
 */
#include "fapi.h"
#include "util.h"
#define NET_NO_PTON
#include "net.h"

static char args[FAPI_ARGLEN], ts[48];
static struct fapi_resolve rs;
static struct fapi_netif ni;

void main(void)
{
    char *a;
    unsigned r, k, n, n6;
    static unsigned char six[4][16];

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (a[0] == '/' && a[1] == '?')) {
        puts("L\x94st einen Rechnernamen \x81" "ber DNS auf.\r\n\r\nNSLOOKUP Name\r\n");
        DosExit(*a ? 0 : 2);
    }
    word(a, rs.name, sizeof(rs.name));
    ni.ifc = 0;
    if (DosNet(NET_IFINFO, &ni, sizeof(ni)) == 0 && (ni.dns[0][0] || ni.dns[0][10])) {
        puts("Server:  ");
        puts(net_ntop(ni.dns[0], ts));
        puts("\r\n\r\n");
    }
    rs.family = AF_INET6;               /* erst AAAA, dann A dazu          */
    r = DosNet(NET_RESOLVE, &rs, sizeof(rs));
    if (r == 0)
        for (k = 0; k < rs.count; k++)
            for (n = 0; n < 16; n++)
                six[k][n] = rs.addr[k][n];
    n6 = r == 0 ? rs.count : 0;
    rs.family = AF_INET;
    r = DosNet(NET_RESOLVE, &rs, sizeof(rs));
    if (r && n6) {                      /* nur IPv6                       */
        rs.count = 0;
        r = 0;
    }
    if (r == 0) {
        for (k = rs.count; k > 0; k--)  /* IPv4 hinter die IPv6           */
            if (k - 1 + n6 < 4)
                for (n = 0; n < 16; n++)
                    rs.addr[k - 1 + n6][n] = rs.addr[k - 1][n];
        for (k = 0; k < n6; k++)
            for (n = 0; n < 16; n++)
                rs.addr[k][n] = six[k][n];
        rs.count = rs.count + n6 > 4 ? 4 : rs.count + n6;
    }
    if (r == FAPI_ERR_NONET || r == FAPI_ERR_TIMEOUT) {
        puts(r == FAPI_ERR_NONET ? "Kein Netz oder kein DNS-Server.\r\n" :
             "Zeit\x81" "berschreitung: der DNS-Server antwortet nicht.\r\n");
        DosExit(2);
    }
    if (r) {
        puts("*** ");
        puts(rs.name);
        puts(" wurde nicht gefunden.\r\n");
        DosExit(1);
    }
    puts("Name:    ");
    puts(rs.name);
    puts(rs.count > 1 ? "\r\nAdressen:  " : "\r\nAddress:  ");
    for (k = 0; k < rs.count; k++) {
        if (k)
            puts("\r\n           ");
        puts(net_ntop(rs.addr[k], ts));
    }
    puts("\r\n");
}
