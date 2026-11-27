/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TIMEOUT.EXE - in Stapeldateien warten, wie timeout von Windows:
 *
 *   TIMEOUT /T Sekunden [/NOBREAK]
 *
 * Zaehlt sichtbar herunter; eine Taste bricht ab (mit /NOBREAK nur
 * Strg+C). /T -1 wartet auf eine Taste.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];

static void put_u(unsigned v)
{
    char b[6];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        putc(b[--n]);
}

void main(void)
{
    char *a, w[16];
    int t = -2, nobreak = 0, want = 0, neg;
    unsigned k, s;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (want || (w[0] >= '0' && w[0] <= '9') || (w[0] == '-' && w[1] >= '0')) {
            neg = w[0] == '-';
            t = 0;
            for (k = neg; w[k] >= '0' && w[k] <= '9'; k++)
                t = t * 10 + (w[k] - '0');
            if (neg) t = -t;
            want = 0;
            continue;
        }
        if (w[0] == '/' && w[1] == '?') {
            puts("Wartet die angegebene Zeit oder bis zu einem Tastendruck.\r\n\r\n"
                 "TIMEOUT /T Sekunden [/NOBREAK]\r\n\r\n"
                 "  /T        Sekunden (-1 = auf eine Taste warten), bis 99999\r\n"
                 "  /NOBREAK  Tasten nicht beachten (nur Strg+C)\r\n");
            return;
        }
        if ((w[0] == '/' || w[0] == '-') && up(w[1]) == 'T' && !w[2]) { want = 1; continue; }
        if ((w[0] == '/' || w[0] == '-') && same(w + 1, "NOBREAK")) { nobreak = 1; continue; }
        puts("FEHLER: Ung\x81ltige Syntax. TIMEOUT /? zeigt die Bedienung.\r\n");
        DosExit(1);
    }
    if (t == -2) {
        puts("FEHLER: Ung\x81ltige Syntax. TIMEOUT /? zeigt die Bedienung.\r\n");
        DosExit(1);
    }
    if (t < 0) {
        puts("Dr\x81" "cken Sie eine beliebige Taste . . .");
        KbdCharIn();
        puts("\r\n");
        return;
    }
    for (s = (unsigned)t; ; s--) {
        puts("\rWartezeit: ");
        put_u(s);
        puts(nobreak ? " Sekunden, Strg+C zum Beenden . . . " :
                       " Sekunden, weiter mit beliebiger Taste . . . ");
        if (!s)
            break;
        for (k = 0; k < 10; k++) {
            DosSleep(100);
            if (!nobreak && KbdPeek()) {
                KbdCharIn();
                puts("\r\n");
                return;
            }
        }
    }
    puts("\r\n");
}
