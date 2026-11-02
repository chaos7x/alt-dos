/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * COMP.EXE - Dateien Byte fuer Byte vergleichen (wie MS-DOS):
 *
 *   COMP [Daten1] [Daten2] [/D] [/A] [/L] [/N=Anzahl] [/C]
 *
 * Daten1 darf Platzhalter enthalten, Daten2 ein Verzeichnis oder ein
 * Muster wie *.BAK sein. Nach 10 Unterschieden bricht der Vergleich einer
 * Datei ab. Ohne Angaben fragt COMP nach den Namen.
 *   /D dezimal  /A Zeichen  /L Zeilennummer  /N=n nur die ersten n Zeilen
 *   /C Gross/klein egal
 * Rueckgabe: 0 alle gleich, 1 Unterschiede, 2 Fehler.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], spec1[FAPI_PATHLEN], spec2[FAPI_PATHLEN];
static char dir1[FAPI_PATHLEN], pat1[FAPI_PATHLEN], f1[FAPI_PATHLEN], f2[FAPI_PATHLEN];
static unsigned char b1[2048], b2[2048];
static int o_dec, o_asc, o_line, o_case, rc;
static unsigned long o_lines;           /* /N=, 0 = alles               */

static void hexn(unsigned long v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--)
        putc(hx[(unsigned)(v >> (4 * i)) & 15]);
}

static void show_byte(unsigned c)
{
    if (o_asc) {
        putc(c >= 32 ? (char)c : '.');
    } else if (o_dec)
        put_num(c, 0, 0);
    else
        hexn(c, 2);
}

/* Muster 2 auf den Namen aus Datei 1 anwenden: *.BAK, NAME.*, ?-Zeichen */
static void part(const char *n, unsigned nl, const char *p, unsigned pl, char *o, unsigned *k)
{
    unsigned i;
    if (pl == 1 && p[0] == '*') {
        for (i = 0; i < nl; i++)
            o[(*k)++] = n[i];
        return;
    }
    for (i = 0; i < pl && p[i] != '*'; i++)
        o[(*k)++] = p[i] == '?' ? (i < nl ? n[i] : 0) : p[i];
    if (i < pl)                         /* "AB*": Rest des Namens        */
        for (; i < nl; i++)
            o[(*k)++] = n[i];
}

static void apply_pat(const char *name, const char *pat, char *o)
{
    unsigned nd = slen(name), pd = slen(pat), nl = nd, pl = pd, k = 0, i;
    for (i = 0; name[i]; i++)
        if (name[i] == '.')
            nd = i;
    for (i = 0; pat[i]; i++)
        if (pat[i] == '.')
            pd = i;
    part(name, nd, pat, pd, o, &k);
    if (pd < pl) {
        o[k++] = '.';
        part(nd < nl ? name + nd + 1 : "", nd < nl ? nl - nd - 1 : 0, pat + pd + 1, pl - pd - 1, o, &k);
    }
    o[k] = 0;
    for (i = 0, nd = 0; i <= k; i++)    /* ?-Luecken entfernen           */
        if (o[i] || i == k)
            o[nd++] = o[i];
}

static void compare(const char *a, const char *b)
{
    static struct fapi_dirent e;
    unsigned h1, h2, n1, n2, i, nd = 0, sizediff = 0;
    unsigned long off = 0, line = 1;
    unsigned char c1, c2;

    puts("Vergleiche ");
    puts(a);
    puts(" und ");
    puts(b);
    puts(" ...\r\n");
    h1 = DosOpen(a);
    if (FAPI_FAILED(h1)) {
        puts("Datei nicht gefunden - ");
        puts(a);
        puts("\r\n");
        rc = 2;
        return;
    }
    h2 = DosOpen(b);
    if (FAPI_FAILED(h2)) {
        DosClose(h1);
        puts("Datei nicht gefunden - ");
        puts(b);
        puts("\r\n");
        rc = 2;
        return;
    }
    (void)e;
    for (;;) {
        n1 = DosRead(h1, b1, sizeof(b1));
        n2 = DosRead(h2, b2, sizeof(b2));
        if (n1 == FAPI_ERROR)
            n1 = 0;
        if (n2 == FAPI_ERROR)
            n2 = 0;
        for (i = 0; i < n1 && i < n2; i++, off++) {
            c1 = b1[i];
            c2 = b2[i];
            if (o_lines && line > o_lines)
                goto done;
            if (o_case && (char)up((char)c1) == (char)up((char)c2))
                c1 = c2;
            if (c1 != c2) {
                if (nd == 0 && rc < 1)
                    rc = 1;
                if (o_line) {
                    puts("Vergleichsfehler in Zeile ");
                    put_num(line, 0, 0);
                } else {
                    puts("Vergleichsfehler bei OFFSET ");
                    if (o_dec)
                        put_num(off, 0, 0);
                    else
                        hexn(off, off > 0xFFFFUL ? 8 : 4);
                }
                puts("\r\nDatei1 = ");
                show_byte(b1[i]);
                puts("\r\nDatei2 = ");
                show_byte(b2[i]);
                puts("\r\n");
                if (++nd == 10) {
                    puts("10 Unterschiede - Vergleich beendet\r\n");
                    goto done;
                }
            }
            if (b1[i] == '\n')
                line++;
        }
        if (n1 != n2) {
            if (!(o_lines && line > o_lines)) {
                puts("Die Dateien sind unterschiedlich gro\xE1.\r\n");
                sizediff = 1;
                if (rc < 1)
                    rc = 1;
            }
            break;
        }
        if (!n1)
            break;
    }
done:
    if (!nd && !sizediff)
        puts("Dateien stimmen \x81" "berein.\r\n");
    puts("\r\n");
    DosClose(h1);
    DosClose(h2);
}

