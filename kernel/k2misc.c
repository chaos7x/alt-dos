/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Seltenes aus kernel.c im zweiten Codesegment (KTEXT2): Text im ROM
   fuer MSD (aus kernel.c verschoben, erstes Codesegment war voll). */
#include "kern.h"
#include "k2.h"                         /* KTEXT2, k2.h zuletzt           */

/* Text im ROM fuer MSD: der laengste mit "BIOS", "(C)" oder "Copyright",
   sonst der laengste ueberwiegend aus Buchstaben/Ziffern (ohne '%') */
void __far rom_str(unsigned long lin, unsigned len, char __far *out, unsigned max)
{
    unsigned char __far *p = MK_FP(SCRATCH, 0);
    unsigned i, s = 0, n = 0, al = 0, best = 0, bs = 0, c, kw = 0, pc = 0, bkw = 0, sp = 0, ks = 0;
    scratch_map(lin, len - 1);
    for (i = 0; i <= len - 1; i++) {
        c = i < len - 1 ? p[i] : 0;
        if (c >= 0x20 && c < 0x7F) {
            if (!n)
                s = i, al = 0, kw = 0, pc = 0, sp = 0;
            n++;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                al++;
            if (c == '%')
                pc = 1;
            if (c == ' ')
                sp = 1, al++;
            if (n >= 4 && p[i - 3] == 'B' && p[i - 2] == 'I' && p[i - 1] == 'O' && c == 'S' &&
                kw < 1)
                kw = 1;                     /* "BIOS" zaehlt weniger als  */
            if (n >= 4 && ((p[i - 2] == '(' && (p[i - 1] | 0x20) == 'c' && c == ')') ||
                           (p[i - 3] == 'r' && p[i - 2] == 'i' && p[i - 1] == 'g' && c == 'h'))) {
                if (kw < 2)                 /* ab hier zeigen: davor steht */
                    ks = c == ')' ? i - 2 : (i - 7 >= s ? i - 7 : s);  /* oft Muell */
                kw = 2;                     /* "(C)" / "Copyright"        */
            }
            continue;
        }
        /* Zeichensalat aussortieren: mindestens 8 Zeichen, ein Leerzeichen,
           zu drei Vierteln Buchstaben/Ziffern/Leerzeichen */
        if (n >= 8 && sp && al * 4 >= n * 3 && !pc && (kw > bkw || (kw == bkw && n > best))) {
            best = n, bs = s, bkw = kw;
            if (kw == 2 && ks > s) {        /* "(C)"/"Copyright" voran   */
                best -= ks - s;
                bs = ks;
            }
        }
        n = 0;
    }
    if (best >= max)
        best = max - 1;
    for (i = 0; i < best; i++)
        out[i] = p[bs + i];
    out[i] = 0;
}

