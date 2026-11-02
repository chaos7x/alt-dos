/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BUSY.EXE - rechnet, ohne die FAPI zu rufen (Test fuer praeemptives
 * Multitasking, AT-DOS 6.0):
 *
 *   BUSY [Runden] [/W]
 *
 * Jede Runde sind 65536 Schritte reine Rechnerei. Unter 5.0 haelt das
 * das ganze System an; ab 6.0 unterbricht der Takt-Interrupt das
 * Programm nach seiner Zeitscheibe, andere Sitzungen bleiben bedienbar
 * und Strg+C bricht ab. /W wartet am Ende auf eine Taste.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static volatile unsigned sum;

void main(void)
{
    char *a;
    unsigned runden = 200, r, i, w = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Rechnet ohne FAPI-Aufrufe (Test f\x81r pr\x84" "emptives Multitasking).\r\n\r\n"
                 "BUSY [Runden] [/W]     Runde = 65536 Schritte, Vorgabe 200\r\n");
            return;
        }
        if (a[0] == '/' && up(a[1]) == 'W')
            w = 1;
        else if (*a >= '0' && *a <= '9')
            for (runden = 0; *a >= '0' && *a <= '9'; a++)
                runden = runden * 10 + (*a - '0');
    }
    puts("BUSY rechnet ...\r\n");
    for (r = 0; r < runden; r++)
        for (i = 0; i < 65535U; i++)
            sum += i ^ r;               /* keine FAPI-Aufrufe            */
    puts("BUSY fertig.\r\n");
    if (w)
        KbdCharIn();
}
