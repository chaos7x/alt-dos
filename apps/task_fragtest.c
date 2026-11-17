/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FRAGTEST.EXE - Test fuer das Verschieben von Datensegmenten (6.1):
 *
 *   FRAGTEST [/S:kb] [/W | /T:s] [/B]
 *
 * Legt 4-KB-Segmente mit Muster an, bis der Speicher (oder die Grenze von
 * 400) erreicht ist, gibt jedes zweite frei und verlangt dann 64-KB-
 * Segmente - die gibt es nur, wenn der Kernel Segmente zusammenschiebt.
 * Zum Schluss werden alle Muster geprueft. /W wartet vor der Pruefung auf
 * eine Taste (so haelt ein zweiter FRAGTEST in einer anderen Sitzung die
 * Luecken, waehrend dieser mit /B nur grosse Segmente verlangt). /T:s
 * wartet stattdessen s Sekunden (fuer DETACH mit Umleitung).
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define NS 500
static unsigned sel[NS], big[64];
static char args[FAPI_ARGLEN];

static void fill(unsigned s, unsigned words)
{
    unsigned __far *p = MK_FP(s, 0);
    unsigned i;
    for (i = 0; i < words; i++)
        p[i] = s ^ (i * 3);
}

static unsigned check(unsigned s, unsigned words)
{
    unsigned __far *p = MK_FP(s, 0);
    unsigned i;
    for (i = 0; i < words; i++)
        if (p[i] != (s ^ (i * 3)))
            return 1;
    return 0;
}

static void say(const char *a, unsigned long v, const char *b)
{
    puts(a);
    put_num(v, 0, 0);
    puts(b);
}

void main(void)
{
    unsigned n = 0, kept = 0, nb = 0, bad = 0, i, s, kb = 4, secs = 0;
    int o_w = 0, o_b = 0;
    char *a;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (same(a, "/W") || (a[0] == '/' && (a[1] == 'w' || a[1] == 'W')))
            o_w = 1;
        if (a[0] == '/' && (a[1] == 'b' || a[1] == 'B'))
            o_b = 1;
        if (a[0] == '/' && (a[1] == 't' || a[1] == 'T') && a[2] == ':') {
            for (secs = 0, i = 3; a[i] >= '0' && a[i] <= '9'; i++)
                secs = secs * 10 + (a[i] - '0');
            o_w = 1;
        }
        if (a[0] == '/' && (a[1] == 's' || a[1] == 'S') && a[2] == ':')
            for (kb = 0, i = 3; a[i] >= '0' && a[i] <= '9'; i++)
                kb = kb * 10 + (a[i] - '0');
    }
    if (!o_b) {
        if (!kb || kb > 32)
            kb = 4;
        while (n < NS && (s = DosAllocSeg(kb << 10)) != FAPI_ERROR) {
            fill(s, kb << 9);
            sel[n++] = s;
        }
        for (i = 0; i < n; i += 2) {
            DosFreeSeg(sel[i]);
            sel[i] = 0;
        }
        kept = n / 2;
        say("", kb, "-KB-Segmente: ");
        say("", n, " angelegt, ");
        say("", kept, " behalten\r\n");
    }
    if (!o_w) {
        while (nb < 64 && (s = DosAllocSeg(0)) != FAPI_ERROR) {
            fill(s, 32768U);
            big[nb++] = s;
        }
        say("64-KB-Segmente: ", nb, " bekommen\r\n");
    } else {
        if (secs)
            for (i = 0; i < secs; i++)
                DosSleep(1000);
        else {
            puts("Taste dr\x81" "cken zum Pr\x81" "fen ...\r\n");
            KbdCharIn();
        }
    }
    for (i = 0; i < n; i++)
        if (sel[i] && check(sel[i], kb << 9))
            bad++;
    for (i = 0; i < nb; i++)
        if (check(big[i], 32768U))
            bad++;
    say("Muster gepr\x81" "ft: ", bad, bad ? " FEHLER\r\n" : " Fehler, alles gleich\r\n");
    DosExit(bad ? 1 : 0);
}
