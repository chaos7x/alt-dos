/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - PCNET.SYS: AMD PCnet-PCI (Am79C970/970A/971/972/973/975,
 * PCI 1022h:2000h) - Karte von VMware ("vlance"), VirtualBox, QEMU
 * ("pcnet") und 86Box ("pcnetpci", "pcnetfast" ...).
 *
 * Betrieb: 16-Bit-Ports (WIO, Zustand nach dem Reset), Software-Stil 2
 * (32-Bit-Deskriptoren zu 16 Byte, BCR20), Ringe 16 Empfangs- und 8
 * Sendepuffer zu 1.536 Byte in einem DH_DMABUF-Puffer (physisch am
 * Stueck, Busmaster). Der Initialisierungsblock liegt im selben Puffer.
 * Mehrfachempfang ueber ein ganz gesetztes Adressfilter (NF_ALLMULTI).
 * Nachschlagewerk: AMD-Datenblatt Am79C970A; Verhalten der Nachbildungen
 * in QEMU/86Box nur gelesen.
 *
 * Schalter: /NOIRQ (Abfrage aus dem Nachlauf von TCPIP.SYS), /PROMISC.
 */
#include "drvlib.h"
#include "netlib.h"

/* Ports im WIO-Betrieb */
#define APROM   0x00                    /* MAC in den ersten 6 Byte         */
#define RDP     0x10                    /* Daten des CSR, auf das RAP zeigt */
#define RAP     0x12
#define RESET   0x14                    /* Lesen = Reset                    */
#define BDP     0x16                    /* Daten des BCR                    */

/* CSR0 */
#define C0_INIT 0x0001
#define C0_STRT 0x0002
#define C0_STOP 0x0004
#define C0_TDMD 0x0008
#define C0_IENA 0x0040
#define C0_IDON 0x0100
#define C0_TINT 0x0200
#define C0_RINT 0x0400
#define C0_MERR 0x0800
#define C0_MISS 0x1000
#define C0_CERR 0x2000
#define C0_BABL 0x4000
#define C0_ACK  (C0_IDON | C0_TINT | C0_RINT | C0_MERR | C0_MISS | C0_CERR | C0_BABL)

#define D_OWN   0x8000                  /* Statuswort der Deskriptoren      */
#define D_ERR   0x4000
#define D_STP   0x0200
#define D_ENP   0x0100

#define NRX     16                      /* 2^4                              */
#define NTX     8                       /* 2^3                              */
#define BSZ     1536u

#pragma pack(1)
struct desc {                           /* Software-Stil 2                  */
    unsigned long  base;
    unsigned short blen;                /* -Laenge | F000h                  */
    unsigned short stat;
    unsigned long  mcnt;                /* Empfang: Laenge mit CRC          */
    unsigned long  rsv;
};
struct initblk {
    unsigned short mode;
    unsigned short lens;                /* RLEN Bits 4-7, TLEN Bits 12-15   */
    unsigned char  padr[6];
    unsigned short rsv;
    unsigned char  ladrf[8];
    unsigned long  rdra, tdra;
};
#pragma pack()

/* Aufteilung des Puffers */
#define O_RX    0                       /* 16 x 16                          */
#define O_TX    (O_RX + NRX * 16)       /* 8 x 16                           */
#define O_IB    (O_TX + NTX * 16)       /* 28                               */
#define O_RBUF  512
#define O_TBUF  (O_RBUF + NRX * BSZ)
#define BUFSZ   (O_TBUF + NTX * BSZ)    /* 37.376 Byte                      */

static unsigned io, irq, ploc;
static unsigned bsel;
static unsigned long bphys;
static unsigned rxi, txi, txd, txn;     /* naechster Empfang, Senden, Reap  */
static int ifc = -1;
static unsigned irq_on, promisc;
static struct net_reg reg;
static struct net_pkt rp;
static struct net_stats st;
static char line[96];

#define RXD(i)  ((struct desc __far *)MK_FP(bsel, O_RX + (i) * 16))
#define TXD(i)  ((struct desc __far *)MK_FP(bsel, O_TX + (i) * 16))

static unsigned csr_rd(unsigned r)
{
    outw(io + RAP, r);
    return inw(io + RDP);
}

static void csr_wr(unsigned r, unsigned v)
{
    outw(io + RAP, r);
    outw(io + RDP, v);
}

static void bcr_wr(unsigned r, unsigned v)
{
    outw(io + RAP, r);
    outw(io + BDP, v);
}

