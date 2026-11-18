/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.2 - NE2000.SYS: Novell NE1000/NE2000 und Nachbauten (DP8390)
 *
 * ISA: NE1000 (8 Bit), NE2000 (16 Bit), RTL8019AS (ISA-PnP, die Ressourcen
 * vergibt pnp.c), PCI: RTL8029, Winbond W89C940 u. a. (ne2k_pci).
 * Nur 286-Befehle: der Treiber soll auch unter 5.x laufen.
 *
 * Die Karte hat 16 KB (NE1000: 8 KB) eigenen Speicher: Seiten 40h-45h
 * ein Sendepuffer, 46h-7Fh der Empfangsring. Daten laufen per PIO ueber
 * den Datenport (Remote-DMA des DP8390), im IRQ direkt in den Platz, den
 * der Stapel ueber DH_NETRX vergibt. Nachschlagewerke: Datenblatt
 * DP8390D (National Semiconductor), Linux 8390.c/ne.c nur gelesen.
 *
 * Suche (erst lesen): /P:300 /I:3 legen Port und IRQ fest; sonst PCI,
 * dann 300h, 280h, 320h, 340h, 360h, 240h. Ein Port gilt erst, wenn das
 * Befehlsregister wie ein DP8390 aussieht, der Zaehler beim Lesen 0 wird
 * und das Adress-PROM die Kennung "WW" traegt. Ohne /I sucht der Treiber
 * den IRQ, indem die Karte einen Interrupt ausloest (IRR der PICs).
 * /NOIRQ: Abfragebetrieb (Empfang beim Senden und aus dem Nachlauf).
 */
#include "drvlib.h"
#include "netlib.h"

#define CR      0x00
#define PSTART  0x01
#define PSTOP   0x02
#define BNRY    0x03
#define TPSR    0x04
#define TBCR0   0x05
#define TBCR1   0x06
#define ISR     0x07
#define RSAR0   0x08
#define RSAR1   0x09
#define RBCR0   0x0A
#define RBCR1   0x0B
#define RCR     0x0C
#define TCR     0x0D
#define DCR     0x0E
#define IMR     0x0F
#define CNTR0   0x0D                    /* lesen, Seite 0                  */
#define CURR    0x07                    /* Seite 1                         */
#define DATA    0x10
#define RESET   0x1F

#define C_STOP  0x01
#define C_START 0x02
#define C_TXP   0x04
#define C_RREAD 0x08
#define C_RWRITE 0x10
#define C_NODMA 0x20
#define C_PAGE1 0x40

#define I_PRX   0x01
#define I_PTX   0x02
#define I_RXE   0x04
#define I_TXE   0x08
#define I_OVW   0x10
#define I_CNT   0x20
#define I_RDC   0x40
#define I_RST   0x80

static unsigned io, irq, w16, txpage, rxs, pstop;
static unsigned irq_on, rcr = 0x0C;     /* Rundruf + Mehrfach              */
static unsigned char txbusy;
static int ifc = -1;
static struct net_reg reg;
static struct net_pkt rp;
static struct net_stats st;
static unsigned char prom[32], hdr[4];
static unsigned char txb[1536];
static char line[100];
static const char *kind = "NE2000";

static unsigned char rd(unsigned r) { return inp(io + r); }
static void wr(unsigned r, unsigned v) { outp(io + r, (unsigned char)v); }

static void rdma_wait(void)
{
    unsigned k;
    for (k = 0; k < 20000 && !(rd(ISR) & I_RDC); k++)
        ;
    wr(ISR, I_RDC);
}

/* n Bytes Kartenspeicher ab addr nach d (n bei 16 Bit gerade) */
static void card_read(unsigned addr, void __far *d, unsigned n)
{
    if (w16 && (n & 1))
        n++;
    wr(RBCR0, n);
    wr(RBCR1, n >> 8);
    wr(RSAR0, addr);
    wr(RSAR1, addr >> 8);
    wr(CR, C_RREAD | C_START);
    if (w16)
        insw_far(io + DATA, d, n >> 1);
    else
        insb_far(io + DATA, d, n);
    rdma_wait();
}

