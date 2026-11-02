/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HEAD.EXE - Anfang von Dateien ausgeben (nach dem Vorbild von GNU head):
 *
 *   HEAD [-n N | -N] [-c N] [-q | -v] [DATEI...]
 *
 * Vorgabe: die ersten 10 Zeilen. -c N: die ersten N Bytes. Mehrere Dateien
 * bekommen eine Kopfzeile "==> Name <==" (-q ohne, -v immer). Ohne Datei
 * oder mit - die Standardeingabe.
 */
#include "fapi.h"
#include "util.h"
#define TI_NUM
#define TI_FILE
#include "textin.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN];
static unsigned long count = 10;
static int bytes, quiet, verbose, nfiles, rc;

static void head(unsigned h)
{
    unsigned long n = 0;
    int c;
    ti_open(h);
    while (n < count && (c = ti_get()) >= 0) {
        to_put((char)c);
        if (bytes || c == '\n')
            n++;
    }
}

void main(void)
{
    char *a;
    int ok, pass, first = 1;
    unsigned h;

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (!pass)
                puts("Gibt den Anfang von Dateien aus (nach dem Vorbild von GNU head).\r\n\r\n"
                     "HEAD [-n N | -N] [-c N] [-q | -v] [Datei...]\r\n\r\n"
                     "  -n N   die ersten N Zeilen (Vorgabe 10), auch -N\r\n"
                     "  -c N   die ersten N Bytes\r\n"
                     "  -q     keine Kopfzeilen, -v immer Kopfzeilen\r\n");
            return;
        }
        if (a[0] == '-' && a[1] && a[1] != ' ') {
            char o = a[1];
            char *v = a[2] && a[2] != ' ' ? a + 2 : skip(next_word(a));
            if (o >= '0' && o <= '9') {
                if (!pass)
                    count = ti_num(a + 1, &ok);
                continue;
            }
            if (o == 'n' || o == 'c') {
                if (!pass) {
                    count = ti_num(v, &ok);
                    bytes = o == 'c';
                    if (!ok) {
                        puts("HEAD: ung\x81ltige Anzahl\r\n");
                        DosExit(1);
                    }
                }
                if (v != a + 2)
                    a = next_word(a);
            } else if (o == 'q')
                quiet = 1;
            else if (o == 'v')
                verbose = 1;
            else if (!pass) {
                puts("HEAD: unbekannter Schalter - ");
                puts(a);
                puts("\r\n");
                DosExit(1);
            }
            continue;
        }
        if (!pass) {
            nfiles++;
            continue;
        }
        word(a, name, sizeof(name));
        h = ti_file(name);
        if (FAPI_FAILED(h)) {
            to_flush();
            puts("HEAD: Datei nicht gefunden - ");
            puts(name);
            puts("\r\n");
            rc = 1;
            continue;
        }
        if ((nfiles > 1 && !quiet) || verbose) {
            if (!first)
                to_str("\r\n");
            to_str("==> ");
            to_str(name);
            to_str(" <==\r\n");
        }
        first = 0;
        head(h);
        if (h != STDIN)
            DosClose(h);
    }
    if (!nfiles)
        head(STDIN);
    to_flush();
    DosExit(rc);
}
