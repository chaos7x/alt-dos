/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - E1000.SYS: Intel PRO/1000 (8254x) - Karte von QEMU/Proxmox
 * ("e1000" = 82540EM), VirtualBox (82540EM, 82543GC, 82545EM) und VMware
 * ("e1000" = 82545EM).
 *
 * Register ueber den Speicherbereich (BAR 0, DH_PHYSMAP, 24 KB, 32-Bit-
 * Zugriffe) - den I/O-Zugang (IOADDR/IODATA) bildet QEMU nicht nach. Ringe:
 * 8 Empfangspuffer zu 2.048 Byte (RCTL.BSIZE), 8 Sendepuffer zu 1.536 Byte,
 * Deskriptoren zu 16 Byte (klassisches Format), alles in einem DH_DMABUF-
 * Puffer. Mehrfachempfang: alle (RCTL.MPE, NF_ALLMULTI). Nachschlagewerk:
 * Intel "PCI/PCI-X Family of Gigabit Ethernet Controllers Software
 * Developer's Manual" (8254x); Nachbildung in QEMU nur gelesen.
 *
 * Schalter: /NOIRQ (Abfrage aus dem Nachlauf von TCPIP.SYS), /PROMISC.
 */
#include "drvlib.h"
#include "netlib.h"

unsigned long mrd(void __far *p);
#pragma aux mrd = "push ecx" "mov ecx,es:[bx]" "mov ax,cx" "shr ecx,16" \
    "mov dx,cx" "pop ecx" \
    __parm [__es __bx] __value [__dx __ax] __modify __exact [__ax __dx];
void mwr(void __far *p, unsigned lo, unsigned hi);
#pragma aux mwr = "push ecx" "mov cx,dx" "shl ecx,16" "mov cx,ax" \
    "mov es:[bx],ecx" "pop ecx" \
    __parm [__es __bx] [__ax] [__dx] __modify __exact [];

/* Register */
#define CTRL    0x0000
#define STATUS  0x0008
#define EERD    0x0014
#define ICR     0x00C0
#define IMS     0x00D0
#define IMC     0x00D8
#define RCTL    0x0100
#define TCTL    0x0400
#define TIPG    0x0410
#define RDBAL   0x2800
#define RDBAH   0x2804
#define RDLEN   0x2808
#define RDH     0x2810
#define RDT     0x2818
#define TDBAL   0x3800
#define TDBAH   0x3804
#define TDLEN   0x3808
#define TDH     0x3810
#define TDT     0x3818
#define MTA     0x5200
#define RAL0    0x5400
#define RAH0    0x5404

#define I_TXDW  0x0001
#define I_LSC   0x0004
#define I_RXDMT 0x0010
#define I_RXO   0x0040
#define I_RXT0  0x0080
#define I_ALL   (I_TXDW | I_LSC | I_RXDMT | I_RXO | I_RXT0)

#define NRX     8
#define NTX     8
#define RBSZ    2048u
#define TBSZ    1536u

#pragma pack(1)
struct rdesc {
    unsigned long  addr, addr_hi;
    unsigned short len, csum;
    unsigned char  status, err;
    unsigned short special;
};
struct tdesc {
    unsigned long  addr, addr_hi;
    unsigned short len;
    unsigned char  cso, cmd, status, css;
    unsigned short special;
};
#pragma pack()

#define O_RX    0                       /* 8 x 16                           */
#define O_TX    128                     /* 8 x 16                           */
#define O_RBUF  256
#define O_TBUF  (O_RBUF + NRX * RBSZ)
#define BUFSZ   (O_TBUF + NTX * TBSZ)   /* 28.928 Byte                      */

static unsigned mm, irq, ploc, dev;
static unsigned long ph;
static unsigned bsel;
static unsigned long bphys;
static unsigned rxi, txi, txd, txn;
static int ifc = -1;
static unsigned irq_on;
static unsigned long rctl;
static struct net_reg reg;
static struct net_pkt rp;
static struct net_stats st;
static char line[100];

