/*
 * UNIQ.EXE - gleiche aufeinanderfolgende Zeilen zusammenfassen (nach dem
 * Vorbild von GNU uniq; meist hinter SORT):
 *
 *   UNIQ [-c] [-d | -u] [-i] [EINGABE [AUSGABE]]
 *
 *   -c  Anzahl voranstellen   -d  nur doppelte   -u  nur einmalige
 *   -i  Gross/klein egal
 * Zeilen bis 4 KB (laengere werden gekuerzt verglichen und ausgegeben).
 */
#include "fapi.h"
#include "util.h"
#include "textin.h"

#define LMAX 4096

static char args[FAPI_ARGLEN], inname[FAPI_PATHLEN], outname[FAPI_PATHLEN];
static char la[LMAX + 1], lb[LMAX + 1];
static int o_c, o_d, o_u, o_i;
static unsigned hout = STDOUT;

static void wout(const char *s, unsigned n)
{
    if (hout == STDOUT) {
        unsigned i;
        for (i = 0; i < n; i++)
            to_put(s[i]);
    } else
        DosWrite(hout, s, n);
}

/* Zeile lesen ohne CR LF; -> Laenge, -1 = Ende */
static int rd(char *b)
{
    int c, n = 0;
    while ((c = ti_get()) >= 0 && c != '\n')
        if (c != '\r' && n < LMAX)
            b[n++] = (char)c;
    b[n] = 0;
    return (c < 0 && n == 0) ? -1 : n;
}

static int eq(const char *a, const char *b)
{
    if (o_i) {
        while (*a && up(*a) == up(*b))
            a++, b++;
        return up(*a) == up(*b);
    }
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void emit(const char *l, unsigned long n)
{
    static char nb[12];
    static const unsigned long pw[7] = { 1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    unsigned i, k = 0, d, any = 0;
    if ((o_d && n < 2) || (o_u && n > 1))
        return;
    if (o_c) {
        for (i = 0; i < 7; i++) {
            for (d = 0; n >= pw[i]; d++)
                n -= pw[i];
            nb[k++] = (d || any || i == 6) ? (char)('0' + d) : ' ';
            if (d)
                any = 1;
        }
        nb[k++] = ' ';
        wout(nb, k);
    }
    wout(l, slen(l));
    wout("\r\n", 2);
}

void main(void)
{
    char *a, *cur = la, *nxt = lb, *t;
    unsigned long n;
    unsigned h = STDIN;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Fasst gleiche aufeinanderfolgende Zeilen zusammen (nach GNU uniq).\r\n\r\n"
                 "UNIQ [-c] [-d | -u] [-i] [Eingabe [Ausgabe]]\r\n\r\n"
                 "  -c Anzahl voranstellen  -d nur doppelte  -u nur einmalige\r\n"
                 "  -i Gro\xE1/klein egal      Beispiel: SORT < LISTE.TXT | UNIQ -c\r\n");
            return;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            char *o;
            for (o = a + 1; *o && *o != ' '; o++)
                switch (*o) {
                case 'c': o_c = 1; break;
                case 'd': o_d = 1; break;
                case 'u': o_u = 1; break;
                case 'i': o_i = 1; break;
                default:
                    puts("UNIQ: unbekannter Schalter - ");
                    puts(a);
                    puts("\r\n");
                    DosExit(1);
                }
        } else if (!inname[0])
            word(a, inname, sizeof(inname));
        else
            word(a, outname, sizeof(outname));
    }
    if (inname[0] && !(inname[0] == '-' && !inname[1])) {
        h = DosOpen(inname);
        if (FAPI_FAILED(h)) {
            puts("UNIQ: Datei nicht gefunden - ");
            puts(inname);
            puts("\r\n");
            DosExit(1);
        }
    }
    if (outname[0]) {
        hout = DosCreate(outname);
        if (FAPI_FAILED(hout)) {
            puts("UNIQ: Ausgabe kann nicht angelegt werden - ");
            puts(outname);
            puts("\r\n");
            DosExit(1);
        }
    }
    ti_open(h);
    if (rd(cur) >= 0) {
        n = 1;
        while (rd(nxt) >= 0) {
            if (eq(cur, nxt)) {
                n++;
                continue;
            }
            emit(cur, n);
            t = cur;
            cur = nxt;
            nxt = t;
            n = 1;
        }
        emit(cur, n);
    }
    to_flush();
    if (hout != STDOUT)
        DosClose(hout);
    if (h != STDIN)
        DosClose(h);
}
