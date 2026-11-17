/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HEXDUMP.EXE / XXD.EXE - Dateien als Hex ansehen (nur Verhalten und
 * Schalter nachgebildet, kein fremder Code):
 *
 *   HEXDUMP [-C|-b|-c|-d|-o|-x] [-n Laenge] [-s Stelle] [-v] [DATEI...]
 *       wie hexdump von util-linux; Vorgabe 16-Bit-Worte hex, -C Bytes
 *       mit ASCII; gleiche Zeilen werden zu '*' (ausser -v).
 *   XXD [-a] [-b] [-c n] [-e] [-g n] [-i] [-l n] [-o n] [-p] [-r] [-s n]
 *       [-u] [Ein [Aus]]
 *       wie xxd von vim; -r wandelt zurueck (auch -r -p) und schreibt in
 *       eine vorhandene Ausgabedatei an die Stellen der Zeilen (Patchen).
 *
 * Zahlen dezimal, 0x.. hex, 0.. oktal, k/m dahinter = KB/MB.
 * task_xxd.c bindet diese Datei mit XXD ein.
 */
#include "fapi.h"
#include "util.h"
#define TI_FILE
#include "textin.h"

#ifdef XXD
#define PROG "xxd"
#else
#define PROG "hexdump"
#endif

typedef unsigned long u32;

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN];
#ifdef XXD
static char oname[FAPI_PATHLEN];
#endif
static unsigned char cur[256];
#ifndef XXD
static unsigned char prev[256];
#endif
static u32 skip_n, len_n = 0xFFFFFFFFUL;
static int has_len, rc;
static const char *hx = "0123456789abcdef";

static void bad(const char *a)
{
    to_flush();
    puts(PROG ": ung\x81ltiger Schalter oder Wert - ");
    puts(a);
    puts("\r\n");
    DosExit(1);
}

/* Zahl wie strtol(.., 0) mit k/m; ok = 0 bei Fehler */
static u32 num(const char *s, int *ok)
{
    u32 v = 0;
    unsigned base = 10, d;
    const char *p = s;

    *ok = 0;
    if (*p == '+')
        p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && p[1] >= '0' && p[1] <= '7')
        base = 8;
    if (!*p || *p == ' ')
        return 0;
    for (; *p && *p != ' '; p++) {
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        if (d >= base)
            return 0;
        v = base == 16 ? (v << 4) + d : base == 8 ? (v << 3) + d
                       : (v << 3) + (v << 1) + d;
    }
    if (*p == 'k' || *p == 'K') {
        v <<= 10;
        p++;
    } else if (*p == 'm' || *p == 'M') {
        v <<= 20;
        p++;
    }
    if (*p && *p != ' ')
        return 0;
    *ok = 1;
    return v;
}

/* Wert hinter dem Schalter (direkt dahinter oder naechstes Wort) */
static u32 opt_num(char **o, char **a)
{
    int ok;
    u32 v;
    char *s = *o + 1;
    if (!*s || *s == ' ') {
        *a = next_word(*a);
        s = *a;
    }
    v = num(s, &ok);
    if (!ok)
        bad(*a);
    *o = s;
    while (**o && **o != ' ')
        (*o)++;
    (*o)--;
    return v;
}

static void hexn(u32 v, unsigned digits)
{
    while (digits--)
        to_put(hx[(unsigned)(v >> (4 * digits)) & 15]);
}

/* Eingabe oeffnen, -s ueberspringen; -> Handle oder FAPI_ERROR */
static unsigned in_open(const char *a)
{
    unsigned h = ti_file(a);
    u32 k;
    if (FAPI_FAILED(h))
        return h;
    ti_open(h);
    if (skip_n) {
        k = skip_n;
        if (h != STDIN && DosSeek(h, &k) == 0)
            ;
        else
            for (k = skip_n; k && ti_get() >= 0; k--)
                ;
    }
    return h;
}

/* bis n Bytes nach cur (Grenze -n/-l beachten) -> Anzahl */
static unsigned fill(unsigned n)
{
    unsigned i = 0;
    int c;
    while (i < n && len_n) {
        if ((c = ti_get()) < 0)
            break;
        cur[i++] = (unsigned char)c;
        len_n--;
    }
    return i;
}

#ifndef XXD
/* ---- HEXDUMP ---------------------------------------------------------- */
static void mcpy_prev(unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        prev[i] = cur[i];
}


static int same_line(unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (cur[i] != prev[i])
            return 0;
    return 1;
}

static char fmt = 'X';                  /* 'X' = Vorgabe (ohne -x)       */
static int o_v;