/* txb (n Bytes) in den Sendepuffer der Karte und senden */
static void tx_start(unsigned n)
{
    wr(ISR, I_RDC);
    wr(RBCR0, n);
    wr(RBCR1, n >> 8);
    wr(RSAR0, 0);
    wr(RSAR1, txpage);
    wr(CR, C_RWRITE | C_START);
    if (w16)
        outsw_far(io + DATA, getds(), (unsigned)txb, n >> 1);
    else
        outsb_far(io + DATA, getds(), (unsigned)txb, n);
    rdma_wait();
    wr(TPSR, txpage);
    wr(TBCR0, n);
    wr(TBCR1, n >> 8);
    wr(CR, C_NODMA | C_START | C_TXP);
}

/* ---- Empfang --------------------------------------------------------- */
static void rx(void)
{
    unsigned nxt, cur, len, start, end, first, k;
    unsigned char __far *d;

    for (k = 0; k < 32; k++) {
        wr(CR, C_NODMA | C_PAGE1 | C_START);
        cur = rd(CURR);
        wr(CR, C_NODMA | C_START);
        nxt = rd(BNRY) + 1;
        if (nxt >= pstop)
            nxt = rxs;
        if (nxt == cur)
            break;
        card_read(nxt << 8, (unsigned char __far *)hdr, 4);
        len = (hdr[2] | (unsigned)hdr[3] << 8) - 4;
        if (len < 14 || len > 1522 || hdr[1] < rxs || hdr[1] >= pstop) {
            st.rx_err++;                /* Kopf kaputt: Ring leeren        */
            wr(BNRY, cur == rxs ? pstop - 1 : cur - 1);
            break;
        }
        if (len > 1518)
            len = 1518;
        if (hdr[0] & 1) {               /* fehlerfrei empfangen            */
            rp.ifc = ifc;
            rp.len = len;
            if (devhlp(DH_NETRX, 0, (unsigned)&rp, 0) == 0) {
                d = MK_FP(rp.sel, rp.off);
                start = (nxt << 8) + 4;
                end = pstop << 8;
                if (start + len > end) {    /* ueber das Ringende           */
                    first = end - start;
                    card_read(start, d, first);
                    card_read(rxs << 8, d + first, len - first);
                } else
                    card_read(start, d, len);
                devhlp(DH_NETRXDONE, 0, (unsigned)&rp, 0);
                st.rx_ok++;
            } else
                st.rx_drop++;
        } else
            st.rx_err++;
        nxt = hdr[1];
        wr(BNRY, nxt == rxs ? pstop - 1 : nxt - 1);
    }
}

/* Ring uebergelaufen: wie im Datenblatt anhalten, leeren, weiter */
static void overrun(void)
{
    unsigned k, tx = rd(CR) & C_TXP, resend;
    wr(CR, C_NODMA | C_STOP);
    for (k = 0; k < 5000; k++)          /* ~2 ms warten                    */
        rd(ISR);
    wr(RBCR0, 0);
    wr(RBCR1, 0);
    resend = tx && !(rd(ISR) & (I_PTX | I_TXE));
    wr(TCR, 0x02);                      /* Schleife intern                 */
    wr(CR, C_NODMA | C_START);
    rx();
    wr(ISR, I_OVW);
    wr(TCR, 0x00);
    if (resend)
        wr(CR, C_NODMA | C_START | C_TXP);
    st.rx_err++;
}

static void service(void)
{
    unsigned s, k;
    for (k = 0; k < 8 && ((s = rd(ISR)) & (I_PRX | I_PTX | I_RXE | I_TXE | I_OVW | I_CNT)); k++) {
        if (s & I_OVW) {
            overrun();
            continue;
        }
        wr(ISR, s & (I_PRX | I_PTX | I_RXE | I_TXE | I_CNT));
        if (s & (I_PRX | I_RXE))
            rx();
        if (s & (I_PTX | I_TXE)) {
            txbusy = 0;
            if (s & I_PTX)
                st.tx_ok++;
            else
                st.tx_err++;
        }
        if (s & I_CNT) {                /* Fehlerzaehler lesen = loeschen  */
            rd(CNTR0);
            rd(CNTR0 + 1);
            rd(CNTR0 + 2);
        }
    }
}

