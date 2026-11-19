/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - Module fuer die DOS-Box: Anmeldung und Vermittlung
 * (KTEXT4, Entwurf docs/VDD.md, Schnittstelle include/vdd.h)
 *
 * Ein Modul ist ein Treiber, der sich in INIT mit DH_VDDREG meldet. Der
 * Monitor (v86.c) ruft vdd_io/vdd_int/vdd_event, wenn er einen Port oder
 * Interrupt nicht selbst behandelt bzw. bei Ereignissen der Box. Hier
 * steht nur die Tabelle und der Fernaufruf; die Nachbildung der Geraete
 * liegt im Modul (DS = Moduldaten, ES:BX = struct vdd_call).
 */
#include "kern.h"
#include "../include/drv.h"
#include "../include/vdd.h"
#include "../include/fapi_abi.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */

unsigned __far kf_box_fapi(unsigned fn, unsigned usel, unsigned a1, unsigned a2, unsigned a3);
unsigned __far kf_box_aud(unsigned usel, unsigned fn, unsigned a, unsigned b);
int  __far kf_box_ctl(unsigned op, unsigned a);

/* Fernaufruf cs:ip mit DS = ds, ES:BX = es:bx -> AX (wie net.c) */
unsigned vd_call(unsigned cs, unsigned ip, unsigned ds, unsigned es, unsigned bx);
#pragma aux vd_call = \
    "push ds" "push bp" "push ax" "push dx" "mov bp,sp" "mov ds,si" \
    "call dword ptr ss:[bp]" "add sp,4" "pop bp" "pop ds" \
    __parm [__ax] [__dx] [__si] [__es] [__bx] __value [__ax] \
    __modify [__bx __cx __dx __si __di __es];

typedef char vdd_chk[VDD_NTASK == NTASK ? 1 : -1];
static struct vdd_reg vt[NVDD];         /* Kopie der Anmeldung           */
static unsigned char vt_drv[NVDD];      /* Treiber + 1, 0 = frei         */
static unsigned char vd_any;            /* ein Modul will "alle Ports"   */
#define CS(i)   DRVC_SEL(vt_drv[i] - 1)
#define DS(i)   DRVD_SEL(vt_drv[i] - 1)

static void fcopy(void __far *d, const void __far *s, unsigned n)
{
    unsigned char __far *a = d;
    const unsigned char __far *b = s;
    while (n--)
        *a++ = *b++;
}

/* DH_VDDREG: d = Treiber des Aufrufers */
unsigned __far vdd_reg(int d, unsigned dsel, unsigned bx)
{
    const struct vdd_reg __far *r = MK_FP(dsel, bx);
    unsigned i, k;
    if (d < 0 || r->version != VDD_VERSION || r->size < sizeof(struct vdd_reg))
        return 1;
    for (i = 0; i < NVDD && vt_drv[i]; i++)
        if (vt_drv[i] == d + 1)
            return 1;                   /* schon angemeldet              */
    for (i = 0; i < NVDD && vt_drv[i]; i++)
        ;
    if (i == NVDD)
        return 1;
    fcopy(&vt[i], r, sizeof(struct vdd_reg));
    vt_drv[i] = (unsigned char)(d + 1);
    for (vd_any = 0, k = 0; k < NVDD; k++)
        if (vt_drv[k] && (vt[k].flags & VDDF_ANYPORT) && vt[k].io_off)
            vd_any = 1;
    return 0;
}

/* Treiber d ist weg (entladen oder INIT gescheitert) */
void __far vdd_drop(int d)
{
    unsigned i;
    for (i = 0; i < NVDD; i++)
        if (vt_drv[i] == d + 1)
            vt_drv[i] = 0;
    for (vd_any = 0, i = 0; i < NVDD; i++)
        if (vt_drv[i] && (vt[i].flags & VDDF_ANYPORT) && vt[i].io_off)
            vd_any = 1;
}

/* Port, den der Monitor nicht kennt: IN -> Wert, OUT -> 0; VDD_NOTMINE,
   wenn kein Modul ihn nimmt */
unsigned __far vdd_io(int t, unsigned fn, unsigned port, unsigned val, int fgb)
{
    struct vdd_call c;                  /* Kernelstapel (SS = KDATA): das
                                           Modul darf ueber VS_FAPI schlafen */
    unsigned i, r;
    if (!vd_any)
        return VDD_NOTMINE;
    for (i = 0; i < NVDD; i++) {
        if (!vt_drv[i] || !vt[i].io_off || !(vt[i].flags & VDDF_ANYPORT))
            continue;
        c.fn = fn;
        c.task = (unsigned)t;
        c.port = port;
        c.val = val;
        c.flags = (unsigned)fgb;
        r = vd_call(CS(i), vt[i].io_off, DS(i), KDATA, (unsigned)&c);
        if (r != VDD_NOTMINE)
            return fn == VDD_IN || fn == VDD_INW ? c.val : 0;
    }
    return VDD_NOTMINE;
}