static void dec_pad(u32 v, unsigned w, char pad)
{
    char b[12];
    unsigned n = 0;
    do {                                /* nur 16 Bit: Division ok       */
        b[n++] = (char)('0' + (unsigned)v % 10);
        v = (unsigned)v / 10;
    } while (v);
    while (w-- > n)
        to_put(pad);
    while (n)
        to_put(b[--n]);
}

static void oct_n(unsigned v, unsigned digits)
{
    while (digits--)
        to_put((char)('0' + ((v >> (3 * digits)) & 7)));
}

static void put_c(unsigned char c)
{
    static const char esc[] = "\0\a\b\t\n\v\f\r";
    static const char let[] = "0abtnvfr";
    unsigned i;
    for (i = 0; i < 8; i++)
        if (c == (unsigned char)esc[i]) {
            to_str("  \\");
            to_put(let[i]);
            return;
        }
    if (c >= 0x20 && c < 0x7F) {
        to_str("   ");
        to_put((char)c);
    } else {
        to_put(' ');
        oct_n(c, 3);
    }
}

static void line_hd(u32 off, unsigned n)
{
    unsigned i, w;
    if (fmt == 'C') {
        hexn(off, 8);
        to_str("  ");
        for (i = 0; i < 16; i++) {
            if (i == 8)
                to_put(' ');
            if (i < n) {
                to_put(hx[cur[i] >> 4]);
                to_put(hx[cur[i] & 15]);
                to_put(' ');
            } else
                to_str("   ");
        }
        to_str(" |");
        for (i = 0; i < n; i++)
            to_put(cur[i] >= 0x20 && cur[i] < 0x7F ? (char)cur[i] : '.');
        to_put('|');
    } else {
        hexn(off, 7);
        for (i = 0; i < 16; ) {
            if (fmt == 'b' || fmt == 'c') {
                if (i >= n)
                    to_str("    ");
                else if (fmt == 'b') {
                    to_put(' ');
                    oct_n(cur[i], 3);
                } else
                    put_c(cur[i]);
                i++;
                continue;
            }
            w = cur[i] | (i + 1 < n ? cur[i + 1] << 8 : 0);
            if (i >= n) {
                to_str(fmt == 'X' ? "     " : "        ");
            } else if (fmt == 'X') {
                to_put(' ');
                hexn(w, 4);
            } else if (fmt == 'x') {
                to_str("    ");
                hexn(w, 4);
            } else if (fmt == 'd') {
                to_str("   ");
                dec_pad(w, 5, '0');
            } else {
                to_str("  ");
                oct_n(w >> 15, 1);
                oct_n(w, 5);
            }
            i += 2;
        }
    }
    to_str("\r\n");
}

static void dump(unsigned h)
{
    u32 off = skip_n;
    unsigned n;
    int star = 0, have = 0;
    (void)h;
    while ((n = fill(16)) != 0) {
        if (!o_v && have && n == 16 && same_line(16)) {
            if (!star)
                to_str("*\r\n");
            star = 1;
        } else {
            line_hd(off, n);
            star = 0;
        }
        mcpy_prev(n);
        have = 1;
        off += n;
        if (n < 16)
            break;
    }
    if (have) {
        hexn(off, fmt == 'C' ? 8 : 7);
        to_str("\r\n");
    }
}
#else
/* ---- XXD -------------------------------------------------------------- */
static unsigned cols, grp, o_a, o_b, o_e, o_i, o_p, o_r, o_u;
static int cols_set, grp_set;
static u32 o_off;

static unsigned char zb[256];           /* Nullzeile fuer -a             */

static void line_xx(const unsigned char *cur, u32 off, unsigned n)
{
    unsigned i, j, col = 0, acol, bw = o_b ? 8 : 2;
    const char *h = o_u ? "0123456789ABCDEF" : hx;

    hexn(off + o_off, 8);
    to_str(": ");
    acol = cols * bw + (cols - 1) / grp + 2 + (o_e ? 1 : 0);
    for (i = 0; i < n; i++) {
        j = i;
        if (o_e) {                      /* Gruppe rueckwaerts            */
            unsigned g0 = i - i % grp, ge = g0 + grp;
            j = g0 + (ge - 1 - i);
            if (ge > n) {               /* letzte, unvollstaendige Gruppe */
                if (j >= n) {
                    to_str("  ");
                    col += 2;
                    goto sep;
                }
            }
        }
        if (o_b) {
            unsigned k;
            for (k = 8; k--; )
                to_put((char)('0' + ((cur[j] >> k) & 1)));
            col += 8;
        } else {
            to_put(h[cur[j] >> 4]);
            to_put(h[cur[j] & 15]);
            col += 2;
        }
sep:
        if ((i + 1) % grp == 0 && i + 1 < cols) {
            to_put(' ');
            col++;
        }
    }
    while (col < acol) {
        to_put(' ');
        col++;
    }
    for (i = 0; i < n; i++)
        to_put(cur[i] >= 0x20 && cur[i] < 0x7F ? (char)cur[i] : '.');
    to_str("\r\n");
}