static void ask_name(const char *q, char *o)
{
    static char ln[FAPI_PATHLEN];
    unsigned n = 0;
    char c;
    puts(q);
    for (;;) {
        c = (char)KbdCharIn();
        if (c == 13)
            break;
        if (c == 8 && n) {
            n--;
            puts("\b \b");
        } else if ((unsigned char)c >= ' ' && n < sizeof(ln) - 1) {
            ln[n++] = c;
            putc(c);
        }
    }
    ln[n] = 0;
    puts("\r\n");
    scopy(o, ln, FAPI_PATHLEN);
}

void main(void)
{
    static struct fapi_dirent e;
    char *a, *b;
    unsigned n, any = 0, k;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/') {
            char c = up(a[1]);
            if (c == '?') {
                puts("Vergleicht den Inhalt zweier Dateien oder Dateigruppen.\r\n\r\n"
                     "COMP [Daten1] [Daten2] [/D] [/A] [/L] [/N=Anzahl] [/C]\r\n\r\n"
                     "  /D  Unterschiede dezimal     /A  Unterschiede als Zeichen\r\n"
                     "  /L  Zeilennummern            /N=n  nur die ersten n Zeilen\r\n"
                     "  /C  Gro\xE1-/Kleinschreibung egal\r\n");
                return;
            }
            if (c == 'D') o_dec = 1;
            else if (c == 'A') o_asc = 1;
            else if (c == 'L') o_line = 1;
            else if (c == 'C') o_case = 1;
            else if (c == 'N' && a[2] == '=') {
                for (k = 3; a[k] >= '0' && a[k] <= '9'; k++)
                    o_lines = (o_lines << 3) + (o_lines << 1) + (unsigned)(a[k] - '0');
                o_line = 1;
            }
        } else if (!spec1[0])
            word(a, spec1, sizeof(spec1));
        else
            word(a, spec2, sizeof(spec2));
    }
    if (!spec1[0])
        ask_name("Name der ersten zu vergleichenden Datei: ", spec1);
    if (!spec2[0])
        ask_name("Name der zweiten zu vergleichenden Datei: ", spec2);
    if (!spec1[0] || !spec2[0])
        DosExit(2);
    abs_path(spec1, dir1);
    if (is_dir(dir1))
        scat(dir1, slen(dir1) > 3 ? "\\*.*" : "*.*", sizeof(dir1));
    b = base_name(dir1);
    scopy(pat1, b, sizeof(pat1));
    *b = 0;
    if (slen(dir1) > 3)
        dir1[slen(dir1) - 1] = 0;
    abs_path(spec2, f2);
    for (n = 0; DosFindEntry(&e, n, dir1) != FAPI_ERROR; n++) {
        if ((e.attr & 0x18) || !match(e.name, pat1))
            continue;
        any = 1;
        join(f1, dir1, e.name, sizeof(f1));
        if (is_dir(f2))
            join(spec2, f2, e.name, sizeof(spec2));
        else if (has_wild(f2)) {
            static char nm[FAPI_PATHLEN];
            scopy(spec2, f2, sizeof(spec2));
            b = base_name(spec2);
            apply_pat(e.name, b, nm);
            *b = 0;
            scat(spec2, nm, sizeof(spec2));
        } else
            scopy(spec2, f2, sizeof(spec2));
        compare(f1, spec2);
    }
    if (!any) {
        puts("Datei nicht gefunden - ");
        puts(spec1);
        puts("\r\n");
        DosExit(2);
    }
    DosExit(rc);
}