#define RXD(i)  ((struct rdesc __far *)MK_FP(bsel, O_RX + (i) * 16))
#define TXD(i)  ((struct tdesc __far *)MK_FP(bsel, O_TX + (i) * 16))

static unsigned long rr(unsigned r)
{
    return mrd(MK_FP(mm, r));
}

static void wr(unsigned r, unsigned long v)
{
    mwr(MK_FP(mm, r), (unsigned)v, (unsigned)(v >> 16));
}

static void rx(void)
{
    struct rdesc __far *d;
    unsigned k, len;
    for (k = 0; k < NRX; k++) {
        d = RXD(rxi);
        if (!(d->status & 1))           /* DD: fertig                      */
            break;
        len = d->len;
        if (!(d->status & 2) || (d->err & 0x97) || len < 14 || len > 1518)
            st.rx_err++;
        else {
            rp.ifc = ifc;
            rp.len = len;               /* ohne CRC (RCTL.SECRC)            */
            if (devhlp(DH_NETRX, 0, (unsigned)&rp, 0) == 0) {
                fmove(MK_FP(rp.sel, rp.off), MK_FP(bsel, O_RBUF + rxi * RBSZ), rp.len);
                devhlp(DH_NETRXDONE, 0, (unsigned)&rp, 0);
                st.rx_ok++;
            } else
                st.rx_drop++;
        }
        d->status = 0;                  /* QEMU schreibt err/len nicht immer */
        d->err = 0;
        d->len = 0;
        wr(RDT, rxi);                   /* Platz zurueck an die Karte       */
        rxi = (rxi + 1) & (NRX - 1);
    }
}

static void tx_reap(void)
{
    while (txn && (TXD(txd)->status & 1)) {
        st.tx_ok++;
        txd = (txd + 1) & (NTX - 1);
        txn--;
    }
}

static void e_irq(unsigned n)
{
    unsigned long s;
    unsigned k;
    (void)n;
    for (k = 0; k < 8; k++) {
        s = rr(ICR);                    /* Lesen quittiert                  */
        if (!((unsigned)s & I_ALL))
            break;
        if ((unsigned)s & I_RXO)
            st.rx_drop++;
        if ((unsigned)s & (I_RXT0 | I_RXDMT | I_RXO))
            rx();
        if ((unsigned)s & I_TXDW)
            tx_reap();
    }
}

static unsigned e_send(void __far *v)
{
    struct net_pkt __far *p = v;
    struct tdesc __far *d;
    unsigned char __far *t;
    unsigned len = p->len;

    if (len > 1514)
        return NS_ERR;
    cli();
    tx_reap();
    if (txn >= NTX - 1) {               /* ein Platz bleibt frei (TDT != TDH) */
        sti();
        return NS_FULL;
    }
    t = MK_FP(bsel, O_TBUF + txi * TBSZ);
    fmove(t, MK_FP(p->sel, p->off), len);
    while (len < 60)
        t[len++] = 0;
    d = TXD(txi);
    d->len = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = 0x0B;                      /* EOP | IFCS | RS                 */
    txi = (txi + 1) & (NTX - 1);
    txn++;
    wr(TDT, txi);
    sti();
    return NS_OK;
}

static unsigned e_ctl(void __far *v)
{
    struct net_ctl __far *c = v;
    unsigned s;
    switch (c->cmd) {
    case NC_LINK:
        cli();
        s = (unsigned)rr(STATUS);
        sti();
        c->result = (s & 2 ? 1 : 0) | (s & 1 ? 2 : 0) |
                    ((s >> 6 & 3) == 0 ? 1 : (s >> 6 & 3) == 1 ? 10 : 100) << 8;
        return 0;
    case NC_MCAST:
        return 0;
    case NC_PROMISC:
        cli();
        rctl = c->arg ? rctl | 0x08 : rctl & ~0x08UL;
        wr(RCTL, rctl);
        sti();
        return 0;
    case NC_POLL:
        if (!irq_on)
            e_irq(0);
        return 0;
    case NC_STATS:
        fmove(MK_FP(c->sel, c->off), &st, sizeof(st));
        return 0;
    }
    return 1;
}

