/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SORT.EXE - Zeilen sortieren (wie MS-DOS):
 *
 *   SORT [/R] [/+n] [< Eingabe] [> Ausgabe]      Befehl | SORT
 *
 * Ohne Unterscheidung von Gross/klein (Umlaute wie ihre Grossbuchstaben),
 * /R absteigend, /+n vergleicht ab Spalte n. Bis 63 KB Eingabe.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define MAXLINES 8000

static char args[FAPI_ARGLEN], in[XFER];
static unsigned dseg, lseg, nlines, col;
static int rev;

static char __far *line(unsigned i)
{
    unsigned __far *t = MK_FP(lseg, 0);
    return MK_FP(dseg, t[i]);
}

static int cmp(char __far *a, char __far *b)
{
    unsigned i;
    for (i = 0; i < col && *a; i++)
        a++;
    for (i = 0; i < col && *b; i++)
        b++;
    while (*a && up(*a) == up(*b))
        a++, b++;
    return (int)(unsigned char)up(*a) - (int)(unsigned char)up(*b);
}

void main(void)
{
    char __far *d;
    unsigned __far *t;
    unsigned n, i, k, gap, used = 0, start = 0, x;
    char *a;
    int c;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && up(a[1]) == 'R')
            rev = 1;
        else if (a[0] == '/' && a[1] == '+') {
            for (col = 0, a += 2; *a >= '0' && *a <= '9'; a++)
                col = col * 10 + (unsigned)(*a - '0');
            if (col)
                col--;
        } else if (a[0] == '/' && a[1] == '?') {
            puts("Sortiert die Eingabe zeilenweise.\r\n\r\n"
                 "SORT [/R] [/+n] [< Eingabe] [> Ausgabe]\r\n\r\n"
                 "  /R   absteigend      /+n  ab Spalte n vergleichen\r\n");
            return;
        }
    }
    dseg = DosAllocSeg(0);
    lseg = DosAllocSeg(MAXLINES * 2);
    if (FAPI_FAILED(dseg) || FAPI_FAILED(lseg)) {
        puts("SORT: Nicht genug Arbeitsspeicher\r\n");
        DosExit(1);
    }
    d = MK_FP(dseg, 0);
    t = MK_FP(lseg, 0);
    while ((n = DosRead(STDIN, in, sizeof(in))) != 0 && !FAPI_FAILED(n)) {
        for (i = 0; i < n; i++) {
            c = in[i];
            if (c == '\r' || c == 0x1A)
                continue;
            if (used >= 0xFFF0 || nlines >= MAXLINES) {
                puts("SORT: Eingabe zu gro\xE1\r\n");
                DosExit(1);
            }
            if (c == '\n') {
                d[used++] = 0;
                t[nlines++] = start;
                start = used;
            } else
                d[used++] = (char)c;
        }
    }
    if (used > start) {                 /* letzte Zeile ohne Zeilenende */
        d[used++] = 0;
        t[nlines++] = start;
    }
    for (gap = nlines / 2; gap; gap /= 2)   /* Shellsort               */
        for (i = gap; i < nlines; i++)
            for (k = i; k >= gap; k -= gap) {
                c = cmp(line(k - gap), line(k));
                if (rev ? c >= 0 : c <= 0)
                    break;
                x = t[k];
                t[k] = t[k - gap];
                t[k - gap] = x;
            }
    for (i = 0; i < nlines; i++) {
        char __far *s = line(i);
        for (n = 0; s[n]; n++)
            ;
        for (k = 0; k < n; k += sizeof(in) - 2) {
            unsigned m = n - k > sizeof(in) - 2 ? sizeof(in) - 2 : n - k, j;
            for (j = 0; j < m; j++)
                in[j] = s[k + j];
            DosWrite(STDOUT, in, m);
        }
        DosWrite(STDOUT, "\r\n", 2);
    }
}
