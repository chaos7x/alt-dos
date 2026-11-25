/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VIOBLK.SYS - Platten ueber virtio-blk (QEMU, Proxmox/KVM: "VirtIO Block")
 * - AT-DOS 6.3, braucht einen 386
 *
 * CONFIG.SYS:  DEVICE=VIOBLK.SYS [/NOIRQ]
 *
 * Legacy-Schnittstelle (drivers/virtio.h): je Geraet eine Einheit, eine
 * Warteschlange und eine Sperrgruppe - zwei VirtIO-Platten arbeiten
 * gleichzeitig. Ein Auftrag ist eine Kette aus drei Deskriptoren (Kopf mit
 * Art und Sektor, Daten direkt im Puffer des Kernels, Statusbyte), einer
 * je Einheit zur Zeit. Lesen bis 16 Sektoren am Stueck, Schreiben je
 * Sektor, FLUSH wenn das Geraet es anbietet. Schreibgeschuetzte Platten
 * (VIRTIO_BLK_F_RO) melden DRV_ERR_WP.
 *
 * Starten laesst sich von einer VirtIO-Platte ueber das INT 13h von
 * SeaBIOS (Bootsektor, CONFIG.SYS, vorgeladene Treiber), danach uebernimmt
 * dieser Treiber.
 *
 * Gebaut mit -zu wie alle Treiber: keine Adressen lokaler Variablen.
 */
#include "drvlib.h"
#include "virtio.h"

#define F_RO        0x00000020UL        /* VIRTIO_BLK_F_RO               */
#define F_FLUSH     0x00000200UL        /* VIRTIO_BLK_F_FLUSH            */
#define T_IN        0
#define T_OUT       1
#define T_FLUSH     4
#define EV(u)       (0x10 + (u))
#define MAXN        16                  /* Sektoren je Lesen             */

static struct vq q[DRV_MAXUNITS];
static unsigned io[DRV_MAXUNITS], iq[DRV_MAXUNITS];
static unsigned char ro[DRV_MAXUNITS], fl[DRV_MAXUNITS];
static unsigned nunits;
static volatile unsigned char irq_on;
static char line[100];

static void v_irq(unsigned n)
{
    unsigned u;
    for (u = 0; u < nunits; u++)
        if (iq[u] == n && (inp(io[u] + VIO_ISR) & 1))  /* Lesen quittiert */
            proc_run(EV(u));
}

/* Auftrag: Art t, Sektor l, Daten buf/len (0 = keine) -> 0 ok */
static int req(unsigned u, unsigned t, unsigned long l, unsigned long buf, unsigned len)
{
    struct vq *v = &q[u];
    unsigned char __far *h = MK_FP(v->sel, v->bytes);
    unsigned long hp = v->phys + v->bytes;
    static unsigned long ln;            /* (SS ist der Kernelstapel)     */
    unsigned t0;

    *(unsigned long __far *)h = t;
    *(unsigned long __far *)(h + 4) = 0;
    *(unsigned long __far *)(h + 8) = l;
    *(unsigned long __far *)(h + 12) = 0;
    h[16] = 0xFF;
    if (len) {
        vq_desc(v, 0, hp, 16, VD_NEXT, 1);
        vq_desc(v, 1, buf, len, VD_NEXT | (t == T_IN ? VD_WRITE : 0), 2);
    } else
        vq_desc(v, 0, hp, 16, VD_NEXT, 2);
    vq_desc(v, 2, hp + 16, 1, VD_WRITE, 0);
    vq_put(v, 0);
    outw(io[u] + VIO_QN, 0);
    t0 = ticks();
    for (;;) {
        cli();
        if (VQ_US(v)[1] != v->last) {
            sti();
            break;
        }
        if (irq_on)
            proc_block(EV(u), 2);       /* kehrt mit STI zurueck         */
        else
            sti();
        if ((unsigned)(ticks() - t0) > 1000)
            return 1;                   /* Frist: 10 s                   */
    }
    vq_get(v, &ln);
    return h[16] != 0;
}

