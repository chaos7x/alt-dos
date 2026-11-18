/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PING.EXE - ICMP-Echo an einen Rechner (Bedienung wie PING von Windows):
 *   PING [-t] [-n Anzahl] [-l Groesse] [-w Frist] [-i TTL] [-4|-6] Ziel
 * Ueber FAPI_NET NET_PING (TCPIP.SYS). Namen gehen erst mit DNS (Stufe 4).
 * ERRORLEVEL: 0 mindestens eine Antwort, 1 keine, 2 kein Netz, 4 Aufruf.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"

static char args[FAPI_ARGLEN], target[64], ts[48];
static struct fapi_ping pg;

static unsigned num(char *a)
{
    unsigned v = 0;
    while (*a >= '0' && *a <= '9')
        v = v * 10 + (*a++ - '0');
    return v;
}

/* a / b ohne 32-Bit-Hilfsroutinen */
static unsigned long udiv(unsigned long a, unsigned b)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = r << 1 | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

static void put_u(unsigned v)
{
    put_num(v, 0, 0);
}

static void usage(void)
{
    puts("Sendet ICMP-Echo-Anforderungen an einen Rechner.\r\n\r\n"
         "PING [-t] [-n Anzahl] [-l Gr\x94\xE1" "e] [-w Zeit] [-i TTL] [-4|-6] Ziel\r\n\r\n"
         "  -t      bis Strg+C senden\r\n"
         "  -n      Anzahl der Anforderungen (Vorgabe 4)\r\n"
         "  -l      Datenmenge in Byte (Vorgabe 32, h\x94" "chstens 1472)\r\n"
         "  -w      Frist je Antwort in ms (Vorgabe 4000)\r\n"
         "  -i      Time To Live\r\n");
}

void main(void)
{
    char *a, o;
    unsigned n = 4, len = 32, to = 4000, ttl = 0, sent = 0, got = 0, mn = 0xFFFF,
             mx = 0, rc, t0;
    unsigned long sum = 0;
    int forever = 0, named = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (*a == '-' || *a == '/') {
            o = up(a[1]);
            if (o == '?') {
                usage();
                return;
            }
            if (o == 'T') { forever = 1; continue; }
            if (o == '4') { net_fam = AF_INET; continue; }
            if (o == '6') { net_fam = AF_INET6; continue; }
            if (o == 'N' || o == 'L' || o == 'W' || o == 'I') {
                a = next_word(a);
                if (*a < '0' || *a > '9') {
                    puts("Wert f\x81r die Option fehlt.\r\n");
                    DosExit(4);
                }
                if (o == 'N') n = num(a);
                if (o == 'L') len = num(a);
                if (o == 'W') to = num(a);
                if (o == 'I') ttl = num(a);
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
    if (len > 1472) {               /* IPv6 1452: meldet der Stapel   */
        puts("Die Gr\x94\xE1" "e darf h\x94" "chstens 1472 Byte sein.\r\n");
        DosExit(4);
    }
    if (net_pton(target, pg.addr)) {
        rc = net_lookup(target, pg.addr);
        if (rc == FAPI_ERR_NONET) {
            puts("Kein Netz: TCPIP.SYS oder Netzwerkkarte fehlt, oder kein DNS-Server.\r\n");
            DosExit(2);
        }
        if (rc) {
            puts("Ping-Anforderung konnte Host \"");
            puts(target);
            puts("\" nicht finden. \x9A" "berpr\x81" "fen Sie den Namen, und versuchen Sie es erneut.\r\n");
            DosExit(1);
        }
        named = 1;
    }
    net_ntop(pg.addr, ts);
    puts("\r\nPing wird ausgef\x81hrt f\x81r ");
    if (named) {
        puts(target);
        puts(" [");
        puts(ts);
        puts("]");
    } else
        puts(ts);
    puts(" mit ");
    put_u(len);
    puts(" Bytes Daten:\r\n");
    for (pg.seq = 1; forever || sent < n; pg.seq++) {
        pg.len = len;
        pg.timeout = to;
        pg.ttl_out = ttl;
        t0 = DosQTicks();
        rc = DosNet(NET_PING, &pg, sizeof(pg));
        sent++;
        if (rc == FAPI_ERR_NONET) {
            puts("Kein Netz: TCPIP.SYS oder Netzwerkkarte fehlt, oder keine Adresse.\r\n");
            DosExit(2);
        }
        if (rc == FAPI_ERR_TIMEOUT)
            puts("Zeit\x81" "berschreitung der Anforderung.\r\n");
        else if (rc)
            puts("Fehler beim Senden.\r\n");
        else if (pg.type) {
            puts("Antwort von ");
            puts(net_ntop(pg.from, ts));
            puts(pg.type == 11 ? ": TTL beim \x9A" "bertragen abgelaufen.\r\n" :
                 pg.code == 1 ? ": Zielhost nicht erreichbar.\r\n" :
                 pg.code == 3 ? ": Zielport nicht erreichbar.\r\n" :
                 ": Zielnetz nicht erreichbar.\r\n");
        } else {
            got++;
            puts("Antwort von ");
            puts(net_ntop(pg.from, ts));
            puts(": Bytes=");
            put_u(len);
            if (pg.rtt < 10)
                puts(" Zeit<10ms");
            else {
                puts(" Zeit=");
                put_u(pg.rtt);
                puts("ms");
            }
            if (net_is4(pg.from)) {     /* IPv6: wie Windows ohne TTL    */
                puts(" TTL=");
                put_u(pg.ttl);
            }
            puts("\r\n");
            sum += pg.rtt;
            if (pg.rtt < mn) mn = pg.rtt;
            if (pg.rtt > mx) mx = pg.rtt;
        }
        if (forever || sent < n) {      /* eine Sekunde Abstand           */
            unsigned w = (DosQTicks() - t0) * 10;
            if (w < 1000)
                DosSleep(1000 - w);
        }
    }
    puts("\r\nPing-Statistik f\x81r ");
    puts(net_ntop(pg.addr, ts));
    puts(":\r\n    Pakete: Gesendet = ");
    put_u(sent);
    puts(", Empfangen = ");
    put_u(got);
    puts(", Verloren = ");
    put_u(sent - got);
    puts("\r\n    (");
    {
        unsigned long l = sent - got;
        put_u(sent ? (unsigned)udiv((l << 6) + (l << 5) + (l << 2), sent) : 0);
    }
    puts("% Verlust),\r\n");
    if (got) {
        puts("Ca. Zeitangaben in Millisek.:\r\n    Minimum = ");
        put_u(mn);
        puts("ms, Maximum = ");
        put_u(mx);
        puts("ms, Mittelwert = ");
        put_u((unsigned)udiv(sum, got));
        puts("ms\r\n");
    }
    DosExit(got ? 0 : 1);
}
