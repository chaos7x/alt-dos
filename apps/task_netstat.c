/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NETSTAT.EXE - Verbindungen und offene Ports zeigen (wie NETSTAT von
 * Windows, Spalte Programm wie netstat -b):
 *   NETSTAT [-a]     ohne -a nur TCP-Verbindungen, mit -a auch lauschende
 *                    Ports und UDP
 * Ueber FAPI_NET NET_SOCKLIST (TCPIP.SYS). Adressen sind immer Zahlen.
 * ERRORLEVEL: 0 ok, 2 kein Netz.
 */
#include "fapi.h"
#include "util.h"
#define NET_NO_PTON
#include "net.h"

static char args[FAPI_ARGLEN], ts[64];
static struct fapi_sockinfo si;

static const char *const st_name[] = {
    "GESCHLOSSEN", "ABH\x99REN", "SYN_GESENDET", "SYN_EMPFANGEN", "HERGESTELLT",
    "FIN_WARTEN_1", "FIN_WARTEN_2", "SCHLIESSEN_WARTEN", "SCHLIESSEND",
    "LETZTES_ACK", "WARTEND"
};

static int zero16(const unsigned char *a)
{
    int k;
    for (k = 0; k < 16; k++)
        if (a[k])
            return 0;
    return 1;
}

static void col(const char *s, unsigned w)
{
    unsigned n = slen(s);
    puts(s);
    while (n++ < w)
        putc(' ');
}

static char *cat(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *num(char *p, unsigned v)
{
    char t[6];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    *p = 0;
    return p;
}

/* "a.b.c.d:port", "[v6]:port", "0.0.0.0:port"; port 0 und remote -> "*" */
static void addr(const unsigned char *a, unsigned fam, unsigned port, int remote)
{
    char *p = ts;
    if (remote && !port && zero16(a)) {
        col("*:*", 22);
        return;
    }
    if (zero16(a)) {
        p = cat(p, fam == AF_INET6 ? "[::]" : "0.0.0.0");
    } else if (net_is4(a)) {
        net_ntop(a, p);
        p += slen(p);
    } else {
        *p++ = '[';
        net_ntop(a, p);
        p += slen(p);
        *p++ = ']';
    }
    *p++ = ':';
    if (port)
        num(p, port);
    else {
        *p++ = '*';
        *p = 0;
    }
    col(ts, 22);
}

void main(void)
{
    char *a;
    int all = 0, any = 0;
    unsigned i, rc;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if ((*a == '-' || *a == '/') && a[1] == '?') {
            puts("Zeigt Netzwerkverbindungen und offene Ports an.\r\n\r\n"
                 "NETSTAT [-a]\r\n\r\n"
                 "  -a  auch lauschende Ports und UDP\r\n");
            return;
        }
        if ((*a == '-' || *a == '/') && up(a[1]) == 'A')
            all = 1;
        else if ((*a == '-' || *a == '/') && up(a[1]) == 'N')
            ;                           /* Adressen sind ohnehin Zahlen    */
        else {
            puts("Ung\x81ltige Option.\r\n");
            DosExit(1);
        }
    }
    for (i = 0;; i++) {
        si.idx = i;
        rc = DosNet(NET_SOCKLIST, &si, sizeof(si));
        if (rc == FAPI_ERR_NONET) {
            puts("Kein Netz: TCPIP.SYS ist nicht geladen.\r\n");
            DosExit(2);
        }
        if (rc)
            break;
        if (!si.type)
            continue;
        if (!all && (si.type != SOCK_STREAM || si.state <= 1))
            continue;
        if (!any) {
            puts("\r\nAktive Verbindungen\r\n\r\n");
            col("Proto", 6);
            col("Lokale Adresse", 22);
            col("Remoteadresse", 22);
            col("Status", 18);
            puts("Programm\r\n");
            any = 1;
        }
        col(si.type == SOCK_STREAM ? "TCP" : "UDP", 6);
        addr(si.laddr, si.family, si.lport, 0);
        if (si.type == SOCK_STREAM && si.state == 1)
            addr(si.raddr, si.family, 0, 1);
        else
            addr(si.raddr, si.family, si.rport, si.type != SOCK_STREAM);
        col(si.type == SOCK_STREAM && si.state <= 10 ? st_name[si.state] : "", 18);
        puts(si.prog[0] ? si.prog : "-");
        puts("\r\n");
    }
    if (!any)
        puts(all ? "Keine offenen Sockets.\r\n" : "Keine Verbindungen.\r\n");
}
