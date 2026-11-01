/* Gemeinsame Hilfen fuer die Werkzeuge, siehe util.h */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "util.h"

char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) :
           c == '\x84' ? '\x8E' : c == '\x94' ? '\x99' : c == '\x81' ? '\x9A' : c;
}

unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

void scopy(char *d, const char *s, unsigned max)
{
    while (max > 1 && *s) {
        *d++ = *s++;
        max--;
    }
    *d = 0;
}

void scat(char *d, const char *s, unsigned max)
{
    unsigned n = slen(d);
    if (n < max)
        scopy(d + n, s, max - n);
}

int same(const char *a, const char *b)
{
    while (*a && *a != ' ' && up(*a) == up(*b))
        a++, b++;
    return (*a == 0 || *a == ' ') && *b == 0;
}

void putc(char c)
{
    static char b;
    b = c;
    DosWrite(STDOUT, &b, 1);
}

void put_num(unsigned long v, int width, int dots)
{
    static const unsigned long pw[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    static char d[12], o[16];
    int i, n = 0, k = 0, started = 0;
    for (i = 0; i < 10; i++) {
        char c = '0';
        while (v >= pw[i]) {
            v -= pw[i];
            c++;
        }
        if (c != '0' || started || i == 9) {
            d[n++] = c;
            started = 1;
        }
    }
    for (i = 0; i < n; i++) {
        o[k++] = d[i];
        if (dots && (n - i - 1) % 3 == 0 && i != n - 1)
            o[k++] = '.';
    }
    o[k] = 0;
    for (i = k; i < width; i++)
        putc(' ');
    puts(o);
}

char *skip(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

char *next_word(char *p)
{
    while (*p && *p != ' ' && *p != '\t')
        p++;
    return skip(p);
}

char *word(char *p, char *out, unsigned max)
{
    unsigned i = 0;
    p = skip(p);
    while (*p && *p != ' ' && *p != '\t') {
        if (i < max - 1)
            out[i++] = *p;
        p++;
    }
    out[i] = 0;
    return skip(p);
}

int has_wild(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return 1;
    return 0;
}

static int match_part(const char *n, unsigned nl, const char *p, unsigned pl)
{
    unsigned i = 0, k;
    for (k = 0; k < pl; k++) {
        if (p[k] == '*')
            return 1;
        if (i >= nl || (p[k] != '?' && up(p[k]) != up(n[i])))
            return 0;
        i++;
    }
    return i == nl;
}

int match(const char *name, const char *pat)
{
    const char *nd = name, *pd = pat;
    unsigned nl, pl;
    if (pat[0] == '*' && pat[1] == 0)
        return 1;
    while (*nd && *nd != '.') nd++;
    while (*pd && *pd != '.') pd++;
    nl = (unsigned)(nd - name);
    pl = (unsigned)(pd - pat);
    if (!match_part(name, nl, pat, pl))
        return 0;
    if (*nd) nd++;
    if (*pd) pd++;
    return match_part(nd, slen(nd), pd, slen(pd));
}

char *base_name(char *path)
{
    char *b = path, *p;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == ':')
            b = p + 1;
    return b;
}

void join(char *out, const char *dir, const char *name, unsigned max)
{
    unsigned n;
    if (out != dir)
        scopy(out, dir, max);
    n = slen(out);
    if (n && out[n - 1] != '\\' && out[n - 1] != ':')
        scat(out, "\\", max);
    scat(out, name, max);
}

int is_dir(const char *p)
{
    static struct fapi_dirent e;
    static char cur[FAPI_CURDIRLEN];
    unsigned n = slen(p);
    if (n == 0 || p[n - 1] == '\\' || p[n - 1] == ':' || (p[0] == '.' && (!p[1] ||
        (p[1] == '.' && !p[2]))))
        return 1;
    if (DosFindEntry(&e, 0, p) != FAPI_ERROR)
        return 1;
    DosQCurDir(cur);                    /* leeres Verzeichnis: per CD   */
    if (DosChDir(p) != FAPI_ERROR) {
        DosChDir(cur + 2);
        return 1;
    }
    return 0;
}

int ask_yn(void)
{
    unsigned c;
    for (;;) {
        c = (unsigned)up((char)KbdCharIn());
        if (c == 'J' || c == 'Y') { puts("J\r\n"); return 1; }
        if (c == 'N') { puts("N\r\n"); return 0; }
        if (c == 0)
            return 0;                   /* Strg+C / Signal              */
    }
}

static char xbuf[XFER];

int copy_file(const char *from, const char *to)
{
    unsigned hi, ho, n, k;
    int rc = 0;

    hi = DosOpen(from);
    if (hi == FAPI_ERR_INUSE)
        return -6;                      /* schreibt gerade ein anderer  */
    if (FAPI_FAILED(hi))
        return -1;
    ho = DosCreate(to);
    if (FAPI_FAILED(ho)) {
        DosClose(hi);
        return -2;
    }
    while ((n = DosRead(hi, xbuf, XFER)) != 0) {
        if (FAPI_FAILED(n)) {
            rc = -3;
            break;
        }
        if ((k = DosWrite(ho, xbuf, n)) != n) {
            rc = k == FAPI_ERR_FULL || !FAPI_FAILED(k) ? -7 : -4;  /* voll */
            break;
        }
    }
    DosClose(hi);
    if (FAPI_FAILED(DosClose(ho)) && !rc)
        rc = -5;
    if (rc)
        DosDelete(to);
    return rc;
}

#define MAXDEPTH 24

int walk_tree(char *path, walk_fn fn)
{
    static struct fapi_dirent e;
    static unsigned idx[MAXDEPTH], len[MAXDEPTH];
    int depth = 0, r;
    unsigned n = 0, l;

    for (;;) {
        if (DosFindEntry(&e, n, path) == FAPI_ERROR) {
            r = fn(path, 0, WT_LEAVE, depth);
            if (r & WT_STOP)
                return 1;
            if (depth == 0)
                return 0;
            depth--;
            path[len[depth]] = 0;
            n = idx[depth];
            if ((r & WT_REMOVED) && n)
                n--;                    /* Verzeichnis selbst ist weg  */
            continue;
        }
        n++;
        if (e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2])))
            continue;
        l = slen(path);
        join(path, path, e.name, FAPI_PATHLEN);
        r = fn(path, &e, (e.attr & 0x10) ? WT_DIR : WT_FILE, depth);
        if (r & WT_STOP)
            return 1;
        if ((e.attr & 0x10) && (r & WT_DESCEND) && depth < MAXDEPTH) {
            idx[depth] = n;
            len[depth] = l;
            depth++;
            n = 0;
            continue;
        }
        path[l] = 0;
        if (r & WT_REMOVED)
            n--;
    }
}

