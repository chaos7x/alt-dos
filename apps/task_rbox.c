/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RBOX.EXE - Kompatibilitaetsbox (5.1, Stufe 2): ein .COM im echten Real
 * Mode, noch ohne DOS. RBOX Datei.COM - laedt das Programm nach 1000:0100
 * und laesst es laufen, solange die Sitzung vorn ist (Alt+Esc wechselt,
 * zurueck in die Sitzung setzt die Box fort).
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN], buf[512];

void main(void)
{
    char *a;
    unsigned h, sel, n, i, len = 0, r;
    char __far *seg;

    DosGetArgs(args);
    a = skip(args);
    if (!*a || (a[0] == '/' && a[1] == '?')) {
        puts("Startet ein .COM in der Kompatibilitaetsbox (Real Mode).\r\n\r\n"
             "RBOX Datei.COM\r\n");
        return;
    }
    h = DosOpen(a);
    if (FAPI_FAILED(h)) {
        puts("Datei nicht gefunden.\r\n");
        DosExit(2);
    }
    sel = DosAllocSeg(0);
    if (FAPI_FAILED(sel)) {
        puts("Zu wenig Arbeitsspeicher.\r\n");
        DosExit(3);
    }
    seg = (char __far *)((unsigned long)sel << 16);
    for (;;) {
        n = DosRead(h, buf, sizeof(buf));
        if (FAPI_FAILED(n) || n == 0)
            break;
        if (len + n > 0xFE00) {
            puts("Programm zu gross.\r\n");
            DosExit(3);
        }
        for (i = 0; i < n; i++)
            seg[len + i] = buf[i];
        len += n;
    }
    DosClose(h);
    r = DosRBox(RB_LOAD, sel, len);
    if (r) {
        puts(r == FAPI_ERR_INUSE ? "Speicher unter 640 KB ist belegt.\r\n"
                                 : "Die Box geht hier nicht (Kernel unter 1 MB oder schon belegt).\r\n");
        DosExit(3);
    }
    while ((r = DosRBox(RB_RUN, 0, 0)) == RB_SWITCH)
        ;
    puts(r == RB_EXIT ? "Box beendet.\r\n" : "Box: unerwarteter Rueckweg.\r\n");
}
