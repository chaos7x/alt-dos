/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TASKA: zaehlt und gibt nach jedem Durchlauf per DosYield() ab.
   Zeigt Signale: Strg+C und Herunterfahren werden abgefangen. */
#include "fapi.h"

static volatile int stop;

static void on_signal(unsigned sig)
{
    if (sig == SIG_BREAK)
        puts("\nStrg+C abgefangen - TASKA r\x84umt auf und endet.\n");
    else
        puts("\nSystem f\x84hrt herunter - TASKA r\x84umt auf und endet.\n");
    stop = 1;
}

static char cnt[] = "0000000";

static void count_up(char *d)
{
    int i = 6;
    while (i >= 0) {
        if (d[i] == '9') { d[i] = '0'; i--; }
        else             { d[i]++;     break; }
    }
}

void main(void)
{
    puts("TASKA: z\x84hlt und gibt nach jedem Durchlauf mit DosYield() ab.\n"
         "Eine Taste beendet das Programm, Strg+C bricht es ab.\n\n");
    DosSetSigHandler(on_signal, SIG_BREAK | SIG_SHUTDOWN);
    while (!stop) {
        count_up(cnt);
        puts("\rDurchl\x84ufe: ");
        puts(cnt);
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        DosYield();
    }
    puts("\n");
}
