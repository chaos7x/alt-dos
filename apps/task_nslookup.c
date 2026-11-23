/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NSLOOKUP.EXE - DNS abfragen, Bedienung und Ausgabe wie nslookup von
 * Windows (nicht interaktiv):
 *
 *   NSLOOKUP [-type=Typ] [-timeout=s] [-port=n] [-norecurse] Name [Server]
 *   NSLOOKUP Adresse [Server]          (Rueckwaertssuche, PTR)
 *
 * Ohne Typ fragt es A und AAAA. Einteilige Namen bekommen den DNS-Suffix
 * der Karte (DHCP-Option 15). Eigene Anfragen ueber dnsq.h, ohne den
 * Aufloeser von TCPIP.SYS und ohne HOSTS (wie bei Windows).
 * ERRORLEVEL: 0 gefunden, 1 unbekannt/keine Saetze, 2 kein Netz/keine
 * Antwort.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#include "dnsq.h"

static char args[FAPI_ARGLEN], name[260], qn[300], srvs[128], sname[256], tb[16];
static char ln[400], cn[256];
static unsigned char srv[16], ad[16];
static struct dq_rr rr;
static struct fapi_netif ni;
static unsigned port = 53, type, found;
static int rd = 1;

static void out(const char *s) { puts(s); }
static void outn(unsigned long v) { dq_num(tb, v); out(tb); }

static int has_dot(const char *s)
{
    for (; *s; s++)
        if (*s == '.')
            return 1;
    return 0;
}

/* Satzdaten im Stil von Windows ausgeben */
static void show(void)
{
    unsigned o = rr.rd, q;
    switch (rr.type) {
    case 1: case 28:
        dq_rdata(ln, sizeof(ln), rr.type, o, rr.rl);
        out(rr.name);
        out(rr.type == 1 ? "\tinternet address = " : "\tAAAA IPv6 address = ");
        out(ln);
        break;
    case 2:
        dq_name(o, ln, sizeof(ln));
        out(rr.name); out("\tnameserver = "); out(ln);
        break;
    case 5:
        dq_name(o, ln, sizeof(ln));
        out(rr.name); out("\tcanonical name = "); out(ln);
        break;
    case 12:
        dq_name(o, ln, sizeof(ln));
        out(rr.name); out("\tname = "); out(ln);
        break;
    case 15:
        dq_name(o + 2, ln, sizeof(ln));
        out(rr.name); out("\tMX preference = "); outn(dq_g16(o));
        out(", mail exchanger = "); out(ln);
        break;
    case 16:
        dq_rdata(ln, sizeof(ln), 16, o, rr.rl);
        out(rr.name); out("\ttext =\r\n\r\n\t"); out(ln); out("\r\n");
        break;
    case 6:
        out(rr.name);
        q = dq_name(o, ln, sizeof(ln));
        out("\r\n\tprimary name server = "); out(ln);
        q = q ? dq_name(q, ln, sizeof(ln)) : 0;
        out("\r\n\tresponsible mail addr = "); out(ln);
        if (q && q + 20 <= dq_len) {
            out("\r\n\tserial  = "); outn(dq_g32(q));
            out("\r\n\trefresh = "); outn(dq_g32(q + 4)); out(" Sekunden");
            out("\r\n\tretry   = "); outn(dq_g32(q + 8)); out(" Sekunden");
            out("\r\n\texpire  = "); outn(dq_g32(q + 12)); out(" Sekunden");
            out("\r\n\tdefault TTL = "); outn(dq_g32(q + 16)); out(" Sekunden");
        }
        break;
    case 33:
        dq_name(o + 6, ln, sizeof(ln));
        out(rr.name); out("\tSRV service location:");
        out("\r\n\t  priority       = "); outn(dq_g16(o));
        out("\r\n\t  weight         = "); outn(dq_g16(o + 2));
        out("\r\n\t  port           = "); outn(dq_g16(o + 4));
        out("\r\n\t  svr hostname   = "); out(ln);
        break;
    default:
        dq_rdata(ln, sizeof(ln), rr.type, o, rr.rl);
        dq_tname(tb, rr.type);
        out(rr.name); out("\t"); out(tb); out(" = "); out(ln);
    }
    out("\r\n");
}

/* Name des Servers per PTR, sonst "UnKnown" */
static void server_name(void)
{
    static char rn[80];
    unsigned o, k, an;
    scopy(sname, "UnKnown", sizeof(sname));
    dq_rev(rn, srv);
    if (dq_ask(srv, port, rn, 12, 1, 0) || (dq_msg[3] & 15))
        return;
    an = dq_g16(6);
    o = dq_skipq(12);
    for (k = 0; k < an && o; k++) {
        o = dq_rr(o, &rr);
        if (o && rr.type == 12) {
            dq_name(rr.rd, sname, sizeof(sname));
            k = slen(sname);
            if (k > 1 && sname[k - 1] == '.')
                sname[k - 1] = 0;
            return;
        }
    }
}

