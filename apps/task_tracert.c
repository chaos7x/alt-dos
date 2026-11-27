/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TRACERT.EXE - Weg der Pakete zu einem Rechner, wie tracert von Windows:
 *
 *   TRACERT [-d] [-h Hops] [-w ms] [-4 | -6] Ziel
 *
 * Je Hop drei ICMP-Echo-Anfragen mit wachsender TTL (NET_PING ttl_out);
 * die Router antworten mit "TTL abgelaufen", das Ziel mit dem Echo.
 * Ohne -d werden die Adressen per DNS (PTR) zu Namen.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#define DQ_MIN
#include "dnsq.h"

static char args[FAPI_ARGLEN], target[128], ts[48], hn[256];
static struct fapi_ping pg;
static unsigned char dns[16];
static int have_dns;
static struct dq_rr rr;

static void memcpy16(unsigned char *d, const unsigned char *s)
{
    int i;
    for (i = 0; i < 16; i++)
        d[i] = s[i];
}

static unsigned num(const char *s)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

static void put_u(unsigned v)
{
    char b[6];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        putc(b[--n]);
}

/* PTR-Name der Adresse nach hn -> 1 gefunden */
static int rname(const unsigned char *a)
{
    static char rn[80];
    unsigned o, k, an;
    if (!have_dns)
        return 0;
    dq_tries = 1;
    dq_tmo = 2;
    dq_rev(rn, a);
    if (dq_ask(dns, 53, rn, 12, 1, 0) || (dq_msg[3] & 15))
        return 0;
    an = dq_g16(6);
    o = dq_skipq(12);
    for (k = 0; k < an && o; k++) {
        o = dq_rr(o, &rr);
        if (o && rr.type == 12) {
            dq_name(rr.rd, hn, sizeof(hn));
            k = slen(hn);
            if (k > 1 && hn[k - 1] == '.')
                hn[k - 1] = 0;
            return 1;
        }
    }
    return 0;
}

static void usage(void)
{
    puts("Verfolgt den Weg der Pakete zu einem Rechner.\r\n\r\n"
         "TRACERT [-d] [-h Hops] [-w ms] [-4 | -6] Ziel\r\n\r\n"
         "  -d       Adressen nicht in Namen aufl\x94sen\r\n"
         "  -h Hops  h\x94" "chstens so viele Hops (Vorgabe 30)\r\n"
         "  -w ms    Wartezeit je Antwort (Vorgabe 4000)\r\n"
         "  -4, -6   nur IPv4 bzw. IPv6\r\n");
}

void main(void)
{
    char *a, o;
    unsigned hops = 30, to = 4000, ttl, k, rc;
    int nores = 0, named = 0, done = 0, got;
    unsigned char hop[16];

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' || *a == '/') {
            o = up(a[1]);
            if (o == '?') { usage(); return; }
            if (o == 'D') { nores = 1; continue; }
            if (o == '4') { net_fam = AF_INET; continue; }
            if (o == '6') { net_fam = AF_INET6; continue; }
            if (o == 'H' || o == 'W') {
                a = next_word(a);
                if (o == 'H') hops = num(a);
                else to = num(a);
                continue;
            }
            puts("Ung\x81ltige Option.\r\n");
            DosExit(4);
        }
        word(a, target, sizeof(target));
    }
    if (!target[0]) {
        usage();
        DosExit(4);
    }
    if (net_pton(target, pg.addr)) {
        rc = net_lookup(target, pg.addr);
        if (rc == FAPI_ERR_NONET) {
            puts("Kein Netz: TCPIP.SYS oder Netzwerkkarte fehlt.\r\n");
            DosExit(2);
        }
        if (rc) {
            puts("Der Zielname ");
            puts(target);
            puts(" konnte nicht aufgel\x94st werden.\r\n");
            DosExit(1);
        }
        named = 1;
    }
    have_dns = !nores && dq_server(0, dns) == 0;
    net_ntop(pg.addr, ts);
    puts("\r\nRoutenverfolgung zu ");
    if (named) {
        puts(target);
        puts(" [");
        puts(ts);
        puts("]");
    } else
        puts(ts);
    puts("\r\n\x81" "ber maximal ");
    put_u(hops);
    puts(" Hops:\r\n\r\n");
    for (ttl = 1; ttl <= hops && !done; ttl++) {
        got = 0;
        if (ttl < 10) putc(' ');
        if (ttl < 100) putc(' ');
        put_u(ttl);
        for (k = 0; k < 3; k++) {
            pg.len = 32;
            pg.timeout = to;
            pg.ttl_out = ttl;
            pg.seq = ttl * 3 + k;
            rc = DosNet(NET_PING, &pg, sizeof(pg));
            if (rc == FAPI_ERR_NONET) {
                puts("\r\nKein Netz.\r\n");
                DosExit(2);
            }
            if (rc) {
                puts("        *");
                continue;
            }
            if (!got)
                memcpy16(hop, pg.from);
            got = 1;
            if (pg.rtt < 1) {
                puts("    <1 ms");
            } else {
                unsigned w = pg.rtt, d = 1;
                while (w >= 10) { w /= 10; d++; }
                for (; d < 6; d++) putc(' ');
                put_u(pg.rtt);
                puts(" ms");
            }
            if (!pg.type || (pg.type == 3 && pg.code != 0))
                done = 1;               /* Ziel antwortet (Echo oder Port) */
        }
        puts("  ");
        if (!got)
            puts("Zeit\x81" "berschreitung der Anforderung.");
        else {
            net_ntop(hop, ts);
            if (rname(hop)) {
                puts(hn);
                puts(" [");
                puts(ts);
                puts("]");
            } else
                puts(ts);
            if (pg.type == 3 && pg.code == 0)
                puts(": Zielnetz nicht erreichbar.");
            else if (pg.type == 3 && pg.code == 1)
                puts(": Zielhost nicht erreichbar.");
        }
        puts("\r\n");
    }
    puts("\r\nAblaufverfolgung beendet.\r\n");
    DosExit(done ? 0 : 1);
}
