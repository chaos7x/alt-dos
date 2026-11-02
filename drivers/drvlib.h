/* SPDX-License-Identifier: GPL-2.0-or-later */
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

/* Interruptroutine: void f(unsigned irq), laeuft mit gesperrten Interrupts */
extern void (*irq_fn)(unsigned);
#pragma aux irq_fn "_irq_fn"
void irq_entry(void);

#define REQ ((struct drv_req __far *)MK_FP(req_sel, req_off))

unsigned char inp(unsigned port);
#pragma aux inp = "in al,dx" __parm [__dx] __value [__al];
void outp(unsigned port, unsigned char v);
#pragma aux outp = "out dx,al" __parm [__dx] [__al];
/* n Worte vom Port nach es:di */
void insw_far(unsigned port, void __far *dst, unsigned n);
#pragma aux insw_far = "rep insw" __parm [__dx] [__es __di] [__cx] __modify [__di __cx];
/* n Bytes vom Port nach es:di / von sel:off zum Port */
void insb_far(unsigned port, void __far *dst, unsigned n);
#pragma aux insb_far = "rep insb" __parm [__dx] [__es __di] [__cx] __modify [__di __cx];
void outsb_far(unsigned port, unsigned sel, unsigned off, unsigned n);
#pragma aux outsb_far = "push ds" "mov ds,ax" "rep outsb" "pop ds" \
    __parm [__dx] [__ax] [__si] [__cx] __modify [__si __cx];
/* n Worte von sel:off zum Port */
void outsw_far(unsigned port, unsigned sel, unsigned off, unsigned n);
#pragma aux outsw_far = "push ds" "mov ds,ax" "rep outsw" "pop ds" \
    __parm [__dx] [__ax] [__si] [__cx] __modify [__si __cx];

void cli(void);
#pragma aux cli = "cli";
void sti(void);
#pragma aux sti = "sti";

/* IRQ belegen; alle IRQs eines Treibers landen in derselben Routine
   (sie bekommt die Nummer). 0 = ok */
unsigned set_irq(unsigned irq, void (*fn)(unsigned))
{
    irq_fn = fn;
    return devhlp(DH_SETIRQ, 0, irq, (unsigned)irq_entry);
}

void unset_irq(unsigned irq)
{
    devhlp(DH_UNSETIRQ, 0, irq, 0);
}

/* nach CLI rufen; schlaeft bis proc_run(ev) oder Frist (Takte, 0 = keine).
   Kehrt mit STI zurueck: 0 = geweckt, 1 = Frist um */
unsigned proc_block(unsigned ev, unsigned t)
{
    return devhlp(DH_PROCBLOCK, 0, ev, t);
}

unsigned proc_run(unsigned ev)
{
    return devhlp(DH_PROCRUN, 0, ev, 0);
}

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

/* Ports/DMA in die Belegungsliste des Kernels (RES_IO, RES_DMA):
   0 = ok, 1 = gehoert einem anderen Treiber */
unsigned claim(unsigned type, unsigned start, unsigned len)
{
    return devhlp(DH_CLAIM, len, type, start);
}

/* PCI ueber den Kernel (6.0); loc = Bus << 8 | Geraet << 3 | Funktion */
static unsigned long pci_val;           /* im DS (DevHlp schreibt hierher) */

unsigned pci_find(unsigned vendor, unsigned device, unsigned n)
{
    return devhlp(DH_PCIFIND, n, vendor, device);
}

unsigned long pci_rd(unsigned loc, unsigned reg)
{
    devhlp(DH_PCIREAD, (unsigned)&pci_val, loc, reg);
    return pci_val;
}

void pci_wr(unsigned loc, unsigned reg, unsigned long v, unsigned size)
{
    pci_val = v;
    devhlp(DH_PCIWRITE, (unsigned)&pci_val, loc, reg | (size << 8));
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

/* Hexzahl mit n Stellen an p anhaengen, gibt Ende zurueck */
char *hex(char *p, unsigned long v, int n)
{
    unsigned d;
    while (n--) {
        d = (unsigned)(v >> (n * 4)) & 15;
        *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    *p = 0;
    return p;
}

/* Schalter k (z. B. "/NOIRQ") in den Argumenten, gross oder klein */
int has_opt(const char __far *a, const char *k)
{
    int i;
    for (; *a; a++) {
        for (i = 0; k[i] && (a[i] & 0xDF) == (k[i] & 0xDF); i++)
            ;
        if (!k[i])
            return 1;
    }
    return 0;
}

char *scat(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

#endif