/* Interrupt n der Box, den ein Modul angemeldet hat: Register in c (der
   Monitor fuellt und uebernimmt sie) -> 0 erledigt, VDD_NOTMINE */
unsigned __far vdd_int(int t, unsigned n, struct vdd_call *c)
{
    unsigned i, k;
    for (i = 0; i < NVDD; i++) {
        if (!vt_drv[i] || !vt[i].int_off)
            continue;
        for (k = 0; k < VDD_NINT; k++)
            if (vt[i].ints[k] == n && n)
                break;
        if (k == VDD_NINT)
            continue;
        c->fn = VDD_INT;
        c->task = (unsigned)t;
        c->port = n;
        if (vd_call(CS(i), vt[i].int_off, DS(i), KDATA, (unsigned)c) != VDD_NOTMINE)
            return 0;
    }
    return VDD_NOTMINE;
}

/* Ereignis der Box an alle Module (val: VDD_EV_PMHOOK Vektor) */
void __far vdd_event(int t, unsigned ev, unsigned val)
{
    struct vdd_call c;
    unsigned i;
    for (i = 0; i < NVDD; i++) {
        if (!vt_drv[i] || !vt[i].ev_off)
            continue;
        c.fn = ev;
        c.task = (unsigned)t;
        c.port = 0;
        c.val = val;
        vd_call(CS(i), vt[i].ev_off, DS(i), KDATA, (unsigned)&c);
    }
}

/* Takt an die Module, die ihn wollen (v86.c ruft nur, wenn eins fuer diese
   Box VS_TICK gesetzt hat; IF an) */
void __far vdd_tick(int t, int fgb)
{
    struct vdd_call c;
    unsigned i;
    for (i = 0; i < NVDD; i++) {
        if (!vt_drv[i] || !vt[i].tick_off)
            continue;
        c.fn = VDD_TICK;
        c.task = (unsigned)t;
        c.port = c.val = 0;
        c.flags = (unsigned)fgb;        /* Bit 0: Box vorn               */
        vd_call(CS(i), vt[i].tick_off, DS(i), KDATA, (unsigned)&c);
    }
}

/* DH_VDDSVC: Dienste fuer ein Modul (Auftrag im DS des Moduls). Die
   Felder werden vorher kopiert: VS_FAPI kann schlafen, und das Modul
   kann den Auftrag fuer eine andere Box wiederverwenden. */
unsigned __far vdd_svc(int d, unsigned dsel, unsigned bx)
{
    struct vdd_svc k;                   /* Kopie auf dem Kernelstapel    */
    unsigned short w[8];
    unsigned char vs_buf[VDD_FBUF];     /* Kernelstapel: die FAPI kann
                                           schlafen (Sperre im Treiber
                                           abgegeben), andere Boxen auch */
    unsigned a[3], r, n;
    if (d < 0)
        return 0xFFFF;
    fcopy(&k, MK_FP(dsel, bx), sizeof(k));
    if (k.task >= NTASK)
        return 0xFFFF;
    switch (k.fn) {
    case VS_RD:                         /* Box-Speicher -> Puffer        */
    case VS_WR:                         /* Puffer -> Box-Speicher        */
        if (k.lin >= 0x400000UL || k.len > 0x400000UL - k.lin)
            return 0xFFFF;              /* nur die Seitentabelle pt_box  */
        win_save(w);
        v86_xfer(k.task, k.lin, dsel, k.buf, k.len, k.fn == VS_RD);
        win_restore(w);
        return 0;
    case VS_AUD:                        /* Ton im Namen der Box          */
        if (k.ffn < 0xFE && (int)k.task != kf_box_ctl(BOX_TASK, 0))
            return 0xFFFF;              /* (der Kernel ordnet Ton dem
                                           laufenden Task zu)            */
        return kf_box_aud(dsel, k.ffn, k.a1, k.a2);
    case VS_FAPI:                       /* im Namen der Box (current)    */
        n = k.len > VDD_FBUF ? VDD_FBUF : k.len;
        a[0] = k.a1;
        a[1] = k.a2;
        a[2] = k.a3;
        if (k.pa >= 1 && k.pa <= 3) {
            fcopy(vs_buf, MK_FP(dsel, k.buf), n);
            a[k.pa - 1] = (unsigned)vs_buf;
        }
        kf_box_ctl(BOX_LOCK, 1);
        r = kf_box_fapi(k.ffn, KDATA, a[0], a[1], a[2]);
        kf_box_ctl(BOX_LOCK, 0);
        if (k.pa >= 1 && k.pa <= 3)
            fcopy(MK_FP(dsel, k.buf), vs_buf, n);
        return r;
    }
    if (k.fn >= VS_IRQ && k.fn <= VS_HK)        /* Zustand der Box       */
        return v86_vsvc(k.task, k.fn, k.a1, k.a2, k.a3);
    return 0xFFFF;
}