/* eine Frage stellen; 0 = Antwort da (auch NXDOMAIN), sonst Fehler */
static unsigned ask(const char *n, unsigned t)
{
    unsigned r = dq_ask(srv, port, n, t, rd, 0);
    if (r == FAPI_ERR_NONET) {
        out("Kein Netz (TCPIP.SYS fehlt).\r\n");
        DosExit(2);
    }
    if (r) {
        out("Zeit\x81" "berschreitung bei DNS-Anforderung.\r\n    Das Zeitlimit betr\x84gt ");
        outn(dq_tmo);
        out(" Sekunden.\r\n");
    }
    return r;
}

/* A + AAAA zusammen ausgeben (ohne -type) */
static int addresses(const char *n)
{
    static unsigned char addr[8][16];
    static char alias[4][128];
    unsigned na = 0, nal = 0, pass, k, o, an, t, i;
    int nx = 0, any = 0;
    cn[0] = 0;
    for (pass = 0; pass < 2; pass++) {
        t = pass ? 1 : 28;
        if (ask(n, t))
            return 2;
        if ((dq_msg[3] & 15) == 3) {
            nx = 1;
            break;
        }
        if (!pass && !(dq_msg[2] & 4))
            any |= 2;                   /* nicht autorisierend          */
        an = dq_g16(6);
        o = dq_skipq(12);
        for (k = 0; k < an && o; k++) {
            o = dq_rr(o, &rr);
            if (!o)
                break;
            if (rr.type == 5 && !pass && nal < 4) {
                scopy(alias[nal], rr.name, 128);
                i = slen(alias[nal]);
                if (i > 1 && alias[nal][i - 1] == '.') alias[nal][i - 1] = 0;
                nal++;
            }
            if (rr.type == t && na < 8) {
                if (t == 1) {
                    for (i = 0; i < 10; i++) addr[na][i] = 0;
                    addr[na][10] = addr[na][11] = 0xFF;
                    for (i = 0; i < 4; i++) addr[na][12 + i] = dq_msg[rr.rd + i];
                } else
                    for (i = 0; i < 16; i++) addr[na][i] = dq_msg[rr.rd + i];
                na++;
                scopy(cn, rr.name, sizeof(cn));
            }
        }
    }
    if (nx || !na) {
        out("*** ");
        out(sname);
        out(nx ? " wurde " : " hat f\x81r ");
        out(n);
        out(nx ? " nicht gefunden: Non-existent domain\r\n"
               : " keine A- oder AAAA-Eintr\x84ge\r\n");
        return 1;
    }
    if (any & 2)
        out("Nicht autorisierende Antwort:\r\n");
    i = slen(cn);
    if (i > 1 && cn[i - 1] == '.')
        cn[i - 1] = 0;
    out("Name:    ");
    out(cn);
    out(na > 1 ? "\r\nAddresses:  " : "\r\nAddress:  ");
    for (k = 0; k < na; k++) {
        if (k)
            out("\r\n\t  ");
        out(net_ntop(addr[k], ln));
    }
    out("\r\n");
    if (nal) {
        out("Aliases:  ");
        for (k = 0; k < nal; k++) {
            if (k)
                out("\r\n\t  ");
            out(alias[k]);
        }
        out("\r\n");
    }
    out("\r\n");
    return 0;
}

/* -type=X oder PTR: alle Saetze der Antwort im Stil von Windows */
static int records(const char *n, unsigned t)
{
    unsigned an, ns, k, o;
    if (ask(n, t))
        return 2;
    if ((dq_msg[3] & 15) == 3) {
        out("*** ");
        out(sname);
        out(" wurde ");
        out(n);
        out(" nicht gefunden: Non-existent domain\r\n");
        return 1;
    }
    if (dq_msg[3] & 15) {
        out("*** ");
        out(sname);
        out(" kann ");
        out(n);
        out(" nicht finden: ");
        out((dq_msg[3] & 15) < 6 ? dq_rcodes[dq_msg[3] & 15] : "Fehler");
        out("\r\n");
        return 1;
    }
    an = dq_g16(6);
    ns = dq_g16(8);
    if (!(dq_msg[2] & 4))
        out("Nicht autorisierende Antwort:\r\n");
    o = dq_skipq(12);
    for (k = 0; k < an && o; k++) {
        o = dq_rr(o, &rr);
        if (o) {
            show();
            found++;
        }
    }
    if (!an && o) {                     /* keine Saetze: SOA der Zone    */
        for (k = 0; k < ns && o; k++) {
            o = dq_rr(o, &rr);
            if (o && rr.type == 6) {
                out("\r\n");
                show();
            }
        }
    }
    out("\r\n");
    return an ? 0 : 1;
}