static void rx(void)
{
    struct desc __far *d;
    unsigned k, len, s;
    for (k = 0; k < NRX; k++) {
        d = RXD(rxi);
        s = d->stat;
        if (s & D_OWN)
            break;
        len = (unsigned)d->mcnt & 0x0FFF;
        if ((s & (D_ERR | D_STP | D_ENP)) != (D_STP | D_ENP) || len < 18 || len > 1518)
            st.rx_err++;
        else {
            rp.ifc = ifc;
            rp.len = len - 4;           /* ohne CRC                         */
            if (devhlp(DH_NETRX, 0, (unsigned)&rp, 0) == 0) {
                fmove(MK_FP(rp.sel, rp.off), MK_FP(bsel, O_RBUF + rxi * BSZ), rp.len);
                devhlp(DH_NETRXDONE, 0, (unsigned)&rp, 0);
                st.rx_ok++;
            } else
                st.rx_drop++;
        }
        d->mcnt = 0;
        d->blen = (unsigned)(-BSZ) | 0xF000;
        d->stat = D_OWN;                /* zurueck an die Karte             */
        rxi = (rxi + 1) & (NRX - 1);
    }
}

static void tx_reap(void)
{
    struct desc __far *d;
    while (txn) {
        d = TXD(txd);
        if (d->stat & D_OWN)
            break;
        if (d->stat & D_ERR)
            st.tx_err++;
        else
            st.tx_ok++;
        txd = (txd + 1) & (NTX - 1);
        txn--;
    }
}

static void pc_irq(unsigned n)
{
    unsigned s, k;
    (void)n;
    outw(io + RAP, 0);                  /* RAP steht immer auf CSR0          */
    for (k = 0; k < 8; k++) {
        s = inw(io + RDP);
        if (!(s & C0_ACK))
            break;
        outw(io + RDP, (s & C0_ACK) | C0_IENA);     /* 1 schreiben = quittieren */
        if (s & (C0_MISS | C0_BABL | C0_CERR))
            st.rx_drop++;
        if (s & C0_MERR)
            st.rx_err++;
        if (s & C0_RINT)
            rx();
        if (s & C0_TINT)
            tx_reap();
    }
}

static unsigned pc_send(void __far *v)
{
    struct net_pkt __far *p = v;
    struct desc __far *d;
    unsigned char __far *t;
    unsigned len = p->len;

    if (len > 1514)
        return NS_ERR;
    cli();                              /* txn/txd teilt sich der IRQ      */
    tx_reap();
    if (txn >= NTX) {
        sti();
        return NS_FULL;
    }
    t = MK_FP(bsel, O_TBUF + txi * BSZ);
    fmove(t, MK_FP(p->sel, p->off), len);
    while (len < 60)
        t[len++] = 0;
    d = TXD(txi);
    d->blen = (unsigned)(-(int)len) | 0xF000;
    d->rsv = 0;
    d->mcnt = 0;
    d->stat = D_OWN | D_STP | D_ENP;
    txi = (txi + 1) & (NTX - 1);
    txn++;
    outw(io + RAP, 0);
    outw(io + RDP, C0_TDMD | (irq_on ? C0_IENA : 0));   /* sofort senden */
    sti();
    return NS_OK;
}

