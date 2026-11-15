/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CAT.EXE - Dateien aneinanderhaengen und ausgeben (nach dem Vorbild von
 * GNU cat, nur Verhalten und Schalter):
 *
 *   CAT [-A -b -e -E -n -s -t -T -v] [DATEI...]
 *
 * Ohne Datei oder mit - die Standardeingabe. Platzhalter (*.TXT) werden
 * selbst aufgeloest. CR LF gilt als Zeilenende (-E setzt $ davor, -v zeigt
 * das CR nicht als ^M).
 */
#include "fapi.h"
#include "util.h"
#define TI_FILE
#include "textin.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], dirp[FAPI_PATHLEN];
static char full[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static int o_n, o_b, o_s, o_E, o_T, o_v, nfiles, rc;
static unsigned long line = 1;
static int bol = 1, blanks;             /* Zeilenanfang, Leerzeilen davor */

static void put_lnum(void)
{
    static const unsigned long p10[] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    char b[11];
    unsigned i, n = 0;
    unsigned long v = line++;
    for (i = 0; i < 10; i++) {
        unsigned d = 0;
        while (v >= p10[i]) {
            v -= p10[i];
            d++;
        }
        if (d || n || i == 9)
            b[n++] = (char)('0' + d);
    }
    for (i = n; i < 6; i++)
        to_put(' ');
    for (i = 0; i < n; i++)
        to_put(b[i]);
    to_put('\t');
}

static void put_vis(int c)
{
    if (c == '\t' && !o_T) {
        to_put('\t');
        return;
    }
    if (o_v || (c == '\t' && o_T)) {
        if (c >= 128) {
            to_str("M-");
            c -= 128;
        }
        if (c < 32) {
            to_put('^');
            c += 64;
        } else if (c == 127) {
            to_put('^');
            c = '?';
        }
    }
    to_put((char)c);
}

/* Zeilenende: line_end(crlf) */
static void line_end(int cr)
{
    if (o_E)
        to_put('$');
    if (cr)
        to_put('\r');
    to_put('\n');
}

static void cat(unsigned h)
{
    int c, cr = 0;
    if (!o_n && !o_b && !o_s && !o_E && !o_T && !o_v) {    /* schnell */
        unsigned n;
        to_flush();
        while ((n = DosRead(h, ti_buf, sizeof(ti_buf))) != 0 && n != FAPI_ERROR)
            DosWrite(STDOUT, ti_buf, n);
        return;
    }
    ti_open(h);
    for (;;) {
        c = ti_get();
        if (cr) {                       /* CR davor: Zeilenende?          */
            cr = 0;
            if (c == '\n')
                goto eol;
            if (bol) {
                if (o_b || o_n)
                    put_lnum();
                bol = 0;
                blanks = 0;
            }
            put_vis('\r');
        }
        if (c < 0)
            break;
        if (c == '\r') {
            cr = 1;
            continue;
        }
        if (c == '\n') {
            cr = 0;
            goto eol2;
        }
        if (bol) {
            if (o_b || o_n)
                put_lnum();
            bol = 0;
            blanks = 0;
        }
        put_vis(c);
        continue;
eol:
        cr = 1;
eol2:
        if (bol) {                      /* Leerzeile                      */
            if (o_s && blanks) {
                cr = 0;
                continue;
            }
            blanks = 1;
            if (o_n && !o_b)
                put_lnum();
        }
        line_end(cr);
        cr = 0;
        bol = 1;
    }
}

static void do_file(const char *a)
{
    static struct fapi_dirent e;
    unsigned h, n, found = 0;
    char *b;

    if (!(a[0] == '-' && !a[1]) && (has_wild(a))) {
        abs_path(a, dirp);
        b = base_name(dirp);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(dirp) > 3 && dirp[slen(dirp) - 1] == '\\')
            dirp[slen(dirp) - 1] = 0;
        for (n = 0; DosFindEntry(&e, n, dirp) != FAPI_ERROR; n++) {
            if ((e.attr & 0x18) || !match(e.name, pat))
                continue;
            join(full, dirp, e.name, sizeof(full));
            h = DosOpen(full);
            if (!FAPI_FAILED(h)) {
                cat(h);
                DosClose(h);
                found++;
            }
        }
        if (found)
            return;
    } else {
        h = ti_file(a);
        if (!FAPI_FAILED(h)) {
            cat(h);
            if (h != STDIN)
                DosClose(h);
            return;
        }
    }
    to_flush();
    puts("cat: ");
    puts(a);
    puts(": Datei nicht gefunden\r\n");
    rc = 1;
}

void main(void)
{
    char *a, *o;
    int pass;

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (!pass)
                puts("H\x84ngt Dateien aneinander und gibt sie aus (nach dem Vorbild von GNU cat).\r\n\r\n"
                     "CAT [-A -b -e -E -n -s -t -T -v] [Datei...]\r\n\r\n"
                     "  -n   Zeilen nummerieren       -b   nur nichtleere Zeilen\r\n"
                     "  -s   Leerzeilen zusammenfassen\r\n"
                     "  -E   $ am Zeilenende          -T   Tabulator als ^I\r\n"
                     "  -v   Steuerzeichen als ^X, M-  -A = -vET, -e = -vE, -t = -vT\r\n"
                     "Ohne Datei oder mit - die Standardeingabe; Platzhalter gehen.\r\n");
            return;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            if (pass)
                continue;
            for (o = a + 1; *o && *o != ' '; o++)
                switch (*o) {
                case 'A': o_v = o_E = o_T = 1; break;
                case 'b': o_b = 1; break;
                case 'e': o_v = o_E = 1; break;
                case 'E': o_E = 1; break;
                case 'n': o_n = 1; break;
                case 's': o_s = 1; break;
                case 't': o_v = o_T = 1; break;
                case 'T': o_T = 1; break;
                case 'u': break;        /* ungepuffert: wie GNU ohne Wirkung */
                case 'v': o_v = 1; break;
                default:
                    puts("cat: unbekannter Schalter - ");
                    puts(a);
                    puts("\r\n");
                    DosExit(1);
                }
            continue;
        }
        if (!pass) {
            nfiles++;
            continue;
        }
        word(a, name, sizeof(name));
        do_file(name);
    }
    if (!nfiles)
        cat(STDIN);
    to_flush();
    DosExit(rc);
}