static void usage(void)
{
    out("Fragt DNS-Server ab.\r\n\r\n"
        "NSLOOKUP [-type=Typ] [-timeout=s] [-port=n] [-norecurse] Name [Server]\r\n"
        "NSLOOKUP Adresse [Server]\r\n\r\n"
        "  Typ   A AAAA NS CNAME SOA PTR MX TXT SRV CAA ANY (ohne: A und AAAA)\r\n"
        "  Ohne Server fragt NSLOOKUP den DNS-Server von TCPIP.SYS.\r\n"
        "  Mehr Einzelheiten zeigt DIG.\r\n");
}

static int opt(const char *a, const char *w)
{
    unsigned i;
    for (i = 0; w[i]; i++)
        if (up(a[1 + i]) != up(w[i]))
            return 0;
    return a[1 + i] == '=' || a[1 + i] == 0 || a[1 + i] == ' ';
}

void main(void)
{
    static char w[260];
    char *a, *v;
    unsigned k;
    int r, ns = 0;

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (a[0] == '/' && a[1] == '?')) {
        usage();
        DosExit(*a ? 0 : 2);
    }
    for (; *a; a = skip(next_word(a))) {
        word(a, w, sizeof(w));
        if (w[0] == '-' && w[1]) {
            for (v = w; *v && *v != '='; v++)
                ;
            if (*v)
                v++;
            if (opt(w, "type") || opt(w, "querytype") || opt(w, "q") || opt(w, "ty")) {
                type = dq_type(v);
                if (!type) {
                    out("*** Unbekannter Typ: ");
                    out(v);
                    out("\r\n");
                    DosExit(2);
                }
            } else if (opt(w, "timeout")) {
                dq_tmo = 0;
                for (; *v >= '0' && *v <= '9'; v++)
                    dq_tmo = dq_tmo * 10 + (*v - '0');
                if (!dq_tmo)
                    dq_tmo = 1;
            } else if (opt(w, "retry")) {
                dq_tries = 0;
                for (; *v >= '0' && *v <= '9'; v++)
                    dq_tries = dq_tries * 10 + (*v - '0');
                if (!dq_tries)
                    dq_tries = 1;
            } else if (opt(w, "port")) {
                port = 0;
                for (; *v >= '0' && *v <= '9'; v++)
                    port = port * 10 + (*v - '0');
            } else if (opt(w, "norecurse"))
                rd = 0;
            else if (opt(w, "recurse"))
                rd = 1;
            else {
                out("*** Unbekannte Option: ");
                out(w);
                out("\r\n");
            }
            continue;
        }
        if (ns == 0)
            scopy(name, w, sizeof(name));
        else if (ns == 1)
            scopy(srvs, w, sizeof(srvs));
        ns++;
    }
    if (dq_server(srvs, srv)) {
        out(srvs[0] ? "*** Server nicht gefunden: " : "*** Kein DNS-Server bekannt (TCPIP.SYS, DHCP oder /DNS:)");
        out(srvs);
        out("\r\n");
        DosExit(2);
    }
    if (!name[0]) {
        usage();
        DosExit(2);
    }
    server_name();
    out("Server:  ");
    out(sname);
    out("\r\nAddress:  ");
    out(net_ntop(srv, ln));
    out("\r\n\r\n");

    if (!net_pton(name, ad)) {          /* Adresse: PTR                  */
        dq_rev(qn, ad);
        DosExit(records(qn, 12));
    }
    scopy(qn, name, sizeof(qn));
    ni.ifc = 0;                         /* einteilig: DNS-Suffix dazu    */
    if (!has_dot(name) && DosNet(NET_IFINFO, &ni, sizeof(ni)) == 0 && ni.domain[0]) {
        scat(qn, ".", sizeof(qn));
        scat(qn, ni.domain, sizeof(qn));
    }
    k = slen(qn);
    r = type ? records(qn, type) : addresses(qn);
    if (r == 1 && k != slen(name)) {    /* mit Suffix nichts: so wie er ist */
        scopy(qn, name, sizeof(qn));
        r = type ? records(qn, type) : addresses(qn);
    }
    DosExit(r);
}
