/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - gemeinsamer Teil der VirtIO-Treiber (VIOBLK.SYS, VIONET.SYS)
 *
 * Legacy-Schnittstelle der VirtIO-Spezifikation 1.0 (Abschnitt 4.1.4.8,
 * "Legacy Interfaces: A Note on PCI Device Layout"; nur gelesen, eigene
 * Umsetzung): Register im I/O-BAR 0, Warteschlange am Stueck auf 4 KB
 * ausgerichtet (Deskriptoren, verfuegbarer Ring, dahinter auf 4 KB
 * gerundet der benutzte Ring), Seitennummer 32 Bit. QEMU/Proxmox geben
 * auf dem PCI-Bus von i440fx "transitional" Geraete mit diesem Teil
 * (1AF4:1000 Netz, 1AF4:1001 Platte); reine 1.0-Geraete (1AF4:1041 ff.,
 * q35 mit PCIe-Ports) haben ihn nicht.
 *
 * Speicher der Warteschlangen per DH_DMABUF (Seiten am Stueck, unter
 * 16 MB, physische Adresse bekannt). Zugriffe ueber ferne volatile
 * Zeiger; x86 haelt die Reihenfolge der Schreibzugriffe ein.
 */
#ifndef VIRTIO_H
#define VIRTIO_H

unsigned long inl(unsigned port);
#pragma aux inl = "in eax,dx" "mov edx,eax" "shr edx,16" \
    __parm [__dx] __value [__dx __ax];
void outl(unsigned port, unsigned lo, unsigned hi);
#pragma aux outl = "push ebx" "mov bx,cx" "shl ebx,16" "mov bx,ax" \
    "mov eax,ebx" "out dx,eax" "pop ebx" \
    __parm [__dx] [__ax] [__cx] __modify [__ax];
#ifndef NETLIB_H                        /* (netlib.h hat sie schon)      */
unsigned inw(unsigned port);
#pragma aux inw = "in ax,dx" __parm [__dx] __value [__ax];
void outw(unsigned port, unsigned v);
#pragma aux outw = "out dx,ax" __parm [__dx] [__ax];
#endif

#define VIO_HF      0x00                /* Merkmale des Geraets (32)      */
#define VIO_GF      0x04                /* angenommene Merkmale (32)      */
#define VIO_QA      0x08                /* Seitennummer der Schlange      */
#define VIO_QS      0x0C                /* Groesse (16, nur lesen)        */
#define VIO_QSEL    0x0E
#define VIO_QN      0x10                /* Anstoss: Nummer der Schlange   */
#define VIO_ST      0x12                /* Zustand (8)                    */
#define VIO_ISR     0x13                /* Lesen quittiert (Bit 0 Schlange,
                                           Bit 1 Konfiguration)           */
#define VIO_CFG     0x14                /* Geraetekonfiguration (ohne MSI-X) */

#define VS_ACK      1
#define VS_DRIVER   2
#define VS_DOK      4
#define VS_FAILED   128

#define VD_NEXT     1
#define VD_WRITE    2                   /* Geraet schreibt hinein         */

#define VQ_MAX      256                 /* groessere Schlangen lehnen wir ab */

struct vq {
    unsigned       sel;                 /* Selektor (DH_DMABUF)           */
    unsigned long  phys;
    unsigned       n;                   /* Groesse (vom Geraet)           */
    unsigned       avo, uso;            /* Lage der Ringe im Segment      */
    unsigned       last;                /* zuletzt gesehener used.idx     */
    unsigned       bytes;
};

/* Groesse der Schlange mit n Eintraegen (4 KB gerundet) */
static unsigned vq_bytes(unsigned n, unsigned *avo, unsigned *uso)
{
    unsigned a = n * 16, u;
    *avo = a;
    u = (a + 6 + 2 * n + 4095) & ~4095u;
    *uso = u;
    return (u + 6 + 8 * n + 4095) & ~4095u;
}

