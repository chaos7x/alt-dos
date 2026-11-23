/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DIG.EXE - DNS abfragen wie dig von BIND (Verhalten nachgebaut, eigener
 * Code; Ausgabe im Format von dig, damit sie sich mit Linux vergleichen
 * laesst):
 *
 *   DIG [@Server] [-x Adresse] [-p Port] [-4|-6] Name [Typ] [Klasse]
 *       [+short] [+tcp] [+norecurse] [+noall +answer] [+time=s] [+tries=n]
 *
 * Typen A, AAAA, NS, CNAME, SOA, PTR, MX, TXT, SRV, CAA, HINFO, ANY,
 * TYPEnnn. Ohne Server der DNS-Server von TCPIP.SYS (DHCP, /DNS:).
 * ERRORLEVEL: 0 Antwort (auch NXDOMAIN, wie dig), 9 keine Antwort,
 * 10 Aufruf/kein Netz.
 */
#include "fapi.h"
#include "util.h"
#define NET_RESOLVE_FN
#include "net.h"
#include "dnsq.h"

static char args[FAPI_ARGLEN], name[260], srvs[128], tb[12], ln[400];
static unsigned char srv[16];
static struct dq_rr rr;
static int o_short, o_tcp, o_norec, o_answer = 1, o_auth = 1,
           o_add = 1, o_quest = 1, o_stats = 1, o_cmd = 1, o_comm = 1;

static void out(const char *s) { puts(s); }

static void tab(const char *s, unsigned col)
{
    unsigned n = slen(s);
    out(s);
    do {
        putc('\t');
        n = (n + 8) & ~7u;
    } while (n < col);
}

static int plus(const char *a, const char *w)
{
    unsigned i;
    for (i = 0; w[i]; i++)
        if (up(a[i]) != up(w[i]))
            return 0;
    return a[i] <= ' ' || a[i] == '=';
}

/* einen Abschnitt ausgeben: n Saetze ab o -> Stelle dahinter */
static unsigned section(unsigned o, unsigned n, const char *title, int show)
{
    unsigned i;
    int head = 0;
    for (i = 0; i < n && o; i++) {
        o = dq_rr(o, &rr);
        if (!o)
            break;
        if (rr.type == 41 || !show)     /* OPT                          */
            continue;
        dq_rdata(ln, sizeof(ln), rr.type, rr.rd, rr.rl);
        if (o_short) {
            out(ln);
            out("\r\n");
            continue;
        }
        if (!head && o_comm) {
            out("\r\n;; ");
            out(title);
            out(" SECTION:\r\n");
            head = 1;
        }
        tab(rr.name, 24);
        dq_num(tb, rr.ttl);
        out(tb);
        out("\t");
        out(rr.cls == 1 ? "IN" : rr.cls == 3 ? "CH" : "CLASS?");
        out("\t");
        dq_tname(tb, rr.type);
        out(tb);
        out("\t");
        out(ln);
        out("\r\n");
    }
    return o;
}

static void usage(void)
{
    out("DNS-Abfrage wie dig.\r\n\r\n"
        "DIG [@Server] [-x Adresse] [-p Port] [-4|-6] Name [Typ]\r\n"
        "    [+short] [+tcp] [+norecurse] [+noall +answer] [+time=s] [+tries=n]\r\n\r\n"
        "  Typ   A AAAA NS CNAME SOA PTR MX TXT SRV CAA HINFO ANY TYPEnnn\r\n"
        "  -x    R\x81" "ckw\x84rtssuche (PTR) zu einer IPv4- oder IPv6-Adresse\r\n"
        "  Ohne @Server fragt DIG den DNS-Server von TCPIP.SYS.\r\n");
}

