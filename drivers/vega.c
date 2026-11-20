/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VEGA.SYS / VCGA.SYS - Grafik der DOS-Box auf EGA- und CGA-Karten (6.3,
 * Stufe 4 nach docs/VDD.md; wie VEGA.SYS/VCGA.SYS bei OS/2 2.x)
 *
 * Die Register dieser Karten kann man nicht lesen. Hat eine Box Rohgrafik
 * (eigener BIOS-Modus), laesst der Kernel ihre Ports 3B0h-3DFh gesperrt
 * und reicht jeden Zugriff hierher: vorn an die Karte durchreichen und
 * mitschreiben, hinten nur mitschreiben. Beim Wegschalten liefert das
 * Modul den Stand (VDD_EV_VSAVE -> VS_VREGS, im Aufbau der VGA-Tabellen,
 * damit der Kernel daraus wie bei der VGA die Art des Bildes bestimmt),
 * beim Zurueckholen schreibt es ihn in die Karte (VDD_EV_VREST). Das Bild
 * sichert der Kernel. Mit -DCGA entsteht VCGA.SYS (6845 an 3D4h/3D5h,
 * Modus 3D8h, Farbe 3D9h), sonst VEGA.SYS (Misc, Sequenzer, CRTC, Grafik,
 * Attribut).
 */
#include "drvlib.h"
#include "netlib.h"                     /* fent()                        */
#include "../include/vdd.h"

#define NT VDD_NTASK

static struct vdd_svc sv;
static struct vdd_reg reg;
static unsigned char img[64];           /* VS_VREGS                      */

#ifdef CGA
#define NAME     "VCGA"
#define CLASSF   VDDF_VCGA
static struct cg {
    unsigned char ci, crtc[18], mode, color;
} g[NT];

static unsigned io(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, p = c->port, fg = c->flags & 1;
    struct cg *x;
    if (t >= NT || p < 0x3D0 || p > 0x3DF || !(c->flags & 2))
        return VDD_NOTMINE;
    x = &g[t];
    if (c->fn == VDD_IN) {
        c->val = fg ? inp(p) : (p == 0x3DA ? 0x09 : 0xFF);
        return 0;
    }
    if (c->fn != VDD_OUT)
        return VDD_NOTMINE;
    if (p < 0x3D8) {                    /* 3D0-3D7: 6845 (gespiegelt)    */
        if (!(p & 1))
            x->ci = (unsigned char)(c->val & 0x1F);
        else if (x->ci < 18)
            x->crtc[x->ci] = (unsigned char)c->val;
    } else if (p == 0x3D8)
        x->mode = (unsigned char)c->val;
    else if (p == 0x3D9)
        x->color = (unsigned char)c->val;
    if (fg)
        outp(p, (unsigned char)c->val);
    return 0;
}

static void save(unsigned t)
{
    unsigned i;
    for (i = 0; i < 64; i++)
        img[i] = 0;
    img[0] = g[t].mode;                 /* (die VGA-Felder, die der       */
    for (i = 0; i < 18; i++)            /* Kernel prueft: einfach, Speicher */
        img[6 + i] = g[t].crtc[i];      /* bei B8000 - Art 3, wenn der    */
    img[31 + 6] = 0x0F;                 /* BIOS-Modus 4-6 ist)            */
    img[31 + 8] = 0xFF;
}

static void restore(unsigned t)
{
    unsigned i;
    outp(0x3D8, (unsigned char)(g[t].mode & ~0x08));   /* Bild aus      */
    for (i = 0; i < 18; i++) {
        outp(0x3D4, (unsigned char)i);
        outp(0x3D5, g[t].crtc[i]);
    }
    outp(0x3D4, g[t].ci);
    outp(0x3D9, g[t].color);
    outp(0x3D8, g[t].mode);
}

#else
#define NAME     "VEGA"
#define CLASSF   VDDF_VEGA
static struct eg {
    unsigned char misc, si, seq[5], ci, crtc[25], gi, gc[9], ff, ai, ac[21];
} g[NT];

