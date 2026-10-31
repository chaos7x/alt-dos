/*
 * FIND.EXE - Text in Dateien suchen, wie MS-DOS 6:
 *   FIND [/V] [/C] [/N] [/I] "Text" [Datei ...]
 * /V Zeilen ohne den Text, /C nur zaehlen, /N Zeilennummern, /I Gross-/
 * Kleinschreibung egal. Ohne Datei liest FIND die Standardeingabe.
 * Mehr kann GREP (regulaere Ausdruecke).
 */
#include "fapi.h"

static char args[FAPI_ARGLEN], pat[FAPI_ARGLEN], line[512], in[512];
static int opt_v, opt_c, opt_n, opt_i;
static unsigned plen;

static char low(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int contains(const char *s, unsigned len)
{
    unsigned i, k;
    if (plen == 0)
        return 1;
    for (i = 0; i + plen <= len; i++) {
        for (k = 0; k < plen; k++)
            if (opt_i ? low(s[i + k]) != low(pat[k]) : s[i + k] != pat[k])
                break;
        if (k == plen)
            return 1;
    }
    return 0;
}

static void put_dec(unsigned long v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        unsigned d = 0;
        while (v >= 10) { v -= 10; d++; }       /* ohne Langdivision    */
        b[--i] = (char)('0' + (unsigned)v);
        v = d;
    } while (v);
    puts(b + i);
}

static unsigned long found;              /* Treffer aller Dateien     */
static int had_err;

static void search(unsigned h)
{
    unsigned n, i, ll = 0;
    unsigned long lineno = 0, count = 0;
    int hit;

    for (;;) {
        n = DosRead(h, in, sizeof(in));
        if (n == FAPI_ERROR)
            n = 0;
        for (i = 0; i <= n; i++) {
            if (i < n && in[i] != '\n') {
                if (in[i] != '\r' && ll < sizeof(line) - 1)
                    line[ll++] = in[i];
                continue;
            }
            if (i == n && (n != 0 || ll == 0))
                break;                          /* Zeile geht weiter    */
            lineno++;
            hit = contains(line, ll) != opt_v;
            if (hit) {
                count++;
                found++;
                if (!opt_c) {
                    if (opt_n) {
                        puts("[");
                        put_dec(lineno);
                        puts("]");
                    }
                    line[ll] = 0;
                    puts(line);
                    puts("\r\n");
                }
            }
            ll = 0;
        }
        if (n == 0)
            break;
    }
    if (opt_c) {
        put_dec(count);
        puts("\r\n");
    }
}

void main(void)
{
    char *a = args, *f;
    unsigned h;
    int files = 0;

    DosGetArgs(args);
    for (;;) {
        while (*a == ' ')
            a++;
        if (a[0] != '/')
            break;
        switch (low(a[1])) {
        case 'v': opt_v = 1; break;
        case 'c': opt_c = 1; break;
        case 'n': opt_n = 1; break;
        case 'i': opt_i = 1; break;
        default:
            puts("FIND: Ung\x81ltiger Schalter\r\n");
            DosExit(2);
        }
        a += 2;
    }
    if (*a != '"') {
        puts("Syntax: FIND [/V] [/C] [/N] [/I] \"Text\" [Datei ...]\r\n");
        DosExit(2);
    }
    for (a++; *a && *a != '"'; a++)
        pat[plen++] = *a;
    if (*a == '"')
        a++;
    for (;;) {
        while (*a == ' ')
            a++;
        if (!*a)
            break;
        for (f = a; *a && *a != ' '; a++)
            ;
        if (*a)
            *a++ = 0;
        files++;
        h = DosOpen(f);
        if (h == FAPI_ERROR) {
            had_err = 1;
            puts("FIND: Datei nicht gefunden - ");
            puts(f);
            puts("\r\n");
            continue;
        }
        puts("\r\n---------- ");
        puts(f);
        puts(opt_c ? ": " : "\r\n");
        search(h);
        DosClose(h);
    }
    if (!files)
        search(STDIN);
    DosExit(had_err ? 2 : found ? 0 : 1);   /* wie MS-DOS 6 FIND      */
}