/* MAC aus dem EEPROM (EERD), falls RAL/RAH nach dem Reset leer sind */
static unsigned ee_word(unsigned a)
{
    unsigned k;
    unsigned long v;
    wr(EERD, (unsigned long)a << 8 | 1);
    for (k = ticks(); (unsigned)(ticks() - k) < 10; ) {
        v = rr(EERD);
        if (v & 0x10)
            return (unsigned)(v >> 16);
    }
    return 0;
}

static const unsigned ids[] = {
    0x100E, 0x100F, 0x1004, 0x1008, 0x1009, 0x100C, 0x100D, 0x1011,
    0x1015, 0x1016, 0x1017, 0x101E, 0x1026, 0x1027, 0x1028, 0x1076, 0x1079, 0
};

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned long v, lo, hi;
    unsigned i, k;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    for (i = 0; ids[i]; i++)
        if ((ploc = pci_find(0x8086, ids[i], 0)) != 0xFFFF)
            break;
    if (!ids[i]) {
        dputs("  E1000.SYS: keine Intel PRO/1000 (8254x) gefunden\n");
        return;
    }
    dev = ids[i];
    v = pci_rd(ploc, 0x10);             /* BAR 0: Register (Speicher)      */
    if ((v & 1) || (v & 6) == 4 && pci_rd(ploc, 0x14)) {
        dputs("  E1000.SYS: Register nicht erreichbar (BAR 0)\n");
        return;
    }
    ph = v & ~0xFUL;
    irq = (unsigned)pci_rd(ploc, 0x3C) & 0xFF;
    v = pci_rd(ploc, 0x04);
    pci_wr(ploc, 0x04, (unsigned)v | 0x0006, 2);        /* Speicher + Busmaster */
    mm = devhlp(DH_PHYSMAP, (unsigned)(ph >> 16), 0x6000, (unsigned)ph);
    if (!mm) {
        dputs("  E1000.SYS: kein Platz zum Einblenden der Register\n");
        return;
    }

    wr(IMC, 0xFFFFFFFFUL);
    wr(CTRL, rr(CTRL) | 0x04000000UL);  /* RST                             */
    for (k = ticks(); (unsigned)(ticks() - k) < 2; )
        ;
    for (k = ticks(); rr(CTRL) & 0x04000000UL; )
        if ((unsigned)(ticks() - k) > 100) {
            dputs("  E1000.SYS: Karte reagiert nicht auf Reset\n");
            return;
        }
    wr(IMC, 0xFFFFFFFFUL);
    (void)rr(ICR);
    wr(CTRL, (rr(CTRL) | 0x60UL) & ~0x88UL);    /* SLU, ASDE; ohne ILOS/PHY_RST */

    lo = rr(RAL0);
    hi = rr(RAH0);
    if (!lo && !(hi & 0xFFFF)) {
        for (i = 0; i < 3; i++) {
            k = ee_word(i);
            reg.mac[2 * i] = (unsigned char)k;
            reg.mac[2 * i + 1] = (unsigned char)(k >> 8);
        }
        lo = reg.mac[0] | (unsigned long)reg.mac[1] << 8 |
             (unsigned long)reg.mac[2] << 16 | (unsigned long)reg.mac[3] << 24;
        hi = reg.mac[4] | (unsigned)reg.mac[5] << 8;
        wr(RAL0, lo);
    }
    wr(RAH0, (hi & 0xFFFF) | 0x80000000UL);     /* AV: Adresse gilt      */
    for (i = 0; i < 4; i++)
        reg.mac[i] = (unsigned char)(lo >> (8 * i));
    reg.mac[4] = (unsigned char)hi;
    reg.mac[5] = (unsigned char)(hi >> 8);
    for (i = 0; i < 128; i++)
        wr(MTA + 4 * i, 0);

    bsel = devhlp(DH_DMABUF, 0, BUFSZ, (unsigned)&bphys);
    if (!bsel) {
        dputs("  E1000.SYS: kein Speicher f\x81r die Puffer\n");
        return;
    }
    for (i = 0; i < NRX; i++) {
        struct rdesc __far *d = RXD(i);
        d->addr = bphys + O_RBUF + (unsigned long)(i * RBSZ);
        d->addr_hi = 0;
        d->len = d->csum = d->special = 0;
        d->status = d->err = 0;
    }
    for (i = 0; i < NTX; i++) {
        struct tdesc __far *d = TXD(i);
        d->addr = bphys + O_TBUF + (unsigned long)(i * TBSZ);
        d->addr_hi = 0;
        d->len = d->special = 0;
        d->cso = d->css = 0;
        d->status = 1;
        d->cmd = 0;
    }
    rxi = txi = txd = txn = 0;
    wr(RDBAL, bphys + O_RX);
    wr(RDBAH, 0);
    wr(RDLEN, NRX * 16);
    wr(RDH, 0);
    wr(RDT, NRX - 1);
    wr(TDBAL, bphys + O_TX);
    wr(TDBAH, 0);
    wr(TDLEN, NTX * 16);
    wr(TDH, 0);
    wr(TDT, 0);
    wr(TIPG, 0x0060200AUL);
    wr(TCTL, 0x0004010AUL);             /* EN, PSP, CT 10h, COLD 40h       */
    /* EN, BAM, MPE, SECRC, Puffer 2048; /PROMISC: UPE */
    rctl = 0x04008012UL | (has_opt(r->args, "/PROMISC") ? 0x08 : 0);
    wr(RCTL, rctl);

    k = irq && irq < 16 ? (has_opt(r->args, "/NOIRQ") ? 2 : set_irq(irq, e_irq) ? 1 : 0) : 3;
    irq_on = k == 0;
    (void)rr(ICR);
    if (irq_on)
        wr(IMS, I_ALL);

    reg.version = 1;
    reg.size = sizeof(reg);
    reg.mtu = 1500;
    reg.features = NF_ALLMULTI;
    reg.send_off = fent(0, e_send);
    reg.ctl_off = fent(1, e_ctl);
    scat(reg.name, "E1000");
    reg.irq = irq_on ? irq : 0;
    reg.io = 0;
    ifc = (int)devhlp(DH_NETREG, 0, (unsigned)&reg, 0);
    if (ifc < 0 || ifc >= NNETIF) {
        wr(IMC, 0xFFFFFFFFUL);
        wr(RCTL, 0);
        wr(TCTL, 0);
        if (irq_on)
            unset_irq(irq);
        dputs("  E1000.SYS: kein Platz f\x81r eine weitere Netzwerkkarte\n");
        return;
    }

    p = scat(line, "  Intel PRO/1000 (8086:");
    p = hex(p, dev, 4);
    p = scat(p, ", PCI ");
    p = hex(p, ploc >> 8, 2);
    p = scat(p, ":");
    p = hex(p, (ploc >> 3) & 31, 2);
    p = scat(p, ") an ");
    p = hex(p, (unsigned)(ph >> 16), 4);
    p = hex(p, (unsigned)ph, 4);
    p = scat(p, "h, IRQ ");
    p = udec(p, irq);
    p = scat(p, k == 1 ? " belegt" : k == 2 ? " nicht benutzt" : k == 3 ? " ung\x81ltig" : "");
    p = scat(p, ", MAC ");
    for (i = 0; i < 6; i++) {
        p = hex(p, reg.mac[i], 2);
        if (i < 5)
            *p++ = ':';
    }
    p = scat(p, ", Karte ");
    *p++ = (char)('0' + ifc);
    scat(p, "\n");
    dputs(line);
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
    case DRV_DEINIT:                    /* DEVLOAD /U: Karte anhalten      */
        wr(IMC, 0xFFFFFFFFUL);
        wr(RCTL, 0);
        wr(TCTL, 0);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
