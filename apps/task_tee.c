/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TEE.EXE - Standardeingabe auf die Standardausgabe und in Dateien, wie
 * tee (GNU):   ... | TEE [-a] Datei ...      (-a haengt an)
 */
#include "fapi.h"
#include "util.h"

#define NF 8
static char args[FAPI_ARGLEN], buf[4096];
static unsigned fh[NF], nf;

void main(void)
{
    char *a, w[FAPI_PATHLEN];
    int app = 0, rc = 0;
    unsigned n, k;

    DosGetArgs(args);
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '/' && w[1] == '?') {
            puts("Kopiert die Standardeingabe auf die Standardausgabe und in Dateien.\r\n\r\n"
                 "... | TEE [-a] Datei ...\r\n\r\n  -a  an die Dateien anh\x84ngen\r\n");
            return;
        }
        if (w[0] == '-' && w[1] == 'a' && !w[2]) {
            app = 1;
            continue;
        }
    }
    for (a = skip(args); *a; a = skip(a)) {
        a = word(a, w, sizeof(w));
        if (w[0] == '-' && w[1] == 'a' && !w[2])
            continue;
        if (nf < NF) {
            unsigned h = app ? DosAppend(w) : DosCreate(w);
            if (FAPI_FAILED(h)) {
                puts("tee: kann nicht schreiben: ");
                puts(w);
                puts("\r\n");
                rc = 1;
            } else
                fh[nf++] = h;
        }
    }
    while ((n = DosRead(STDIN, buf, sizeof(buf))) != 0 && !FAPI_FAILED(n)) {
        DosWrite(STDOUT, buf, n);
        for (k = 0; k < nf; k++)
            DosWrite(fh[k], buf, n);
    }
    for (k = 0; k < nf; k++)
        DosClose(fh[k]);
    DosExit((unsigned)rc);
}
