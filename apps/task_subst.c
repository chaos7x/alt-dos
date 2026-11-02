/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SUBST.EXE - Laufwerksbuchstabe fuer ein Verzeichnis (wie MS-DOS):
 *
 *   SUBST [Laufwerk1: [Laufwerk2:]Pfad]
 *   SUBST Laufwerk1: /D
 *
 * Ohne Angaben zeigt SUBST alle Zuordnungen ("X: => C:\DOS"). Den Pfad
 * setzt der Kernel um (FAPI DosSubst/DosQSubst); CHKDSK, FORMAT und
 * LABEL lehnen SUBST-Laufwerke ab, weil sie kein eigenes Laufwerk sind.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], path[FAPI_PATHLEN], buf[140];

static void usage(void)
{
    puts("Ordnet einem Pfad eine Laufwerksbezeichnung zu.\r\n\r\n"
         "SUBST [Laufwerk1: [Laufwerk2:]Pfad]\r\n"
         "SUBST Laufwerk1: /D\r\n\r\n"
         "  Laufwerk1:        Laufwerk, das dem Pfad zugeordnet werden soll\r\n"
         "  [Laufwerk2:]Pfad  Laufwerk und Pfad, die Laufwerk1 zugeordnet werden\r\n"
         "  /D                hebt eine Zuordnung auf\r\n\r\n"
         "SUBST ohne Parameter zeigt die aktuellen Zuordnungen.\r\n");
}

void main(void)
{
    char *a, *p;
    unsigned d, r, n = 0;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        usage();
        return;
    }
    if (!*a) {                              /* Liste                     */
        for (d = 0; d < 26; d++)
            if (DosQSubst(d, buf) == 0) {
                putc((char)('A' + d));
                puts(": => ");
                puts(buf);
                puts("\r\n");
                n++;
            }
        return;
    }
    if (a[1] != ':' || up(a[0]) < 'A' || up(a[0]) > 'Z' || (a[2] && a[2] != ' ')) {
        puts("Ung\x81ltiger Parameter - ");
        puts(a);
        puts("\r\n");
        DosExit(1);
    }
    d = (unsigned)(up(a[0]) - 'A');
    p = skip(a + 2);
    if (p[0] == '/' && up(p[1]) == 'D' && !*skip(p + 2)) {
        r = DosSubst(d, 0);
        if (r) {
            puts(r == FAPI_ERR_INUSE ? "Das Laufwerk ist das aktuelle Laufwerk eines Programms.\r\n"
                                     : "Ung\x81ltiger Parameter - keine Zuordnung f\x81r dieses Laufwerk.\r\n");
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
    if (DosQSubst(d, buf) == 0) {
        puts("Laufwerk ist bereits zugeordnet (SUBST ");
        putc((char)('A' + d));
        puts(": /D hebt die Zuordnung auf).\r\n");
        DosExit(1);
    }
    if (DosSubst(d, path)) {
        puts("Ung\x81ltiger Parameter - Laufwerk vorhanden oder Pfad nicht gefunden.\r\n");
        DosExit(1);
    }
}