static void ne_irq(unsigned n)
{
    (void)n;
    service();
}

/* ---- Senden ---------------------------------------------------------- */
static unsigned ne_send(void __far *v)
{
    struct net_pkt __far *p = v;
    unsigned len = p->len, n;

    if (len > 1514)
        return NS_ERR;
    if (!irq_on)
        service();                      /* ohne IRQ: hier nachsehen        */
    if (txbusy) {
        if (rd(CR) & C_TXP)
            return NS_FULL;
        txbusy = 0;                     /* fertig, IRQ kam nicht           */
    }
    fmove(txb, MK_FP(p->sel, p->off), len);
    n = len;
    while (n < 60)
        txb[n++] = 0;
    if (w16 && (n & 1))
        txb[n++] = 0;
    txbusy = 1;
    tx_start(n);
    return NS_OK;
}

static unsigned mc_bit(const unsigned char __far *a)
{
    unsigned long c = 0xFFFFFFFFUL;
    unsigned i, k;
    unsigned char b;
    for (i = 0; i < 6; i++)
        for (b = a[i], k = 0; k < 8; k++, b >>= 1)
            c = (c << 1) ^ (((unsigned)(c >> 31) ^ b) & 1 ? 0x04C11DB7UL : 0);
    return (unsigned)(c >> 26);
}

static unsigned ne_ctl(void __far *v)
{
    struct net_ctl __far *c = v;
    unsigned char m[8];
    unsigned i, b;
    switch (c->cmd) {
    case NC_LINK:                       /* DP8390 kennt keinen Status     */
        c->result = 1 | (1 << 8);       /* "verbunden, 10 Mbit"           */
        return 0;
    case NC_MCAST:
        for (i = 0; i < 8; i++)
            m[i] = 0;
        for (i = 0; i + 6 <= c->len; i += 6) {
            b = mc_bit((unsigned char __far *)MK_FP(c->sel, c->off) + i);
            m[b >> 3] |= (unsigned char)(1 << (b & 7));
        }
        wr(CR, C_NODMA | C_PAGE1 | C_START);
        for (i = 0; i < 8; i++)
            wr(8 + i, m[i]);
        wr(CR, C_NODMA | C_START);
        return 0;
    case NC_PROMISC:
        rcr = c->arg ? 0x1C : 0x0C;
        wr(RCR, rcr);
        return 0;
    case NC_POLL:
        if (!irq_on)
            service();
        return 0;
    case NC_STATS:
        if (!irq_on)
            service();
        fmove(MK_FP(c->sel, c->off), (void __far *)&st, sizeof(st));
        return 0;
    }
    return 1;
}

/* ---- Suche ----------------------------------------------------------- */
/* sieht der Port wie ein DP8390 aus? (erst lesen, dann wie Linux pruefen) */
static int probe(unsigned b)
{
    unsigned c, d;
    io = b;
    c = rd(CR);
    if (c == 0xFF || (c & 3) == 3)     /* QEMU meldet 00h statt 21h   */
        return 0;
    if (claim(RES_IO, b, 0x20))
        return 0;                       /* gehoert einem anderen Treiber   */
    wr(CR, C_NODMA | C_PAGE1 | C_STOP);
    d = rd(0x0D);
    wr(0x0D, 0xFF);
    wr(CR, C_NODMA);
    rd(CNTR0);                          /* Lesen loescht den Zaehler       */
    if (rd(CNTR0) != 0) {
        wr(CR, c);
        wr(0x0D, d);
        return 0;
    }
    return 1;
}

/* Karte zuruecksetzen, Adress-PROM lesen -> 1 = NE1000/NE2000-Kennung */
static int read_prom(void)
{
    unsigned k, pairs = 1;
    unsigned char __far *pp = (unsigned char __far *)prom;
    wr(RESET, rd(RESET));
    for (k = 0; k < 30000 && !(rd(ISR) & I_RST); k++)
        ;
    wr(ISR, 0xFF);
    wr(CR, C_NODMA | C_STOP);
    wr(DCR, 0x48);                      /* Bytebetrieb                     */
    wr(RBCR0, 0);
    wr(RBCR1, 0);
    wr(IMR, 0);
    wr(ISR, 0xFF);
    wr(RCR, 0x20);                      /* nur beobachten                  */
    wr(TCR, 0x02);
    w16 = 0;
    card_read(0, pp, 32);
    for (k = 0; k < 32; k += 2)
        if (prom[k] != prom[k + 1])
            pairs = 0;
    if (pairs) {                        /* 16-Bit-Karte: Bytes doppelt     */
        for (k = 0; k < 16; k++)
            prom[k] = prom[2 * k];
        w16 = 1;
    }
    return prom[14] == 0x57 && prom[15] == 0x57;
}

