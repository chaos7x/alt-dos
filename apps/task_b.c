/*
 * Task B: fordert per DosAllocSeg() zehn Segmente zu je 64 KB an, beschreibt
 * und prueft sie, gibt die Haelfte selbst frei und beendet sich dann. Den
 * Rest muss der Kernel beim DosExit aufraeumen.
 */
#include <i86.h>
#include "fapi.h"

#define NSEG 10

static void hex4(unsigned v, char *b)
{
    static const char h[] = "0123456789ABCDEF";
    b[0] = h[(v >> 12) & 15];
    b[1] = h[(v >> 8) & 15];
    b[2] = h[(v >> 4) & 15];
    b[3] = h[v & 15];
}

/* Segment mit Muster fuellen und zuruecklesen, 1 = in Ordnung */
static int test_seg(unsigned sel)
{
    unsigned short __far *p = MK_FP(sel, 0);
    unsigned w;

    for (w = 0; w < 32768U; w++)
        p[w] = (unsigned short)(w ^ sel);
    for (w = 0; w < 32768U; w++)
        if (p[w] != (unsigned short)(w ^ sel))
            return 0;
    return 1;
}

void main(void)
{
    static unsigned sel[NSEG];
    static char line[] = "Segment ??: Selektor ????h, 64 KB beschrieben und gepr\x81" "ft  ";
    int i;

    VioWrtStr(0, 2, "Fordere nacheinander 10 Segmente zu je 64 KB per DosAllocSeg() an.");
    DosSleep(2000);

    for (i = 0; i < NSEG; i++) {
        sel[i] = DosAllocSeg(0);
        line[8] = (char)(i + 1 >= 10 ? '1' : ' ');
        line[9] = (char)('0' + (i + 1) % 10);
        if (sel[i] == FAPI_ERROR) {
            VioWrtStr(1, 2, "DosAllocSeg fehlgeschlagen - kein Speicher mehr.");
            break;
        }
        hex4(sel[i], line + 21);
        if (!test_seg(sel[i])) {
            VioWrtStr(1, 2, "Musterfehler im Segment!");
            return;
        }
        VioWrtStr(1, 2, line);
        DosSleep(700);
    }

    DosSleep(2000);
    VioWrtStr(2, 2, "Gebe 5 Segmente mit DosFreeSeg() frei ...");
    for (i = 0; i < NSEG / 2; i++) {
        DosFreeSeg(sel[i]);
        DosSleep(500);
    }
    DosSleep(2000);
    VioWrtStr(2, 2, "Ende ohne Aufr\x84umen - den Rest gibt der Kernel bei DosExit frei.");
    DosSleep(3000);
}
