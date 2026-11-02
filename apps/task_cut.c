/*
 * CUT.EXE - Teile von Zeilen ausgeben, Verhalten nach dem Vorbild von
 * GNU cut (MS-DOS hatte keins; nur Bedienung nachgebaut, kein GNU-Code):
 *
 *   CUT -c LISTE [DATEI...]          Zeichen
 *   CUT -b LISTE [DATEI...]          Bytes (wie -c)
 *   CUT -f LISTE [-d X] [-s] [DATEI...]   Felder, Trennzeichen X (Tab)
 *       --complement                 alles ausser der Liste
 *       --output-delimiter=TEXT      Trenner bei der Ausgabe von Feldern
 *
 * LISTE: N, N-M, N-, -M, durch Kommas getrennt (1 ist das erste). Ohne
 * Datei oder mit - liest CUT die Standardeingabe (Pipe). Felder kommen
 * in der Reihenfolge der Zeile. Zeilen ohne Trennzeichen gibt -f ganz
 * aus, mit -s gar nicht.
 * Rueckgabe: 0 ok, 1 Fehler.
 */
#include "fapi.h"
#include "util.h"

#define LMAX    4096
#define NRANGE  32

static char args[FAPI_ARGLEN], line[LMAX + 2], outd[16], fname[FAPI_PATHLEN];
static unsigned char ib[2048], ob[2048];
static unsigned lo[NRANGE], hi[NRANGE], nr, ipos, ilen, opos;
static int mode, o_s, o_comp, rc;       /* mode: 'c' oder 'f'           */
static char delim = '\t';

static void flush_out(void)
{
    if (opos)
        DosWrite(STDOUT, ob, opos);
    opos = 0;
}

static void out(char c)
{
    ob[opos++] = (unsigned char)c;
    if (opos == sizeof(ob))
        flush_out();
}

static int sel(unsigned n)              /* n ab 1 */
{
    unsigned i;
    for (i = 0; i < nr; i++)
        if (n >= lo[i] && n <= hi[i])
            return !o_comp;
    return o_comp;
}

/* "1,3-5,7-" -> Bereiche; 0 = Fehler */
static int parse_list(const char *s)
{
    unsigned a, b;
    if (!*s)
        return 0;
    while (*s && nr < NRANGE) {
        a = 0;
        b = 0;
        if (*s == '-') {
            a = 1;
            s++;
        } else {
            if (*s < '0' || *s > '9')
                return 0;
            while (*s >= '0' && *s <= '9')
                a = a * 10 + (unsigned)(*s++ - '0');
            if (a == 0)
                return 0;               /* Zaehlung beginnt bei 1       */
            if (*s != '-') {
                b = a;
                goto add;
            }
            s++;
        }
        if (*s >= '0' && *s <= '9')
            while (*s >= '0' && *s <= '9')
                b = b * 10 + (unsigned)(*s++ - '0');
        else
            b = 0xFFFF;                 /* offen bis Zeilenende         */
        if (b < a)
            return 0;
add:
        lo[nr] = a;
        hi[nr++] = b;
        if (*s == ',')
            s++;
        else if (*s && *s != ' ')
            return 0;
        else
            break;
    }
    return 1;
}

static void do_line(unsigned n, int nl, int cr)
{
    unsigned i, f, k;
    int first = 1;

    if (mode == 'c') {
        for (i = 0; i < n; i++)
            if (sel(i + 1))
                out(line[i]);
    } else {
        for (i = 0; i < n && line[i] != delim; i++)
            ;
        if (i == n) {                   /* kein Trennzeichen             */
            if (o_s)
                return;
            for (i = 0; i < n; i++)
                out(line[i]);
        } else {
            for (i = 0, f = 1; ; f++) {   /* Feld f: [k, i)          */
                k = i;
                while (i < n && line[i] != delim)
                    i++;
                if (sel(f)) {
                    if (!first) {
                        if (outd[0]) {
                            unsigned j;
                            for (j = 0; outd[j]; j++)
                                out(outd[j]);
                        } else
                            out(delim);
                    }
                    for (; k < i; k++)
                        out(line[k]);
                    first = 0;
                }
                if (i >= n)
                    break;
                i++;                    /* Trennzeichen                  */
            }
        }
    }
    if (nl) {
        if (cr)
            out('\r');
        out('\n');
    }
}

static int getb(unsigned h)
{
    if (ipos >= ilen) {
        ilen = DosRead(h, ib, sizeof(ib));
        if (ilen == FAPI_ERROR || ilen == 0) {
            ilen = 0;
            return -1;
        }
        ipos = 0;
    }
    return ib[ipos++];
}

