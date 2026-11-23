/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TOUCH.EXE - Datum/Uhrzeit von Dateien auf jetzt setzen, fehlende Dateien
 * leer anlegen, wie touch (GNU):
 *
 *   TOUCH [-c] [-d TT.MM.JJJJ [hh:mm[:ss]]] [-r Bezug] Datei ...
 *
 * -c legt nichts an, -d nimmt die angegebene Zeit, -r die einer anderen
 * Datei. Platzhalter: alle passenden Dateien.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], full[FAPI_PATHLEN], dirp[FAPI_PATHLEN];
static unsigned dt[2];
static struct fapi_finfo fi;
static struct fapi_dirent e;
static int nocreate, rc;

static unsigned num(const char **s)
{
    unsigned v = 0;
    while (**s >= '0' && **s <= '9')
        v = v * 10 + (unsigned)(*(*s)++ - '0');
    return v;
}

/* "TT.MM.JJJJ" ["hh:mm[:ss]"] -> dt; 0 = ok */
static int parse_date(const char *d, const char *t)
{
    unsigned dd, mm, yy, h = 0, mi = 0, sc = 0;
    dd = num(&d);
    if (*d++ != '.') return 1;
    mm = num(&d);
    if (*d++ != '.') return 1;
    yy = num(&d);
    if (yy < 100) yy += 2000;
    if (t && *t) {
        h = num(&t);
        if (*t == ':') { t++; mi = num(&t); }
        if (*t == ':') { t++; sc = num(&t); }
    }
    if (!dd || dd > 31 || !mm || mm > 12 || yy < 1980 || yy > 2107 || h > 23 || mi > 59 || sc > 59)
        return 1;
    dt[0] = ((yy - 1980) << 9) | (mm << 5) | dd;
    dt[1] = (h << 11) | (mi << 5) | (sc >> 1);
    return 0;
}

static int find_one(const char *path, struct fapi_dirent *out)
{
    char *b;
    unsigned n;
    scopy(dirp, path, sizeof(dirp));
    b = base_name(dirp);
    if (b == dirp)
        scopy(dirp, ".", sizeof(dirp)), b = (char *)path;
    else {
        char *nm = (char *)path + (b - dirp);
        if (b - dirp > 1 && (b[-1] == '\\' || b[-1] == '/') && b[-2] != ':')
            b[-1] = 0;
        else
            *b = 0;
        b = nm;
    }
    for (n = 0; DosFindEntry(out, n, dirp) != FAPI_ERROR; n++)
        if (!(out->attr & 0x18) && same(out->name, b))
            return 1;
    return 0;
}

static void touch(const char *p)
{
    unsigned h;
    fi.mask = 2;
    fi.date = dt[0];
    fi.time = dt[1];
    if (!FAPI_FAILED(DosSetFInfo(p, &fi)))
        return;
    if (nocreate)
        return;
    h = DosCreate(p);
    if (FAPI_FAILED(h)) {
        puts("touch: kann nicht anlegen: ");
        puts(p);
        puts("\r\n");
        rc = 1;
        return;
    }
    DosClose(h);
    DosSetFInfo(p, &fi);
}

void main(void)
{
    char *a, w[FAPI_PATHLEN], d2[16];
    int want = 0, any = 0;
    unsigned n;

    DosGetDateTime(dt);
    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want == 'd') {
            char *q = skip(a);
            d2[0] = 0;
            if (*q >= '0' && *q <= '9') {           /* Uhrzeit dahinter */
                unsigned k;
                for (k = 0; q[k] > ' ' && k < sizeof(d2) - 1; k++)
                    d2[k] = q[k];
                d2[k] = 0;
                if (k && (d2[1] == ':' || d2[2] == ':'))
                    a = q + k;
                else
                    d2[0] = 0;
            }
            if (parse_date(w, d2)) {
                puts("touch: ung\x81ltiges Datum (TT.MM.JJJJ [hh:mm[:ss]])\r\n");
                DosExit(1);
            }
            want = 0;
            continue;
        }
        if (want == 'r') {
            if (!find_one(w, &e)) {
                puts("touch: Bezugsdatei fehlt: ");
                puts(w);
                puts("\r\n");
                DosExit(1);
            }
            dt[0] = e.date;
            dt[1] = e.time;
            want = 0;
            continue;
        }
        if (w[0] == '/' && w[1] == '?') {
            puts("Setzt Datum und Uhrzeit von Dateien, legt fehlende leer an.\r\n\r\n"
                 "TOUCH [-c] [-d TT.MM.JJJJ [hh:mm[:ss]]] [-r Bezug] Datei ...\r\n\r\n"
                 "  -c  keine Datei anlegen\r\n"
                 "  -d  diese Zeit statt jetzt\r\n"
                 "  -r  Zeit der Datei Bezug nehmen\r\n");
            return;
        }
        if (w[0] == '-' && w[1] == 'c' && !w[2]) { nocreate = 1; continue; }
        if (w[0] == '-' && (w[1] == 'd' || w[1] == 'r') && !w[2]) { want = w[1]; continue; }
        any = 1;
        if (has_wild(w)) {                      /* alle passenden         */
            char *b;
            scopy(dirp, w, sizeof(dirp));
            b = base_name(dirp);
            scopy(full, b, sizeof(full));
            if (b == dirp)
                scopy(dirp, ".", sizeof(dirp));
            else if (b - dirp > 1 && b[-2] != ':')
                b[-1] = 0;
            else
                *b = 0;
            for (n = 0; DosFindEntry(&e, n, dirp) != FAPI_ERROR; n++)
                if (!(e.attr & 0x18) && match(e.name, full)) {
                    char p[FAPI_PATHLEN];
                    join(p, dirp, e.name, sizeof(p));
                    touch(p);
                }
        } else
            touch(w);
    }
    if (!any) {
        puts("touch: Datei fehlt (TOUCH /? zeigt die Bedienung)\r\n");
        DosExit(1);
    }
    DosExit((unsigned)rc);
}