void main(void)
{
    static char w[260];
    static unsigned char ad[16];
    unsigned type = 0, port = 53, r, o, qd, an, ns, ar, k;
    int rev = 0;
    char *a;

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (a[0] == '/' && a[1] == '?') || plus(a, "-h")) {
        usage();
        DosExit(*a ? 0 : 10);
    }
    for (; *a; a = skip(next_word(a))) {
        word(a, w, sizeof(w));
        if (w[0] == '@')
            scopy(srvs, w + 1, sizeof(srvs));
        else if (w[0] == '+') {
            char *v = w + 1, *eqp = v;
            int no = 0;
            while (*eqp && *eqp != '=') eqp++;
            if (up(v[0]) == 'N' && up(v[1]) == 'O') no = 1, v += 2;
            if (plus(v, "short")) o_short = !no;
            else if (plus(v, "tcp") || plus(v, "vc")) o_tcp = !no;
            else if (plus(v, "recurse")) o_norec = no;
            else if (plus(v, "all")) o_answer = o_auth = o_add = o_quest = o_stats = o_cmd = o_comm = !no;
            else if (plus(v, "answer")) o_answer = !no;
            else if (plus(v, "authority")) o_auth = !no;
            else if (plus(v, "additional")) o_add = !no;
            else if (plus(v, "question")) o_quest = !no;
            else if (plus(v, "stats")) o_stats = !no;
            else if (plus(v, "cmd")) o_cmd = !no;
            else if (plus(v, "comments")) o_comm = !no;
            else if (plus(v, "time") && *eqp) { dq_tmo = 0; for (eqp++; *eqp >= '0' && *eqp <= '9'; eqp++) dq_tmo = dq_tmo * 10 + (*eqp - '0'); if (!dq_tmo) dq_tmo = 1; }
            else if ((plus(v, "tries") || plus(v, "retry")) && *eqp) { dq_tries = 0; for (eqp++; *eqp >= '0' && *eqp <= '9'; eqp++) dq_tries = dq_tries * 10 + (*eqp - '0'); if (!dq_tries) dq_tries = 1; }
            else {
                out(";; Ungueltige Option: ");
                out(w);
                out("\r\n");
            }
        } else if (w[0] == '-' && w[1] == 'x' && !w[2]) {
            a = skip(next_word(a));
            word(a, w, sizeof(w));
            if (net_pton(w, ad)) {
                out("DIG: ung\x81ltige Adresse ");
                out(w);
                out("\r\n");
                DosExit(10);
            }
            dq_rev(name, ad);
            type = 12;
            rev = 1;
        } else if (w[0] == '-' && w[1] == 'p' && !w[2]) {
            a = skip(next_word(a));
            port = 0;
            for (k = 0; a[k] >= '0' && a[k] <= '9'; k++)
                port = port * 10 + (a[k] - '0');
        } else if (w[0] == '-' && w[1] == '4' && !w[2])
            net_fam = AF_INET;
        else if (w[0] == '-' && w[1] == '6' && !w[2])
            net_fam = AF_INET6;
        else if (dq_ieq(w, "IN"))
            ;
        else if (!rev && name[0] && !type && dq_type(w))
            type = dq_type(w);
        else if (!name[0])
            scopy(name, w, sizeof(name));
        else if (!type && dq_type(w))
            type = dq_type(w);
        else {
            out(";; Unbekannte Angabe: ");
            out(w);
            out("\r\n");
        }
    }
    if (!name[0])
        scopy(name, ".", sizeof(name));
    if (!type)
        type = name[0] == '.' && !name[1] ? 2 : 1;
    if (dq_server(srvs, srv)) {
        out(srvs[0] ? ";; Server nicht gefunden: " : ";; Kein DNS-Server bekannt (TCPIP.SYS, DHCP oder /DNS:)");
        out(srvs);
        out("\r\n");
        DosExit(10);
    }
    net_ntop(srv, srvs);
    if (!o_short && o_cmd) {
        out("\r\n; <<>> DiG (AT-DOS) <<>> ");
        if (rev)
            out("-x ");
        out(name);
        out(" ");
        dq_tname(tb, type);
        out(tb);
        out("\r\n;; global options: +cmd\r\n");
    }
    r = dq_ask(srv, port, name[0] == '.' && !name[1] ? "" : name, type, !o_norec, o_tcp);
    if (r) {
        out(r == FAPI_ERR_NONET ? ";; Kein Netz (TCPIP.SYS fehlt).\r\n" :
            ";; connection timed out; no servers could be reached\r\n");
        DosExit(r == FAPI_ERR_NONET ? 10 : 9);
    }
    qd = dq_g16(4);
    an = dq_g16(6);
    ns = dq_g16(8);
    ar = dq_g16(10);
    if (!o_short && o_comm) {
        unsigned rc = dq_msg[3] & 15, f = dq_msg[2] << 8 | dq_msg[3];
        out(";; Got answer:\r\n;; ->>HEADER<<- opcode: QUERY, status: ");
        if (rc < 6)
            out(dq_rcodes[rc]);
        else {
            dq_num(tb, rc);
            out(tb);
        }
        out(", id: ");
        dq_num(tb, dq_g16(0));
        out(tb);
        out("\r\n;; flags: qr");
        if (f & 0x0400) out(" aa");
        if (f & 0x0200) out(" tc");
        if (f & 0x0100) out(" rd");
        if (f & 0x0080) out(" ra");
        if (f & 0x0020) out(" ad");
        if (f & 0x0010) out(" cd");
        out("; QUERY: ");   dq_num(tb, qd); out(tb);
        out(", ANSWER: ");  dq_num(tb, an); out(tb);
        out(", AUTHORITY: "); dq_num(tb, ns); out(tb);
        out(", ADDITIONAL: "); dq_num(tb, ar); out(tb);
        out("\r\n");
    }
    o = 12;
    for (k = 0; k < qd && o; k++) {     /* Frage                        */
        if (!o_short && o_quest && o_comm) {
            if (!k)
                out("\r\n;; QUESTION SECTION:\r\n");
            ln[0] = ';';
            dq_name(o, ln + 1, sizeof(ln) - 1);
            tab(ln, 32);
            out("IN\t");
            dq_tname(tb, type);
            out(tb);
            out("\r\n");
        }
        o = dq_skipq(o);
    }
    if (o) o = section(o, an, "ANSWER", o_answer);
    if (o) o = section(o, ns, "AUTHORITY", o_auth && !o_short);
    if (o) o = section(o, ar, "ADDITIONAL", o_add && !o_short);
    if (!o_short && o_stats) {
        static unsigned short dt[2];
        out("\r\n;; Query time: ");
        dq_num(tb, dq_ms);
        out(tb);
        out(" msec\r\n;; SERVER: ");
        out(srvs);
        out("#");
        dq_num(tb, port);
        out(tb);
        out("(");
        out(srvs);
        out(dq_tcp ? ") (TCP)\r\n" : ") (UDP)\r\n");
        DosGetDateTime(dt);
        out(";; WHEN: ");
        {
            unsigned d = dt[0], t = dt[1];
            char *p = ln;
            p = dq_num(p, d & 31); *p++ = '.';
            p = dq_num(p, (d >> 5) & 15); *p++ = '.';
            p = dq_num(p, 1980 + (d >> 9)); *p++ = ' ';
            *p++ = (char)('0' + (t >> 11) / 10); *p++ = (char)('0' + (t >> 11) % 10); *p++ = ':';
            *p++ = (char)('0' + ((t >> 5) & 63) / 10); *p++ = (char)('0' + ((t >> 5) & 63) % 10); *p++ = ':';
            *p++ = (char)('0' + (t & 31) * 2 / 10); *p++ = (char)('0' + (t & 31) * 2 % 10);
            *p = 0;
            out(ln);
        }
        out("\r\n;; MSG SIZE  rcvd: ");
        dq_num(tb, dq_len);
        out(tb);
        out("\r\n\r\n");
    }
    DosExit(0);
}
