/*
 * GREP.EXE - Zeilen mit einem Muster suchen, nach dem Vorbild von GNU grep
 * (nur Verhalten und Bedienung, kein GNU-Code):
 *
 *   GREP [-ivnclhHrRsE] MUSTER [DATEI ...]
 *
 *   -i  Gross-/Kleinschreibung egal      -v  Zeilen OHNE Treffer
 *   -n  Zeilennummern                    -c  nur Anzahl je Datei
 *   -l  nur Dateinamen mit Treffern      -h  keine Dateinamen davor
 *   -H  immer Dateinamen davor           -r, -R  Unterverzeichnisse durchsuchen
 *   -s  keine Meldungen zu fehlenden Dateien
 *   -E  erweiterte Ausdruecke (+ ? | ohne \)
 *
 * Muster (regulaere Ausdruecke): . beliebiges Zeichen, * beliebig oft,
 * \+ bzw. + einmal oder mehr, \? bzw. ? hoechstens einmal, ^ Zeilenanfang,
 * $ Zeilenende, [abc] [a-z] [^0-9] Zeichenklassen, \ hebt auf.
 * Alternativen mit \| bzw. | (eine Ebene, ohne Klammern).
 * Dateinamen duerfen * und ? enthalten (wie unter DOS); ohne Datei liest
 * GREP die Standardeingabe. Rueckgabe: noch ohne ERRORLEVEL.
 */
#include "fapi.h"

static char args[FAPI_ARGLEN], line[1024], in[1024];
static char pat[FAPI_ARGLEN];
static int opt_i, opt_v, opt_n, opt_c, opt_l, opt_h, opt_H, opt_r, opt_s, opt_E;
static int show_names;

#define NALT 8
static char *alt[NALT];                 /* Alternativen des Musters      */
static int nalt;

/* ---- regulaere Ausdruecke (rekursiv, wie bei Kernighan/Pike) ----------- */
static char low(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int ceq(char a, char b)
{
    return opt_i ? low(a) == low(b) : a == b;
}

/* Laenge eines Atoms: \x, [..], einzelnes Zeichen */
static int atom_len(const char *p)
{
    const char *q = p;
    if (*q == '\\' && q[1])
        return 2;
    if (*q == '[') {
        q++;
        if (*q == '^')
            q++;
        if (*q == ']')
            q++;
        while (*q && *q != ']')
            q++;
        return *q ? (int)(q - p + 1) : 1;
    }
    return 1;
}

/* passt Zeichen c auf das Atom bei p? */
static int atom_match(const char *p, char c)
{
    int neg = 0, ok = 0;
    if (*p == '.')
        return 1;
    if (*p == '\\')
        return ceq(p[1], c);
    if (*p != '[')
        return ceq(*p, c);
    p++;
    if (*p == '^') {
        neg = 1;
        p++;
    }
    do {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char lo = p[0], hi = p[2];
            if ((c >= lo && c <= hi) ||
                (opt_i && low(c) >= low(lo) && low(c) <= low(hi)))
                ok = 1;
            p += 3;
        } else {
            if (ceq(*p, c))
                ok = 1;
            p++;
        }
    } while (*p && *p != ']');
    return ok != neg;
}

/* Wiederholungszeichen nach einem Atom: '*', '+', '?' oder 0 */
static char rep_at(const char *p, int *skip)
{
    *skip = 0;
    if (*p == '*') {
        *skip = 1;
        return '*';
    }
    if (opt_E && (*p == '+' || *p == '?')) {
        *skip = 1;
        return *p;
    }
    if (!opt_E && p[0] == '\\' && (p[1] == '+' || p[1] == '?')) {
        *skip = 2;
        return p[1];
    }
    return 0;
}

static int match_here(const char *re, const char *s, const char *end);

static int match_rep(const char *atom, char r, const char *rest,
                     const char *s, const char *end)
{
    const char *t = s;
    int min = r == '+' ? 1 : 0, max = r == '?' ? 1 : 32767, n = 0;

    while (t < end && n < max && atom_match(atom, *t)) {
        t++;
        n++;
    }
    for (;;) {                          /* gierig, dann zurueck         */
        if (n >= min && match_here(rest, t, end))
            return 1;
        if (n == 0)
            return 0;
        t--;
        n--;
    }
}

static int match_here(const char *re, const char *s, const char *end)
{
    int al, skip;
    char r;

    for (;;) {
        if (*re == 0)
            return 1;
        if (re[0] == '$' && re[1] == 0)
            return s == end;
        al = atom_len(re);
        r = rep_at(re + al, &skip);
        if (r)
            return match_rep(re, r, re + al + skip, s, end);
        if (s == end || !atom_match(re, *s))
            return 0;
        re += al;
        s++;
    }
}

