/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WHERE.EXE - Dateien im aktuellen Verzeichnis und im PATH suchen, wie
 * where von Windows (WHICH in LINUX.MAC):
 *
 *   WHERE [/R Verz] [/Q] [/F] [/T] Muster ...
 *
 * Muster ohne Endung: auch .COM, .EXE, .CMD, .BAT (in dieser Reihenfolge);
 * * und ? erlaubt. /R sucht im Baum unter Verz statt im PATH. /Q nur
 * ERRORLEVEL (0 gefunden, 1 nichts, 2 Fehler), /F Namen in "", /T mit
 * Groesse, Datum und Zeit.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], pathv[512], dir[FAPI_PATHLEN], full[FAPI_PATHLEN];
static char pats[8][FAPI_PATHLEN], rdir[FAPI_PATHLEN];
static unsigned npat;
static int quiet, quote, times, recurse, found;
static struct fapi_dirent e;
static const char *const exts[] = { ".COM", ".EXE", ".CMD", ".BAT" };

static int has_ext(const char *p)
{
    const char *b = base_name((char *)p);
    while (*b)
        if (*b++ == '.')
            return 1;
    return 0;
}

static void two(unsigned v)
{
    putc((char)('0' + v / 10));
    putc((char)('0' + v % 10));
}

static void show(const char *p, struct fapi_dirent *d)
{
    found = 1;
    if (quiet)
        return;
    if (times) {
        put_num(d->size, 12, 1);
        puts("   ");
        two(d->date & 31); putc('.');
        two((d->date >> 5) & 15); putc('.');
        put_num(1980 + (d->date >> 9), 0, 0);
        puts("  ");
        two(d->time >> 11); putc(':');
        two((d->time >> 5) & 63);
        puts("   ");
    }
    if (quote) putc('"');
    puts(p);
    if (quote) putc('"');
    puts("\r\n");
}

/* Treffer von pat im Verzeichnis d */
static void in_dir(const char *d, const char *pat)
{
    unsigned n;
    for (n = 0; DosFindEntry(&e, n, d) != FAPI_ERROR; n++) {
        if (e.attr & 0x18)
            continue;
        if (!match(e.name, pat))
            continue;
        join(full, d, e.name, sizeof(full));
        show(full, &e);
    }
}

static void search_pat(const char *d, const char *p)
{
    unsigned k;
    char t[FAPI_PATHLEN];
    if (has_ext(p) || has_wild(p)) {
        in_dir(d, p);
        if (has_ext(p))
            return;
    }
    for (k = 0; k < 4; k++) {
        scopy(t, p, sizeof(t));
        scat(t, exts[k], sizeof(t));
        in_dir(d, t);
    }
}

static int visit(char *path, struct fapi_dirent *d, int what, int depth)
{
    unsigned k;
    (void)depth;
    if (what == WT_DIR)
        return WT_DESCEND;
    if (what != WT_FILE)
        return 0;
    for (k = 0; k < npat; k++) {
        if (match(d->name, pats[k]))
            show(path, d);
        else if (!has_ext(pats[k])) {
            unsigned j;
            char t[FAPI_PATHLEN];
            for (j = 0; j < 4; j++) {
                scopy(t, pats[k], sizeof(t));
                scat(t, exts[j], sizeof(t));
                if (match(d->name, t)) {
                    show(path, d);
                    break;
                }
            }
        }
    }
    return 0;
}

void main(void)
{
    char *a, w[FAPI_PATHLEN], *p;
    unsigned k;
    int want_r = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want_r) {
            abs_path(w, rdir);
            want_r = 0;
            continue;
        }
        if ((w[0] == '/' || w[0] == '-') && w[1] && !w[2]) {
            switch (up(w[1])) {
            case 'R': recurse = 1; want_r = 1; continue;
            case 'Q': quiet = 1; continue;
            case 'F': quote = 1; continue;
            case 'T': times = 1; continue;
            case '?':
                puts("Sucht Dateien im aktuellen Verzeichnis und im PATH.\r\n\r\n"
                     "WHERE [/R Verz] [/Q] [/F] [/T] Muster ...\r\n\r\n"
                     "  /R  im Verzeichnisbaum unter Verz suchen\r\n"
                     "  /Q  nichts ausgeben, nur ERRORLEVEL (0 gefunden, 1 nicht)\r\n"
                     "  /F  Namen in Anf\x81hrungszeichen\r\n"
                     "  /T  mit Gr\x94\xE1" "e, Datum und Uhrzeit\r\n");
                return;
            }
        }
        if (npat < 8)
            scopy(pats[npat++], w, sizeof(pats[0]));
    }
    if (!npat) {
        puts("WHERE: Muster fehlt (WHERE /? zeigt die Bedienung).\r\n");
        DosExit(2);
    }
    if (recurse) {
        if (!rdir[0]) {
            puts("WHERE: Verzeichnis f\x81r /R fehlt.\r\n");
            DosExit(2);
        }
        walk_tree(rdir, visit);
    } else {
        for (k = 0; k < npat; k++) {
            /* Muster mit Pfad: nur dort */
            if (base_name(pats[k]) != pats[k]) {
                scopy(dir, pats[k], sizeof(dir));
                p = base_name(dir);
                scopy(w, p, sizeof(w));
                *p = 0;
                if (slen(dir) > 3 && (p[-1] == '\\' || p[-1] == '/'))
                    p[-1] = 0;
                search_pat(dir, w);
                continue;
            }
            search_pat(".", pats[k]);
            if (FAPI_FAILED(DosGetEnv("PATH", pathv, sizeof(pathv))))
                pathv[0] = 0;
            for (p = pathv; *p; ) {
                unsigned n = 0;
                while (*p && *p != ';' && n < sizeof(dir) - 1)
                    dir[n++] = *p++;
                dir[n] = 0;
                if (*p == ';')
                    p++;
                if (n > 3 && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
                    dir[n - 1] = 0;
                if (n)
                    search_pat(dir, pats[k]);
            }
        }
    }
    if (!found && !quiet)
        puts("Die Dateien wurden nicht gefunden.\r\n");
    DosExit(found ? 0 : 1);
}
