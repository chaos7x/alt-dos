/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * STRINGS.EXE - lesbare Zeichenketten in Dateien finden (nach dem Vorbild
 * von GNU strings, nur Verhalten und Schalter):
 *
 *   STRINGS [-a] [-f] [-n N | -N] [-t d|o|x] [-o] [-w] [-e s|S|l|b] [DATEI...]
 *
 * Wie GNU strings ohne Objektformat: immer die ganze Datei. Eine Kette
 * sind mindestens N (Vorgabe 4) druckbare Zeichen hintereinander (ASCII
 * 20h-7Eh und Tabulator; -w auch CR/LF/FF/VT; -e S auch Bytes ab 80h;
 * -e l/b 16-Bit-Zeichen little/big endian, das andere Byte 0). -t gibt
 * die Stelle des Anfangs aus (dezimal, oktal, hex), -f den Dateinamen.
 * Ohne Datei oder mit - die Standardeingabe, Platzhalter gehen.
 */
#include "fapi.h"
#include "util.h"
#define TI_FILE
#define TI_NUM
#include "textin.h"

#define SMAX 1024                       /* hoechste Mindestlaenge         */

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], dirp[FAPI_PATHLEN];
static char full[FAPI_PATHLEN], pat[FAPI_PATHLEN], sbuf[SMAX];
static unsigned min_len = 4, o_f, o_w, nfiles, rc;
static char o_t, enc = 's';             /* Stellenart, Zeichenbreite     */

static void put_off(unsigned long v)
{
    static const unsigned long p10[] = { 1000000000UL, 100000000UL, 10000000UL,
        1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    char b[12];
    unsigned i, n = 0, d;

    if (o_t == 'd') {
        for (i = 0; i < 10; i++) {
            d = 0;
            while (v >= p10[i]) {
                v -= p10[i];
                d++;
            }
            if (d || n || i == 9)
                b[n++] = (char)('0' + d);
        }
    } else {
        unsigned sh = o_t == 'x' ? 4 : 3, top = o_t == 'x' ? 28 : 30;
        for (i = top + sh; i; ) {
            i -= sh;
            d = (unsigned)(v >> i) & (o_t == 'x' ? 15 : 7);
            if (d || n || !i)
                b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        }
    }
    for (i = n; i < 7; i++)             /* wie GNU: %7lu und Leerzeichen  */
        to_put(' ');
    for (i = 0; i < n; i++)
        to_put(b[i]);
    to_put(' ');
}

static int printable(int c)
{
    if (c >= 0x20 && c < 0x7F)
        return 1;
    if (c == '\t')
        return 1;
    if (o_w && (c == '\r' || c == '\n' || c == '\f' || c == '\v'))
        return 1;
    return enc == 'S' && c >= 0x80;
}

/* naechstes Zeichen nach Kodierung; -1 Ende, -2 nicht druckbar.
   Wie GNU: ein ungueltiges 16-Bit-Zeichen gibt sein zweites Byte zurueck
   (weiter geht es ein Byte spaeter, nicht zwei) - *used = gelesene Bytes */
static int back = -1;

static int get_b(void)
{
    int c = back;
    if (c >= 0) {
        back = -1;
        return c;
    }
    return ti_get();
}

static int next_char(unsigned *used)
{
    int a, b, lo, hi;

    *used = 1;
    a = get_b();
    if (a < 0)
        return -1;
    if (enc == 'l' || enc == 'b') {
        b = get_b();
        if (b < 0)
            return -1;
        lo = enc == 'l' ? a : b;        /* -e b: hohes Byte zuerst       */
        hi = enc == 'l' ? b : a;
        if (hi != 0 || !printable(lo)) {
            back = b;
            return -2;
        }
        *used = 2;
        return lo;
    }
    return printable(a) ? a : -2;
}

static void scan(unsigned h, const char *fname)
{
    unsigned long pos = 0, start = 0;
    unsigned n = 0, used, i;
    int c;

    ti_open(h);
    back = -1;
    for (;;) {
        c = next_char(&used);
        if (c >= 0) {
            if (n < min_len) {
                if (!n)
                    start = pos;
                sbuf[n++] = (char)c;
                if (n == min_len) {     /* lang genug: Kopf, bisheriges   */
                    if (o_f) {
                        to_str(fname);
                        to_str(": ");
                    }
                    if (o_t)
                        put_off(start);
                    for (i = 0; i < n; i++)
                        to_put(sbuf[i]);
                }
            } else
                to_put((char)c);
        } else {
            if (n >= min_len) {
                to_put('\r');
                to_put('\n');
            }
            n = 0;
            if (c == -1)
                break;
        }
        pos += used;
    }
}

static void do_file(const char *a)
{
    static struct fapi_dirent e;
    unsigned h, n, found = 0;
    char *b;

    if (!(a[0] == '-' && !a[1]) && has_wild(a)) {
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
                scan(h, full);
                DosClose(h);
                found++;
            }
        }
        if (found)
            return;
    } else {
        h = ti_file(a);
        if (!FAPI_FAILED(h)) {
            scan(h, a[0] == '-' && !a[1] ? "{Standardeingabe}" : a);
            if (h != STDIN)
                DosClose(h);
            return;
        }
    }
    to_flush();
    puts("strings: ");
    puts(a);
    puts(": Datei nicht gefunden\r\n");
    rc = 1;
}

