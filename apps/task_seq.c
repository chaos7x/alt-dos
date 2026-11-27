/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SEQ.EXE - Zahlenfolgen ausgeben, wie seq (GNU), ganze Zahlen:
 *   SEQ [-w] [-s Trenner] [Anfang [Schritt]] Ende
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], sep[16] = "\r\n";

static long lnum(const char *s, int *ok)
{
    long v = 0;
    int neg = 0;
    *ok = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        v = (v << 3) + (v << 1) + (*s++ - '0');
        *ok = 1;
    }
    if (*s)
        *ok = 0;
    return neg ? -v : v;
}

static unsigned digits(long v)
{
    unsigned n = v < 0 ? 2 : 1;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    while (u >= 10) {
        unsigned long q = 0, r = 0;
        int i;
        for (i = 31; i >= 0; i--) {
            r = (r << 1) | ((u >> i) & 1);
            if (r >= 10) { r -= 10; q |= 1UL << i; }
        }
        u = q;
        n++;
    }
    return n;
}

static void out(long v, unsigned w)
{
    char b[12];
    int n = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    do {
        unsigned long q = 0, r = 0;
        int i;
        for (i = 31; i >= 0; i--) {
            r = (r << 1) | ((u >> i) & 1);
            if (r >= 10) { r -= 10; q |= 1UL << i; }
        }
        b[n++] = (char)('0' + (unsigned)r);
        u = q;
    } while (u);
    if (v < 0) { putc('-'); if (w) w--; }
    while (w > (unsigned)n) { putc('0'); w--; }
    while (n)
        putc(b[--n]);
}

void main(void)
{
    char *a, w[32];
    long v[3], first = 1, step = 1, last, i;
    int nv = 0, ok, eq = 0, want = 0, firstout = 1;
    unsigned wd = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want) {
            scopy(sep, w, sizeof(sep));
            want = 0;
            continue;
        }
        if (w[0] == '/' && w[1] == '?') {
            puts("Gibt eine Zahlenfolge aus.\r\n\r\n"
                 "SEQ [-w] [-s Trenner] [Anfang [Schritt]] Ende\r\n\r\n"
                 "  -w  mit f\x81hrenden Nullen auf gleiche Breite\r\n"
                 "  -s  Trenner statt Zeilenende\r\n");
            return;
        }
        if (w[0] == '-' && w[1] == 'w' && !w[2]) { eq = 1; continue; }
        if (w[0] == '-' && w[1] == 's' && !w[2]) { want = 1; continue; }
        if (nv < 3) {
            v[nv] = lnum(w, &ok);
            if (!ok) {
                puts("seq: keine ganze Zahl: ");
                puts(w);
                puts("\r\n");
                DosExit(1);
            }
            nv++;
        }
    }
    if (!nv) {
        puts("seq: Operand fehlt (SEQ /? zeigt die Bedienung)\r\n");
        DosExit(1);
    }
    if (nv == 1) last = v[0];
    else if (nv == 2) { first = v[0]; last = v[1]; }
    else { first = v[0]; step = v[1]; last = v[2]; }
    if (!step)
        DosExit(1);
    if (eq) {
        unsigned a1 = digits(first), a2 = digits(last);
        wd = a1 > a2 ? a1 : a2;
    }
    for (i = first; step > 0 ? i <= last : i >= last; i += step) {
        if (!firstout)
            puts(sep);
        out(i, wd);
        firstout = 0;
    }
    if (!firstout)
        puts("\r\n");
}