/* Schlange q des Geraets an io anlegen; extra Byte dahinter fuer den
   Treiber (Koepfe, Puffer). 0 = ok, 1 = Groesse 0/zu gross, 2 = kein Speicher */
static int vq_setup(unsigned io, unsigned q, struct vq *v, unsigned extra)
{
    unsigned char __far *p;
    unsigned i, total;
    outw(io + VIO_QSEL, q);
    v->n = inw(io + VIO_QS);
    if (!v->n || v->n > VQ_MAX || (v->n & (v->n - 1)))
        return 1;
    v->bytes = vq_bytes(v->n, &v->avo, &v->uso);
    total = v->bytes + extra;
    v->sel = devhlp(DH_DMABUF, 0, total, (unsigned)&v->phys);
    if (!v->sel)
        return 2;
    p = MK_FP(v->sel, 0);
    for (i = 0; i < total; i++)
        p[i] = 0;
    v->last = 0;
    outw(io + VIO_QSEL, q);
    outl(io + VIO_QA, (unsigned)(v->phys >> 12), (unsigned)(v->phys >> 28));
    return 0;
}

#define VQ_D(v, i)  ((volatile unsigned long __far *)MK_FP((v)->sel, (i) * 16))
#define VQ_AV(v)    ((volatile unsigned short __far *)MK_FP((v)->sel, (v)->avo))
#define VQ_US(v)    ((volatile unsigned short __far *)MK_FP((v)->sel, (v)->uso))

/* Deskriptor i: physische Adresse a, Laenge len, Merkmale fl, Folger nx */
static void vq_desc(struct vq *v, unsigned i, unsigned long a, unsigned long len,
                    unsigned fl, unsigned nx)
{
    volatile unsigned long __far *d = VQ_D(v, i);
    d[0] = a;
    d[1] = 0;
    d[2] = len;
    ((volatile unsigned short __far *)d)[6] = fl;
    ((volatile unsigned short __far *)d)[7] = nx;
}

/* Kette ab Kopf h in den verfuegbaren Ring (ohne Anstoss) */
static void vq_put(struct vq *v, unsigned h)
{
    volatile unsigned short __far *av = VQ_AV(v);
    unsigned k = av[1];
    av[2 + (k & (v->n - 1))] = h;
    av[1] = k + 1;
}

/* Neues im benutzten Ring? -> Kopf der fertigen Kette, Laenge in *len;
   0xFFFF = nichts */
static unsigned vq_get(struct vq *v, unsigned long *len)
{
    volatile unsigned short __far *us = VQ_US(v);
    volatile unsigned long __far *e;
    unsigned h;
    if (us[1] == v->last)
        return 0xFFFF;
    e = (volatile unsigned long __far *)(us + 2) + 2 * (v->last & (v->n - 1));
    h = (unsigned)e[0];
    *len = e[1];
    v->last++;
    return h;
}

/* Geraet zuruecksetzen und erkennen lassen, Merkmale aushandeln */
static unsigned long vio_begin(unsigned io, unsigned long want)
{
    unsigned long hf;
    outp(io + VIO_ST, 0);
    outp(io + VIO_ST, VS_ACK);
    outp(io + VIO_ST, VS_ACK | VS_DRIVER);
    hf = inl(io + VIO_HF) & want;
    outl(io + VIO_GF, (unsigned)hf, (unsigned)(hf >> 16));
    return hf;
}

/* I/O-BAR 0 des Geraets an loc (Befehlsregister: I/O + Busmaster) -> Port,
   0 = keiner */
static unsigned vio_bar(unsigned loc)
{
    unsigned long v = pci_rd(loc, 0x10);
    if (!(v & 1))
        return 0;
    pci_wr(loc, 0x04, (unsigned)pci_rd(loc, 0x04) | 0x0005, 2);
    return (unsigned)v & 0xFFFC;
}

#endif