static unsigned pc_ctl(void __far *v)
{
    struct net_ctl __far *c = v;
    unsigned b;
    switch (c->cmd) {
    case NC_LINK:
        cli();
        outw(io + RAP, 4);              /* BCR4: LED0 = Verbindung          */
        b = inw(io + BDP);
        outw(io + RAP, 0);
        sti();
        c->result = (b & 0x8000 ? 1 : 0) | (10 << 8);
        return 0;
    case NC_MCAST:                      /* Filter ist ganz offen            */
        return 0;
    case NC_POLL:
        if (!irq_on)
            pc_irq(0);
        return 0;
    case NC_STATS:
        fmove(MK_FP(c->sel, c->off), &st, sizeof(st));
        return 0;
    }
    return 1;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    struct initblk __far *ib;
    unsigned long v;
    unsigned i, k, n;
    char *p;
    static const char *const names[] = {
        "Am79C970", "Am79C970A", "Am79C971", "Am79C972", "Am79C973", "Am79C975", "PCnet"
    };

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    ploc = pci_find(0x1022, 0x2000, 0);
    if (ploc == 0xFFFF) {
        dputs("  PCNET.SYS: keine AMD PCnet-PCI gefunden\n");
        return;
    }
    v = pci_rd(ploc, 0x10);             /* BAR 0: I/O                      */
    if (!(v & 1)) {
        dputs("  PCNET.SYS: BAR 0 ist kein I/O-Bereich\n");
        return;
    }
    io = (unsigned)v & 0xFFFC;
    irq = (unsigned)pci_rd(ploc, 0x3C) & 0xFF;
    if (claim(RES_IO, io, 0x20)) {
        dputs("  PCNET.SYS: Ports geh\x94ren schon einem anderen Treiber\n");
        return;
    }
    v = pci_rd(ploc, 0x04);
    pci_wr(ploc, 0x04, (unsigned)v | 0x0005, 2);        /* I/O + Busmaster */

    (void)inw(io + RESET);              /* Reset, danach 16-Bit-Betrieb     */
    for (k = ticks(); (unsigned)(ticks() - k) < 2; )
        ;
    if (csr_rd(0) != C0_STOP) {         /* erst lesen: passt es?           */
        dputs("  PCNET.SYS: Karte antwortet nicht wie eine PCnet\n");
        return;
    }
    v = (unsigned long)csr_rd(89) << 16 | csr_rd(88);   /* Chip-Kennung    */
    switch ((unsigned)(v >> 12) & 0xFFFF) {
    case 0x2420: n = 0; break;
    case 0x2621: n = 1; break;
    case 0x2623: n = 2; break;
    case 0x2624: n = 3; break;
    case 0x2625: n = 4; break;
    case 0x2627: n = 5; break;
    default: n = 6;
    }
    bsel = devhlp(DH_DMABUF, 0, BUFSZ, (unsigned)&bphys);
    if (!bsel) {
        dputs("  PCNET.SYS: kein Speicher f\x81r die Puffer\n");
        return;
    }
    promisc = has_opt(r->args, "/PROMISC");
    for (i = 0; i < 6; i++)
        reg.mac[i] = inp(io + APROM + i);

    for (i = 0; i < NRX; i++) {
        struct desc __far *d = RXD(i);
        d->base = bphys + O_RBUF + (unsigned long)(i * BSZ);
        d->blen = (unsigned)(-BSZ) | 0xF000;
        d->mcnt = 0;
        d->rsv = 0;
        d->stat = D_OWN;
    }
    for (i = 0; i < NTX; i++) {
        struct desc __far *d = TXD(i);
        d->base = bphys + O_TBUF + (unsigned long)(i * BSZ);
        d->blen = 0;
        d->stat = 0;
        d->mcnt = d->rsv = 0;
    }
    ib = MK_FP(bsel, O_IB);
    ib->mode = promisc ? 0x8000 : 0;
    ib->lens = 4 << 4 | 3 << 12;        /* 16 Empfang, 8 Senden            */
    for (i = 0; i < 6; i++)
        ib->padr[i] = reg.mac[i];
    ib->rsv = 0;
    for (i = 0; i < 8; i++)
        ib->ladrf[i] = 0xFF;            /* alle Mehrfachadressen           */
    ib->rdra = bphys + O_RX;
    ib->tdra = bphys + O_TX;
    rxi = txi = txd = txn = 0;

    bcr_wr(20, 2);                      /* Software-Stil 2 (32 Bit)         */
    csr_wr(1, (unsigned)(bphys + O_IB));
    csr_wr(2, (unsigned)((bphys + O_IB) >> 16));
    csr_wr(3, 0x0100);                  /* IDON nicht als Interrupt         */
    csr_wr(4, csr_rd(4) | 0x0800);      /* kurze Rahmen selbst auffuellen   */
    csr_wr(0, C0_INIT);
    for (k = ticks(); !(csr_rd(0) & C0_IDON); )
        if ((unsigned)(ticks() - k) > 100) {
            dputs("  PCNET.SYS: Initialisierung l\x84uft nicht durch\n");
            csr_wr(0, C0_STOP);
            return;
        }

    k = irq && irq < 16 ? (has_opt(r->args, "/NOIRQ") ? 2 : set_irq(irq, pc_irq) ? 1 : 0) : 3;
    irq_on = k == 0;
    csr_wr(0, C0_IDON | C0_STRT | (irq_on ? C0_IENA : 0));
    outw(io + RAP, 0);

    reg.version = 1;
    reg.size = sizeof(reg);
    reg.mtu = 1500;
    reg.features = NF_ALLMULTI;
    reg.send_off = fent(0, pc_send);
    reg.ctl_off = fent(1, pc_ctl);
    scat(reg.name, "PCNET");
    reg.irq = irq_on ? irq : 0;
    reg.io = io;
    ifc = (int)devhlp(DH_NETREG, 0, (unsigned)&reg, 0);
    if (ifc < 0 || ifc >= NNETIF) {
        csr_wr(0, C0_STOP);
        if (irq_on)
            unset_irq(irq);
        dputs("  PCNET.SYS: kein Platz f\x81r eine weitere Netzwerkkarte\n");
        return;
    }

    p = scat(line, "  ");
    p = scat(p, names[n]);
    p = scat(p, " (PCI ");
    p = hex(p, ploc >> 8, 2);
    p = scat(p, ":");
    p = hex(p, (ploc >> 3) & 31, 2);
    p = scat(p, ") an ");
    p = hex(p, io, 4);
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
        csr_wr(0, C0_STOP);
        outw(io + RAP, 0);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
