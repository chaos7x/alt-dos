/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - RTL8139.SYS: Realtek RTL8139/8139C/8139C+ (PCI 10ECh:8139h)
 *
 * Klassischer Betrieb (auch der C+ kennt ihn): Empfang in einen Ring von
 * 8 KB + 16 Byte (WRAP: die Karte schreibt ueber das Ende hinaus weiter,
 * dafuer 1,5 KB Reserve dahinter), Senden ueber vier Puffer zu 1.536 Byte.
 * Beides per Busmaster aus einem DH_DMABUF-Puffer (physisch am Stueck).
 * Im IRQ wird jeder Rahmen einmal in den Platz kopiert, den der Stapel
 * ueber DH_NETRX vergibt (include/net.h). Nachschlagewerk: Datenblatt
 * RTL8139C(L); Verhalten der Nachbildungen in QEMU/86Box nur gelesen.
 *
 * Schalter: /NOIRQ (Abfrage aus dem Nachlauf des Kernels gibt es nicht -
 * ohne IRQ empfaengt die Karte nur, wenn gesendet wird; fuer Tests),
 * /PROMISC (alle Rahmen annehmen).
 */
#include "drvlib.h"
#include "netlib.h"

/* 32-Bit-Ports (386, PCI) */
unsigned long inl(unsigned port);
#pragma aux inl = "in eax,dx" "mov edx,eax" "shr edx,16" \
    __parm [__dx] __value [__dx __ax] __modify __exact [__ax __dx];
void outl(unsigned port, unsigned lo, unsigned hi);
#pragma aux outl = "push ebx" "shl ebx,16" "mov bx,ax" "xchg eax,ebx" \
    "out dx,eax" "xchg eax,ebx" "pop ebx" \
    __parm [__dx] [__ax] [__bx] __modify __exact [];

/* Register (I/O) */
#define IDR0    0x00                    /* MAC                             */
#define MAR0    0x08                    /* Mehrfach-Filter (64 Bit)        */
#define TSD0    0x10                    /* Sendestatus 0-3 (je 4 Byte)     */
#define TSAD0   0x20                    /* Sendeadresse 0-3                */
#define RBSTART 0x30
#define CR      0x37
#define CAPR    0x38
#define CBR     0x3A
#define IMR     0x3C
#define ISR     0x3E
#define TCR     0x40
#define RCR     0x44
#define MPC     0x4C                    /* verlorene Rahmen                */
#define CR9346  0x50
#define CONFIG1 0x52
#define MSR     0x58
#define BMCR    0x62

#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04
#define CR_BUFE 0x01

#define I_ROK   0x0001
#define I_RER   0x0002
#define I_TOK   0x0004
#define I_TER   0x0008
#define I_RXOVW 0x0010
#define I_LINK  0x0020
#define I_FOVW  0x0040
#define I_SERR  0x8000
#define I_ALL   (I_ROK | I_RER | I_TOK | I_TER | I_RXOVW | I_LINK | I_FOVW | I_SERR)

#define TSD_OWN 0x2000                  /* Puffer ist in den FIFO kopiert  */
#define TSD_TOK 0x8000
#define TSD_TUN 0x4000

#define RCR_AAP  0x0001                 /* alle (Promiscuous)              */
#define RCR_APM  0x0002                 /* eigene Adresse                  */
#define RCR_AM   0x0004                 /* Mehrfach                        */
#define RCR_AB   0x0008                 /* Rundruf                         */
#define RCR_WRAP 0x0080
/* MXDMA unbegrenzt (Bits 8-10), RXFTH keine Schwelle (13-15), RBLEN 8K */
#define RCR_VAL  (RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | 0x0700 | 0xE000)

#define RXLEN   8192
#define RXBUF   (RXLEN + 16 + 1536)     /* Ring + Reserve fuer WRAP        */
#define TXSZ    1536
#define BUFSZ   (RXBUF + 4 * TXSZ)      /* 15.920 Byte                     */

static unsigned io, irq, ploc;
static unsigned bsel;                   /* DMA-Puffer                      */
static unsigned long bphys;
static unsigned rxpos;                  /* naechster Rahmen im Ring        */
static unsigned char txbusy[4], txcur;
static int ifc = -1;
static unsigned irq_on, promisc;
static struct net_reg reg;
static struct net_pkt rp;               /* DS, nicht Stapel (SS != DS)     */
static struct net_stats st;
static char line[96];

static unsigned char rd8(unsigned r)  { return inp(io + r); }
static void wr8(unsigned r, unsigned char v) { outp(io + r, v); }
static void wr32(unsigned r, unsigned long v) { outl(io + r, (unsigned)v, (unsigned)(v >> 16)); }

