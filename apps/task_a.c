/* TASKA: zaehlt und gibt nach jedem Durchlauf per DosYield() ab. */
#include "fapi.h"

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
    for (;;) {
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