/* Vorsilbe p? -> Rest dahinter, sonst 0 (Gross/klein zaehlt wie GNU) */
static char *pre(char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return s;
}

static void bad(const char *a)
{
    puts("strings: ung\x81ltiger Schalter - ");
    puts(a);
    puts("\r\n");
    DosExit(1);
}

/* Zahl hinter dem Schalter: direkt dahinter oder naechstes Wort */
static char *arg_of(char *o, char **a)
{
    if (o[1] && o[1] != ' ')
        return o + 1;
    *a = next_word(*a);
    if (!**a)
        bad("(Wert fehlt)");
    return *a;
}

static void set_min(const char *s, char *a)
{
    int ok;
    unsigned long v = ti_num(s, &ok);
    if (!ok || !v || v > SMAX)
        bad(a);
    min_len = (unsigned)v;
}

void main(void)
{
    char *a, *o, *v;
    int pass;

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (!pass)
                puts("Findet lesbare Zeichenketten in Dateien (nach dem Vorbild von GNU strings).\r\n\r\n"
                     "STRINGS [-a] [-f] [-n N] [-t d|o|x] [-o] [-w] [-e s|S|l|b] [Datei...]\r\n\r\n"
                     "  -n N, -N  Mindestl\x84nge (Vorgabe 4)\r\n"
                     "  -t d|o|x  Stelle in der Datei dezimal/oktal/hex davor, -o = -t o\r\n"
                     "  -f        Dateinamen davor\r\n"
                     "  -w        auch CR, LF, FF und VT geh\x94ren zur Kette\r\n"
                     "  -e s      7-Bit-ASCII (Vorgabe), S auch Bytes ab 80h,\r\n"
                     "            l / b 16-Bit-Zeichen little / big endian (Unicode)\r\n"
                     "  -a        ganze Datei (immer so)\r\n"
                     "Ohne Datei oder mit - die Standardeingabe; Platzhalter gehen.\r\n");
            return;
        }
        if (a[0] == '-' && a[1] == '-' && a[2] != ' ' && a[2]) {
            if (pass)
                continue;                       /* lange Schalter wie GNU */
            word(a, name, sizeof(name));
            if ((v = pre(name, "--all")) != 0 && !*v)
                ;
            else if ((v = pre(name, "--data")) != 0 && !*v)
                ;
            else if ((v = pre(name, "--print-file-name")) != 0 && !*v)
                o_f = 1;
            else if ((v = pre(name, "--include-all-whitespace")) != 0 && !*v)
                o_w = 1;
            else if ((v = pre(name, "--bytes=")) != 0)
                set_min(v, a);
            else if ((v = pre(name, "--radix=")) != 0 && v[0] && !v[1]
                     && (*v == 'd' || *v == 'o' || *v == 'x'))
                o_t = *v;
            else if ((v = pre(name, "--encoding=")) != 0 && v[0] && !v[1]
                     && (*v == 's' || *v == 'S' || *v == 'l' || *v == 'b'))
                enc = *v;
            else
                bad(name);
            continue;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            if (a[1] >= '0' && a[1] <= '9') {   /* -8 = -n 8             */
                if (!pass) {
                    word(a + 1, name, sizeof(name));
                    set_min(name, a);
                }
                continue;
            }
            for (o = a + 1; *o && *o != ' '; o++)
                switch (*o) {
                case 'a': case 'd': break;      /* ganze Datei: immer   */
                case 'f': o_f = 1; break;
                case 'w': o_w = 1; break;
                case 'o': o_t = 'o'; break;
                case 'n':
                    v = arg_of(o, &a);
                    if (!pass) {
                        word(v, name, sizeof(name));
                        set_min(name, a);
                    }
                    goto next;
                case 't':
                    v = arg_of(o, &a);
                    if ((*v != 'd' && *v != 'o' && *v != 'x') || (v[1] && v[1] != ' '))
                        bad(a);
                    o_t = *v;
                    goto next;
                case 'e':
                    v = arg_of(o, &a);
                    if ((*v != 's' && *v != 'S' && *v != 'l' && *v != 'b') || (v[1] && v[1] != ' '))
                        bad(a);
                    enc = *v;
                    goto next;
                default:
                    if (!pass)
                        bad(a);
                }
next:
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
        scan(STDIN, "{Standardeingabe}");
    to_flush();
    DosExit(rc);
}
