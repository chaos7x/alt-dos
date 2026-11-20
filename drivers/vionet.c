/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VIONET.SYS - Netzwerkkarte virtio-net (QEMU, Proxmox/KVM: "VirtIO
 * (paravirtualisiert)") - AT-DOS 6.3, braucht einen 386
 *
 * CONFIG.SYS:  DEVICE=VIONET.SYS [/NOIRQ]   (danach DEVICE=TCPIP.SYS)
 *
 * Legacy-Schnittstelle (drivers/virtio.h), Schlange 0 Empfang, 1 Senden.
 * Jeder Rahmen hat vorn den Kopf virtio_net_hdr (10 Byte, ohne
 * Pruefsummen-/GSO-Hilfe: alles 0); Kopf und Daten als eigene Deskriptoren
 * (die Legacy-Schnittstelle verlangt es ohne VIRTIO_F_ANY_LAYOUT). 16
 * Empfangs- und 8 Sendepuffer zu 1.536 Byte in einem DH_DMABUF-Puffer.
 * Angenommen werden nur VIRTIO_NET_F_MAC und _STATUS; ohne Steuer-
 * schlange laesst QEMU alles durch (Mehrfachempfang ohne Liste).
 */
#include "drvlib.h"
#include "netlib.h"
#include "virtio.h"

#define F_MAC       0x00000020UL
#define F_STATUS    0x00010000UL
#define NRX         16
#define NTX         8
#define BSZ         1536u
#define HDR         10u
#define O_RXB       0
#define O_TXB       (NRX * BSZ)
#define O_HDR       (O_TXB + NTX * BSZ) /* Koepfe: je Puffer 16 Byte     */
#define BUFSZ       (O_HDR + (NRX + NTX) * 16)

static struct vq rq, tq;
static unsigned io, irq, ploc, bsel, irq_on;
static unsigned long bphys, hf;
static unsigned char busy[NTX];
static unsigned txn;
static int ifc = -1;
static struct net_reg reg;
static struct net_pkt rp;
static struct net_stats st;
static char line[100];
static unsigned long rlen;

#define RXHP(i)  (bphys + O_HDR + (i) * 16)
#define TXHP(i)  (bphys + O_HDR + (NRX + (i)) * 16)

/* Empfangspuffer i wieder einreihen: Deskriptoren 2i (Kopf), 2i+1 (Daten) */
static void rx_give(unsigned i)
{
    vq_desc(&rq, 2 * i, RXHP(i), HDR, VD_NEXT | VD_WRITE, 2 * i + 1);
    vq_desc(&rq, 2 * i + 1, bphys + O_RXB + (unsigned)(i * BSZ), BSZ, VD_WRITE, 0);
    vq_put(&rq, 2 * i);
}

static void rx(void)
{
    unsigned h, i, len, k = 0;
    while ((h = vq_get(&rq, &rlen)) != 0xFFFF) {
        i = h >> 1;
        if (i >= NRX)
            continue;
        len = (unsigned)rlen - HDR;
        if (rlen < HDR + 14 || rlen > HDR + 1514)
            st.rx_err++;
        else {
            rp.ifc = ifc;
            rp.len = len;
            if (devhlp(DH_NETRX, 0, (unsigned)&rp, 0) == 0) {
                fmove(MK_FP(rp.sel, rp.off), MK_FP(bsel, O_RXB + i * BSZ), len);
                devhlp(DH_NETRXDONE, 0, (unsigned)&rp, 0);
                st.rx_ok++;
            } else
                st.rx_drop++;
        }
        rx_give(i);
        k++;
    }
    if (k)
        outw(io + VIO_QN, 0);
}

static void tx_reap(void)
{
    unsigned h;
    while ((h = vq_get(&tq, &rlen)) != 0xFFFF)
        if ((h >> 1) < NTX && busy[h >> 1]) {
            busy[h >> 1] = 0;
            txn--;
            st.tx_ok++;
        }
}

static void v_irq(unsigned n)
{
    (void)n;
    if (inp(io + VIO_ISR) & 1) {        /* Lesen quittiert                */
        rx();
        tx_reap();
    }
}

static unsigned v_send(void __far *v)
{
    struct net_pkt __far *p = v;
    unsigned i, len = p->len;
    if (len > 1514)
        return NS_ERR;
    cli();
    tx_reap();
    for (i = 0; i < NTX && busy[i]; i++)
        ;
    if (i == NTX) {
        sti();
        return NS_FULL;
    }
    fmove(MK_FP(bsel, O_TXB + i * BSZ), MK_FP(p->sel, p->off), len);
    busy[i] = 1;
    txn++;
    vq_desc(&tq, 2 * i, TXHP(i), HDR, VD_NEXT, 2 * i + 1);
    vq_desc(&tq, 2 * i + 1, bphys + O_TXB + (unsigned)(i * BSZ), len, 0, 0);
    vq_put(&tq, 2 * i);
    outw(io + VIO_QN, 1);
    sti();
    return NS_OK;
}

static unsigned v_ctl(void __far *v)
{
    struct net_ctl __far *c = v;
    switch (c->cmd) {
    case NC_LINK:
        c->result = (hf & F_STATUS ? inw(io + VIO_CFG + 6) & 1 : 1) | 2 | 100 << 8;
        return 0;
    case NC_MCAST:
    case NC_PROMISC:                    /* ohne Steuerschlange: alles da  */
        return 0;
    case NC_POLL:
        if (!irq_on) {
            cli();
            rx();
            tx_reap();
            sti();
        }
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
    unsigned i, k, e;
    unsigned char __far *b;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    if ((ploc = pci_find(0x1AF4, 0x1000, 0)) == 0xFFFF) {
        dputs(pci_find(0x1AF4, 0x1041, 0) != 0xFFFF ?
              "  VIONET.SYS: nur VirtIO 1.0 ohne Legacy-Teil (q35/PCIe:\n"
              "  disable-legacy=off setzen oder i440fx nehmen)\n" :
              "  VIONET.SYS: keine VirtIO-Netzwerkkarte gefunden\n");
        return;
    }
    if (!(io = vio_bar(ploc))) {
        dputs("  VIONET.SYS: kein I/O-Bereich (BAR 0)\n");
        return;
    }
    irq = (unsigned)pci_rd(ploc, 0x3C) & 0xFF;
    hf = vio_begin(io, F_MAC | F_STATUS);
    if ((e = vq_setup(io, 0, &rq, 0)) != 0 || (e = vq_setup(io, 1, &tq, 0)) != 0 ||
        !(bsel = devhlp(DH_DMABUF, 0, BUFSZ, (unsigned)&bphys))) {
        outp(io + VIO_ST, VS_FAILED);
        dputs(e == 1 ? "  VIONET.SYS: Warteschlange zu gro\xE1\n" :
                       "  VIONET.SYS: kein Speicher f\x81r die Puffer\n");
        return;
    }
    b = MK_FP(bsel, 0);
    for (i = 0; i < BUFSZ; i++)
        b[i] = 0;
    for (i = 0; i < 6; i++)
        reg.mac[i] = hf & F_MAC ? inp(io + VIO_CFG + i) : (unsigned char)(i ? 0x11 * i : 0x52);
    for (i = 0; i < NRX; i++)
        rx_give(i);

    k = irq && irq < 16 ? (has_opt(r->args, "/NOIRQ") ? 2 : set_irq(irq, v_irq) ? 1 : 0) : 3;
    irq_on = k == 0;
    outp(io + VIO_ST, VS_ACK | VS_DRIVER | VS_DOK);
    outw(io + VIO_QN, 0);               /* Empfangspuffer stehen bereit   */

    reg.version = 1;
    reg.size = sizeof(reg);
    reg.mtu = 1500;
    reg.features = NF_ALLMULTI;
    reg.send_off = fent(0, v_send);
    reg.ctl_off = fent(1, v_ctl);
    scat(reg.name, "VIONET");
    reg.irq = irq_on ? irq : 0;
    reg.io = io;
    ifc = (int)devhlp(DH_NETREG, 0, (unsigned)&reg, 0);
    if (ifc < 0 || ifc >= NNETIF) {
        outp(io + VIO_ST, 0);
        if (irq_on)
            unset_irq(irq);
        dputs("  VIONET.SYS: kein Platz f\x81r eine weitere Netzwerkkarte\n");
        return;
    }

    p = scat(line, "  VirtIO-Netz (PCI ");
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
    case DRV_DEINIT:                    /* DEVLOAD /U: Geraet zuruecksetzen */
        outp(io + VIO_ST, 0);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
