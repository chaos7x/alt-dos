/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IPCONFIG.EXE - Netzwerkeinstellungen anzeigen (wie IPCONFIG von Windows):
 *   IPCONFIG [/ALL] [/RELEASE [n]] [/RENEW [n]]
 * Ueber FAPI_NET NET_IFINFO/NET_DHCP (TCPIP.SYS); zeigt beide Familien.
 * ERRORLEVEL: 0 ok, 1 Fehler, 2 kein Netz.
 */
#include "fapi.h"
#include "util.h"
#define NET_NO_PTON
#include "net.h"

static char args[FAPI_ARGLEN], ts[48];
static struct fapi_netif ni;
static struct fapi_netcmd nc;

static int zero16(const unsigned char *a)
{
    int k;
    for (k = 0; k < 16; k++)
        if (a[k])
            return 0;
    return 1;
}

static void field(const char *name)     /* "   Name . . . . . : "         */
{
    unsigned n = slen(name);
    puts("   ");
    puts(name);
    putc(' ');
    for (n++; n < 36; n += 2)
        puts(n + 1 < 36 ? ". " : " ");
    puts(": ");
}

static void hexb(unsigned char b)
{
    static const char hx[] = "0123456789ABCDEF";
    putc(hx[b >> 4]);
    putc(hx[b & 15]);
}

static void mask_of(unsigned plen)      /* Praefixlaenge IPv4 -> Maske    */
{
    unsigned k, b, m;
    for (k = 0; k < 4; k++) {
        m = 0;
        for (b = 0; b < 8; b++)
            if (k * 8 + b < plen)
                m |= 0x80 >> b;
        put_num(m, 0, 0);
        if (k < 3)
            putc('.');
    }
}

static void dur(unsigned long s)        /* Sekunden -> "1 Tag 2:03:04"    */
{
    unsigned d = 0, h, m;
    while (s >= 86400UL) { s -= 86400UL; d++; }
    for (h = 0; s >= 3600; s -= 3600, h++) ;
    for (m = 0; s >= 60; s -= 60, m++) ;
    if (d) {
        put_num(d, 0, 0);
        puts(d == 1 ? " Tag " : " Tage ");
    }
    put_num(h, 0, 0);
    putc(':');
    putc((char)('0' + m / 10)); putc((char)('0' + m % 10));
    putc(':');
    putc((char)('0' + (unsigned)s / 10)); putc((char)('0' + (unsigned)s % 10));
    puts(" (Std:Min:Sek)");
}

static void show(int all)
{
    unsigned k;
    puts("\r\nEthernet-Adapter Karte ");
    put_num(ni.ifc, 0, 0);
    puts(" (");
    puts(ni.name);
    puts("):\r\n\r\n");
    if (!(ni.link & 1)) {
        field("Medienstatus");
        puts("Medium getrennt\r\n");
    }
    field("Verbindungsspezifisches DNS-Suffix");
    puts(ni.domain);
    puts("\r\n");
    if (all) {
        field("Physische Adresse");
        for (k = 0; k < 6; k++) {
            hexb(ni.mac[k]);
            if (k < 5)
                putc('-');
        }
        puts("\r\n");
        field("Verbindung");
        if (ni.link & 1) {
            put_num((ni.link >> 8) * 10, 0, 0);
            puts(ni.link & 2 ? " Mbit/s, vollduplex\r\n" : " Mbit/s\r\n");
        } else
            puts("keine\r\n");
        field("Anschluss");
        puts("I/O ");
        hexb((unsigned char)(ni.io >> 8));
        hexb((unsigned char)ni.io);
        puts("h, IRQ ");
        put_num(ni.irq, 0, 0);
        puts("\r\n");
        field("DHCP aktiviert");
        puts(ni.naddr && ni.addr[0].kind == NA_STATIC ? "Nein\r\n" : "Ja\r\n");
    }
    for (k = 0; k < ni.naddr && k < NET_MAXADDR; k++) {
        if (net_is4(ni.addr[k].a)) {
            field("IPv4-Adresse");
            puts(net_ntop(ni.addr[k].a, ts));
            puts("\r\n");
            field("Subnetzmaske");
            mask_of(ni.addr[k].plen - 96);
            puts("\r\n");
        } else {
            field(ni.addr[k].kind == NA_LINK ? "Verbindungslokale IPv6-Adresse" : "IPv6-Adresse");
            puts(net_ntop(ni.addr[k].a, ts));
            putc('/');
            put_num(ni.addr[k].plen, 0, 0);
            puts("\r\n");
        }
    }
    if (!ni.naddr) {
        field("IPv4-Adresse");
        puts("(noch keine - DHCP l\x84uft)\r\n");
    }
    field("Standardgateway");
    if (!zero16(ni.gw6)) {
        puts(net_ntop(ni.gw6, ts));
        puts("\r\n");
        if (!zero16(ni.gw4))
            puts("                                       ");
    }
    if (!zero16(ni.gw4))
        puts(net_ntop(ni.gw4, ts));
    puts("\r\n");
    if (!all)
        return;
    if (!zero16(ni.dhcp)) {
        field("DHCP-Server");
        puts(net_ntop(ni.dhcp, ts));
        puts("\r\n");
        field("Lease l\x84uft ab in");
        dur(ni.lease);
        puts("\r\n");
    }
    field("DNS-Server");
    if (!zero16(ni.dns[0]))
        puts(net_ntop(ni.dns[0], ts));
    puts("\r\n");
    if (!zero16(ni.dns[1])) {
        puts("                                       ");
        puts(net_ntop(ni.dns[1], ts));
        puts("\r\n");
    }
    field("Rahmen empfangen/gesendet");
    put_num(ni.rx_ok, 0, 1);
    puts(" / ");
    put_num(ni.tx_ok, 0, 1);
    puts("\r\n");
    field("Fehler empfangen/gesendet/verworfen");
    put_num(ni.rx_err, 0, 1);
    puts(" / ");
    put_num(ni.tx_err, 0, 1);
    puts(" / ");
    put_num(ni.rx_drop, 0, 1);
    puts("\r\n");
}

