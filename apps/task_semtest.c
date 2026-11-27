/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SEMTEST.EXE - Test fuer die Semaphoren (FAPI_SEM, 6.5):
 *
 *   SEMTEST          Selbsttest in einem Task (RAM- und Systemsemaphoren)
 *   SEMTEST S n      Diener: \SEM\PING/\SEM\PONG anlegen, n Runden
 *   SEMTEST C n      Kunde dazu (anderer Task)
 *   SEMTEST O n      \SEM\OWN anlegen, nehmen, nach n s ohne Freigabe enden
 *   SEMTEST R        \SEM\OWN oeffnen und nehmen -> 1 = Vorbesitzer beendet
 *   SEMTEST M        \SEM\M1, \SEM\M2 anlegen, setzen, auf eine warten
 *   SEMTEST X        \SEM\M2 freigeben
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static unsigned long ram;               /* RAM-Semaphore                  */
static struct fapi_sem a, b;
static struct fapi_muxsem mx;
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

static unsigned num(char *p)
{
    unsigned n = 0;
    for (p = skip(p); *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (*p - '0');
    return n ? n : 10;
}

static void self(void)
{
    a.sem = (unsigned long)(void __far *)&ram;
    res("RAM setzen", DosSemSet(&a), 0);
    a.ms = 300;
    res("RAM warten 300 ms (gesetzt)", DosSemWait(&a), FAPI_ERR_TIMEOUT);
    res("RAM freigeben", DosSemClear(&a), 0);
    res("RAM warten (frei)", DosSemWait(&a), 0);
    a.ms = 0;
    res("RAM nehmen", DosSemRequest(&a), 0);
    res("RAM nochmal nehmen, Frist 0", DosSemRequest(&a), FAPI_ERR_TIMEOUT);
    res("RAM freigeben", DosSemClear(&a), 0);

    b.ms = 0;
    res("System anlegen", DosCreateSem(&b, "\\sem\\selbst"), 0);
    a.ms = 0;
    res("System doppelt anlegen", DosCreateSem(&a, "\\SEM\\SELBST"), FAPI_ERR_EXIST);
    res("System ohne \\SEM\\", DosCreateSem(&a, "\\TMP\\X"), FAPI_ERROR);
    res("System oeffnen", DosOpenSem(&a, "\\SEM\\Selbst"), 0);
    res("gleiche Kennung", a.sem == b.sem, 1);
    b.ms = 0;
    res("System nehmen", DosSemRequest(&b), 0);
    res("System nochmal nehmen", DosSemRequest(&b), 0);
    res("System freigeben (1)", DosSemClear(&b), 0);
    b.ms = 200;
    res("System warten (noch genommen)", DosSemWait(&b), FAPI_ERR_TIMEOUT);
    res("System freigeben (2)", DosSemClear(&b), 0);
    res("System warten (frei)", DosSemWait(&b), 0);
    res("System setzen", DosSemSet(&b), 0);
    ram = 1;
    mx.n = 2;
    mx.ms = 200;
    mx.sem[0] = b.sem;
    mx.sem[1] = (unsigned long)(void __far *)&ram;
    res("Mux, beide gesetzt", DosMuxSemWait(&mx), FAPI_ERR_TIMEOUT);
    ram = 0;
    res("Mux, zweite frei", DosMuxSemWait(&mx), 0);
    res("Mux Nummer", mx.index, 1);
    res("System schliessen (1)", DosCloseSem(&b), 0);
    res("System schliessen (2)", DosCloseSem(&a), 0);
    res("geschlossene Kennung", DosSemSet(&a), FAPI_ERROR);
    res("oeffnen nach dem Loeschen", DosOpenSem(&a, "\\SEM\\SELBST"), FAPI_ERROR);
    puts(bad ? "SEMTEST: Fehler\n" : "SEMTEST: alles in Ordnung\n");
}

void main(void)
{
    unsigned n, i, r;
    char *p, m;

    DosGetArgs(args);
    p = skip(args);
    m = up(*p);
    n = num(p + (*p != 0));
    a.ms = b.ms = 0;
    switch (m) {
    case 0:
        self();
        break;
    case 'S':
        if (DosCreateSem(&a, "\\SEM\\PING") || DosCreateSem(&b, "\\SEM\\PONG")) {
            puts("SEMTEST S: Anlegen gescheitert\n");
            DosExit(1);
        }
        DosSemSet(&a);
        DosSemSet(&b);
        a.ms = 30000;
        for (i = 0; i < n; i++) {
            if ((r = DosSemWait(&a)) != 0) {
                res("Diener wartet", r, 0);
                break;
            }
            DosSemSet(&a);
            DosSemClear(&b);
        }
        puts("Diener: ");
        put_num(i, 0, 0);
        puts(" Runden\n");
        bad = i != n;
        break;
    case 'C':
        if (DosOpenSem(&a, "\\SEM\\PING") || DosOpenSem(&b, "\\SEM\\PONG")) {
            puts("SEMTEST C: kein Diener\n");
            DosExit(1);
        }
        b.ms = 5000;
        for (i = 0; i < n; i++) {
            DosSemSet(&b);
            DosSemClear(&a);
            if ((r = DosSemWait(&b)) != 0) {
                res("Kunde wartet", r, 0);
                break;
            }
        }
        puts("Kunde: ");
        put_num(i, 0, 0);
        puts(" Runden\n");
        bad = i != n;
        break;
    case 'O':
        res("anlegen", DosCreateSem(&a, "\\SEM\\OWN"), 0);
        a.ms = 0;
        res("nehmen", DosSemRequest(&a), 0);
        puts("Besitzer endet ohne Freigabe\n");
        for (i = 0; i < n; i++)
            DosSleep(1000);
        DosExit(0);
    case 'R':
        res("oeffnen", DosOpenSem(&a, "\\SEM\\OWN"), 0);
        a.ms = 0xFFFFFFFFUL;
        res("nehmen (Vorbesitzer beendet = 1)", DosSemRequest(&a), 1);
        res("freigeben", DosSemClear(&a), 0);
        res("nochmal nehmen (0)", DosSemRequest(&a), 0);
        break;
    case 'M':
        res("M1", DosCreateSem(&a, "\\SEM\\M1"), 0);
        res("M2", DosCreateSem(&b, "\\SEM\\M2"), 0);
        DosSemSet(&a);
        DosSemSet(&b);
        mx.n = 2;
        mx.ms = 60000;
        mx.sem[0] = a.sem;
        mx.sem[1] = b.sem;
        res("Mux warten", DosMuxSemWait(&mx), 0);
        res("Mux Nummer (1)", mx.index, 1);
        break;
    case 'X':
        res("M2 oeffnen", DosOpenSem(&b, "\\SEM\\M2"), 0);
        res("M2 freigeben", DosSemClear(&b), 0);
        break;
    default:
        puts("SEMTEST [S n | C n | O n | R | M | X]\n");
    }
    DosExit(bad ? 1 : 0);
}
