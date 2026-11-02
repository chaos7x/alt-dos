/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATTRIB.EXE - Dateiattribute anzeigen und aendern, wie MS-DOS:
 *
 *   ATTRIB [+R|-R] [+A|-A] [+S|-S] [+H|-H] [[Laufwerk:][Pfad]Datei] [/S]
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], spec[FAPI_PATHLEN], dir[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static unsigned set_m, clr_m, found, show_dir;
static int recurse;

static int visit(char *path, struct fapi_dirent *e, int what, int depth)
{
    static struct fapi_finfo fi;
    unsigned a;
    (void)depth;
    if (what == WT_LEAVE)
        return 0;
    if (what == WT_DIR && !match(e->name, pat))
        return recurse ? WT_DESCEND : 0;
    if (!match(e->name, pat))
        return 0;
    found++;
    a = e->attr;
    if (set_m | clr_m) {
        if (what == WT_DIR && !((set_m | clr_m) & 0x06))
            return recurse ? WT_DESCEND : 0;    /* Verzeichnis: nur S/H */
        fi.mask = 1;
        fi.attr = (a | set_m) & ~clr_m;
        if (FAPI_FAILED(DosSetFInfo(path, &fi))) {
            puts("Zugriff verweigert - ");
            puts(path);
            puts("\r\n");
        }
    } else if (what == WT_FILE || show_dir) {
        putc(a & 0x20 ? 'A' : ' ');
        puts("    ");
        putc(a & 0x04 ? 'S' : ' ');
        putc(a & 0x02 ? 'H' : ' ');
        putc(a & 0x01 ? 'R' : ' ');
        puts("     ");
        puts(path);
        puts("\r\n");
    }
    return (what == WT_DIR && recurse) ? WT_DESCEND : 0;
}

void main(void)
{
    char *a, *b;
    unsigned m;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '+' || a[0] == '-') {
            switch (up(a[1])) {
            case 'R': m = 0x01; break;
            case 'H': m = 0x02; break;
            case 'S': m = 0x04; break;
            case 'A': m = 0x20; break;
            default:
                puts("Ung\x81ltiger Parameter - ");
                puts(a);
                puts("\r\n");
                DosExit(1);
            }
            if (a[0] == '+')
                set_m |= m;
            else
                clr_m |= m;
        } else if (a[0] == '/' && up(a[1]) == 'S') {
            recurse = 1;
        } else if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt oder \x84ndert Dateiattribute.\r\n\r\n"
                 "ATTRIB [+R|-R] [+A|-A] [+S|-S] [+H|-H] [[Laufwerk:][Pfad]Datei] [/S]\r\n\r\n"
                 "  +  Attribut setzen     -  Attribut l\x94schen\r\n"
                 "  R  Schreibgesch\x81tzt   A  Archiv   S  System   H  Versteckt\r\n"
                 "  /S auch in allen Unterverzeichnissen\r\n");
            return;
        } else {
            word(a, spec, sizeof(spec));
        }
    }
    if (!spec[0])
        scopy(spec, "*.*", sizeof(spec));
    abs_path(spec, dir);
    if (!has_wild(dir) && is_dir(dir) && !base_name(dir)[0]) {
        scopy(pat, "*.*", sizeof(pat));     /* Stammverzeichnis           */
    } else {
        if (!has_wild(dir) && is_dir(dir))
            show_dir = 1;                   /* wie NT: das Verzeichnis selbst */
        b = base_name(dir);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(dir) > 3)
            dir[slen(dir) - 1] = 0;     /* "C:\DOS\" -> "C:\DOS"       */
    }
    walk_tree(dir, visit);
    if (!found) {
        puts("Datei nicht gefunden - ");
        puts(spec);
        puts("\r\n");
        DosExit(1);
    }
}
