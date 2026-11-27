/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ROUTE.EXE - Routentabelle zeigen wie route print von Windows:
 *
 *   ROUTE PRINT [-4 | -6]
 *
 * Die Routen ergeben sich bei TCPIP.SYS aus den Adressen der Karten
 * (eigenes Netz "Auf Verbindung", Standard-Gateway aus DHCP bzw. /GW:
 * und dem Router-Advertisement). Eigene Routen (ADD, DELETE, CHANGE)
 * kennt der Stapel nicht.
 */
#include "fapi.h"
#include "util.h"
#define NET_NO_PTON
#include "net.h"

static char args[FAPI_ARGLEN], ts[48], ms[20];
static struct fapi_netif ni;
static int no4, no6;

static const char line[] =
    "===========================================================================\r\n";

static void rpad(const char *s, unsigned w)      /* rechtsbuendig      */
{
    unsigned n = slen(s);
    while (n++ < w)
        putc(' ');
    puts(s);
}

static void lpad(const char *s, unsigned w)
{
    unsigned n = slen(s);
    puts(s);
    while (n++ < w)
        putc(' ');
}

static void put_u(unsigned v)
{
    char b[6];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        putc(b[--n]);
}

static int zero16(const unsigned char *a)
{
    int i;
    for (i = 0; i < 16; i++)
        if (a[i])
            return 0;
    return 1;
}

static int gw_set(const unsigned char *a)
{
    return !zero16(a) && !(net_is4(a) && !a[12] && !a[13] && !a[14] && !a[15]);
}

/* IPv4-Adresse aus 4 Byte mit ::ffff: nach ts */
static char *v4(const unsigned char *b4, char *out)
{
    unsigned char a[16];
    int i;
    for (i = 0; i < 10; i++)
        a[i] = 0;
    a[10] = a[11] = 0xFF;
    for (i = 0; i < 4; i++)
        a[12 + i] = b4[i];
    return net_ntop(a, out);
}

static void mask4(unsigned plen, unsigned char *m)
{
    unsigned i;
    for (i = 0; i < 4; i++) {
        unsigned b = plen >= 8 ? 8 : plen;
        m[i] = (unsigned char)(b ? 0xFF << (8 - b) : 0);
        plen -= b;
    }
}

static void row4(const unsigned char *dst, const unsigned char *msk, const char *gw,
                 const char *ifa, unsigned metric)
{
    rpad(v4(dst, ts), 17);
    rpad(v4(msk, ms), 17);
    rpad(gw, 17);
    rpad(ifa, 17);
    putc(' ');
    rpad("", 0);
    {
        char b[6];
        int n = 0;
        unsigned v = metric;
        do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n < 5) b[n++] = ' ';
        while (n) putc(b[--n]);
    }
    puts("\r\n");
}

