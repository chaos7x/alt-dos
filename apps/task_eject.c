/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EJECT.EXE - Medium auswerfen (CD-ROM, ZIP, MO):  EJECT Laufwerk:
 * Ueber FAPI 55h DosEject; der Treiber schickt START STOP UNIT mit LoEj.
 * Abgelehnt, solange auf dem Laufwerk Dateien offen sind.
 * ERRORLEVEL: 0 ausgeworfen, 1 abgelehnt/Fehler, 4 falscher Aufruf.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];

void main(void)
{
    char *a;
    unsigned d = 0xFFFF, rc;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Wirft das Medium eines Wechsellaufwerks aus (CD-ROM, ZIP, MO).\r\n\r\n"
                 "EJECT Laufwerk:\r\n");
            return;
        }
        if (a[1] == ':' && a[0] != '/')
            d = (unsigned)(up(a[0]) - 'A');
    }
    if (d >= 26) {
        puts("Aufruf: EJECT Laufwerk:\r\n");
        DosExit(4);
    }
    rc = DosEject(d);
    if (rc == 0) {
        puts("Medium ausgeworfen.\r\n");
        return;
    }
    putc((char)('A' + d));
    puts(rc == FAPI_ERR_INUSE ? ": Auf dem Laufwerk sind noch Dateien ge\x94" "ffnet.\r\n" :
         rc == FAPI_ERR_DENIED ? ": Kein Wechseldatentr\x84ger (oder der Treiber kann nicht auswerfen).\r\n" :
         ": Auswerfen fehlgeschlagen.\r\n");
    DosExit(1);
}
