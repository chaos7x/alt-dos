/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - Netzwerk: Vermittlung zwischen Kartentreibern und Stapel
 * (KTEXT4, Entwurf docs/NETZWERK.md, Schnittstelle include/net.h)
 *
 * Der Kernel kennt keine Protokolle. Er merkt sich die angemeldeten
 * Karten (DH_NETREG) und den Stapel (DH_NETPROTO), reicht Empfang,
 * Senden und Steuerung als ferne Aufrufe zwischen ihnen weiter, ruft aus
 * dem Scheduler den Nachlauf des Stapels und leitet FAPI_NET dorthin.
 * Ohne Kernelsperre: der Kernel wird nie unterbrochen ausser durch IRQs;
 * rx/done laufen mit gesperrten Interrupts, alles andere zwischen
 * Schlafpunkten.
 */
#include "kern.h"
#include "../include/drv.h"
#include "../include/net.h"
#include "../include/fapi_abi.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */

extern volatile unsigned long ticks;    /* start.asm                     */
extern int current;                     /* kernel.c: laufender Task      */

/* Fernaufruf cs:ip mit DS = ds, ES:BX = es:bx -> AX */
unsigned nf_call(unsigned cs, unsigned ip, unsigned ds, unsigned es, unsigned bx);
#pragma aux nf_call = \
    "push ds" "push bp" "push ax" "push dx" "mov bp,sp" "mov ds,si" \
    "call dword ptr ss:[bp]" "add sp,4" "pop bp" "pop ds" \
    __parm [__ax] [__dx] [__si] [__es] [__bx] __value [__ax] \
    __modify [__bx __cx __dx __si __di __es];

static struct net_reg nif[NNETIF];      /* Kopie der Anmeldung           */
static unsigned char nif_drv[NNETIF];   /* Treiber + 1, 0 = frei         */
static struct net_proto np;
static unsigned char np_drv;            /* Stapel: Treiber + 1           */
static volatile unsigned char net_pend; /* Nachlauf faellig              */
static unsigned char net_tick;          /* Stapel will je Takt laufen    */
static unsigned char net_busy;          /* Nachlauf laeuft gerade        */
static unsigned long net_last;
static struct net_pkt nl_pkt;           /* link_off                      */
static struct net_run nl_run;

#define PCS()   DRVC_SEL(np_drv - 1)
#define PDS()   DRVD_SEL(np_drv - 1)
#define CCS(i)  DRVC_SEL(nif_drv[i] - 1)
#define CDS(i)  DRVD_SEL(nif_drv[i] - 1)

static void fcopy(void __far *d, const void __far *s, unsigned n)
{
    unsigned char __far *a = d;
    const unsigned char __far *b = s;
    while (n--)
        *a++ = *b++;
}

static void link_tell(int i)            /* Stapel: neue Karte i          */
{
    if (!np_drv || !np.link_off)
        return;
    nl_pkt.ifc = i;
    nl_pkt.len = nl_pkt.off = nl_pkt.sel = 0;
    nf_call(PCS(), np.link_off, PDS(), KDATA, (unsigned)&nl_pkt);
}

