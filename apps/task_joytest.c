/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * JOYTEST.EXE - Gameport anzeigen (AT-DOS 6.3, FAPI_JOYSTICK):
 *
 *   JOYTEST [/1]
 *
 * Zeigt Tasten und die vier Achsen (Zeit in us bis zum Ende der Messung,
 * "--" = Achse nicht angeschlossen) laufend, bis eine Taste kommt; /1
 * misst einmal (fuer Stapeldateien). ERRORLEVEL 1: kein Gameport.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static struct fapi_joy j;

static void show(void)
{
    unsigned i;
    puts("Tasten ");
    for (i = 0; i < 4; i++)
        putc(j.buttons & (1 << i) ? (char)('1' + i) : '-');
    puts("  X1 Y1 X2 Y2:");
    for (i = 0; i < 4; i++) {
        if (j.axis[i] == 0xFFFF)
            puts("     --");
        else
            put_num(j.axis[i], 7, 0);
    }
    puts(" us");
}

void main(void)
{
    char *a;
    int once = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt Gameport-Tasten und -Achsen.\r\n\r\n"
                 "JOYTEST [/1]     /1 = einmal messen\r\n");
            return;
        }
        if (a[0] == '/' && a[1] == '1')
            once = 1;
    }
    if (FAPI_FAILED(DosJoystick(&j))) {
        puts("Kein Gameport gefunden.\r\n");
        DosExit(1);
    }
    if (once) {
        show();
        puts("\r\n");
        return;
    }
    puts("Taste beendet.\r\n");
    while (!FAPI_FAILED(DosJoystick(&j))) {
        show();
        puts("\r");
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        DosSleep(100);
    }
    puts("\r\n");
}