static void do_file(unsigned h)
{
    unsigned n = 0;
    int c, cr;
    ipos = ilen = 0;
    for (;;) {
        c = getb(h);
        if (c < 0 || c == 0x1A) {       /* Ende (auch ^Z wie bei DOS)   */
            if (n)
                do_line(n, 0, 0);
            return;
        }
        if (c == '\n') {
            cr = n && line[n - 1] == '\r';
            do_line(cr ? n - 1 : n, 1, cr);
            n = 0;
        } else if (n < LMAX)
            line[n++] = (char)c;        /* laengere Zeilen werden gekuerzt */
    }
}

static int pfx_is(const char *a, const char *p)    /* ohne Gross/klein */
{
    while (*p && up(*a) == *p)
        a++, p++;
    return *p == 0;
}

static void usage(void)
{
    puts("Gibt Teile jeder Zeile aus (nach dem Vorbild von GNU cut).\r\n\r\n"
         "CUT -c LISTE [Datei...]\r\n"
         "CUT -f LISTE [-d Zeichen] [-s] [Datei...]\r\n\r\n"
         "  -c LISTE   Zeichen (auch -b)       -f LISTE   Felder\r\n"
         "  -d Z       Trennzeichen (Tab)      -s         nur Zeilen mit Trennzeichen\r\n"
         "  --complement               alles au\xE1" "er der Liste\r\n"
         "  --output-delimiter=TEXT    Trenner f\x81r die Ausgabe von Feldern\r\n"
         "LISTE: N, N-M, N-, -M mit Kommas, z. B. 1,3-5. Ohne Datei: Standardeingabe.\r\n"
         "Beispiel: DIR | CUT -c1-12\r\n");
}

static void bad(const char *what)
{
    puts("CUT: ");
    puts(what);
    puts("\r\nCUT /? zeigt die Hilfe.\r\n");
    DosExit(1);
}

void main(void)
{
    char *a, *v;
    int files = 0, pass;
    unsigned h;

    DosGetArgs(args);
    /* zwei Durchgaenge: erst Schalter, dann Dateien */
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if ((a[0] == '/' && a[1] == '?') || (a[0] == '-' && a[1] == '-' && same(a, "--HELP"))) {
            if (!pass)
                usage();
            return;
        }
        if (a[0] == '-' && a[1] == '-' && a[2]) {
            if (pass)
                continue;
            if (same(a, "--COMPLEMENT"))
                o_comp = 1;
            else if (pfx_is(a, "--OUTPUT-DELIMITER=")) {
                for (v = a; *v && *v != '='; v++)
                    ;
                if (*v == '=') {
                    word(v + 1, outd, sizeof(outd));
                } else
                    bad("--output-delimiter=TEXT erwartet");
            } else
                bad("unbekannter Schalter");
            continue;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            char o = a[1];
            if (pass) {                 /* Werte hinter -c/-f/-d ueberspringen */
                if ((o == 'c' || o == 'b' || o == 'f' || o == 'd' ||
                     o == 'C' || o == 'B' || o == 'F' || o == 'D') && (!a[2] || a[2] == ' '))
                    a = next_word(a);
                continue;
            }
            v = a[2] && a[2] != ' ' ? a + 2 : skip(next_word(a));
            switch (o) {
            case 'c': case 'C': case 'b': case 'B':
            case 'f': case 'F':
                if (mode)
                    bad("nur eine der Arten -c, -b, -f");
                mode = (o == 'f' || o == 'F') ? 'f' : 'c';
                if (!parse_list(v))
                    bad("ung\x81ltige Liste");
                if (v != a + 2)
                    a = next_word(a);
                break;
            case 'd': case 'D':
                if (v[0] == '"' && v[1] && v[2] == '"')
                    delim = v[1];
                else if (v[0])
                    delim = v[0];
                else
                    bad("-d braucht ein Zeichen");
                if (v != a + 2)
                    a = next_word(a);
                break;
            case 's': case 'S':
                o_s = 1;
                break;
            case 'n': case 'N':
                break;                  /* wie GNU: ohne Wirkung        */
            default:
                bad("unbekannter Schalter");
            }
            continue;
        }
        if (!pass)
            continue;
        if (!mode)
            bad("eine der Arten -c, -b oder -f angeben");
        files = 1;
        if (a[0] == '-' && (!a[1] || a[1] == ' ')) {
            do_file(STDIN);
            continue;
        }
        word(a, fname, sizeof(fname));
        h = DosOpen(fname);
        if (FAPI_FAILED(h)) {
            flush_out();
            puts("CUT: Datei nicht gefunden - ");
            puts(fname);
            puts("\r\n");
            rc = 1;
            continue;
        }
        do_file(h);
        DosClose(h);
    }
    if (!mode)
        bad("eine der Arten -c, -b oder -f angeben");
    if (!files)
        do_file(STDIN);
    flush_out();
    DosExit(rc);
}