/* Empfang: alle fertigen Rahmen aus dem Ring an den Stapel */
static void rx(void)
{
    unsigned char __far *b = MK_FP(bsel, 0);
    unsigned stat, len, k;

    for (k = 0; k < 64 && !(rd8(CR) & CR_BUFE); k++) {
        stat = *(unsigned __far *)(b + rxpos);
        len = *(unsigned __far *)(b + rxpos + 2);   /* mit 4 Byte CRC  */
        if (!(stat & 1) || len < 18 || len > 1522) {
            st.rx_err++;                /* Ring kaputt: Empfang neu        */
            wr8(CR, CR_TE);             /* (kurze Rahmen unter 64 Byte     */
            wr32(RBSTART, bphys);       /* sind kein Fehler - QEMU reicht  */
            wr8(CR, CR_RE | CR_TE);     /* sie ungepolstert durch)         */
            wr32(RCR, RCR_VAL | promisc);
            rxpos = 0;
            outw(io + CAPR, 0xFFF0);    /* Lesestelle mit zurueck, sonst   */
            return;                     /* gilt der Ring als voll und die  */
        }                               /* Karte empfaengt nie wieder      */
        rp.ifc = ifc;
        rp.len = len - 4;
        if (devhlp(DH_NETRX, 0, (unsigned)&rp, 0) == 0) {
            fmove(MK_FP(rp.sel, rp.off), b + rxpos + 4, rp.len);
            devhlp(DH_NETRXDONE, 0, (unsigned)&rp, 0);
            st.rx_ok++;
        } else
            st.rx_drop++;
        rxpos = (rxpos + len + 4 + 3) & ~3u;
        if (rxpos >= RXLEN)
            rxpos -= RXLEN;
        outw(io + CAPR, rxpos - 16);    /* Karte darf bis hier schreiben   */
    }
}

static void tx_reap(void)               /* fertige Sendepuffer freigeben   */
{
    unsigned i;
    unsigned long s;
    for (i = 0; i < 4; i++)
        if (txbusy[i]) {
            s = inl(io + TSD0 + 4 * i);
            if (s & (TSD_TOK | TSD_TUN | 0x40000000UL)) {   /* fertig/Abbruch */
                txbusy[i] = 0;
                if (s & TSD_TOK)
                    st.tx_ok++;
                else
                    st.tx_err++;
                st.tx_coll += (unsigned)(s >> 24) & 15;
            }
        }
}

static void rtl_irq(unsigned n)
{
    unsigned s;
    (void)n;
    while ((s = inw(io + ISR)) & I_ALL) {
        outw(io + ISR, s);              /* quittieren (1 schreiben)        */
        if (s & (I_ROK | I_RER | I_RXOVW | I_FOVW)) {
            if (s & (I_RER | I_RXOVW | I_FOVW))
                st.rx_err++;
            rx();
        }
        if (s & (I_TOK | I_TER))
            tx_reap();
        if (s & I_SERR) {               /* Busfehler: Karte neu starten    */
            wr8(CR, CR_RE | CR_TE);
        }
    }
}

/* Stapel -> Karte: Rahmen in einen freien Puffer und los */
static unsigned rtl_send(void __far *v)
{
    struct net_pkt __far *p = v;
    unsigned char __far *t;
    unsigned i, len = p->len, f;

    if (len > 1514)
        return NS_ERR;
    f = 0;
    if (!irq_on)
        tx_reap();
    i = txcur;
    if (txbusy[i]) {
        tx_reap();
        if (txbusy[i])
            return NS_FULL;
    }
    t = MK_FP(bsel, RXBUF + i * TXSZ);
    fmove(t, MK_FP(p->sel, p->off), len);
    while (len < 60)                    /* kurze Rahmen auffuellen         */
        t[len++] = 0;
    txbusy[i] = 1;
    txcur = (unsigned char)((i + 1) & 3);
    wr32(TSD0 + 4 * i, 0x00080000UL | len);     /* Schwelle 256 Byte, los */
    (void)f;
    return NS_OK;
}

/* CRC-32 der Mehrfachadresse (Bitfolge wie Ethernet), obere 6 Bit = Bit
   im Filter */
static unsigned mc_bit(const unsigned char __far *a)
{
    unsigned long c = 0xFFFFFFFFUL;
    unsigned i, k;
    unsigned char b;
    for (i = 0; i < 6; i++)
        for (b = a[i], k = 0; k < 8; k++, b >>= 1)
            c = (c << 1) ^ (((c >> 31) ^ b) & 1 ? 0x04C11DB7UL : 0);
    return (unsigned)(c >> 26);
}