#pragma code_seg("ITEXT", "ICODE")   /* nur beim INIT */
static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned loc, n = 0, k, i, e, firq = 0, used = 0;
    unsigned long hf, lo, hi;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    for (i = 0; n < DRV_MAXUNITS && (loc = pci_find(0x1AF4, 0x1001, i)) != 0xFFFF; i++) {
        if (!(io[n] = vio_bar(loc)))
            continue;
        hf = vio_begin(io[n], F_RO | F_FLUSH);
        e = vq_setup(io[n], 0, &q[n], 32);
        p = scat(line, "  VirtIO-Platte (PCI ");
        p = hex(p, loc >> 8, 2);
        p = scat(p, ":");
        p = hex(p, (loc >> 3) & 31, 2);
        p = scat(p, ") an ");
        p = hex(p, io[n], 4);
        p = scat(p, "h");
        if (e) {
            scat(p, e == 1 ? ": Warteschlange zu gro\xE1\n" : ": kein Speicher\n");
            dputs(line);
            outp(io[n] + VIO_ST, VS_FAILED);
            continue;
        }
        ro[n] = (hf & F_RO) != 0;
        fl[n] = (hf & F_FLUSH) != 0;
        iq[n] = (unsigned)pci_rd(loc, 0x3C) & 0xFF;
        lo = inl(io[n] + VIO_CFG);
        hi = inl(io[n] + VIO_CFG + 4);
        r->blocks[n] = hi ? 0xFFFFFFFFUL : lo;
        r->bsize[n] = DRV_BS_GROUP(n);
        outp(io[n] + VIO_ST, VS_ACK | VS_DRIVER | VS_DOK);
        p = scat(p, ", IRQ ");
        p = udec(p, iq[n]);
        p = scat(p, ", ");
        if (hi || (lo >> 27)) {
            p = udec(p, hi ? 0xFFFF : (unsigned)(lo >> 21));
            p = scat(p, " GB");
        } else {
            p = udec(p, (unsigned)(lo >> 11));
            p = scat(p, " MB");
        }
        if (ro[n])
            p = scat(p, ", schreibgesch\x81tzt");
        scat(p, "\n");
        dputs(line);
        n++;
    }
    if (!n) {
        if (pci_find(0x1AF4, 0x1042, 0) != 0xFFFF)
            dputs("  VIOBLK.SYS: nur VirtIO 1.0 ohne Legacy-Teil (q35/PCIe:\n"
                  "  disable-legacy=off setzen oder i440fx nehmen)\n");
        else
            dputs("  VIOBLK.SYS: keine VirtIO-Platte gefunden\n");
        return;
    }
    nunits = n;
    /* Interrupts: jeder IRQ einmal (die Routine prueft alle Einheiten) */
    if (!has_opt(r->args, "/NOIRQ")) {
        for (k = 0; k < n; k++) {
            if (!iq[k] || iq[k] >= 16) {
                firq = 1;
                break;
            }
            for (i = 0; i < k && iq[i] != iq[k]; i++)
                ;
            if (i == k && set_irq(iq[k], v_irq)) {
                firq = 1;
                break;
            }
            used++;
        }
        if (firq) {                     /* gemischt geht nicht: alle ab   */
            for (k = 0; k < n; k++) {
                for (i = 0; i < k && iq[i] != iq[k]; i++)
                    ;
                if (i == k && k < used)
                    unset_irq(iq[k]);
            }
            dputs("  VIOBLK.SYS: IRQ belegt, Abfragebetrieb\n");
        } else
            irq_on = 1;
    }
    r->units = (unsigned char)n;
    r->count = MAXN;
    r->status = DRV_OK;
}
#pragma code_seg("_TEXT", "CODE")

static void xfer(struct drv_req __far *r, int write)
{
    unsigned u = r->unit, n = write || r->count == 0 ? 1 : r->count;
    unsigned long l = r->lba, buf = r->buf_phys;
    int tries;

    if (u >= nunits) {
        r->status = DRV_ERR_IO;
        return;
    }
    if (write && ro[u]) {
        r->status = DRV_ERR_WP;
        return;
    }
    if (n > MAXN)
        n = MAXN;
    for (tries = 0; tries < 3; tries++)
        if (req(u, write ? T_OUT : T_IN, l, buf, n << 9) == 0) {
            r->status = DRV_OK;
            return;
        }
    r->status = DRV_ERR_IO;
}

void strategy(void)
{
    struct drv_req __far *r = REQ;      /* REQ gilt nur bis zum Schlafen  */
    unsigned u;

    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_READ:
        xfer(r, 0);
        break;
    case DRV_WRITE:
        xfer(r, 1);
        break;
    case DRV_FLUSH:
        for (u = 0; u < nunits; u++)
            if (fl[u])
                req(u, T_FLUSH, 0, 0, 0);
        r->status = DRV_OK;
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: Geraete zuruecksetzen */
        for (u = 0; u < nunits; u++)    /* (danach gehoert der Speicher   */
            outp(io[u] + VIO_ST, 0);    /* der Schlangen nicht mehr uns)  */
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
