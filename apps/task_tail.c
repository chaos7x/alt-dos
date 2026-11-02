/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TAIL.EXE - Ende von Dateien ausgeben (nach dem Vorbild von GNU tail):
 *
 *   TAIL [-n N | -N | -n +N] [-c N] [-f] [-q | -v] [DATEI...]
 *
 * Vorgabe: die letzten 10 Zeilen. -n +N ab Zeile N, -c N die letzten N
 * Bytes. -f bleibt an der (letzten) Datei und zeigt, was dazukommt (wie
 * tail -f fuer Protokolle; eine Taste beendet). Gelesen wird durch einen
 * 64-KB-Ring: es zaehlt, was in den letzten 64 KB steht.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"
#define TI_NUM
#define TI_FILE
#include "textin.h"

static char args[FAPI_ARGLEN], name[FAPI_PATHLEN], lname[FAPI_PATHLEN];
static unsigned long count = 10;
static int bytes, from_start, follow, quiet, verbose, nfiles, rc;
static unsigned rsel;
static unsigned long fpos;              /* gelesene Bytes der letzten Datei */

#define RING(i) (((unsigned char __far *)MK_FP(rsel, 0))[(unsigned)(i)])

static void tail(unsigned h)
{
    unsigned long total = 0, n = 0, keep, i;
    unsigned w = 0, start;
    int c;

    ti_open(h);
    fpos = 0;
    if (from_start) {                   /* ab Zeile/Byte N              */
        while ((c = ti_get()) >= 0) {
            fpos++;
            if (n + 1 >= count)
                to_put((char)c);
            if (bytes || c == '\n')
                n++;
        }
        return;
    }
    while ((c = ti_get()) >= 0) {       /* letzte 64 KB im Ring          */
        RING(w++) = (unsigned char)c;
        total++;
    }
    fpos = total;
    keep = total > 0xFFFFUL ? 0xFFFFUL : total;
    if (count == 0)
        keep = 0;
    else if (bytes) {
        if (count < keep)
            keep = count;
    } else {
        /* rueckwaerts N Zeilenenden zaehlen (das letzte zaehlt nicht) */
        for (i = 0, n = 0; i < keep; i++) {
            c = RING((unsigned)(w - 1 - (unsigned)i));
            if (c == '\n' && i > 0 && ++n == count)
                break;
        }
        keep = i;
    }
    start = (unsigned)(w - (unsigned)keep);
    for (i = 0; i < keep; i++)
        to_put((char)RING((unsigned)(start + (unsigned)i)));
}

void main(void)
{
    char *a;
    int ok, pass, first = 1, c;
    unsigned h = FAPI_ERROR, last = FAPI_ERROR;

    DosGetArgs(args);
    for (pass = 0; pass < 2; pass++)
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            if (!pass)
                puts("Gibt das Ende von Dateien aus (nach dem Vorbild von GNU tail).\r\n\r\n"
                     "TAIL [-n N | -N | -n +N] [-c N] [-f] [-q | -v] [Datei...]\r\n\r\n"
                     "  -n N   die letzten N Zeilen (Vorgabe 10); -n +N ab Zeile N\r\n"
                     "  -c N   die letzten N Bytes\r\n"
                     "  -f     dranbleiben und Neues zeigen (Taste beendet)\r\n"
                     "  -q     keine Kopfzeilen, -v immer Kopfzeilen\r\n");
            return;
        }
        if ((a[0] == '-' || a[0] == '+') && a[1] && a[1] != ' ') {
            char o = a[1];
            char *v = a[2] && a[2] != ' ' ? a + 2 : skip(next_word(a));
            if (o >= '0' && o <= '9') {
                if (!pass) {
                    count = ti_num(a + 1, &ok);
                    from_start = a[0] == '+';
                }
                continue;
            }
            if (o == 'n' || o == 'c') {
                if (!pass) {
                    from_start = *v == '+';
                    count = ti_num(v + from_start, &ok);
                    bytes = o == 'c';
                    if (!ok) {
                        puts("TAIL: ung\x81ltige Anzahl\r\n");
                        DosExit(1);
                    }
                }
                if (v != a + 2)
                    a = next_word(a);
            } else if (o == 'f')
                follow = 1;
            else if (o == 'q')
                quiet = 1;
            else if (o == 'v')
                verbose = 1;
            else if (!pass) {
                puts("TAIL: unbekannter Schalter - ");
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
        if (!rsel) {
            rsel = DosAllocSeg(0);
            if (FAPI_FAILED(rsel) || !rsel) {
                puts("TAIL: nicht gen\x81gend Arbeitsspeicher\r\n");
                DosExit(1);
            }
        }
        word(a, name, sizeof(name));
        h = ti_file(name);
        if (FAPI_FAILED(h)) {
            to_flush();
            puts("TAIL: Datei nicht gefunden - ");
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
        tail(h);
        if (last != FAPI_ERROR && last != STDIN)
            DosClose(last);
        last = h;
        scopy(lname, name, sizeof(lname));
    }
    if (!nfiles) {
        rsel = DosAllocSeg(0);
        if (FAPI_FAILED(rsel) || !rsel)
            DosExit(1);
        tail(STDIN);
        last = FAPI_ERROR;
    }
    to_flush();
    if (follow && last != FAPI_ERROR && last != STDIN) {
        /* Die Groesse steht je offenem Handle: was ein anderer Task
           anhaengt, sieht ein offenes Handle nicht. Daher jedes Mal neu
           oeffnen und an die alte Stelle springen (wie tail -F). */
        DosClose(last);
        last = FAPI_ERROR;
        for (;;) {                      /* -f: dranbleiben              */
            h = DosOpen(lname);
            if (!FAPI_FAILED(h)) {
                DosSeek(h, &fpos);
                ti_open(h);
                while ((c = ti_get()) >= 0) {
                    to_put((char)c);
                    fpos++;
                }
                to_flush();
                DosClose(h);
            }
            if (KbdPeek()) {
                KbdCharIn();
                break;
            }
            DosSleep(500);
        }
    }
    if (last != FAPI_ERROR && last != STDIN)
        DosClose(last);
    DosExit(rc);
}
