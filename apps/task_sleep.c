/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SLEEP.EXE - warten, wie sleep (GNU):  SLEEP Zahl[s|m|h] ...  (auch 0.5)
 * Strg+C bricht ab.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];

void main(void)
{
    char *a, w[24];
    unsigned long ms = 0, v, frac;
    unsigned k, fd;
    int any = 0;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            puts("Wartet die angegebene Zeit.\r\n\r\nSLEEP Zahl[s|m|h] ...   (auch Dezimalstellen, z. B. 0.5)\r\n");
            return;
        }
        v = 0;
        for (k = 0; w[k] >= '0' && w[k] <= '9'; k++)
            v = (v << 3) + (v << 1) + (w[k] - '0');
        frac = 0;
        fd = 0;
        if (w[k] == '.' || w[k] == ',') {
            for (k++; w[k] >= '0' && w[k] <= '9'; k++)
                if (fd < 3) {
                    frac = (frac << 3) + (frac << 1) + (w[k] - '0');
                    fd++;
                }
            while (fd < 3) {
                frac = (frac << 3) + (frac << 1);
                fd++;
            }
        }
        v = (v << 10) - (v << 4) - (v << 3) + frac;     /* * 1000 + Rest  */
        if (w[k] == 'm') v = (v << 6) - (v << 2);       /* * 60           */
        else if (w[k] == 'h') { v = (v << 6) - (v << 2); v = (v << 6) - (v << 2); }
        else if (w[k] && w[k] != 's') {
            puts("sleep: ung\x81ltige Zeit: ");
            puts(w);
            puts("\r\n");
            DosExit(1);
        }
        ms += v;
        any = 1;
    }
    if (!any) {
        puts("sleep: Zeit fehlt\r\n");
        DosExit(1);
    }
    while (ms) {
        unsigned s = ms > 30000UL ? 30000u : (unsigned)ms;
        DosSleep(s);
        ms -= s;
    }
}
