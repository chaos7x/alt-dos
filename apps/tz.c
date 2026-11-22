/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zeitzone nach POSIX-TZ (wie Unix/DJGPP), z. B. CET-1CEST,M3.5.0,M10.5.0/3
 * – aus SNTP.EXE herausgeloest, damit NET TIME dieselbe Rechnung nimmt.
 * Regeln nur Mm.w.d[/Zeit]; ohne TZ Mitteleuropa (tz_init).
 */
#define FAPI_NO_IMPL
#include "fapi.h"
#include "util.h"
#include "arcutil.h"
#include "tz.h"

unsigned long au_mul16(unsigned a, unsigned b);
#pragma aux au_mul16 = "mul bx" __parm [__ax] [__bx] __value [__dx __ax] __modify [__dx];

static char *tz_dec(unsigned v, char *p)
{
    char t[6];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static long off_std, off_dst;           /* Ortszeit = UTC + off (Sek.)    */
static int has_dst;
static unsigned r_m[2], r_w[2], r_d[2];
static long r_t[2];                     /* Uhrzeit des Wechsels (Sek.)    */
char n_std[12], n_dst[12];

static const char *tz_name(const char *p, char *n)
{
    unsigned k = 0;
    if (*p == '<') {
        for (p++; *p && *p != '>'; p++)
            if (k < 11)
                n[k++] = *p;
        if (*p)
            p++;
    } else
        while ((*p | 32) >= 'a' && (*p | 32) <= 'z')
            if (k < 11)
                n[k++] = *p++;
            else
                p++;
    n[k] = 0;
    return p;
}

/* [+-]hh[:mm[:ss]] -> Sekunden */
static const char *tz_hms(const char *p, long *v)
{
    int neg = 0;
    long s = 0;
    unsigned x, f = 3600;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    for (;;) {
        for (x = 0; *p >= '0' && *p <= '9'; p++)
            x = x * 10 + (*p - '0');
        s += (long)au_mul16(x, f);
        if (*p != ':' || f == 1)
            break;
        p++;
        f /= 60;
    }
    *v = neg ? -s : s;
    return p;
}

static const char *tz_rule(const char *p, int i)
{
    r_t[i] = 7200;                      /* Vorgabe 02:00                  */
    if (*p != 'M')
        return 0;
    p++;
    for (r_m[i] = 0; *p >= '0' && *p <= '9'; p++)
        r_m[i] = r_m[i] * 10 + (*p - '0');
    if (*p++ != '.')
        return 0;
    r_w[i] = *p++ - '0';
    if (*p++ != '.')
        return 0;
    r_d[i] = *p++ - '0';
    if (*p == '/')
        p = tz_hms(p + 1, &r_t[i]);
    if (r_m[i] < 1 || r_m[i] > 12 || r_w[i] < 1 || r_w[i] > 5 || r_d[i] > 6)
        return 0;
    return p;
}

void tz_parse(const char *s)
{
    const char *p = tz_name(s, n_std);
    long v;
    p = tz_hms(p, &v);
    off_std = -v;                       /* POSIX: Westen positiv          */
    has_dst = 0;
    if ((*p | 32) >= 'a' && (*p | 32) <= 'z' || *p == '<') {
        p = tz_name(p, n_dst);
        off_dst = off_std + 3600;
        if (*p && *p != ',') {
            p = tz_hms(p, &v);
            off_dst = -v;
        }
        if (*p == ',' && (p = tz_rule(p + 1, 0)) != 0 && *p == ',' && tz_rule(p + 1, 1))
            has_dst = 1;
    }
}

static unsigned mlen(unsigned y, unsigned m)
{
    static const unsigned char ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && !(y & 3) ? 29 : ml[m - 1];
}

/* Wechsel i im Jahr y als Ortszeit-Sekunden seit 1970 (ohne Versatz) */
static unsigned long rule_at(unsigned y, int i)
{
    unsigned long d0, r;
    unsigned wd, day;
    d0 = fat2unix((y - 1980) << 9 | r_m[i] << 5 | 1, 0);
    au_div(au_div(d0, 86400UL, 0) + 4, 7, &r);         /* 1.1.1970 = Do */
    wd = (unsigned)r;
    day = (r_d[i] + 7 - wd) % 7 + 1 + (r_w[i] - 1) * 7;
    while (day > mlen(y, r_m[i]))
        day -= 7;
    return d0 + (au_mul16(day - 1, 43200U) << 1) + r_t[i];
}

/* Versatz fuer die UTC-Zeit u; *dst = Sommerzeit */
long tz_off(unsigned long u, int *dst)
{
    unsigned date, time, y;
    unsigned long s, e;
    *dst = 0;
    if (!has_dst)
        return off_std;
    unix2fat(u + off_std, &date, &time);
    y = 1980 + (date >> 9);
    s = rule_at(y, 0) - off_std;        /* Beginn in Normalzeit angegeben */
    e = rule_at(y, 1) - off_dst;        /* Ende in Sommerzeit angegeben    */
    *dst = s < e ? u >= s && u < e : u >= s || u < e;
    return *dst ? off_dst : off_std;
}

char *tz_put_off(char *p, long o)
{
    *p++ = 'U'; *p++ = 'T'; *p++ = 'C';
    if (!o)
        return p;
    *p++ = o < 0 ? '-' : '+';
    if (o < 0)
        o = -o;
    {
        unsigned long r;
        p = tz_dec((unsigned)au_div((unsigned long)o, 3600, &r), p);
        if (r) {
            *p++ = ':';
            *p++ = (char)('0' + (unsigned)r / 600);
            *p++ = (char)('0' + (unsigned)r % 600 / 60);
        }
    }
    return p;
}

/* TZ aus der Umgebung (Vorgabe Mitteleuropa) */
void tz_init(void)
{
    static char tz[80];
    if (DosGetEnv("TZ", tz, sizeof(tz)) == FAPI_ERROR || !tz[0])
        scopy(tz, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(tz));
    tz_parse(tz);
}