static int match_one(const char *re, const char *s, const char *end)
{
    if (*re == '^')
        return match_here(re + 1, s, end);
    do {
        if (match_here(re, s, end))
            return 1;
    } while (s++ < end);
    return 0;
}

static int matches(const char *s, unsigned len)
{
    int i;
    for (i = 0; i < nalt; i++)
        if (match_one(alt[i], s, s + len))
            return 1;
    return 0;
}

/* Muster an | (bzw. \|) in Alternativen teilen */
static void split_alt(void)
{
    char *p = pat, *w = pat;
    alt[0] = pat;
    nalt = 1;
    while (*p) {
        if (opt_E ? *p == '|' : (p[0] == '\\' && p[1] == '|')) {
            *w++ = 0;
            p += opt_E ? 1 : 2;
            if (nalt < NALT)
                alt[nalt++] = w;
            continue;
        }
        if (*p == '\\' && p[1]) {
            *w++ = *p++;
        }
        *w++ = *p++;
    }
    *w = 0;
}

/* ---- Ausgabe ------------------------------------------------------------ */
static void put_dec(unsigned long v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        unsigned d = 0;
        while (v >= 10) { v -= 10; d++; }
        b[--i] = (char)('0' + (unsigned)v);
        v = d;
    } while (v);
    puts(b + i);
}

static unsigned long total;
static int had_err;                     /* Datei fehlte: Rueckgabewert 2 */

/* eine Datei (oder STDIN) durchsuchen */
static void grep_handle(unsigned h, const char *name)
{
    unsigned n, i, ll = 0;
    unsigned long lineno = 0, count = 0;
    int binary = 0, first = 1;

    for (;;) {
        n = DosRead(h, in, sizeof(in));
        if (n == FAPI_ERROR)
            n = 0;
        if (first) {                    /* Nullbyte am Anfang: Binaerdatei */
            for (i = 0; i < n; i++)
                if (in[i] == 0)
                    binary = 1;
            first = 0;
        }
        for (i = 0; i <= n; i++) {
            if (i < n && in[i] != '\n') {
                if (in[i] != '\r' && ll < sizeof(line) - 1)
                    line[ll++] = in[i];
                continue;
            }
            if (i == n && (n != 0 || ll == 0))
                break;
            lineno++;
            if (matches(line, ll) != opt_v) {
                count++;
                if (opt_l)
                    break;
                if (binary && !opt_c) {
                    puts("Bin\x84rdatei ");
                    puts(name);
                    puts(" enth\x84lt Treffer\r\n");
                    total += count;
                    return;
                }
                if (!opt_c) {
                    if (show_names) {
                        puts(name);
                        puts(":");
                    }
                    if (opt_n) {
                        put_dec(lineno);
                        puts(":");
                    }
                    line[ll] = 0;
                    puts(line);
                    puts("\r\n");
                }
            }
            ll = 0;
        }
        if (n == 0 || (opt_l && count))
            break;
    }
    total += count;
    if (opt_l && count) {
        puts(name);
        puts("\r\n");
    } else if (opt_c) {
        if (show_names) {
            puts(name);
            puts(":");
        }
        put_dec(count);
        puts("\r\n");
    }
}

static void grep_file(const char *path)
{
    unsigned h = DosOpen(path);
    if (h == FAPI_ERROR) {
        had_err = 1;
        if (!opt_s) {
            puts("GREP: ");
            puts(path);
            puts(": Datei nicht gefunden\r\n");
        }
        return;
    }
    grep_handle(h, path);
    DosClose(h);
}

/* DOS-Platzhalter: Name gegen Muster (Name und Endung getrennt) */
static int wmatch_part(const char *n, unsigned nl, const char *p, unsigned pl)
{
    unsigned i = 0, k;
    for (k = 0; k < pl; k++) {
        if (p[k] == '*')
            return 1;
        if (i >= nl || (p[k] != '?' && low(p[k]) != low(n[i])))
            return 0;
        i++;
    }
    return i == nl;
}

static int wmatch(const char *name, const char *pat2)
{
    const char *nd = name, *pd = pat2;
    unsigned nl, pl;
    if (pat2[0] == '*' && pat2[1] == 0)
        return 1;
    while (*nd && *nd != '.') nd++;
    while (*pd && *pd != '.') pd++;
    if (!wmatch_part(name, (unsigned)(nd - name), pat2, (unsigned)(pd - pat2)))
        return 0;
    if (*nd) nd++;
    if (*pd) pd++;
    for (nl = 0; nd[nl]; nl++) ;
    for (pl = 0; pd[pl]; pl++) ;
    return wmatch_part(nd, nl, pd, pl);
}

