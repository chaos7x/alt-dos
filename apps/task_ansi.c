/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ANSI.EXE - ANSI-Folgen in der Bildschirmausgabe ein-/ausschalten
 * (wie ANSI unter OS/2; die Auswertung steckt im Kernel, FAPI VioSetAnsi):
 *
 *   ANSI [ON | OFF]
 *
 * Gilt fuer die eigene Sitzung; neue Sitzungen beginnen mit ANSI ON.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];

void main(void)
{
    char *a;
    unsigned on;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Schaltet die Auswertung von ANSI-Steuerfolgen (Farben, Cursor)\r\n"
             "in der Bildschirmausgabe dieser Sitzung ein oder aus.\r\n\r\n"
             "ANSI [ON | OFF]\r\n\r\n"
             "Ohne Parameter wird der aktuelle Zustand angezeigt.\r\n");
        return;
    }
    if (!*a)
        on = VioSetAnsi(2);
    else if (same(a, "ON"))
        on = (VioSetAnsi(1), 1);
    else if (same(a, "OFF"))
        on = (VioSetAnsi(0), 0);
    else {
        puts("Ung\x81ltiger Parameter - ");
        puts(a);
        puts("\r\n");
        DosExit(1);
        return;
    }
    puts(on ? "ANSI-Steuerfolgen werden ausgewertet (ANSI ON).\r\n"
            : "ANSI-Steuerfolgen werden nicht ausgewertet (ANSI OFF).\r\n");
}
