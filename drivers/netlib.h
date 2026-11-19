/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hilfen fuer Netzwerk-Treiber (Kartentreiber und TCPIP.SYS):
 * ferne Einspruenge ueber dcrt0 (fent0..4), 16-Bit-Ports, Kopieren.
 * Nur 286-Befehle (NE2000.SYS und TCPIP.SYS sollen auch auf 5.x laufen);
 * 32-Bit-Ports stehen im jeweiligen PCI-Treiber.
 */
#ifndef NETLIB_H
#define NETLIB_H
#include "../include/net.h"

typedef unsigned (*fent_t)(void __far *);
extern fent_t fent_fn[5];
#pragma aux fent_fn "_fent_fn"
void fent0(void);
void fent1(void);
void fent2(void);
void fent3(void);
void fent4(void);

/* Einsprung n auf f legen -> Offset fuer net_reg/net_proto */
static unsigned fent(int n, fent_t f)
{
    static void (* const tab[5])(void) = { fent0, fent1, fent2, fent3, fent4 };
    fent_fn[n] = f;
    return (unsigned)tab[n];
}

unsigned getds(void);
#pragma aux getds = "mov ax,ds" __value [__ax];
unsigned inw(unsigned port);
#pragma aux inw = "in ax,dx" __parm [__dx] __value [__ax];
void outw(unsigned port, unsigned v);
#pragma aux outw = "out dx,ax" __parm [__dx] [__ax];

/* n Bytes fern kopieren (wortweise, 286) */
void fmove(void __far *d, const void __far *s, unsigned n);
#pragma aux fmove = "push ds" "cld" "mov ds,dx" "shr cx,1" \
    "rep movsw" "adc cx,cx" "rep movsb" "pop ds" \
    __parm [__es __di] [__dx __si] [__cx] __modify [__ax __cx __si __di];

#endif