static int has_wild(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return 1;
    return 0;
}

/* Verzeichnis dirbuf[0..dl) durchsuchen (leer = aktuelles); mit -r auch
   Unterverzeichnisse. Ein gemeinsamer Puffer statt lokaler Arrays: der
   Stack eines Programms ist nur 2 KB gross. */
static char dirbuf[FAPI_PATHLEN], pathbuf[FAPI_PATHLEN];

static unsigned join_name(char *d, unsigned dl, const char *name)
{
    unsigned k = dl;
    if (dl && d[dl - 1] != '\\' && d[dl - 1] != ':')
        d[k++] = '\\';
    while (*name && k < FAPI_PATHLEN - 14)
        d[k++] = *name++;
    d[k] = 0;
    return k;
}

static void grep_dir(unsigned dl, const char *wild, int depth)
{
    static struct fapi_dirent e;
    unsigned n, k;

    for (n = 0; ; n++) {
        dirbuf[dl] = 0;
        if (DosFindEntry(&e, n, dl ? dirbuf : 0) == FAPI_ERROR)
            break;
        if (e.attr & 0x06)
            continue;                   /* versteckt, System            */
        if (e.attr & 0x10) {
            if (opt_r && depth < 8 && e.name[0] != '.') {
                k = join_name(dirbuf, dl, e.name);
                grep_dir(k, wild, depth + 1);
                dirbuf[dl] = 0;
            }
            continue;
        }
        if (wmatch(e.name, wild)) {
            for (k = 0; k < dl; k++)
                pathbuf[k] = dirbuf[k];
            join_name(pathbuf, dl, e.name);
            grep_file(pathbuf);
        }
    }
}

void main(void)
{
    char *a = args, *f;
    int files = 0, pat_set = 0, multi = 0;
    unsigned k;

    DosGetArgs(args);
    for (;;) {
        while (*a == ' ')
            a++;
        if (a[0] != '-' || !a[1] || pat_set)
            break;
        for (a++; *a && *a != ' '; a++)
            switch (*a) {
            case 'i': opt_i = 1; break;
            case 'v': opt_v = 1; break;
            case 'n': opt_n = 1; break;
            case 'c': opt_c = 1; break;
            case 'l': opt_l = 1; break;
            case 'h': opt_h = 1; break;
            case 'H': opt_H = 1; break;
            case 'r':
            case 'R': opt_r = 1; break;    /* GNU: -R folgt Links, gibt es hier nicht */
            case 's': opt_s = 1; break;
            case 'E': opt_E = 1; break;
            default:
                puts("GREP: unbekannte Option\r\n"
                     "Aufruf: GREP [-ivnclhHrRsE] MUSTER [DATEI ...]\r\n");
                DosExit(2);
            }
    }
    if (!*a) {
        puts("Aufruf: GREP [-ivnclhHrRsE] MUSTER [DATEI ...]\r\n");
        DosExit(2);
    }
    /* Muster, auch in "..." mit Leerzeichen */
    k = 0;
    if (*a == '"') {
        for (a++; *a && *a != '"'; a++)
            pat[k++] = *a;
        if (*a)
            a++;
    } else
        while (*a && *a != ' ')
            pat[k++] = *a++;
    pat[k] = 0;
    split_alt();

    /* mehr als eine Datei? dann Namen davor (wie GNU grep) */
    for (f = a; *f; f++)
        if (*f != ' ') {
            const char *g = f;
            while (*f && *f != ' ')
                f++;
            files++;
            if (has_wild(g) || opt_r)
                multi = 1;
            if (!*f)
                break;
        }
    show_names = opt_H || (!opt_h && (files > 1 || multi || (opt_r && !files)));

    files = 0;
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
        if (has_wild(f) || opt_r) {
            char *b = f, *p;
            unsigned dl;
            for (p = f; *p; p++)
                if (*p == '\\' || *p == ':')
                    b = p + 1;
            dl = (unsigned)(b - f);
            for (k = 0; k < dl; k++)
                dirbuf[k] = f[k];
            if (!has_wild(b)) {         /* -r VERZ oder VERZ\: alles darin */
                for (k = 0; f[k]; k++)
                    dirbuf[k] = f[k];
                dl = k;
                b = "*.*";
            }
            grep_dir(dl, b, 0);
        } else
            grep_file(f);
    }
    if (!files) {
        if (opt_r)
            grep_dir(0, "*.*", 0);      /* aktuelles Verzeichnis        */
        else
            grep_handle(STDIN, "(Standardeingabe)");
    }
    DosExit(had_err ? 2 : total ? 0 : 1);   /* wie GNU grep           */
}
