/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * JOIN.EXE - Laufwerk in ein Verzeichnis einhaengen (wie MS-DOS 3.1-5.0):
 *
 *   JOIN [Laufwerk1: [Laufwerk2:]Pfad]
 *   JOIN Laufwerk1: /D
 *
 * Ohne Angaben zeigt JOIN alle Verbindungen ("D: => C:\CDROM"). Das
 * Verzeichnis muss leer sein; fehlt es, legt der Kernel es an. Danach ist
 * Laufwerk1 nur noch ueber den Pfad erreichbar (FAPI DosJoin/DosQJoin).
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], path[FAPI_PATHLEN], buf[FAPI_CURDIRLEN];

static void usage(void)
{
    puts("Verbindet ein Laufwerk mit einem Verzeichnis eines anderen Laufwerks.\r\n\r\n"
         "JOIN [Laufwerk1: [Laufwerk2:]Pfad]\r\n"
         "JOIN Laufwerk1: /D\r\n\r\n"
         "  Laufwerk1:        Laufwerk, das mit dem Pfad verbunden werden soll\r\n"
         "  [Laufwerk2:]Pfad  leeres Verzeichnis, unter dem Laufwerk1 erscheint\r\n"
         "  /D                hebt eine Verbindung auf\r\n\r\n"
         "JOIN ohne Parameter zeigt die aktuellen Verbindungen.\r\n");
}

static void bad(const char *a)
{
    puts("Ung\x81ltiger Parameter - ");
    puts(a);
    puts("\r\n");
    DosExit(1);
}

void main(void)
{
    char *a, *p;
    unsigned d, r;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        usage();
        return;
    }
    if (!*a) {                              /* Liste                     */
        for (d = 0; d < 26; d++)
            if (DosQJoin(d, buf) == 0) {
                putc((char)('A' + d));
                puts(": => ");
                puts(buf);
                puts("\r\n");
            }
        return;
    }
    if (a[1] != ':' || up(a[0]) < 'A' || up(a[0]) > 'Z' || (a[2] && a[2] != ' '))
        bad(a);
    d = (unsigned)(up(a[0]) - 'A');
    p = skip(a + 2);
    if (p[0] == '/' && up(p[1]) == 'D' && !*skip(p + 2)) {
        r = DosJoin(d, 0);
        if (r == FAPI_ERR_INUSE) {
            puts("Das Laufwerk wird benutzt (aktuelles Verzeichnis darin oder offene Dateien).\r\n");
            DosExit(1);
        }
        if (r) {
            puts("Ung\x81ltiger Parameter - das Laufwerk ist nicht verbunden.\r\n");
            DosExit(1);
        }
        return;
    }
    if (!*p) {
        puts("Falsche Anzahl von Parametern\r\n");
        DosExit(1);
    }
    word(p, path, sizeof(path));
    if (*skip(next_word(p))) {
        puts("Zu viele Parameter\r\n");
        DosExit(1);
    }
    if (DosQJoin(d, buf) == 0) {
        puts("Laufwerk ist bereits verbunden (JOIN ");
        putc((char)('A' + d));
        puts(": /D hebt die Verbindung auf).\r\n");
        DosExit(1);
    }
    r = DosJoin(d, path);
    if (r == FAPI_ERR_DENIED) {
        puts("Das Verzeichnis ist nicht leer.\r\n");
        DosExit(1);
    }
    if (r == FAPI_ERR_INUSE) {
        puts("Das Laufwerk wird benutzt (aktuelles Laufwerk, offene Dateien,\r\n"
             "SUBST oder JOIN darauf).\r\n");
        DosExit(1);
    }
    if (r) {
        puts("Verbindung nicht m\x94glich - Laufwerk fehlt, ist das Startlaufwerk,\r\n"
             "oder der Pfad ist ung\x81ltig (nicht der Stamm, anderes Laufwerk).\r\n");
        DosExit(1);
    }
}
