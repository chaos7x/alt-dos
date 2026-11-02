/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MAUSTEST.EXE - zeigt den Mauszeiger und jedes Ereignis (MouOpen,
 * MouReadEvent, MouShowPtr). Beenden mit einer Taste.
 */
#include "fapi.h"
#include "util.h"

static struct fapi_mouevent ev;

void main(void)
{
    unsigned n = MouOpen();

    if (!n) {
        puts("Keine Maus (DEVICE=MOUSE.SYS in der CONFIG.SYS?)\r\n");
        DosExit(1);
    }
    put_num(n, 0, 0);
    puts(" Tasten. Maus bewegen und klicken, Ende mit einer Taste.\r\n");
    MouShowPtr(1);
    for (;;) {
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        if (MouReadEvent(&ev, 0) != 0) {
            DosSleep(20);
            continue;
        }
        puts("Zeile ");
        if (ev.row == 0xFF)
            puts("--");
        else
            put_num(ev.row, 2, 0);
        puts(" Spalte ");
        put_num(ev.col, 2, 0);
        puts(ev.buttons & MOU_LEFT ? "  links" : "       ");
        puts(ev.buttons & MOU_MIDDLE ? " mitte" : "      ");
        puts(ev.buttons & MOU_RIGHT ? " rechts" : "");
        puts("\r\n");
    }
    MouShowPtr(0);
}
