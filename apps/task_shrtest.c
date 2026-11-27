/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SHRTEST.EXE - Test fuer gemeinsame Segmente (FAPI_SHRSEG, 6.5):
 *
 *   SHRTEST S    Diener: \SHAREMEM\TEST (8 KB) anlegen, auf den Kunden
 *                warten, ihm ein zweites Segment ohne Namen geben
 *   SHRTEST C    Kunde (anderer Task): Segment holen, pruefen, aendern
 *   SHRTEST      Fehlerfaelle in einem Task
 *
 * Aufbau von \SHAREMEM\TEST: +0 RAM-Semaphore "Kunde ist da", +4 RAM-
 * Semaphore "Segment gegeben", +8 Task des Kunden, +10 Selektor fuer den
 * Kunden, +12 Antwort des Kunden, ab +16 Muster.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

#define NAME "\\SHAREMEM\\TEST"
static char args[FAPI_ARGLEN];
static struct fapi_sem s1, s2;
static struct fapi_proc pi;
static unsigned bad;

static void res(const char *what, unsigned got, unsigned want)
{
    puts(what);
    puts(got == want ? ": OK\n" : ": FEHLER, Ergebnis ");
    if (got != want) {
        put_num(got, 0, 0);
        puts(" statt ");
        put_num(want, 0, 0);
        puts("\n");
        bad++;
    }
}

static unsigned pattern(unsigned __far *p, unsigned from, unsigned to, unsigned key, int wr)
{
    unsigned i, e = 0;
    for (i = from; i < to; i++)
        if (wr)
            p[i] = i * 7 ^ key;
        else if (p[i] != (i * 7 ^ key))
            e++;
    return e;
}

static unsigned self_task(void)
{
    unsigned t;
    for (t = 0; DosProc(PROC_INFO, t, &pi) == 0; t++)
        if (pi.flags & PROC_SELF)
            return t;
    return 0xFFFF;
}

void main(void)
{
    unsigned sel, sel2, r;
    unsigned __far *p, __far *q;
    char m;

    DosGetArgs(args);
    m = up(*skip(args));
    switch (m) {
    case 0:
        sel = DosAllocShrSeg(4096, "\\sharemem\\selbst");
        res("anlegen", FAPI_FAILED(sel), 0);
        res("doppelt anlegen", DosAllocShrSeg(4096, "\\SHAREMEM\\SELBST"), FAPI_ERR_EXIST);
        res("falscher Name", DosAllocShrSeg(4096, "\\TMP\\X"), FAPI_ERROR);
        sel2 = DosGetShrSeg("\\SHAREMEM\\Selbst");
        res("holen", FAPI_FAILED(sel2), 0);
        res("anderer Selektor", sel != sel2, 1);
        p = MK_FP(sel, 0);
        q = MK_FP(sel2, 0);
        res("mit Nullen gefuellt", p[100], 0);
        p[100] = 0x1234;
        res("derselbe Speicher", q[100], 0x1234);
        res("freigeben (1)", DosFreeSeg(sel), 0);
        res("zweiter Selektor gilt noch", q[100], 0x1234);
        res("freigeben (2)", DosFreeSeg(sel2), 0);
        res("holen nach dem Freigeben", DosGetShrSeg("\\SHAREMEM\\SELBST"), FAPI_ERROR);
        sel = DosAllocSeg(4096);
        res("gewoehnliches Segment geben", DosGiveSeg(sel, self_task()), FAPI_ERROR);
        DosFreeSeg(sel);
        sel = DosAllocShrSeg(0, 0);
        res("ohne Namen, 64 KB", FAPI_FAILED(sel), 0);
        p = MK_FP(sel, 0);
        p[32767] = 0x55AA;
        res("letztes Wort", p[32767], 0x55AA);
        res("an freien Task geben", DosGiveSeg(sel, 7), FAPI_ERROR);
        res("freigeben", DosFreeSeg(sel), 0);
        break;
    case 'S':
        sel = DosAllocShrSeg(8192, NAME);
        res("Diener: anlegen", FAPI_FAILED(sel), 0);
        if (FAPI_FAILED(sel))
            break;
        p = MK_FP(sel, 0);
        pattern(p, 8, 4096, 0x1111, 1);
        p[0] = p[2] = 1;                /* beide Semaphoren gesetzt       */
        s1.sem = (unsigned long)(void __far *)p;
        s1.ms = 30000;
        res("Diener: Kunde meldet sich", DosSemWait(&s1), 0);
        sel2 = DosAllocShrSeg(4096, 0);
        res("Diener: zweites Segment", FAPI_FAILED(sel2), 0);
        q = MK_FP(sel2, 0);
        pattern(q, 0, 2048, 0x2222, 1);
        r = DosGiveSeg(sel2, p[4]);
        res("Diener: geben", FAPI_FAILED(r), 0);
        p[5] = r;
        DosFreeSeg(sel2);               /* der Kunde haelt es weiter      */
        p[0] = 1;
        s2.sem = (unsigned long)(void __far *)(p + 2);
        DosSemClear(&s2);
        res("Diener: Kunde fertig", DosSemWait(&s1), 0);
        res("Diener: Antwort", p[6], 0xC0DE);
        res("Diener: Muster des Kunden", pattern(p, 8, 4096, 0x3333, 0), 0);
        break;
    case 'C':
        sel = DosGetShrSeg(NAME);
        res("Kunde: holen", FAPI_FAILED(sel), 0);
        if (FAPI_FAILED(sel))
            break;
        p = MK_FP(sel, 0);
        res("Kunde: Muster des Dieners", pattern(p, 8, 4096, 0x1111, 0), 0);
        p[4] = self_task();
        s1.sem = (unsigned long)(void __far *)p;
        s2.sem = (unsigned long)(void __far *)(p + 2);
        s2.ms = 10000;
        DosSemClear(&s1);
        res("Kunde: Segment bekommen", DosSemWait(&s2), 0);
        q = MK_FP(p[5], 0);
        res("Kunde: Muster im gegebenen Segment", pattern(q, 0, 2048, 0x2222, 0), 0);
        res("Kunde: gegebenes freigeben", DosFreeSeg(p[5]), 0);
        pattern(p, 8, 4096, 0x3333, 1);
        p[6] = 0xC0DE;
        DosSemClear(&s1);
        break;
    default:
        puts("SHRTEST [S | C]\n");
    }
    puts(bad ? "SHRTEST: Fehler\n" : "SHRTEST: alles in Ordnung\n");
    DosExit(bad ? 1 : 0);
}