void main(void)
{
    char *a, w[32], ifa[48], gwb[48];
    unsigned ifc, k, i, r;
    unsigned char net[4], m[4], all[4], host[4];

    DosGetArgs(args);
    a = skip(args);
    word(a, w, sizeof(w));
    if (!w[0] || (w[0] == '/' && w[1] == '?') || !same(w, "PRINT")) {
        if (w[0] && !(w[0] == '/' && w[1] == '?') &&
            (same(w, "ADD") || same(w, "DELETE") || same(w, "CHANGE"))) {
            puts("ROUTE: eigene Routen kennt TCPIP.SYS nicht - die Tabelle ergibt sich\r\n"
                 "aus den Adressen der Karten und dem Standard-Gateway.\r\n");
            DosExit(1);
        }
        puts("Zeigt die Routentabelle.\r\n\r\nROUTE PRINT [-4 | -6]\r\n");
        return;
    }
    for (a = next_word(a); *a; a = next_word(a)) {
        if (a[0] == '-' && a[1] == '4') no6 = 1;
        if (a[0] == '-' && a[1] == '6') no4 = 1;
    }
    puts(line);
    puts("Schnittstellenliste\r\n");
    for (ifc = 0; ; ifc++) {
        ni.ifc = ifc;
        r = DosNet(NET_IFINFO, &ni, sizeof(ni));
        if (r == FAPI_ERR_NONET) {
            puts("Kein Netz: TCPIP.SYS fehlt.\r\n");
            DosExit(2);
        }
        if (r)
            break;
        puts("  ");
        rpad("", 0);
        put_u(ifc + 1);
        puts("...");
        for (k = 0; k < 6; k++) {
            static const char hx[] = "0123456789abcdef";
            putc(hx[ni.mac[k] >> 4]);
            putc(hx[ni.mac[k] & 15]);
            putc(' ');
        }
        puts("......");
        puts(ni.name);
        puts("\r\n");
    }
    puts(line);
    if (!no4) {
        puts("\r\nIPv4-Routentabelle\r\n");
        puts(line);
        puts("Aktive Routen:\r\n     Netzwerkziel    Netzwerkmaske          Gateway    Schnittstelle Metrik\r\n");
        for (ifc = 0; ; ifc++) {
            ni.ifc = ifc;
            if (DosNet(NET_IFINFO, &ni, sizeof(ni)))
                break;
            for (k = 0; k < ni.naddr; k++) {
                if (!net_is4(ni.addr[k].a))
                    continue;
                net_ntop(ni.addr[k].a, ifa);
                for (i = 0; i < 4; i++)
                    host[i] = ni.addr[k].a[12 + i], all[i] = 0;
                if (gw_set(ni.gw4))
                    row4(all, all, net_ntop(ni.gw4, gwb), ifa, 25);
                mask4(ni.addr[k].plen > 96 ? ni.addr[k].plen - 96 : 0, m);
                for (i = 0; i < 4; i++)
                    net[i] = (unsigned char)(host[i] & m[i]);
                row4(net, m, "Auf Verbindung", ifa, 281);
                for (i = 0; i < 4; i++)
                    m[i] = 0xFF;
                row4(host, m, "Auf Verbindung", ifa, 281);
            }
        }
        puts(line);
    }
    if (!no6) {
        puts("\r\nIPv6-Routentabelle\r\n");
        puts(line);
        puts("Aktive Routen:\r\n If Metrik Netzwerkziel             Gateway\r\n");
        for (ifc = 0; ; ifc++) {
            ni.ifc = ifc;
            if (DosNet(NET_IFINFO, &ni, sizeof(ni)))
                break;
            if (gw_set(ni.gw6) && !net_is4(ni.gw6)) {
                rpad("", 0);
                puts(ifc + 1 < 10 ? "  " : " ");
                put_u(ifc + 1);
                puts("    281 ");
                lpad("::/0", 25);
                puts(net_ntop(ni.gw6, gwb));
                puts("\r\n");
            }
            for (k = 0; k < ni.naddr; k++) {
                unsigned char p[16];
                unsigned pl = ni.addr[k].plen, b;
                if (net_is4(ni.addr[k].a))
                    continue;
                for (i = 0; i < 16; i++) {
                    b = pl >= 8 ? 8 : pl;
                    p[i] = (unsigned char)(ni.addr[k].a[i] & (b ? 0xFF << (8 - b) : 0));
                    pl -= b;
                }
                net_ntop(p, ts);
                scat(ts, "/", sizeof(ts));
                {
                    char nb[4];
                    unsigned v = ni.addr[k].plen, n = 0;
                    char t[4];
                    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
                    for (i = 0; n; i++)
                        nb[i] = t[--n];
                    nb[i] = 0;
                    scat(ts, nb, sizeof(ts));
                }
                puts(ifc + 1 < 10 ? "  " : " ");
                put_u(ifc + 1);
                puts("    281 ");
                lpad(ts, 25);
                puts("Auf Verbindung\r\n");
                puts(ifc + 1 < 10 ? "  " : " ");
                put_u(ifc + 1);
                puts("    281 ");
                net_ntop(ni.addr[k].a, ts);
                scat(ts, "/128", sizeof(ts));
                lpad(ts, 25);
                if (slen(ts) >= 25)
                    putc(' ');
                puts("Auf Verbindung\r\n");
            }
        }
        puts(line);
    }
}