static unsigned irr_now(void)
{
    outp(0x20, 0x0A);
    outp(0xA0, 0x0A);
    return inp(0x20) | ((unsigned)inp(0xA0) << 8);
}

/* Karte einen Interrupt ausloesen lassen (Remote-DMA mit 0 Bytes, wie Linux) */
static unsigned find_irq(void)
{
    static const unsigned char cand[] = { 3, 5, 9, 10, 11, 12, 15, 7, 4 };
    unsigned off, on = 0, i, k, n = 0, found = 0;
    cli();
    off = irr_now();
    for (i = 0; i < 200; i++)
        off |= irr_now();
    wr(ISR, 0xFF);
    wr(IMR, 0);
    wr(RBCR0, 0);
    wr(RBCR1, 0);
    wr(CR, C_RREAD | C_START);
    /* Erst RDC stehen lassen, dann freigeben: INT = ISR & IMR bleibt oben,
       bis ISR geloescht wird (86Box gibt beim Remote-DMA selbst nur einen
       kurzen Puls, der im PIC nicht stehen bleibt) */
    for (i = 0; i < 1000 && !(rd(ISR) & I_RDC); i++)
        ;
    wr(IMR, I_RDC);
    for (i = 0; i < 20000; i++) {
        on = irr_now() & ~off & ~0x0107u;       /* Takt, Tastatur, Kaskade */
        if (on)
            break;
    }
    wr(IMR, 0);
    wr(ISR, 0xFF);
    sti();
    for (k = 0; k < sizeof(cand); k++)
        if (on & (1u << cand[k])) {
            found = cand[k];
            n++;
        }
    return n == 1 ? found : 0;
}

static const unsigned pci_ids[][2] = {
    { 0x10EC, 0x8029 }, { 0x1050, 0x0940 }, { 0x1050, 0x5A5A },
    { 0x11F6, 0x1401 }, { 0x8E2E, 0x3000 }, { 0x4A14, 0x5000 },
    { 0x1106, 0x0926 }, { 0x10BD, 0x0E34 }, { 0x12C3, 0x0058 }
};

static unsigned hexval(const char __far *a)
{
    unsigned v = 0, d;
    for (;; a++) {
        d = *a >= '0' && *a <= '9' ? *a - '0' : (*a | 32) >= 'a' && (*a | 32) <= 'f' ?
            (*a | 32) - 'a' + 10 : 16;
        if (d == 16)
            return v;
        v = v << 4 | d;
    }
}

static const char __far *opt(const char __far *a, const char *k)
{
    int i;
    for (; *a; a++) {
        for (i = 0; k[i] && (a[i] & 0xDF) == (k[i] & 0xDF); i++)
            ;
        if (!k[i])
            return a + i;
    }
    return 0;
}

