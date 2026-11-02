/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TREE.EXE - Verzeichnisstruktur grafisch anzeigen (wie MS-DOS):
 *
 *   TREE [Laufwerk:][Pfad] [/F] [/A]
 *
 *   /F  auch die Dateien     /A  ASCII statt Rahmenzeichen
 */
#include "fapi.h"
#include "util.h"

#define MAXD 24

static char args[FAPI_ARGLEN], spec[FAPI_PATHLEN], path[FAPI_PATHLEN];
static char pre[MAXD * 4 + 8];
static int files, ascii, any;
static struct fapi_dirent e;

static int is_sub(void)
{
    return (e.attr & 0x10) && !(e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2])));
}

/* naechstes Unterverzeichnis ab Index n -> Index oder -1 */
static int next_dir(unsigned n)
{
    for (; DosFindEntry(&e, n, path) != FAPI_ERROR; n++)
        if (is_sub())
            return (int)n;
    return -1;
}

static void list_files(void)
{
    unsigned n, shown = 0;
    int more = next_dir(0) >= 0;
    for (n = 0; DosFindEntry(&e, n, path) != FAPI_ERROR; n++) {
        if (e.attr & 0x1E)              /* wie MS-DOS: ohne versteckte,
                                           System, Bezeichnung */
            continue;
        puts(pre);
        puts(more ? (ascii ? "|   " : "\xB3   ") : "    ");
        puts(e.name);
        puts("\r\n");
        shown++;
    }
    if (shown) {
        puts(pre);
        puts(more ? (ascii ? "|" : "\xB3") : "");
        puts("\r\n");
    }
}

void main(void)
{
    static struct fapi_label lb;
    static unsigned idx[MAXD], plen[MAXD], prel[MAXD];
    static char name[FAPI_PATHLEN];
    char *a;
    int depth = 0, i, last;
    unsigned d;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a))
        if (a[0] == '/' && up(a[1]) == 'F')
            files = 1;
        else if (a[0] == '/' && up(a[1]) == 'A')
            ascii = 1;
        else if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt die Verzeichnisstruktur eines Laufwerks oder Pfads.\r\n\r\n"
                 "TREE [Laufwerk:][Pfad] [/F] [/A]\r\n\r\n"
                 "  /F  auch die Dateien anzeigen\r\n"
                 "  /A  ASCII- statt Rahmenzeichen\r\n");
            return;
        } else
            word(a, spec, sizeof(spec));
    abs_path(spec[0] ? spec : ".", path);
    if (!is_dir(path)) {
        puts("Ung\x81ltiger Pfad - ");
        puts(spec);
        puts("\r\n");
        DosExit(1);
    }
    d = (unsigned)(up(path[0]) - 'A');
    puts("Verzeichnisliste\r\n");
    if (DosLabel(d, &lb, 0) == 0 && lb.serial) {
        static char hx[] = "0123456789ABCDEF";
        int k;
        puts("Datentr\x84ger-Seriennummer ist ");
        for (k = 28; k >= 0; k -= 4) {
            putc(hx[(unsigned)(lb.serial >> k) & 15]);
            if (k == 16)
                putc('-');
        }
        puts("\r\n");
    }
    puts(spec[0] ? path : "");
    if (!spec[0]) {
        putc(path[0]);
        puts(":.");
    }
    puts("\r\n");

    idx[0] = 0;
    prel[0] = 0;
    pre[0] = 0;
    if (files)
        list_files();
    for (;;) {
        i = next_dir(idx[depth]);
        if (i < 0) {                        /* Ebene fertig             */
            if (depth == 0)
                break;
            depth--;
            path[plen[depth]] = 0;
            pre[prel[depth]] = 0;
            continue;
        }
        any = 1;
        scopy(name, e.name, sizeof(name));
        idx[depth] = (unsigned)i + 1;
        last = next_dir((unsigned)i + 1) < 0;
        puts(pre);
        puts(ascii ? (last ? "\\---" : "+---") : (last ? "\xC0\xC4\xC4\xC4" : "\xC3\xC4\xC4\xC4"));
        puts(name);
        puts("\r\n");
        if (depth + 1 >= MAXD)
            continue;
        plen[depth] = slen(path);
        prel[depth] = slen(pre);
        join(path, path, name, sizeof(path));
        scat(pre, last ? "    " : (ascii ? "|   " : "\xB3   "), sizeof(pre));
        depth++;
        idx[depth] = 0;
        if (files)
            list_files();
    }
    if (!any)
        puts("Keine Unterverzeichnisse vorhanden\r\n");
}