/* Pfad absolut machen: "X:\DIR\NAME" (ohne . und ..-Aufloesung ausser
   fuehrendem Laufwerk/aktuellem Verzeichnis) */
int abs_path(const char *in, char *out)
{
    static char cur[FAPI_CURDIRLEN];
    static char tmp[FAPI_PATHLEN];
    unsigned d;

    if (in[0] && in[1] == ':') {
        d = (unsigned)(up(in[0]) - 'A');
        in += 2;
    } else
        d = DosQCurDisk();
    if (in[0] == '\\') {
        out[0] = (char)('A' + d);
        out[1] = ':';
        scopy(out + 2, in, FAPI_PATHLEN - 2);
        for (d = 0; out[d]; d++)
            out[d] = up(out[d]);
        return 0;
    }
    /* relativ: aktuelles Verzeichnis des Laufwerks (nur das aktuelle
       Laufwerk ist bekannt; sonst Stamm) */
    if (d == DosQCurDisk()) {
        DosQCurDir(cur);
        scopy(tmp, cur, sizeof(tmp));
    } else {
        tmp[0] = (char)('A' + d);
        scopy(tmp + 1, ":\\", sizeof(tmp) - 1);
    }
    if (in[0] == '.' && in[1] == 0)
        in++;
    if (*in)
        join(tmp, tmp, in, sizeof(tmp));
    scopy(out, tmp, FAPI_PATHLEN);
    for (d = 0; out[d]; d++)            /* wie MS-DOS: gross            */
        out[d] = up(out[d]);
    return 0;
}