static void init(void)
{
    static const unsigned isa[] = { 0x300, 0x280, 0x320, 0x340, 0x360, 0x240 };
    struct drv_req __far *r = REQ;
    const char __far *a;
    unsigned i, k, ploc = 0xFFFF, want_irq = 0, found = 0, pci = 0;
    unsigned long v;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    if ((a = opt(r->args, "/I:")) != 0)
        for (; *a >= '0' && *a <= '9'; a++)
            want_irq = want_irq * 10 + (*a - '0');
    if (want_irq == 2)
        want_irq = 9;
    if ((a = opt(r->args, "/P:")) != 0) {
        k = hexval(a);
        found = probe(k) && read_prom();
    } else {
        for (i = 0; i < sizeof(pci_ids) / sizeof(pci_ids[0]) && !found; i++)
            if ((ploc = pci_find(pci_ids[i][0], pci_ids[i][1], 0)) != 0xFFFF &&
                ploc != 0 && ((v = pci_rd(ploc, 0x10)) & 1)) {
                k = (unsigned)v & 0xFFFC;
                v = pci_rd(ploc, 0x04);
                pci_wr(ploc, 0x04, (unsigned)v | 0x0001, 2);    /* I/O an */
                if (probe(k) && read_prom()) {
                    found = 1;
                    pci = 1;
                    want_irq = (unsigned)pci_rd(ploc, 0x3C) & 0xFF;
                    kind = "NE2000-PCI";
                }
            }
        for (i = 0; i < sizeof(isa) / sizeof(isa[0]) && !found; i++)
            found = probe(isa[i]) && read_prom();
    }
    if (!found) {
        dputs("  NE2000.SYS: keine NE1000/NE2000-kompatible Karte gefunden\n");
        return;
    }
    if (!w16 && !pci)
        kind = "NE1000";
    txpage = w16 ? 0x40 : 0x20;
    rxs = txpage + 6;
    pstop = w16 ? 0x80 : 0x40;
    for (i = 0; i < 6; i++)
        reg.mac[i] = prom[i];

    wr(CR, C_NODMA | C_STOP);
    wr(DCR, w16 ? 0x49 : 0x48);
    wr(RBCR0, 0);
    wr(RBCR1, 0);
    wr(RCR, 0x20);
    wr(TCR, 0x02);
    wr(PSTART, rxs);
    wr(PSTOP, pstop);
    wr(BNRY, rxs);
    wr(TPSR, txpage);
    wr(ISR, 0xFF);
    wr(IMR, 0);

    k = 0;                              /* IRQ: 0 ok, 1 belegt, 2 /NOIRQ, 3 keiner */
    if (has_opt(r->args, "/NOIRQ"))
        k = 2;
    else {
        irq = want_irq ? want_irq : find_irq();
        if (!irq || irq > 15)
            k = 3;
        else if (set_irq(irq, ne_irq))
            k = 1;
    }
    irq_on = k == 0;

    wr(CR, C_NODMA | C_PAGE1 | C_STOP);
    for (i = 0; i < 6; i++)
        wr(1 + i, reg.mac[i]);
    for (i = 0; i < 8; i++)
        wr(8 + i, 0xFF);
    wr(CURR, rxs + 1);
    wr(CR, C_NODMA | C_START);
    wr(ISR, 0xFF);
    wr(IMR, irq_on ? (I_PRX | I_PTX | I_RXE | I_TXE | I_OVW | I_CNT) : 0);
    wr(TCR, 0x00);
    if (has_opt(r->args, "/PROMISC"))
        rcr = 0x1C;
    wr(RCR, rcr);

    reg.version = 1;
    reg.size = sizeof(reg);
    reg.mtu = 1500;
    reg.features = NF_MCAST;
    reg.send_off = fent(0, ne_send);
    reg.ctl_off = fent(1, ne_ctl);
    scat(reg.name, w16 ? "NE2000" : "NE1000");
    reg.irq = irq_on ? irq : 0;
    reg.io = io;
    ifc = (int)devhlp(DH_NETREG, 0, (unsigned)&reg, 0);
    if (ifc < 0 || ifc >= NNETIF) {
        wr(IMR, 0);
        wr(CR, C_NODMA | C_STOP);
        if (irq_on)
            unset_irq(irq);
        dputs("  NE2000.SYS: kein Platz f\x81r eine weitere Netzwerkkarte (oder Kernel ohne Netz)\n");
        return;
    }

    p = scat(line, "  ");
    p = scat(p, kind);
    p = scat(p, " an ");
    p = hex(p, io, io > 0xFFF ? 4 : 3);
    p = scat(p, "h, IRQ ");
    if (k == 0)
        p = udec(p, irq);
    else
        p = scat(p, k == 1 ? "belegt" : k == 2 ? "nicht benutzt" : "nicht gefunden (/I:n)");
    p = scat(p, ", ");
    p = scat(p, w16 ? "16" : "8");
    p = scat(p, " Bit, MAC ");
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
        wr(IMR, 0);
        wr(CR, C_NODMA | C_STOP);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
