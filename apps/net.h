/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hilfen fuer Netzwerkprogramme (PING, IPCONFIG ...): Adressen als Text
 * hin und zurueck. Adressen sind immer 16 Byte, IPv4 als ::ffff:a.b.c.d
 * (FAPI_NET, docs/NETZWERK.md) - Programme rechnen nie selbst damit.
 */
#ifndef APPS_NET_H
#define APPS_NET_H
#include "fapi.h"

#define DosNet(fn, p, n)    fapi_call(FAPI_NET, (fn), (unsigned short)(p), (n))
#define htons(v)            ((unsigned short)((v) << 8 | (unsigned short)(v) >> 8))
#define ntohs(v)            htons(v)

#ifdef NET_RESOLVE_FN
static struct fapi_resolve net_rs;
/* Name oder Zahlenadresse -> erste Adresse; 0 = ok, sonst FAPI-Fehler */
static unsigned net_lookup(const char *name, unsigned char *a)
{
    unsigned k, r;
    for (k = 0; name[k] && k < sizeof(net_rs.name) - 1; k++)
        net_rs.name[k] = name[k];
    net_rs.name[k] = 0;
    net_rs.family = 0;
    r = DosNet(NET_RESOLVE, &net_rs, sizeof(net_rs));
    if (r)
        return r;
    for (k = 0; k < 16; k++)
        a[k] = net_rs.addr[0][k];
    return 0;
}
#endif

static int net_is4(const unsigned char *a)
{
    int k;
    for (k = 0; k < 10; k++)
        if (a[k])
            return 0;
    return a[10] == 0xFF && a[11] == 0xFF;
}

static char *nt_dec(char *p, unsigned v)
{
    char t[6];
    int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i)
        *p++ = t[--i];
    return p;
}

/* Adresse -> Text (IPv4 gepunktet, IPv6 nach RFC 5952 mit "::") */
static char *net_ntop(const unsigned char *a, char *out)
{
    char *p = out;
    int k, best = -1, bl = 0, run, i;
    unsigned w;
    if (net_is4(a)) {
        for (k = 12; k < 16; k++) {
            p = nt_dec(p, a[k]);
            if (k < 15)
                *p++ = '.';
        }
        *p = 0;
        return out;
    }
    for (k = 0; k < 8; k = i) {         /* laengste Nullfolge (>= 2)      */
        for (i = k; i < 8 && !a[2 * i] && !a[2 * i + 1]; i++)
            ;
        run = i - k;
        if (run > bl && run > 1)
            best = k, bl = run;
        if (i == k)
            i++;
    }
    for (k = 0; k < 8; k++) {
        if (k == best) {
            *p++ = ':';
            if (k == 0)
                *p++ = ':';
            k += bl - 1;
            continue;
        }
        w = (unsigned)a[2 * k] << 8 | a[2 * k + 1];
        {
            static const char hx[] = "0123456789abcdef";
            int s = 12, any = 0;
            for (; s >= 0; s -= 4)
                if (((w >> s) & 15) || any || s == 0) {
                    *p++ = hx[(w >> s) & 15];
                    any = 1;
                }
        }
        if (k < 7)
            *p++ = ':';
    }
    *p = 0;
    return out;
}

#ifndef NET_NO_PTON
/* Text -> Adresse; 0 = ok. IPv4 "a.b.c.d" oder IPv6 mit "::" */
static int net_pton(const char *s, unsigned char *a)
{
    int k, n = 0, gap = -1, i;
    unsigned v, d;
    unsigned char t[16];
    for (k = 0; k < 16; k++)
        a[k] = t[k] = 0;
    for (k = 0; s[k] && s[k] != ':'; k++)
        ;
    if (!s[k]) {                        /* IPv4                          */
        for (k = 0; k < 4; k++) {
            if (*s < '0' || *s > '9')
                return -1;
            for (v = 0; *s >= '0' && *s <= '9'; s++)
                if ((v = v * 10 + (*s - '0')) > 255)
                    return -1;
            a[12 + k] = (unsigned char)v;
            if (k < 3 && *s++ != '.')
                return -1;
        }
        if (*s)
            return -1;
        a[10] = a[11] = 0xFF;
        return 0;
    }
    if (s[0] == ':' && s[1] == ':') {
        gap = 0;
        s += 2;
    }
    while (*s && n < 8) {
        for (v = 0, i = 0; i < 4; i++, s++) {
            d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ?
                (*s | 32) - 'a' + 10 : 16;
            if (d == 16)
                break;
            v = v << 4 | d;
        }
        if (!i)
            return -1;
        t[2 * n] = (unsigned char)(v >> 8);
        t[2 * n + 1] = (unsigned char)v;
        n++;
        if (*s == ':') {
            s++;
            if (*s == ':') {
                if (gap >= 0)
                    return -1;
                gap = n;
                s++;
            }
        } else if (*s)
            return -1;
    }
    if (*s || (gap < 0 && n != 8) || (gap >= 0 && n > 7))
        return -1;
    if (gap < 0)
        gap = n;
    for (k = 0; k < gap; k++)
        a[2 * k] = t[2 * k], a[2 * k + 1] = t[2 * k + 1];
    for (k = gap; k < n; k++) {
        i = 8 - (n - k);
        a[2 * i] = t[2 * k];
        a[2 * i + 1] = t[2 * k + 1];
    }
    return 0;
}
#endif

#endif
