/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UPTIME.EXE - Uhrzeit, Laufzeit, Programme und Last, wie uptime:
 *   UPTIME [-p] [-s]
 *   -p nur die Laufzeit ("seit 2 Stunden, 5 Minuten"), -s Startzeitpunkt.
 * Last = Anteil der Rechenzeit aller Programme in den letzten 2 Sekunden.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static struct fapi_proc2 pt;
static unsigned dt[2];

static unsigned long div16(unsigned long a, unsigned b, unsigned *r)
{
    unsigned long q = 0, rem = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((a >> i) & 1);
        if (rem >= b) { rem -= b; q |= 1UL << i; }
    }
    if (r) *r = (unsigned)rem;
    return q;
}

static void two(unsigned v)
{
    putc((char)('0' + v / 10));
    putc((char)('0' + v % 10));
}

static unsigned long sample(unsigned *ntask, unsigned long *now)
{
    unsigned t;
    unsigned long sum = 0;
    *ntask = 0;
    for (t = 0; t < 8; t++)
        if (DosProc(PROC_INFO2, t, &pt) == 0 && pt.p.state) {
            sum += pt.cpu;
            (*ntask)++;
            *now = pt.now;
        }
    return sum;
}

static void unit(unsigned long v, const char *one, const char *many, int *first)
{
    if (!v)
        return;
    if (!*first)
        puts(", ");
    put_num(v, 0, 0);
    puts(v == 1 ? one : many);
    *first = 0;
}

void main(void)
{
    char *a;
    int o_p = 0, o_s = 0, first = 1;
    unsigned n, s, m, h;
    unsigned long now = 0, c1, c2, n1, secs, days, load;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt Uhrzeit, Laufzeit seit dem Start, Programme und Last.\r\n\r\n"
                 "UPTIME [-p] [-s]\r\n\r\n  -p  nur die Laufzeit\r\n  -s  Zeitpunkt des Starts\r\n");
            return;
        }
        if (a[0] == '-' && a[1] == 'p') o_p = 1;
        if (a[0] == '-' && a[1] == 's') o_s = 1;
    }
    c1 = sample(&n, &now);
    n1 = now;
    secs = div16(now, 100, 0);
    {
        unsigned long mins = div16(secs, 60, &s);
        unsigned long hrs = div16(mins, 60, &m);
        days = div16(hrs, 24, &h);
    }
    if (o_p) {
        puts("seit ");
        unit(days, " Tag", " Tagen", &first);
        unit(h, " Stunde", " Stunden", &first);
        unit(m, " Minute", " Minuten", &first);
        if (first)
            puts("weniger als einer Minute");
        puts("\r\n");
        return;
    }
    DosGetDateTime(dt);
    if (o_s) {                          /* Start = jetzt - Laufzeit (Tage zaehlen nicht ueber Monatsgrenzen) */
        int sm = (int)((dt[1] >> 11) * 60 + ((dt[1] >> 5) & 63)) -
                 (int)(h * 60 + m);
        unsigned d = dt[0] & 31;
        while (sm < 0) { sm += 1440; d--; }
        put_num(1980 + (dt[0] >> 9), 0, 0);
        putc('-');
        two((dt[0] >> 5) & 15);
        putc('-');
        two(d - (unsigned)days);
        putc(' ');
        two((unsigned)(sm / 60));
        putc(':');
        two((unsigned)(sm % 60));
        puts("\r\n");
        return;
    }
    DosSleep(2000);
    c2 = sample(&n, &now);
    load = now > n1 ? div16(((c2 - c1) << 6) + ((c2 - c1) << 5) + ((c2 - c1) << 2),
                            (unsigned)(now - n1), 0) : 0;
    if (load > 100)
        load = 100;
    putc(' ');
    two(dt[1] >> 11);
    putc(':');
    two((dt[1] >> 5) & 63);
    putc(':');
    two((dt[1] & 31) * 2);
    puts(" l\x84uft seit ");
    if (days) {
        put_num(days, 0, 0);
        puts(days == 1 ? " Tag, " : " Tagen, ");
    }
    put_num(h, 0, 0);
    putc(':');
    two(m);
    puts(",  ");
    put_num(n, 0, 0);
    puts(n == 1 ? " Programm" : " Programme");
    puts(",  Last: ");
    put_num(load, 0, 0);
    puts("%\r\n");
}
