/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hilfen der Packprogramme (siehe arcutil.h). Ohne C-Laufzeitsystem:
 * 32-Bit-Division per Schieben und Abziehen, Multiplikation mit 16-Bit-
 * Faktoren ueber MUL (Inline).
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "util.h"
#include "arcutil.h"

unsigned long au_mul16(unsigned a, unsigned b);
#pragma aux au_mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];

unsigned long au_div(unsigned long v, unsigned long d, unsigned long *rem)
{
    unsigned long q = 0, m = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        m = (m << 1) | ((v >> i) & 1);
        if (m >= d) {
            m -= d;
            q |= 1UL << i;
        }
    }
    if (rem)
        *rem = m;
    return q;
}

static const unsigned short mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

static int leap(unsigned y)
{
    return (y & 3) == 0;                /* 1980-2099 genuegt            */
}

/* FAT-Datum/Uhrzeit -> Sekunden seit 1.1.1970 */
unsigned long fat2unix(unsigned date, unsigned time)
{
    unsigned y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31, k;
    unsigned long days;

    if (m < 1 || m > 12)
        m = 1;
    if (d < 1)
        d = 1;
    days = 3652;                        /* 1970 .. 1979                 */
    for (k = 1980; k < y; k++)
        days += leap(k) ? 366 : 365;
    days += mdays[m - 1] + d - 1;
    if (m > 2 && leap(y))
        days++;
    return (au_mul16((unsigned)days, 43200U) << 1) +
           au_mul16(time >> 11, 3600) + ((time >> 5) & 63) * 60U + ((time & 31) << 1);
}

/* Sekunden seit 1970 -> FAT (vor 1980: 1.1.1980, nach 2107: 31.12.2107) */
void unix2fat(unsigned long t, unsigned *date, unsigned *time)
{
    unsigned long r;
    unsigned days, y, m, n, rr;

    days = (unsigned)au_div(t >> 1, 43200U, &r);
    r = (r << 1) | (t & 1);             /* Sekunden des Tages (< 86400)  */
    if (days < 3652) {
        *date = (0 << 9) | (1 << 5) | 1;
        *time = 0;
        return;
    }
    days -= 3652;
    for (y = 1980; ; y++) {
        n = leap(y) ? 366 : 365;
        if (days < n)
            break;
        days -= n;
        if (y == 2107) {
            *date = (127 << 9) | (12 << 5) | 31;
            *time = (23 << 11) | (59 << 5) | 29;
            return;
        }
    }
    for (m = 11; m > 0; m--)
        if (days >= mdays[m] + (m >= 2 && leap(y) ? 1 : 0))
            break;
    days -= mdays[m] + (m >= 2 && leap(y) ? 1 : 0);
    *date = ((y - 1980) << 9) | ((m + 1) << 5) | (days + 1);
    n = (unsigned)au_div(r, 3600, &r);
    rr = (unsigned)r;
    *time = (n << 11) | ((rr / 60) << 5) | ((rr % 60) >> 1);
}

/* Ersparnis in Prozent mit einer Nachkommastelle: 100 - out*100/in */
void put_ratio(unsigned long in, unsigned long out)
{
    unsigned long p;
    int neg = 0;
    char b[8];
    unsigned n = 0, v;

    if (!in) {
        puts("0,0%");
        return;
    }
    while (in > 0x00FFFFFFUL) {         /* damit x 1000 nicht ueberlaeuft */
        in >>= 4;
        out >>= 4;
    }
    if (!in)
        in = 1;
    p = au_div((out << 10) - (out << 4) - (out << 3), in, 0);  /* out*1000/in */
    if (p > 1000) {
        neg = 1;
        p -= 1000;
    } else
        p = 1000 - p;
    v = (unsigned)p;
    if (neg)
        putc('-');
    b[n++] = (char)('0' + v % 10);
    b[n++] = ',';
    v /= 10;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        putc(b[--n]);
    putc('%');
}

unsigned long oct_get(const char *p, unsigned n)
{
    unsigned long v = 0;
    while (n && (*p == ' ' || *p == 0)) {
        p++;
        n--;
    }
    while (n-- && *p >= '0' && *p <= '7')
        v = (v << 3) | (unsigned)(*p++ - '0');
    return v;
}

void oct_put(char *p, unsigned n, unsigned long v)
{
    unsigned i = n - 1;
    p[i] = 0;
    while (i--) {
        p[i] = (char)('0' + (unsigned)(v & 7));
        v >>= 3;
    }
}

void fat_now(unsigned *date, unsigned *time)
{
    static unsigned short dt[2];
    DosGetDateTime(dt);
    *date = dt[0];
    *time = dt[1];
}