static int all_zero(unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (cur[i])
            return 0;
    return 1;
}

static void c_name(const char *a)       /* -i: Name wie xxd (a-z0-9_)    */
{
    const char *b = a;
    char c;
    for (; *a; a++)
        if (*a == '\\' || *a == ':' || *a == '/')
            b = a + 1;
    if (*b >= '0' && *b <= '9')
        to_put('_');
    for (; *b; b++) {
        c = *b;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        to_put((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_');
    }
}

static void dump(unsigned h, const char *a)
{
    u32 off = skip_n, total = 0;
    unsigned n, i;
    int stdin_in = h == STDIN;
    const char *x = o_u ? "0123456789ABCDEF" : hx;

    if (o_i) {
        if (!stdin_in) {
            to_str("unsigned char ");
            c_name(a);
            to_str("[] = {\r\n");
        }
        i = 0;
        while ((n = fill(cols)) != 0) {
            to_str(i ? ",\r\n  " : "  ");
            for (i = 0; i < n; i++) {
                if (i)
                    to_str(", ");
                to_str(o_u ? "0X" : "0x");
                to_put(x[cur[i] >> 4]);
                to_put(x[cur[i] & 15]);
            }
            total += n;
            i = 1;
        }
        if (total)
            to_str("\r\n");
        if (!stdin_in) {
            to_str("};\r\nunsigned int ");
            c_name(a);
            to_str("_len = ");
            {
                char b[11];
                unsigned k = 0;
                u32 v = total;
                do {
                    u32 q = 0, r = 0;   /* v / 10 ohne 32-Bit-Division  */
                    int bit;
                    for (bit = 31; bit >= 0; bit--) {
                        r = (r << 1) | ((v >> bit) & 1);
                        q <<= 1;
                        if (r >= 10) {
                            r -= 10;
                            q |= 1;
                        }
                    }
                    b[k++] = (char)('0' + (unsigned)r);
                    v = q;
                } while (v);
                while (k)
                    to_put(b[--k]);
            }
            to_str(";\r\n");
        }
        return;
    }
    if (o_p) {
        while ((n = fill(cols)) != 0) {
            for (i = 0; i < n; i++) {
                to_put(x[cur[i] >> 4]);
                to_put(x[cur[i] & 15]);
            }
            to_str("\r\n");
        }
        return;
    }
    /* -a wie xxd: erste Nullzeile einer Strecke zeigen, ab der dritten
       '*'; die letzte zeigen, wenn die Strecke zwei Zeilen lang ist oder
       bis zum Dateiende reicht */
    {
        unsigned z = 0, pn = 0;
        u32 poff = 0;
        while ((n = fill(cols)) != 0) {
            if (o_a && all_zero(n)) {
                z++;
                if (z == 1)
                    line_xx(cur, off, n);
                else {
                    if (z == 3)
                        to_str("*\r\n");
                    poff = off;
                    pn = n;
                }
            } else {
                if (z == 2) {
                    line_xx(zb, poff, pn);
                }
                z = 0;
                line_xx(cur, off, n);
            }
            off += n;
        }
        if (z >= 2) {
            line_xx(zb, poff, pn);
        }
    }
}

/* ---- XXD -r ------------------------------------------------------------ */
static unsigned oh;                     /* Ausgabe                        */
static u32 opos, osize;                 /* Stelle, bekannte Groesse       */
static unsigned char ob[512];
static unsigned obn;

static void o_flush(void)
{
    if (obn)
        DosWrite(oh, ob, obn);
    obn = 0;
}

static void o_byte(unsigned char c)
{
    ob[obn++] = c;
    if (obn == sizeof(ob))
        o_flush();
    if (++opos > osize)
        osize = opos;
}

static void o_seek(u32 to)
{
    u32 k;
    if (to == opos)
        return;
    o_flush();
    if (oh != STDOUT && to <= osize) {
        k = to;
        DosSeek(oh, &k);
        opos = to;
        return;
    }
    if (to < opos)                      /* rueckwaerts auf stdout: nicht  */
        return;                         /* moeglich, wie xxd uebergehen  */
    if (oh != STDOUT && opos < osize) {
        k = osize;
        DosSeek(oh, &k);
        opos = osize;
    }
    while (opos < to)
        o_byte(0);
}

static int hval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void revert(void)
{
    static char ln[600];
    unsigned n, i;
    int c, v, hi;
    u32 off;

    if (o_p) {                          /* reiner Hex-Strom               */
        hi = -1;
        while ((c = ti_get()) >= 0) {
            if ((v = hval(c)) < 0)
                continue;
            if (hi < 0)
                hi = v;
            else {
                o_byte((unsigned char)((hi << 4) | v));
                hi = -1;
            }
        }
        o_flush();
        return;
    }
    for (;;) {
        n = 0;
        while ((c = ti_get()) >= 0 && c != '\n')
            if (n < sizeof(ln) - 1)
                ln[n++] = (char)c;
        ln[n] = 0;
        if (n || c >= 0) {
            off = 0;
            for (i = 0; (v = hval(ln[i])) >= 0; i++)
                off = (off << 4) | v;
            if (ln[i] == ':') {
                i++;
                o_seek(off + o_off);
                hi = -1;
                for (; ln[i] && ln[i] != '\r'; i++) {
                    if (ln[i] == ' ') {
                        if (ln[i + 1] == ' ')
                            break;      /* zwei Leerzeichen: ASCII-Teil  */
                        continue;
                    }
                    if ((v = hval(ln[i])) < 0)
                        break;
                    if (hi < 0)
                        hi = v;
                    else {
                        o_byte((unsigned char)((hi << 4) | v));
                        hi = -1;
                    }
                }
            }
        }
        if (c < 0)
            break;
    }
    o_flush();
}

static u32 fsize(const char *nm)
{
    static struct fapi_dirent de;
    static char d[FAPI_PATHLEN];
    char *b;
    unsigned i;
    if (abs_path(nm, d))
        return 0;
    b = base_name(d);
    scopy(oname, b, sizeof(oname));
    *b = 0;
    if (slen(d) > 3)
        d[slen(d) - 1] = 0;
    for (i = 0; DosFindEntry(&de, i, d) != FAPI_ERROR; i++)
        if (same(de.name, oname))
            return de.size;
    return 0;
}
#endif

#ifdef XXD
#define VALOPT "cglso"
#else
#define VALOPT "ns"
#endif
static int sfind(const char *s, char c)
{
    while (*s)
        if (*s++ == c)
            return 1;
    return 0;
}

void main(void)
{
    char *a, *o;
    unsigned h, nf = 0;
    int pass;
#ifdef XXD
    char *in = 0, *out = 0;
    static char inb[FAPI_PATHLEN];
#endif

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (pass)
                continue;
#ifdef XXD
            puts("Zeigt Dateien als Hex und wandelt zur\x81" "ck (nach dem Vorbild von xxd).\r\n\r\n"
                 "XXD [-a] [-b] [-c n] [-e] [-g n] [-i] [-l n] [-o n] [-p] [-r] [-s n] [-u]\r\n"
                 "    [Eingabe [Ausgabe]]\r\n\r\n"
                 "  -a  Nullzeilen als *          -b  Bits statt Hex\r\n"
                 "  -c  Bytes je Zeile (16)       -g  Bytes je Gruppe (2)\r\n"
                 "  -e  Gruppen little endian     -i  als C-Feld\r\n"
                 "  -l  nur n Bytes               -s  ab Stelle n\r\n"
                 "  -o  n auf die Stellen addieren\r\n"
                 "  -p  nur Hex (30 Bytes je Zeile)  -u  Gro\xE1" "buchstaben\r\n"
                 "  -r  zur\x81" "ckwandeln (auch -r -p); eine vorhandene Ausgabedatei wird\r\n"
                 "      an den Stellen der Zeilen \x81" "berschrieben (Patchen)\r\n"
                 "Zahlen dezimal, 0x.. hex, 0.. oktal, k/m = KB/MB.\r\n");
#else
            puts("Zeigt Dateien als Hex (nach dem Vorbild von hexdump).\r\n\r\n"
                 "HEXDUMP [-C|-b|-c|-d|-o|-x] [-n L\x84nge] [-s Stelle] [-v] [Datei...]\r\n\r\n"
                 "  -C  Bytes hex mit ASCII       -x  16-Bit-Worte hex\r\n"
                 "  -b  Bytes oktal               -c  Zeichen\r\n"
                 "  -d  16-Bit-Worte dezimal      -o  16-Bit-Worte oktal\r\n"
                 "  -n  nur so viele Bytes        -s  ab dieser Stelle\r\n"
                 "  -v  gleiche Zeilen nicht zu * zusammenfassen\r\n"
                 "Zahlen dezimal, 0x.. hex, 0.. oktal, k/m = KB/MB; ohne Datei die\r\n"
                 "Standardeingabe. Ohne Schalter 16-Bit-Worte wie hexdump.\r\n");
#endif
            return;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            if (pass) {                 /* Wert als eigenes Wort ueberspringen */
                for (o = a + 1; *o && *o != ' '; o++)
                    if (sfind(VALOPT, *o)) {
                        if (!o[1] || o[1] == ' ')
                            a = next_word(a);
                        break;
                    }
                continue;
            }
            for (o = a + 1; *o && *o != ' '; o++)
                switch (*o) {
#ifdef XXD
                case 'a': o_a = 1; break;
                case 'b': o_b = 1; break;
                case 'e': o_e = 1; break;
                case 'i': o_i = 1; break;
                case 'p': o_p = 1; break;
                case 'r': o_r = 1; break;
                case 'u': o_u = 1; break;
                case 'c': cols = (unsigned)opt_num(&o, &a); cols_set = 1;
                    if (!cols || cols > 256) bad(a);
                    break;
                case 'g': grp = (unsigned)opt_num(&o, &a); grp_set = 1;
                    break;
                case 'l': len_n = opt_num(&o, &a); has_len = 1; break;
                case 's': skip_n = opt_num(&o, &a); break;
                case 'o': o_off = opt_num(&o, &a); break;
#else
                case 'C': case 'b': case 'c': case 'd': case 'o': case 'x':
                    fmt = *o;
                    break;
                case 'v': o_v = 1; break;
                case 'n': len_n = opt_num(&o, &a); has_len = 1; break;
                case 's': skip_n = opt_num(&o, &a); break;
#endif
                default:
                    bad(a);
                }
            continue;
        }
        if (!pass) {
            nf++;
            continue;
        }
#ifdef XXD
        if (!in) {
            word(a, inb, sizeof(inb));
            in = inb;
        } else if (!out) {
            word(a, name, sizeof(name));
            out = name;
        } else
            bad(a);
#else
        word(a, name, sizeof(name));
        h = in_open(name);
        if (FAPI_FAILED(h)) {
            to_flush();
            puts(PROG ": ");
            puts(name);
            puts(": Datei nicht gefunden\r\n");
            rc = 1;
            continue;
        }
        dump(h);                        /* mehrere Dateien: je fuer sich */
        if (h != STDIN)
            DosClose(h);
#endif
    }
#ifdef XXD
    if (!nf)
        in = "-";
    if (o_b && !grp_set)
        grp = 1;
    if (o_e && !grp_set)
        grp = 4;
    if (!grp_set && !o_b && !o_e)
        grp = 2;
    if (!grp)
        grp = 256;                      /* -g 0: keine Gruppen           */
    if (!cols_set)
        cols = o_p ? 30 : o_i ? 12 : o_b ? 6 : 16;
    if (o_r) {
        h = ti_file(in);
        if (FAPI_FAILED(h)) {
            puts(PROG ": ");
            puts(in);
            puts(": Datei nicht gefunden\r\n");
            DosExit(2);
        }
        ti_open(h);
        oh = STDOUT;
        if (out) {
            osize = fsize(out);
            oh = DosOpenRW(out);
            if (FAPI_FAILED(oh)) {
                oh = DosCreate(out);
                osize = 0;
            }
            if (FAPI_FAILED(oh)) {
                puts(PROG ": ");
                puts(out);
                puts(": l\x84\xE1t sich nicht schreiben\r\n");
                DosExit(2);
            }
        }
        revert();
        if (oh != STDOUT)
            DosClose(oh);
        DosExit(0);
    }
    h = in_open(in);
    if (FAPI_FAILED(h)) {
        puts(PROG ": ");
        puts(in);
        puts(": Datei nicht gefunden\r\n");
        DosExit(2);
    }
    if (out) {                          /* Ausgabe in Datei: ueber STDOUT */
        unsigned oh2 = DosCreate(out);
        if (FAPI_FAILED(oh2)) {
            puts(PROG ": ");
            puts(out);
            puts(": l\x84\xE1t sich nicht schreiben\r\n");
            DosExit(2);
        }
        to_h = oh2;
    }
    dump(h, in);
    to_flush();
#else
    if (!nf) {
        h = in_open("-");
        dump(h);
    }
    to_flush();
#endif
    DosExit(rc);
}
