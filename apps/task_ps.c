/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PS.EXE - laufende Programme zeigen, Bedienung wie ps (procps):
 *
 *   PS            Programme der eigenen Sitzung: PID TTY TIME CMD
 *   PS -e | -A    alle
 *   PS -f         ausfuehrlich: PPID, Zustand, Laufzeit, Befehlszeile
 *   PS -l         dazu Groesse und Art (DOS-Box)
 *   PS aux / ax   BSD-Form: alle, ausfuehrlich
 *   PS -p n,...   nur diese PIDs
 *
 * PID = Platz im Kernel (0-7), TTY = Sitzung (S1 ...), TIME = Rechenzeit,
 * ELAPSED = seit dem Start, S = R laeuft/bereit, S wartet auf ein Kind,
 * D wartet im Treiber.
 */
#include "fapi.h"
#include "util.h"
#define PR_KIND
#define PR_STAT
#define PR_TIME
#include "proc.h"

static char ln[200], args[FAPI_ARGLEN];
static int all, full, lng;
static unsigned pmask;

static void usage(void)
{
    puts("Zeigt laufende Programme.\r\n\r\n"
         "PS [-e|-A] [-f] [-l] [-p PID[,PID...]]\r\n"
         "PS aux\r\n\r\n"
         "  -e, -A  alle Programme (sonst nur die der eigenen Sitzung)\r\n"
         "  -f      ausf\x81hrlich (PPID, Zustand, Laufzeit, Befehlszeile)\r\n"
         "  -l      dazu Gr\x94\xE1" "e in KB und Art\r\n"
         "  -p      nur die genannten PIDs\r\n");
}

static void opt(char *w)
{
    char *p;
    if (w[0] != '-') {                  /* BSD: aux, ax, a, x, u        */
        for (p = w; *p; p++) {
            if (*p == 'a' || *p == 'x' || *p == 'A' || *p == 'X')
                all = 1;
            else if (*p == 'u' || *p == 'U')
                full = 1;
            else if (*p == 'l' || *p == 'L')
                full = lng = 1;
        }
        return;
    }
    for (p = w + 1; *p; p++) {
        if (*p == 'e' || *p == 'A')
            all = 1;
        else if (*p == 'f')
            full = 1;
        else if (*p == 'l')
            lng = 1;
        else if (*p == 'p')
            return;                     /* Liste folgt (unten)          */
        else if (*p == '?' || *p == 'h') {
            usage();
            DosExit(0);
        } else {
            puts("ps: unbekannter Schalter -");
            putc(*p);
            puts("\r\n");
            DosExit(1);
        }
    }
}

static void pids(const char *s)
{
    for (; *s; s++)
        if (*s >= '0' && *s < '0' + NP)
            pmask |= 1u << (*s - '0');
}

void main(void)
{
    char w[64], *a, *o;
    unsigned t, self = NP, n = 0;
    int want_p = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            usage();
            return;
        }
        if (want_p) {
            pids(w);
            want_p = 0;
            continue;
        }
        if (w[0] == '-' && w[1] == 'p') {
            if (w[2])
                pids(w + 2);
            else
                want_p = 1;
            all = 1;
            continue;
        }
        opt(w);
    }
    pr_read();
    for (t = 0; t < NP; t++)
        if (pu[t] && (pt[t].p.flags & PROC_SELF))
            self = t;
    if (full && lng)
        puts("S  PID PPID TTY    KB      TIME  ELAPSED CMD\r\n");
    else if (full)
        puts("S  PID PPID TTY      TIME  ELAPSED CMD\r\n");
    else if (lng)
        puts("S  PID TTY    KB      TIME CMD\r\n");
    else
        puts("  PID TTY          TIME CMD\r\n");
    for (t = 0; t < NP; t++) {
        if (!pu[t])
            continue;
        if (pmask && !(pmask & (1u << t)))
            continue;
        if (!all && self < NP && pt[t].p.session != pt[self].p.session)
            continue;
        o = ln;
        if (full || lng) {
            *o++ = pr_stat(t);
            *o++ = ' ';
        }
        o = pr_num(o, t, full || lng ? 3 : 5);
        if (full) {
            *o++ = ' ';
            if (pt[t].p.parent == 0xFF)
                o = pr_num(o, 0, 4), o[-1] = '-';
            else
                o = pr_num(o, pt[t].p.parent, 4);
        }
        *o++ = ' ';
        *o++ = 'S';
        o = pr_num(o, pt[t].p.session + 1, 0);
        while (o < ln + (full ? 17 : lng ? 12 : 13))
            *o++ = ' ';
        if (lng) {
            o = pr_num(o, pt[t].p.kb, 4);
            *o++ = ' ';
        }
        {
            char tb[16];
            pr_time(tb, pt[t].cpu);
            for (n = slen(tb); n < 9; n++)
                *o++ = ' ';
            scopy(o, tb, 16);
            o += slen(o);
        }
        if (full) {
            char tb[16];
            pr_time(tb, pt[t].age);
            *o++ = ' ';
            for (n = slen(tb); n < 8; n++)
                *o++ = ' ';
            scopy(o, tb, 16);
            o += slen(o);
        }
        *o++ = ' ';
        scopy(o, pt[t].p.name, 13);
        o += slen(o);
        if (full && pt[t].p.args[0]) {
            *o++ = ' ';
            scopy(o, skip(pt[t].p.args), 41);
            o += slen(o);
        }
        if (lng && pt[t].p.kind) {
            scopy(o, " [", 3);
            scat(o, pr_kind(t), 20);
            scat(o, "]", 22);
            o += slen(o);
        }
        *o = 0;
        puts(ln);
        puts("\r\n");
    }
}