/* DevHlp 23-30 (kernel.c devhlp_dispatch); d = Treiber des Aufrufers */
unsigned __far net_devhlp(unsigned fn, int d, unsigned dsel, unsigned bx, unsigned cx)
{
    struct net_pkt __far *p = MK_FP(dsel, bx);
    unsigned i, n;

    if (d < 0)
        return 0xFFFF;
    switch (fn) {
    case DH_NETREG: {
        const struct net_reg __far *r = MK_FP(dsel, bx);
        if (r->version != 1 || r->size < 20)
            return 0xFFFF;
        for (i = 0; i < NNETIF && nif_drv[i]; i++)
            ;
        if (i == NNETIF)
            return 0xFFFF;
        n = r->size < sizeof(struct net_reg) ? r->size : sizeof(struct net_reg);
        {
            unsigned char *z = (unsigned char *)&nif[i];
            unsigned k;
            for (k = 0; k < sizeof(struct net_reg); k++)
                z[k] = 0;
        }
        fcopy(&nif[i], r, n);
        nif_drv[i] = (unsigned char)(d + 1);
        link_tell(i);
        return i;
    }
    case DH_NETPROTO: {
        const struct net_proto __far *r = MK_FP(dsel, bx);
        unsigned char *z = (unsigned char *)&np;
        if (r->version != 1 || np_drv)
            return 1;
        for (i = 0; i < sizeof(np); i++)
            z[i] = 0;
        fcopy(&np, r, r->size < sizeof(np) ? r->size : sizeof(np));
        np_drv = (unsigned char)(d + 1);
        net_pend = 1;
        for (i = 0; i < NNETIF; i++)
            if (nif_drv[i])
                link_tell(i);
        return 0;
    }
    case DH_NETRX:                      /* Karte im IRQ: Platz holen     */
        p->sel = p->off = 0;
        if (!np_drv || p->ifc >= NNETIF || nif_drv[p->ifc] != d + 1)
            return 1;
        nf_call(PCS(), np.rx_off, PDS(), dsel, bx);
        return p->sel == 0;
    case DH_NETRXDONE:
        if (!np_drv || !p->sel)
            return 1;
        nf_call(PCS(), np.done_off, PDS(), dsel, bx);
        net_pend = 1;
        return 0;
    case DH_NETSEND:                    /* Stapel -> Karte               */
        i = p->ifc;
        if (i >= NNETIF || !nif_drv[i] || np_drv != d + 1)
            return NS_ERR;
        return nf_call(CCS(i), nif[i].send_off, CDS(i), dsel, bx);
    case DH_NETCTL:
        i = ((struct net_ctl __far *)p)->ifc;
        if (i >= NNETIF || !nif_drv[i] || !nif[i].ctl_off)
            return 1;
        return nf_call(CCS(i), nif[i].ctl_off, CDS(i), dsel, bx);
    case DH_NETINFO:
        if (bx >= NNETIF || !nif_drv[bx])
            return 1;
        fcopy(MK_FP(dsel, cx), &nif[bx], sizeof(struct net_reg));
        return 0;
    case DH_NETKICK:
        net_pend = 1;
        return 0;
    }
    return 0xFFFF;
}

/* Nachlauf: aus dem Scheduler (vor HLT, bei jedem Durchlauf) und aus dem
   Takt, wenn ein Programm in Ring 3 lief (preempt) */
void __far net_poll(void)
{
    unsigned long now;
    if (!np_drv || net_busy)
        return;
    now = ticks;
    if (!net_pend && !(net_tick && now != net_last))
        return;
    net_busy = 1;
    net_pend = 0;
    net_last = now;
    nl_run.ticks = now;
    net_tick = (unsigned char)(nf_call(PCS(), np.run_off, PDS(), KDATA,
                                       (unsigned)&nl_run) & 1);
    net_busy = 0;
}

/* FAPI_NET: BX = Unterfunktion, CX = Parameterblock, DX = Groesse */
unsigned __far net_fapi(unsigned usel, unsigned fn, unsigned p, unsigned n)
{
    struct net_call c;                  /* Kernelstapel: SS = KDATA      */
    if (!np_drv || !np.call_off)
        return FAPI_ERR_NONET;
    c.fn = fn;
    c.usel = usel;
    c.p = p;
    c.n = n;
    c.task = (unsigned)current;
    return nf_call(PCS(), np.call_off, PDS(), KDATA, (unsigned)&c);
}

/* DEVLOAD /U: Karte mit angemeldetem Stapel bleibt (1 = belegt) */
int __far net_inuse(int d)
{
    int i;
    for (i = 0; i < NNETIF; i++)
        if (nif_drv[i] == d + 1 && np_drv)
            return 1;
    return 0;
}

/* Treiber d ist weg (entladen oder INIT gescheitert) */
void __far net_drop(int d)
{
    int i;
    for (i = 0; i < NNETIF; i++)
        if (nif_drv[i] == d + 1)
            nif_drv[i] = 0;
    if (np_drv == d + 1) {
        np_drv = 0;
        net_tick = 0;
    }
}
