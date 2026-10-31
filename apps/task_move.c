/*
 * MOVE.EXE - Dateien verschieben, Verzeichnisse umbenennen (wie MS-DOS 6):
 *
 *   MOVE [/Y] Quelle Ziel
 *
 * Quelle darf * und ? enthalten; ist Ziel ein Verzeichnis, landen die
 * Dateien darin. Auf demselben Laufwerk wird nur der Eintrag umgehaengt,
 * sonst kopiert und geloescht. Vorhandene Ziele: Frage (Ja/Nein/Alle),
 * mit /Y ohne.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], src[FAPI_PATHLEN], dst[FAPI_PATHLEN];
static char sdir[FAPI_PATHLEN], pat[16], from[FAPI_PATHLEN], to[FAPI_PATHLEN];
static int yes_all, moved, errors;

static int ask_overwrite(const char *p)
{
    unsigned c;
    if (yes_all)
        return 1;
    puts("\x9A" "berschreiben ");
    puts(p);
    puts(" (Ja/Nein/Alle)? ");
    for (;;) {
        c = (unsigned)up((char)KbdCharIn());
        if (c == 'J' || c == 'Y') { puts("J\r\n"); return 1; }
        if (c == 'N' || c == 0) { puts("N\r\n"); return 0; }
        if (c == 'A') { puts("A\r\n"); yes_all = 1; return 1; }
    }
}

static void move_one(const char *f, const char *t)
{
    static struct fapi_dirent e;
    unsigned r;

    puts(f);
    puts(" => ");
    puts(t);
    if (DosFindEntry(&e, 0, t) == FAPI_ERROR) {   /* Ziel ist keine Datei? */
        r = DosOpen(t);
        if (!FAPI_FAILED(r)) {
            DosClose(r);
            puts("\r\n");
            if (!ask_overwrite(t))
                return;
            DosDelete(t);
            puts(f);
            puts(" => ");
            puts(t);
        }
    }
    if (up(f[0]) == up(t[0])) {
        r = DosMove(f, t);
    } else {
        r = copy_file(f, t) ? FAPI_ERROR : DosDelete(f);
    }
    if (FAPI_FAILED(r)) {
        puts(" [Fehler]\r\n");
        errors++;
    } else {
        puts(" [ok]\r\n");
        moved++;
    }
}

void main(void)
{
    static struct fapi_dirent e;
    char *a, *b;
    unsigned n;
    int to_dir, k;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && up(a[1]) == 'Y')
            yes_all = 1;
        else if (a[0] == '/' && a[1] == '-' && up(a[2]) == 'Y')
            yes_all = 0;
        else if (a[0] == '/' && a[1] == '?') {
            puts("Verschiebt Dateien und benennt Verzeichnisse um.\r\n\r\n"
                 "MOVE [/Y | /-Y] Quelle Ziel\r\n\r\n"
                 "  /Y   vorhandene Ziele ohne Frage ersetzen\r\n");
            return;
        } else if (!src[0])
            word(a, src, sizeof(src));
        else
            word(a, dst, sizeof(dst));
    }
    if (!src[0] || !dst[0]) {
        puts("Syntax: MOVE [/Y] Quelle Ziel\r\n");
        DosExit(1);
    }
    abs_path(src, from);
    abs_path(dst, to);
    to_dir = is_dir(to);
    if (!has_wild(from)) {
        if (to_dir && !is_dir(from)) {      /* Datei in ein Verzeichnis */
            join(dst, to, base_name(from), sizeof(dst));
            move_one(from, dst);
        } else {
            move_one(from, to);             /* umbenennen/verschieben   */
        }
        DosExit(errors ? 1 : 0);
    }
    if (!to_dir && has_wild(from)) {
        puts("Ziel muss ein Verzeichnis sein.\r\n");
        DosExit(1);
    }
    scopy(sdir, from, sizeof(sdir));
    b = base_name(sdir);
    scopy(pat, b, sizeof(pat));
    *b = 0;
    if (slen(sdir) > 3)
        sdir[slen(sdir) - 1] = 0;
    for (n = 0; DosFindEntry(&e, n, sdir) != FAPI_ERROR; ) {
        if ((e.attr & 0x18) || !match(e.name, pat)) {
            n++;
            continue;
        }
        join(from, sdir, e.name, sizeof(from));
        join(dst, to, e.name, sizeof(dst));
        if (up(from[0]) == up(dst[0]) && slen(sdir) == slen(to)) {
            n++;                        /* gleiches Verzeichnis: nichts */
            continue;
        }
        k = moved;
        move_one(from, dst);
        if (moved == k)
            n++;                        /* Eintrag bleibt stehen        */
    }
    if (!moved && !errors) {
        puts("Datei nicht gefunden - ");
        puts(src);
        puts("\r\n");
        DosExit(1);
    }
    DosExit(errors ? 1 : 0);
}