static unsigned io(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, p = c->port, fg = c->flags & 1, v;
    struct eg *x;
    if (t >= NT || p < 0x3B0 || p > 0x3DF || !(c->flags & 2))
        return VDD_NOTMINE;
    x = &g[t];
    if (c->fn == VDD_IN) {
        if (p == 0x3DA || p == 0x3BA)
            x->ff = 0;                  /* Attribut-Flipflop zurueck     */
        c->val = fg ? inp(p) : (p == 0x3DA || p == 0x3BA ? 0x09 : 0xFF);
        return 0;
    }
    if (c->fn != VDD_OUT)
        return VDD_NOTMINE;
    v = c->val & 0xFF;
    switch (p) {
    case 0x3C2: x->misc = (unsigned char)v; break;
    case 0x3C4: x->si = (unsigned char)v; break;
    case 0x3C5: if (x->si < 5) x->seq[x->si] = (unsigned char)v; break;
    case 0x3B4: case 0x3D4: x->ci = (unsigned char)v; break;
    case 0x3B5: case 0x3D5: if (x->ci < 25) x->crtc[x->ci] = (unsigned char)v; break;
    case 0x3CE: x->gi = (unsigned char)v; break;
    case 0x3CF: if (x->gi < 9) x->gc[x->gi] = (unsigned char)v; break;
    case 0x3C0:
        if (!x->ff)
            x->ai = (unsigned char)v;
        else if ((x->ai & 0x1F) < 21)
            x->ac[x->ai & 0x1F] = (unsigned char)v;
        x->ff ^= 1;
        break;
    }
    if (fg)
        outp(p, (unsigned char)v);
    return 0;
}

static void save(unsigned t)
{
    struct eg *x = &g[t];
    unsigned i;
    for (i = 0; i < 64; i++)
        img[i] = 0;
    img[0] = x->misc;
    for (i = 0; i < 5; i++)
        img[1 + i] = x->seq[i];
    for (i = 0; i < 25; i++)
        img[6 + i] = x->crtc[i];
    for (i = 0; i < 9; i++)
        img[31 + i] = x->gc[i];
    for (i = 0; i < 21; i++)
        img[40 + i] = x->ac[i];
}

static void restore(unsigned t)
{
    struct eg *x = &g[t];
    unsigned i, cb = x->misc & 1 ? 0x3D4 : 0x3B4;
    outp(0x3C4, 0);                     /* Sequenzer anhalten            */
    outp(0x3C5, 1);
    outp(0x3C2, x->misc);
    for (i = 1; i < 5; i++) {
        outp(0x3C4, (unsigned char)i);
        outp(0x3C5, x->seq[i]);
    }
    outp(0x3C4, 0);
    outp(0x3C5, x->seq[0] | 3);
    for (i = 0; i < 25; i++) {
        outp(cb, (unsigned char)i);
        outp(cb + 1, x->crtc[i]);
    }
    for (i = 0; i < 9; i++) {
        outp(0x3CE, (unsigned char)i);
        outp(0x3CF, x->gc[i]);
    }
    inp(cb + 6);                        /* Flipflop: Index               */
    for (i = 0; i < 21; i++) {
        outp(0x3C0, (unsigned char)i);
        outp(0x3C0, x->ac[i]);
    }
    outp(0x3C0, 0x20);                  /* Bild an                       */
    outp(0x3C4, x->si);                 /* Indizes wie in der Box        */
    outp(cb, x->ci);
    outp(0x3CE, x->gi);
    inp(cb + 6);
    if (x->ff)                          /* Box stand zwischen Index und  */
        outp(0x3C0, x->ai);             /* Wert                          */
}
#endif

static unsigned event(void __far *a)
{
    struct vdd_call __far *c = a;
    unsigned t = c->task, i;
    unsigned char *p;
    if (t >= NT)
        return 0;
    switch (c->fn) {
    case VDD_EV_CREATE:
        p = (unsigned char *)&g[t];
        for (i = 0; i < sizeof(g[0]); i++)
            p[i] = 0;
        break;
    case VDD_EV_VSAVE:
        save(t);
        sv.fn = VS_VREGS;
        sv.task = t;
        sv.buf = (unsigned)img;
        sv.len = 61;
        devhlp(DH_VDDSVC, 0, (unsigned)&sv, 0);
        break;
    case VDD_EV_VREST:
        restore(t);
        break;
    }
    return 0;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    reg.version = VDD_VERSION;
    reg.size = sizeof(reg);
    scat(reg.name, NAME);
    reg.flags = VDDF_ANYPORT | CLASSF;
    reg.io_off = fent(0, io);
    reg.ev_off = fent(1, event);
    if (devhlp(DH_VDDREG, 0, (unsigned)&reg, 0)) {
        dputs(NAME ".SYS: keine passende Grafikkarte (oder schon ein Grafikmodul)\n");
        r->status = DRV_ERR_NODEV;
        return;
    }
#ifdef CGA
    dputs("  Grafik der DOS-Boxen auf der CGA (Register mitgeschrieben)\n");
#else
    dputs("  Grafik der DOS-Boxen auf der EGA (Register mitgeschrieben)\n");
#endif
    r->units = 0;
    r->status = DRV_OK;
}

void strategy(void)
{
    struct drv_req __far *r = REQ;
    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_DEINIT:                    /* nicht entladbar: eine Box mit
                                           Rohgrafik braeuchte den Stand  */
        r->status = DRV_ERR_CMD;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
