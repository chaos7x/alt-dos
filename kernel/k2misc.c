/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Seltenes aus kernel.c im zweiten Codesegment (KTEXT2):
 * Echtzeituhr (CMOS) und die Suche nach dem BIOS-Namen fuer MSD.
 */
#include "k2.h"

/* ---- Echtzeituhr (CMOS) ------------------------------------------------ */
static unsigned cmos(unsigned r)
{
    outb(0x70, (unsigned char)r);
    return inb(0x71);
}

static unsigned from_bcd(unsigned v, unsigned bin)
{
    return bin ? v : (v >> 4) * 10 + (v & 15);
}

/* jetzt, als FAT-Datum << 16 | FAT-Uhrzeit */
unsigned long __far rtc_stamp(void)
{
    unsigned s, m, h, dd, mm, yy, cc, bin, n;

    for (n = 0; n < 2000 && (cmos(0x0A) & 0x80); n++)
        ;                                       /* Uhr zaehlt gerade     */
    bin = cmos(0x0B) & 4;
    s  = from_bcd(cmos(0x00), bin);
    m  = from_bcd(cmos(0x02), bin);
    h  = from_bcd(cmos(0x04) & 0x7F, bin);
    dd = from_bcd(cmos(0x07), bin);
    mm = from_bcd(cmos(0x08), bin);
    yy = from_bcd(cmos(0x09), bin);
    cc = from_bcd(cmos(0x32), bin);
    if (cc != 19 && cc != 20)
        cc = yy < 80 ? 20 : 19;
    yy += cc == 20 ? 100 : 0;                   /* Jahre seit 1900       */
    yy = yy < 80 ? 0 : yy - 80;                 /* FAT zaehlt ab 1980    */
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || h > 23 || m > 59 || s > 59)
        return (1UL << 5 | 1) << 16;            /* Uhr kaputt: 1.1.1980  */
    return ((unsigned long)((yy << 9) | (mm << 5) | dd) << 16) |
           ((h << 11) | (m << 5) | (s >> 1));
}

static unsigned to_bcd(unsigned v, unsigned bin)
{
    return bin ? v : ((v / 10) << 4) | (v % 10);
}

/* Echtzeituhr stellen (FAT-Format); 0 = ok */
int __far rtc_set(unsigned date, unsigned time)
{
    unsigned y = 1980 + (date >> 9), mo = (date >> 5) & 15, dd = date & 31;
    unsigned h = time >> 11, mi = (time >> 5) & 63, sc = (time & 31) << 1;
    unsigned bin = cmos(0x0B) & 4, b = cmos(0x0B), f;

    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || h > 23 || mi > 59 || sc > 59)
        return -1;
    f = irq_off();
    outb(0x70, 0x0B); outb(0x71, b | 0x80);     /* Uhr anhalten          */
    outb(0x70, 0x00); outb(0x71, to_bcd(sc, bin));
    outb(0x70, 0x02); outb(0x71, to_bcd(mi, bin));
    outb(0x70, 0x04); outb(0x71, to_bcd(h, bin));    /* 24-Stunden-Modus */
    outb(0x70, 0x07); outb(0x71, to_bcd(dd, bin));
    outb(0x70, 0x08); outb(0x71, to_bcd(mo, bin));
    outb(0x70, 0x09); outb(0x71, to_bcd(y % 100, bin));
    outb(0x70, 0x32); outb(0x71, to_bcd(y / 100, bin));
    outb(0x70, 0x0B); outb(0x71, b & 0x7F);     /* weiterlaufen          */
    irq_restore(f);
    return 0;
}

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