static unsigned rtl_ctl(void __far *v)
{
    struct net_ctl __far *c = v;
    unsigned char m[8];
    unsigned i, b;
    switch (c->cmd) {
    case NC_LINK:
        b = rd8(MSR);
        c->result = (b & 4 ? 0 : 1) | (b & 8 ? 1 : 10) << 8;
        if (inw(io + BMCR) & 0x0100)
            c->result |= 2;
        return 0;
    case NC_MCAST:
        for (i = 0; i < 8; i++)
            m[i] = 0;
        for (i = 0; i + 6 <= c->len; i += 6) {
            b = mc_bit((unsigned char __far *)MK_FP(c->sel, c->off) + i);
            m[b >> 3] |= (unsigned char)(1 << (b & 7));
        }
        for (i = 0; i < 8; i++)
            wr8(MAR0 + i, m[i]);
        return 0;
    case NC_PROMISC:
        promisc = c->arg ? RCR_AAP : 0;
        wr32(RCR, RCR_VAL | promisc);
        return 0;
    case NC_POLL:
        if (!irq_on)
            rtl_irq(0);
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
    unsigned long v;
    unsigned i, k;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    ploc = pci_find(0x10EC, 0x8139, 0);
    if (ploc == 0xFFFF) {
        dputs("  RTL8139.SYS: keine Realtek RTL8139 gefunden\n");
        return;
    }
    v = pci_rd(ploc, 0x10);             /* BAR 0: I/O                      */
    if (!(v & 1)) {
        dputs("  RTL8139.SYS: BAR 0 ist kein I/O-Bereich\n");
        return;
    }
    io = (unsigned)v & 0xFFFC;
    irq = (unsigned)pci_rd(ploc, 0x3C) & 0xFF;
    if (claim(RES_IO, io, 0x100)) {
        dputs("  RTL8139.SYS: Ports geh\x94ren schon einem anderen Treiber\n");
        return;
    }
    v = pci_rd(ploc, 0x04);
    pci_wr(ploc, 0x04, (unsigned)v | 0x0005, 2);        /* I/O + Busmaster */
    bsel = devhlp(DH_DMABUF, 0, BUFSZ, (unsigned)&bphys);
    if (!bsel) {
        dputs("  RTL8139.SYS: kein Speicher f\x81r die Puffer\n");
        return;
    }
    promisc = has_opt(r->args, "/PROMISC") ? RCR_AAP : 0;

    wr8(CONFIG1, 0);                    /* aus dem Stromsparen holen       */
    wr8(CR, CR_RST);
    for (k = ticks(); rd8(CR) & CR_RST; )
        if ((unsigned)(ticks() - k) > 100) {
            dputs("  RTL8139.SYS: Karte reagiert nicht auf Reset\n");
            return;
        }
    for (i = 0; i < 6; i++)
        reg.mac[i] = rd8(IDR0 + i);
    wr32(RBSTART, bphys);
    for (i = 0; i < 4; i++)
        wr32(TSAD0 + 4 * i, bphys + (unsigned)(RXBUF + i * TXSZ));
    for (i = 0; i < 8; i++)
        wr8(MAR0 + i, 0xFF);            /* bis der Stapel eine Liste gibt  */
    wr8(CR, CR_RE | CR_TE);
    wr32(RCR, RCR_VAL | promisc);
    wr32(TCR, 0x03000600UL);            /* Lueckenzeit Norm, DMA 1024 Byte */
    outl(io + MPC, 0, 0);
    rxpos = 0;

    k = irq && irq < 16 ? (has_opt(r->args, "/NOIRQ") ? 2 : set_irq(irq, rtl_irq) ? 1 : 0) : 3;
    irq_on = k == 0;
    outw(io + ISR, 0xFFFF);
    outw(io + IMR, irq_on ? I_ALL : 0);

    reg.version = 1;
    reg.size = sizeof(reg);
    reg.mtu = 1500;
    reg.features = NF_MCAST;
    reg.send_off = fent(0, rtl_send);
    reg.ctl_off = fent(1, rtl_ctl);
    scat(reg.name, "RTL8139");
    reg.irq = irq_on ? irq : 0;
    reg.io = io;
    ifc = (int)devhlp(DH_NETREG, 0, (unsigned)&reg, 0);
    if (ifc < 0 || ifc >= NNETIF) {
        wr8(CR, 0);
        outw(io + IMR, 0);
        if (irq_on)
            unset_irq(irq);
        dputs("  RTL8139.SYS: kein Platz f\x81r eine weitere Netzwerkkarte\n");
        return;
    }

    p = scat(line, "  RTL8139 (PCI ");
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
    k = rd8(MSR);
    p = scat(p, k & 4 ? ", kein Kabel" : k & 8 ? ", 10 Mbit" : ", 100 Mbit");
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
        outw(io + IMR, 0);
        wr8(CR, 0);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
