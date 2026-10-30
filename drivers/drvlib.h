/*
 * Hilfen fuer Geraetetreiber: Ports, DevHlp, Anforderung, Textausgabe.
 * Treiber laufen in Ring 0 und duerfen Ports direkt ansprechen.
 */
#ifndef DRVLIB_H
#define DRVLIB_H

#include <i86.h>
#include "../include/drv.h"

#pragma pack(1)

extern unsigned short req_sel, req_off;
extern void __far *devhlp_ptr;
#pragma aux devhlp_ptr "_devhlp"

unsigned devhlp(unsigned fn, unsigned dx, unsigned bx, unsigned cx);

#define REQ ((struct drv_req __far *)MK_FP(req_sel, req_off))

unsigned char inp(unsigned port);
#pragma aux inp = "in al,dx" __parm [__dx] __value [__al];
void outp(unsigned port, unsigned char v);
#pragma aux outp = "out dx,al" __parm [__dx] [__al];
/* n Worte vom Port nach es:di */
void insw_far(unsigned port, void __far *dst, unsigned n);
#pragma aux insw_far = "rep insw" __parm [__dx] [__es __di] [__cx] __modify [__di __cx];
/* n Worte von sel:off zum Port */
void outsw_far(unsigned port, unsigned sel, unsigned off, unsigned n);
#pragma aux outsw_far = "push ds" "mov ds,ax" "rep outsw" "pop ds" \
    __parm [__dx] [__ax] [__si] [__cx] __modify [__si __cx];

void dputs(const char *s)
{
    devhlp(DH_PUTS, 0, (unsigned)s, 0);
}

unsigned ticks(void)
{
    return devhlp(DH_TICKS, 0, 0, 0);
}

void delay(unsigned t)
{
    devhlp(DH_DELAY, 0, t, 0);
}

/* Zahl als Dezimaltext an d anhaengen, gibt Ende zurueck */
char *udec(char *d, unsigned v)
{
    char t[6];
    int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i)
        *d++ = t[--i];
    *d = 0;
    return d;
}

char *scat(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

#endif
