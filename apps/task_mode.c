/*
 * MODE.EXE - Bildschirm einstellen (wie MS-DOS 5/6, Teil CON):
 *
 *   MODE CON[:] [COLS=80] [LINES=n]     n = 25, 43 oder 50 (43/50 nur VGA)
 *   MODE CON[:]                         Zustand anzeigen
 *   MODE CO80 | BW80 | 80               zurueck auf 80x25
 *
 * Wie bei OS/2 gilt die Zeilenzahl fuer die eigene Sitzung; andere
 * Sitzungen behalten ihre. Der Bildschirm wird dabei geloescht.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], w[40];

static void status(void)
{
    unsigned n = VioSetLines(0);
    puts("\r\nStatus f\x81r Ger\x84t CON:\r\n"
         "----------------------\r\n"
         "Spalten=80\r\n"
         "Zeilen=");
    put_num(n, 0, 0);
    puts("\r\n");
}

static void usage(void)
{
    puts("Stellt den Bildschirm ein.\r\n\r\n"
         "MODE CON[:] [COLS=80] [LINES=n]   n = 25, 43 oder 50 (43/50 nur VGA)\r\n"
         "MODE CON[:]                       Zustand anzeigen\r\n"
         "MODE CO80 | BW80 | 80             80 Spalten, 25 Zeilen\r\n\r\n"
         "Die Zeilenzahl gilt f\x81r die eigene Sitzung.\r\n");
}

static int set_lines(unsigned n)
{
    if (FAPI_FAILED(VioSetLines(n))) {
        if (n == 25)
            puts("MODE: Zeilenzahl kann nicht ge\x84ndert werden (Vollbild?).\r\n");
        else
            puts("MODE: Diese Zeilenzahl kann die Grafikkarte nicht (43 und 50 nur mit VGA).\r\n");
        DosExit(1);
    }
    return 0;
}

void main(void)
{
    char *a, *v;
    unsigned lines = 0, n;
    int con = 0, any = 0;

    DosGetArgs(args);
    a = skip(args);
    if (!*a) {
        status();
        return;
    }
    while (*a) {
        a = word(a, w, sizeof(w));
        a = skip(a);
        for (v = w; *v; v++)
            *v = up(*v);
        if (same(w, "/?")) {
            usage();
            return;
        }
        if (same(w, "CON") || same(w, "CON:")) {
            con = 1;
            continue;
        }
        if (same(w, "CO80") || same(w, "BW80") || same(w, "80")) {
            lines = 25;
            any = 1;
            continue;
        }
        if (con && w[0] == 'L' && w[1] == 'I' && w[2] == 'N' && w[3] == 'E' &&
            w[4] == 'S' && w[5] == '=') {
            for (n = 0, v = w + 6; *v >= '0' && *v <= '9'; v++)
                n = n * 10 + (unsigned)(*v - '0');
            if (*v || (n != 25 && n != 43 && n != 50)) {
                puts("MODE: Zeilen m\x81ssen 25, 43 oder 50 sein.\r\n");
                DosExit(1);
            }
            lines = n;
            any = 1;
            continue;
        }
        if (con && w[0] == 'C' && w[1] == 'O' && w[2] == 'L' && w[3] == 'S' &&
            w[4] == '=') {
            if (!same(w + 5, "80")) {
                puts("MODE: Nur 80 Spalten m\x94glich.\r\n");
                DosExit(1);
            }
            any = 1;
            continue;
        }
        puts("MODE: Ung\x81ltiger Parameter - ");
        puts(w);
        puts("\r\n");
        DosExit(1);
    }
    if (!any) {
        status();
        return;
    }
    if (lines)
        set_lines(lines);
}
