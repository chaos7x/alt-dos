/*
 * MORE.EXE - seitenweise anzeigen, wie MS-DOS 6:
 *   MORE < DATEI      befehl | MORE      MORE DATEI
 * Nach jeder Bildschirmseite "-- Mehr --"; die Frage steht auf dem
 * Bildschirm (VioWrtTTY), nicht in der Standardausgabe. Rueckwaerts
 * blaettern kann LESS.
 */
#include "fapi.h"

static unsigned ROWS = 22;     /* Zeilen der Sitzung minus eine fuer die Frage */
#define COLS 80

static char in[512], out[512];
static unsigned on;

static void flush(void)
{
    if (on)
        DosWrite(STDOUT, out, on);
    on = 0;
}

void main(void)
{
    static char args[FAPI_ARGLEN];
    unsigned h = STDIN, n, i, lines = 0, col = 0;
    char *a = args, c;

    ROWS = VioGetRows();
    ROWS = FAPI_FAILED(ROWS) || ROWS < 3 ? 22 : ROWS - 1;
    DosGetArgs(args);
    while (*a == ' ')
        a++;
    if (*a) {
        char *e = a;
        while (*e && *e != ' ')
            e++;
        *e = 0;
        h = DosOpen(a);
        if (h == FAPI_ERROR) {
            puts("Datei nicht gefunden.\n");
            return;
        }
    }
    while ((n = DosRead(h, in, sizeof(in))) != 0 && n != FAPI_ERROR) {
        for (i = 0; i < n; i++) {
            c = in[i];
            if (c == 0x1A)
                break;
            out[on++] = c;
            if (on == sizeof(out))
                flush();
            if (c == '\n') {
                lines++;
                col = 0;
            } else if (c != '\r' && ++col == COLS) {
                lines++;
                col = 0;
            }
            if (lines >= ROWS) {
                flush();
                VioWrtTTY("-- Mehr --", 10);
                KbdCharIn();
                VioWrtTTY("\r          \r", 12);
                lines = 0;
            }
        }
    }
    flush();
    if (h != STDIN)
        DosClose(h);
}
