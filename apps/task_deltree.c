/*
 * DELTREE.EXE - Verzeichnis mit allem Inhalt loeschen (wie MS-DOS 6):
 *
 *   DELTREE [/Y] [Laufwerk:]Pfad [[Laufwerk:]Pfad ...]
 *
 * Auch versteckte, System- und schreibgeschuetzte Dateien. Pfade duerfen
 * * und ? enthalten (DELTREE C:\TEMP\*.*). Ohne /Y wird je Eintrag gefragt.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], spec[FAPI_PATHLEN], path[FAPI_PATHLEN];
static char dir[FAPI_PATHLEN], pat[FAPI_PATHLEN];
static int yes, errors;

static void unprotect(const char *p)
{
    static struct fapi_finfo fi;
    fi.mask = 1;
    fi.attr = 0;
    DosSetFInfo(p, &fi);
}

static int del(char *p, struct fapi_dirent *e, int what, int depth)
{
    (void)depth;
    if (what == WT_DIR)
        return WT_DESCEND;
    if (what == WT_FILE) {
        if (e->attr & 0x07)
            unprotect(p);
        if (FAPI_FAILED(DosDelete(p))) {
            errors++;
            return 0;
        }
        return WT_REMOVED;
    }
    if (FAPI_FAILED(DosRmDir(p))) {         /* WT_LEAVE: Verzeichnis leer */
        errors++;
        return 0;
    }
    return WT_REMOVED;
}

/* ein Eintrag (Datei oder Verzeichnis) mit Rueckfrage; 1 = geloescht */
static int remove_one(char *p, unsigned attr)
{
    if (!yes) {
        puts(attr & 0x10 ? "Verzeichnis \"" : "Datei \"");
        puts(p);
        puts(attr & 0x10 ? "\" und alle Unterverzeichnisse l\x94schen? [J/N] "
                         : "\" l\x94schen? [J/N] ");
        if (!ask_yn())
            return 0;
    }
    puts(p);
    puts(" wird gel\x94scht...\r\n");
    if (attr & 0x10) {
        walk_tree(p, del);
        return 1;
    }
    if (attr & 0x07)
        unprotect(p);
    if (FAPI_FAILED(DosDelete(p))) {
        errors++;
        return 0;
    }
    return 1;
}

void main(void)
{
    static struct fapi_dirent e;
    char *a, *b;
    unsigned n, any = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a))
        if (a[0] == '/' && up(a[1]) == 'Y')
            yes = 1;
        else if (a[0] == '/' && a[1] == '?') {
            puts("L\x94scht ein Verzeichnis mit allen Dateien und Unterverzeichnissen.\r\n\r\n"
                 "DELTREE [/Y] [Laufwerk:]Pfad [[Laufwerk:]Pfad ...]\r\n\r\n"
                 "  /Y  ohne R\x81" "ckfrage\r\n");
            return;
        }
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/')
            continue;
        any = 1;
        word(a, spec, sizeof(spec));
        abs_path(spec, path);
        if (slen(path) <= 3) {
            puts("Das Stammverzeichnis wird nicht gel\x94scht.\r\n");
            continue;
        }
        if (!has_wild(path)) {
            if (is_dir(path))
                remove_one(path, 0x10);
            else if (!FAPI_FAILED(n = DosOpen(path))) {
                DosClose(n);
                remove_one(path, 0);
            } else {
                puts("Nicht gefunden - ");
                puts(spec);
                puts("\r\n");
            }
            continue;
        }
        scopy(dir, path, sizeof(dir));
        b = base_name(dir);
        scopy(pat, b, sizeof(pat));
        *b = 0;
        if (slen(dir) > 3)
            dir[slen(dir) - 1] = 0;
        for (n = 0; DosFindEntry(&e, n, dir) != FAPI_ERROR; ) {
            if ((e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2]))) || (e.attr & 0x08) || !match(e.name, pat)) {
                n++;
                continue;
            }
            join(path, dir, e.name, sizeof(path));
            if (!remove_one(path, e.attr))
                n++;
        }
    }
    if (!any) {
        puts("Syntax: DELTREE [/Y] [Laufwerk:]Pfad\r\n");
        DosExit(1);
    }
    DosExit(errors ? 1 : 0);
}