void main(void)
{
    char *a;
    int all = 0, cmd = -1, i, any = 0;
    unsigned only = 0xFFFF, rc;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '/' || *a == '-') {
            if (a[1] == '?') {
                puts("Zeigt die Netzwerkeinstellungen an.\r\n\r\n"
                     "IPCONFIG [/ALL] [/RELEASE [Karte]] [/RENEW [Karte]]\r\n\r\n"
                     "  /ALL      alle Angaben\r\n"
                     "  /RELEASE  DHCP-Adresse zur\x81" "ckgeben\r\n"
                     "  /RENEW    DHCP-Adresse neu anfordern\r\n");
                return;
            }
            if (up(a[1]) == 'A') all = 1;
            else if (up(a[1]) == 'R' && up(a[3]) == 'L') cmd = 1;
            else if (up(a[1]) == 'R') cmd = 0;
            else {
                puts("Ung\x81ltige Option.\r\n");
                DosExit(1);
            }
        } else if (*a >= '0' && *a <= '9')
            only = (unsigned)(*a - '0');
    }
    puts("\r\nAT-DOS-IP-Konfiguration\r\n");
    if (all) {
        ni.ifc = 0;
        if (DosNet(NET_IFINFO, &ni, sizeof(ni)) == 0) {
            puts("\r\n");
            field("Hostname");
            puts(ni.host);
            puts("\r\n");
        }
    }
    for (i = 0; i < 4; i++) {
        ni.ifc = i;
        rc = DosNet(NET_IFINFO, &ni, sizeof(ni));
        if (rc == FAPI_ERR_NONET) {
            puts("\r\nKein Netz: TCPIP.SYS ist nicht geladen.\r\n");
            DosExit(2);
        }
        if (rc)
            continue;
        any = 1;
        if (cmd >= 0 && (only == 0xFFFF || only == (unsigned)i)) {
            nc.ifc = i;
            nc.arg = cmd;
            rc = DosNet(NET_DHCP, &nc, sizeof(nc));
            if (rc == FAPI_ERR_DENIED)
                puts("\r\nKarte hat eine feste Adresse (/IP: bei TCPIP.SYS).\r\n");
            else if (cmd == 0) {        /* auf die neue Adresse warten    */
                unsigned w;
                for (w = 0; w < 100; w++) {
                    DosSleep(100);
                    ni.ifc = i;
                    if (DosNet(NET_IFINFO, &ni, sizeof(ni)) == 0 && ni.naddr)
                        break;
                }
            }
            ni.ifc = i;
            DosNet(NET_IFINFO, &ni, sizeof(ni));
        }
        show(all);
    }
    if (!any) {
        puts("\r\nKeine Netzwerkkarte angemeldet (z. B. DEVICE=RTL8139.SYS).\r\n");
        DosExit(1);
    }
}
