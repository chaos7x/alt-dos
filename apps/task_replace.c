/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * REPLACE.EXE - Dateien ersetzen oder hinzufuegen (wie MS-DOS):
 *
 *   REPLACE [Laufwerk:][Pfad]Dateien [Laufwerk:][Pfad] [/A] [/P] [/R] [/S] [/W] [/U]
 *
 * Ohne /A: jede Quelldatei ersetzt die gleichnamige Datei im Ziel (mit /S
 * auch in allen Unterverzeichnissen des Ziels); mit /A werden nur Dateien
 * hinzugefuegt, die im Ziel fehlen. /U ersetzt nur aeltere, /R auch
 * schreibgeschuetzte, /P fragt, /W wartet auf eine Diskette.
 * Rueckgabe wie MS-DOS: 0 ok, 2 Quelle nicht gefunden, 3 Pfad nicht
 * gefunden, 8 kein Speicher, 11 Syntax.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], srcdir[FAPI_PATHLEN], pat[FAPI_PATHLEN], dst[FAPI_PATHLEN];
static char sfile[FAPI_PATHLEN], tfile[FAPI_PATHLEN], walkp[FAPI_PATHLEN];
static int o_add, o_prompt, o_ro, o_sub, o_wait, o_upd;
static unsigned done_n, errors;
static struct fapi_dirent se;           /* aktuelle Quelldatei           */

static int stamp_newer(unsigned short d1, unsigned short t1, unsigned short d2, unsigned short t2)
{
    return d1 > d2 || (d1 == d2 && t1 > t2);
}

static void replace_one(const char *target, struct fapi_dirent *te)
{
    static struct fapi_finfo fi;
    int r;
    if (o_upd && te && !stamp_newer(se.date, se.time, te->date, te->time))
        return;
    if (te && (te->attr & 0x01) && !o_ro) {
        puts("Zugriff verweigert - ");
        puts(target);
        puts("\r\n");
        errors++;
        return;
    }
    if (o_prompt) {
        puts(te ? "Ersetze " : "F\x81ge hinzu ");
        puts(target);
        puts("? (J/N) ");
        if (!ask_yn())
            return;
    } else {
        puts(te ? "Ersetze " : "F\x81ge hinzu ");
        puts(target);
        puts("\r\n");
    }
    if (te && (te->attr & 0x01)) {
        fi.mask = 1;
        fi.attr = te->attr & ~0x01;
        DosSetFInfo(target, &fi);
    }
    r = copy_file(sfile, target);
    if (r) {
        puts("Fehler beim Kopieren - ");
        puts(target);
        puts("\r\n");
        errors++;
    } else
        done_n++;
}

/* /S: im Zielbaum nach gleichnamigen Dateien suchen */
static int visit(char *path, struct fapi_dirent *e, int what, int depth)
{
    (void)depth;
    if (what == WT_DIR)
        return WT_DESCEND;
    if (what == WT_FILE && same(e->name, se.name))
        replace_one(path, e);
    return 0;
}

static int find_in(const char *dir, const char *name, struct fapi_dirent *out)
{
    unsigned n;
    for (n = 0; DosFindEntry(out, n, dir) != FAPI_ERROR; n++)
        if (!(out->attr & 0x18) && same(out->name, name))
            return 1;
    return 0;
}

void main(void)
{
    static struct fapi_dirent te;
    char *a, *b;
    unsigned n, any = 0;
    int have_src = 0, have_dst = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/') {
            switch (up(a[1])) {
            case '?':
                puts("Ersetzt Dateien.\r\n\r\n"
                     "REPLACE [Laufwerk1:][Pfad1]Dateien [Laufwerk2:][Pfad2] [/A] [/P] [/R] [/W]\r\n"
                     "REPLACE [Laufwerk1:][Pfad1]Dateien [Laufwerk2:][Pfad2] [/P] [/R] [/S] [/W] [/U]\r\n\r\n"
                     "  /A  nur neue Dateien hinzuf\x81gen       /P  vor jeder Datei fragen\r\n"
                     "  /R  auch schreibgesch\x81tzte ersetzen   /S  auch in Unterverzeichnissen\r\n"
                     "  /W  auf eine Diskette warten          /U  nur \x84ltere ersetzen\r\n");
                return;
            case 'A': o_add = 1; break;
            case 'P': o_prompt = 1; break;
            case 'R': o_ro = 1; break;
            case 'S': o_sub = 1; break;
            case 'W': o_wait = 1; break;
            case 'U': o_upd = 1; break;
            default:
                puts("Ung\x81ltiger Parameter - ");
                puts(a);
                puts("\r\n");
                DosExit(11);
            }
        } else if (!have_src) {
            word(a, sfile, sizeof(sfile));
            have_src = 1;
        } else if (!have_dst) {
            word(a, tfile, sizeof(tfile));
            have_dst = 1;
        }
    }
    if (!have_src) {
        puts("Quellpfad erforderlich\r\n");
        DosExit(11);
    }
    if (o_add && (o_sub || o_upd)) {
        puts("Ung\x81ltige Kombination von Parametern\r\n");
        DosExit(11);
    }
    if (o_wait) {
        puts("Dr\x81" "cken Sie eine beliebige Taste, um fortzusetzen . . .");
        KbdCharIn();
        puts("\r\n");
    }
    abs_path(sfile, srcdir);
    b = base_name(srcdir);
    scopy(pat, b, sizeof(pat));
    *b = 0;
    if (slen(srcdir) > 3)
        srcdir[slen(srcdir) - 1] = 0;
    if (have_dst)
        abs_path(tfile, dst);
    else
        abs_path(".", dst);
    if (!is_dir(dst)) {
        puts("Pfad nicht gefunden - ");
        puts(dst);
        puts("\r\n");
        DosExit(3);
    }
    for (n = 0; DosFindEntry(&se, n, srcdir) != FAPI_ERROR; n++) {
        if ((se.attr & 0x18) || !match(se.name, pat))
            continue;
        any = 1;
        join(sfile, srcdir, se.name, sizeof(sfile));
        if (o_add) {
            if (!find_in(dst, se.name, &te)) {
                join(tfile, dst, se.name, sizeof(tfile));
                replace_one(tfile, 0);
            }
        } else if (o_sub) {
            scopy(walkp, dst, sizeof(walkp));
            walk_tree(walkp, visit);
        } else if (find_in(dst, se.name, &te)) {
            join(tfile, dst, se.name, sizeof(tfile));
            replace_one(tfile, &te);
        }
    }
    if (!any) {
        puts("Keine Dateien gefunden - ");
        puts(pat);
        puts("\r\n");
        DosExit(2);
    }
    if (!done_n)
        puts(o_add ? "Keine Datei hinzugef\x81gt\r\n" : "Keine Datei ersetzt\r\n");
    else {
        put_num(done_n, 0, 0);
        puts(o_add ? " Datei(en) hinzugef\x81gt\r\n" : " Datei(en) ersetzt\r\n");
    }
    DosExit(errors ? 1 : 0);
}
