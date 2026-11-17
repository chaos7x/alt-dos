/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AHCI.SYS - SATA-Platten und -CD-Laufwerke am AHCI-Controller (PCI-
 * Klasse 01/06/01: Intel ICH6 und neuer, QEMU q35/-device ahci, Proxmox,
 * VirtualBox, VMware) - AT-DOS 6.x, braucht einen 386
 *
 * CONFIG.SYS:  DEVICE=AHCI.SYS [/NOIRQ]
 *
 * Eigene Umsetzung nach der Serial-ATA-AHCI-Spezifikation 1.3. Die
 * Register liegen im Speicher (BAR 5, "ABAR"); der Kernel blendet sie per
 * DevHlp DH_PHYSMAP ohne Cache ein, zugegriffen wird immer mit 32 Bit
 * (mrd/mwr). Je Port mit Geraet eine Einheit und eine Sperrgruppe: zwei
 * Platten an verschiedenen Ports arbeiten gleichzeitig. Je Einheit eine
 * Befehlsliste (1 KB), ein Bereich fuer empfangene FIS (256 Byte) und
 * eine Befehlstabelle (Befehls-FIS, ATAPI-Paket, ein PRD-Eintrag) im
 * Datensegment des Treibers - Paging bildet es 1:1 ab, die physische
 * Adresse ist Basis + Offset. Benutzt wird nur Platz 0 der Liste (ein
 * Befehl je Port zur Zeit, kein NCQ). Daten gehen per DMA direkt in den
 * Puffer des Kernels. Platten: READ/WRITE DMA (EXT bei 48-Bit-LBA),
 * FLUSH CACHE; CD-Laufwerke (ATAPI) bekommen SCSI-Befehle als Paket wie
 * bei NCR8XX (READ(10), TEST UNIT READY, REQUEST SENSE, Auswerfen).
 *
 * Gebaut mit -zu wie alle Treiber: keine Adressen lokaler Variablen.
 */
#include "drvlib.h"

/* ---- 32-Bit-Zugriffe auf die Register (Speicher) ---------------------- */
unsigned long mrd(void __far *p);
#pragma aux mrd = "push ecx" "mov ecx,es:[bx]" "mov ax,cx" "shr ecx,16" \
    "mov dx,cx" "pop ecx" \
    __parm [__es __bx] __value [__dx __ax] __modify __exact [__ax __dx];
void mwr(void __far *p, unsigned lo, unsigned hi);
#pragma aux mwr = "push ecx" "mov cx,dx" "shl ecx,16" "mov cx,ax" \
    "mov es:[bx],ecx" "pop ecx" \
    __parm [__es __bx] [__ax] [__dx] __modify __exact [];

/* ---- Register --------------------------------------------------------- */
#define CAP     0x00
#define GHC     0x04                    /* Bit 31 AE, Bit 1 IE           */
#define HIS     0x08                    /* Interrupt je Port              */
#define PI      0x0C                    /* vorhandene Ports               */
#define VS      0x10
#define P(p, r) (0x100 + ((p) << 7) + (r))
#define PCLB    0x00
#define PCLBU   0x04
#define PFB     0x08
#define PFBU    0x0C
#define PIS     0x10
#define PIE     0x14
#define PCMD    0x18
#define PTFD    0x20
#define PSIG    0x24
#define PSSTS   0x28
#define PSCTL   0x2C
#define PSERR   0x30
#define PCI_    0x38                    /* Command Issue                  */

#define C_ST    0x0001UL
#define C_SUD   0x0002UL
#define C_POD   0x0004UL
#define C_FRE   0x0010UL
#define C_FR    0x4000UL
#define C_CR    0x8000UL
#define IS_ERR  0x78000000UL            /* TFES, HBFS, HBDS, IFS          */
#define IS_TFES 0x40000000UL
#define IE_ALL  0x7800000FUL            /* Fehler + D2H, PIO, DMA, SDB    */

#define SIG_ATA   0x00000101UL
#define SIG_ATAPI 0xEB140101UL

static unsigned mm;                     /* Selektor der Register          */
static unsigned long lin;               /* lineare = physische Basis      */
static unsigned irq;
static volatile unsigned char irq_on;
static char line[128];

static unsigned long rd(unsigned o) { return mrd(MK_FP(mm, o)); }
static void wr(unsigned o, unsigned long v) { mwr(MK_FP(mm, o), (unsigned)v, (unsigned)(v >> 16)); }

/* ---- Einheiten ---------------------------------------------------------- */
static unsigned char uport[DRV_MAXUNITS];
static unsigned char pkt[DRV_MAXUNITS];     /* ATAPI                      */
static unsigned char l48[DRV_MAXUNITS];     /* 48-Bit-LBA                 */
static unsigned char rem[DRV_MAXUNITS];     /* Wechselplatte (ATAPI)      */
static unsigned char chg[DRV_MAXUNITS];     /* Wechsel gesehen            */
static unsigned bsz[DRV_MAXUNITS];
static unsigned nunits;
static volatile unsigned long got[DRV_MAXUNITS];    /* aus dem Interrupt */
static unsigned char cdbu[DRV_MAXUNITS][12];        /* ATAPI-Paket       */

/* je Einheit 2 KB, auf 1 KB ausgerichtet: Befehlsliste (+0), empfangene
   FIS (+1024), Befehlstabelle (+1280, 144 Byte), Hilfspuffer (+1536) */
static unsigned char area[DRV_MAXUNITS * 2048 + 1024];
static unsigned abase;
#define U_CL(u)  (abase + ((u) << 11))
#define U_FB(u)  (U_CL(u) + 1024)
#define U_CT(u)  (U_CL(u) + 1280)
#define U_BUF(u) (U_CL(u) + 1536)
#define PHYS(o)  (lin + (unsigned)(o))
#define EV(u)    (0x10 + (u))

#define R_OK    0
#define R_CHECK 2                       /* ATAPI: Fehler -> Sense holen   */
#define R_FAIL  3

/* ---- Interrupt ---------------------------------------------------------- */
static void ahci_irq(unsigned n)
{
    unsigned long h = rd(HIS), s;
    unsigned p, u;
    (void)n;
    if (!h)
        return;                         /* geteilter IRQ: nicht unserer   */
    for (p = 0; p < 32; p++) {
        if (!(h & (1UL << p)))
            continue;
        s = rd(P(p, PIS));
        wr(P(p, PIS), s);
        for (u = 0; u < nunits; u++)
            if (uport[u] == p) {
                got[u] |= s;
                proc_run(EV(u));
            }
    }
    wr(HIS, h);
}

/* ---- Port anhalten / starten -------------------------------------------- */
static void wait_clear(unsigned o, unsigned long m, unsigned t)
{
    unsigned t0 = ticks();
    while ((rd(o) & m) && (unsigned)(ticks() - t0) < t)
        inp(0x80);
}

static void port_stop(unsigned p)
{
    wr(P(p, PCMD), rd(P(p, PCMD)) & ~C_ST);
    wait_clear(P(p, PCMD), C_CR, 60);
    wr(P(p, PCMD), rd(P(p, PCMD)) & ~C_FRE);
    wait_clear(P(p, PCMD), C_FR, 60);
}

static void port_run(unsigned u)
{
    unsigned p = uport[u];
    wr(P(p, PCMD), rd(P(p, PCMD)) | C_FRE);    /* FIS-Empfang wieder an  */
    wr(P(p, PSERR), 0xFFFFFFFFUL);
    wr(P(p, PIS), 0xFFFFFFFFUL);
    wait_clear(P(p, PTFD), 0x88, 100);  /* BSY, DRQ                       */
    wr(P(p, PCMD), rd(P(p, PCMD)) | C_ST);
}

static void port_start(unsigned u)
{
    unsigned p = uport[u];
    port_stop(p);
    wr(P(p, PCLB), PHYS(U_CL(u)));
    wr(P(p, PCLBU), 0);
    wr(P(p, PFB), PHYS(U_FB(u)));
    wr(P(p, PFBU), 0);
    wr(P(p, PCMD), rd(P(p, PCMD)) | C_SUD | C_POD);
    port_run(u);
}

/* ---- Befehl ausfuehren -------------------------------------------------- */
/* c = ATA-Befehl (A0h = Paket aus cdbu[u]), Daten len Bytes ab buf.
   Mit IRQ schlafen (je 2 Takte, danach selbst nachsehen), sonst abfragen */
static int cmd(unsigned u, unsigned char c, unsigned long lba, unsigned cnt,
               unsigned long buf, unsigned long len, int write, unsigned tmo)
{
    unsigned char *ct = (unsigned char *)U_CT(u);
    unsigned long *hd = (unsigned long *)U_CL(u);
    unsigned p = uport[u], k, t0;
    unsigned long is, ci;

    for (k = 0; k < 144; k++)
        ct[k] = 0;
    ct[0] = 0x27;                       /* Register-FIS Host -> Geraet    */
    ct[1] = 0x80;                       /* Befehl                         */
    ct[2] = c;
    if (c == 0xA0) {                    /* PACKET                         */
        ct[3] = len ? 1 : 0;            /* Daten per DMA                  */
        for (k = 0; k < 12; k++)
            ct[0x40 + k] = cdbu[u][k];
    } else {
        ct[4] = (unsigned char)lba;
        ct[5] = (unsigned char)(lba >> 8);
        ct[6] = (unsigned char)(lba >> 16);
        if (c == 0x25 || c == 0x35 || c == 0xEA) {
            ct[7] = 0x40;               /* LBA, 48 Bit                    */
            ct[8] = (unsigned char)(lba >> 24);
        } else if (c != 0xEC && c != 0xA1)
            ct[7] = 0x40 | ((unsigned char)(lba >> 24) & 0x0F);
        ct[12] = (unsigned char)cnt;
        ct[13] = (unsigned char)(cnt >> 8);
    }
    if (len) {                          /* ein PRD-Eintrag                */
        *(unsigned long *)(ct + 0x80) = buf;
        *(unsigned long *)(ct + 0x8C) = len - 1;
    }
    hd[0] = 5 | (c == 0xA0 ? 0x20 : 0) | (write ? 0x40 : 0) | (len ? 0x10000UL : 0);
    hd[1] = 0;
    hd[2] = PHYS(ct);
    hd[3] = 0;
    cli();
    got[u] = 0;
    sti();
    wr(P(p, PIS), 0xFFFFFFFFUL);
    wr(P(p, PCI_), 1);
    t0 = ticks();
    for (;;) {
        cli();
        ci = rd(P(p, PCI_));
        is = rd(P(p, PIS)) | got[u];
        if (!(ci & 1) || (is & IS_ERR)) {
            sti();
            break;
        }
        if (irq_on)
            proc_block(EV(u), 2);       /* kehrt mit STI zurueck          */
        else
            sti();
        if ((unsigned)(ticks() - t0) > tmo) {
            port_stop(p);               /* haengt: neu aufsetzen          */
            port_run(u);
            return R_FAIL;
        }
    }
    if ((is & IS_TFES) || (rd(P(p, PTFD)) & 1)) {
        port_stop(p);                   /* Fehler: Port neu starten       */
        port_run(u);
        return c == 0xA0 ? R_CHECK : R_FAIL;
    }
    return R_OK;
}

/* ATAPI: Paket aus cdbu[u] */
static int run(unsigned u, unsigned long buf, unsigned long len, int write, unsigned tmo)
{
    return cmd(u, 0xA0, 0, 0, buf, len, write, tmo);
}

static void cdb6(unsigned u, unsigned char o, unsigned char alloc)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        cdbu[u][i] = 0;
    cdbu[u][0] = o;
    cdbu[u][4] = alloc;
}

/* REQUEST SENSE: Sense Key << 8 | ASC, 0 = keine Antwort */
static unsigned req_sense(unsigned u)
{
    unsigned char *b = (unsigned char *)U_BUF(u);
    cdb6(u, 0x03, 18);
    if (run(u, PHYS(b), 18, 0, 300) != R_OK)
        return 0;
    return ((b[2] & 0x0F) << 8) | b[12];
}

/* READ CAPACITY -> Bloecke (0 = keine Antwort / nicht 512 Byte) */
static unsigned long capacity(unsigned u)
{
    unsigned char *b = (unsigned char *)U_BUF(u);
    unsigned k;
    for (k = 0; k < 12; k++)
        cdbu[u][k] = 0;
    cdbu[u][0] = 0x25;
    if (run(u, PHYS(b), 8, 0, 300) != R_OK || b[6] != 2 || b[7] != 0)
        return 0;
    return (((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
            ((unsigned)b[2] << 8) | b[3]) + 1;
}

/* Unit Attention nach dem Start abholen */
static void settle(unsigned u)
{
    unsigned k;
    for (k = 0; k < 4; k++) {
        cdb6(u, 0x00, 0);
        if (run(u, 0, 0, 0, 300) == R_OK)
            break;
        req_sense(u);
    }
}

/* Modellname aus IDENTIFY (Worte 27-46, Bytes je Wort vertauscht) */
static char *model(char *p, const unsigned char __far *id)
{
    unsigned k;
    for (k = 54; k < 94; k += 2) {
        *p++ = id[k + 1] >= 32 ? (char)id[k + 1] : ' ';
        *p++ = id[k] >= 32 ? (char)id[k] : ' ';
    }
    while (p[-1] == ' ')
        p--;
    *p = 0;
    return p;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char __far *id = MK_FP(r->buf_sel, r->buf_off);
    const unsigned short __far *w = (const unsigned short __far *)id;
    unsigned loc, n = 0, p, k, nb, e;
    unsigned long v, ph, pi, sig, cap;
    char *q;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    lin = r->base;
    abase = (unsigned)(((lin + (unsigned)area + 1023) & ~1023UL) - lin);
    for (k = 0; (loc = devhlp(DH_PCICLASS, k, 0x0106, 0)) != 0xFFFF; k++)
        if (((pci_rd(loc, 0x08) >> 8) & 0xFF) == 0x01)
            break;
    if (loc == 0xFFFF) {
        dputs("  AHCI.SYS: kein AHCI-Controller gefunden\n");
        return;
    }
    v = pci_rd(loc, 0x24);              /* BAR 5: ABAR                    */
    if ((v & 1) || (v & 6) == 4 && pci_rd(loc, 0x28)) {
        dputs("  AHCI.SYS: Register nicht erreichbar (BAR 5)\n");
        return;
    }
    ph = v & ~0xFUL;
    v = pci_rd(loc, 0x04);
    pci_wr(loc, 0x04, (unsigned)v | 0x0006, 2);         /* Speicher + Busmaster */
    mm = devhlp(DH_PHYSMAP, (unsigned)(ph >> 16), 0x1100, (unsigned)ph);
    if (!mm) {
        dputs("  AHCI.SYS: kein Platz zum Einblenden der Register\n");
        return;
    }
    wr(GHC, rd(GHC) | 0x80000000UL);    /* AHCI an                        */
    wr(GHC, rd(GHC) & ~2UL);            /* Interrupts vorerst aus         */
    cap = rd(CAP);
    pi = rd(PI);
    irq = (unsigned)pci_rd(loc, 0x3C) & 0xFF;
    v = rd(VS);

    q = scat(line, "  AHCI ");
    q = udec(q, (unsigned)(v >> 16));
    q = scat(q, ".");
    q = udec(q, ((unsigned)v >> 8) & 0xFF);
    q = scat(q, " (PCI ");
    q = hex(q, loc >> 8, 2);
    q = scat(q, ":");
    q = hex(q, (loc >> 3) & 31, 2);
    q = scat(q, ") an ");
    q = hex(q, ph, 8);
    q = scat(q, "h, ");
    q = udec(q, (unsigned)(cap & 31) + 1);
    q = scat(q, " Ports");
    k = irq && irq < 16 ? (has_opt(r->args, "/NOIRQ") ? 2 : set_irq(irq, ahci_irq) ? 1 : 0) : 3;
    q = scat(q, ", IRQ ");
    q = udec(q, irq);
    q = scat(q, k == 1 ? " belegt, Abfragebetrieb" : k == 2 ? " nicht benutzt (/NOIRQ)" :
                k == 3 ? " ung\x81ltig, Abfragebetrieb" : "");
    scat(q, "\n");
    dputs(line);

    for (p = 0; p < 32; p++) {
        if (!(pi & (1UL << p)))
            continue;
        port_stop(p);                   /* auch Ports ohne Geraet: das    */
        wr(P(p, PIE), 0);               /* BIOS liess sie evtl. laufen    */
        if ((rd(P(p, PSSTS)) & 15) != 3) {
            wr(P(p, PCMD), rd(P(p, PCMD)) | C_SUD | C_POD);
            delay(10);
            if ((rd(P(p, PSSTS)) & 15) != 3)
                continue;
        }
        if (n >= DRV_MAXUNITS)
            continue;
        uport[n] = (unsigned char)p;
        port_start(n);
        sig = rd(P(p, PSIG));
        q = scat(line, "  Port ");
        q = udec(q, p);
        if (sig == SIG_ATAPI) {
            pkt[n] = 1;
            if (cmd(n, 0xA1, 0, 0, r->buf_phys, 512, 0, 300) != R_OK) {
                port_stop(p);
                continue;
            }
            e = (w[0] >> 8) & 0x1F;
            q = scat(q, e == 5 ? ": CD-ROM " : e == 0 ? ": Wechselplatte " : ": Ger\x84t ");
            q = model(q, id);
            if (e == 5) {
                settle(n);
                r->blocks[n] = 0xFFFFFFFFUL;
                bsz[n] = 2048;
                r->bsize[n] = 2048 | DRV_BS_GROUP(n);
            } else if (e == 0) {
                settle(n);
                cap = capacity(n);
                r->blocks[n] = cap;
                bsz[n] = 512;
                rem[n] = 1;
                r->bsize[n] = 512 | DRV_BS_REMOVABLE | DRV_BS_GROUP(n);
                if (cap) {
                    q = scat(q, ", ");
                    q = udec(q, (unsigned)(cap >> 11));
                    q = scat(q, " MB");
                } else
                    q = scat(q, " (kein Medium)");
            } else {
                scat(q, "\n");
                dputs(line);
                port_stop(p);
                continue;
            }
        } else if (sig == SIG_ATA) {
            pkt[n] = 0;
            if (cmd(n, 0xEC, 0, 0, r->buf_phys, 512, 0, 300) != R_OK) {
                port_stop(p);
                continue;
            }
            q = scat(q, ": Festplatte ");
            q = model(q, id);
            l48[n] = (w[83] & 0x0400) != 0;
            cap = ((unsigned long)w[61] << 16) | w[60];
            if (l48[n] && (w[100] || w[101] || w[102] || w[103]))
                cap = w[102] || w[103] ? 0xFFFFFFFFUL :
                      ((unsigned long)w[101] << 16) | w[100];
            r->blocks[n] = cap;
            bsz[n] = 512;
            r->bsize[n] = DRV_BS_GROUP(n);
            q = scat(q, ", ");
            nb = (unsigned)(cap >> 11);
            if (cap >> 27) {            /* ab 64 GB in GB                 */
                q = udec(q, (unsigned)(cap >> 21));
                q = scat(q, " GB");
            } else {
                q = udec(q, nb);
                q = scat(q, " MB");
            }
            if (l48[n])
                q = scat(q, " (LBA48)");
        } else {
            port_stop(p);
            continue;
        }
        scat(q, "\n");
        dputs(line);
        n++;
    }
    nunits = n;
    if (k == 0 && n) {                  /* Interrupts der Ports an        */
        irq_on = 1;
        for (p = 0; p < n; p++) {
            wr(P(uport[p], PIS), 0xFFFFFFFFUL);
            wr(P(uport[p], PIE), IE_ALL);
        }
        wr(HIS, 0xFFFFFFFFUL);
        wr(GHC, rd(GHC) | 2);
    }
    r->units = (unsigned char)n;
    r->count = 16;                      /* Bloecke je Lesebefehl          */
    r->status = n ? DRV_OK : DRV_ERR_NODEV;
}

static void xfer(struct drv_req __far *r, int write)
{
    unsigned u = r->unit, n = write || r->count == 0 ? 1 : r->count;
    unsigned long l = r->lba, buf = r->buf_phys;
    int tries, rc, spin = 0;
    unsigned sk;

    if (u >= nunits) {
        r->status = DRV_ERR_IO;
        return;
    }
    if (write && bsz[u] != 512) {
        r->status = DRV_ERR_WP;
        return;
    }
    if (bsz[u] != 512 || n > 16)
        n = bsz[u] != 512 ? 1 : 16;
    for (tries = 0; tries < 3; tries++) {
        if (!pkt[u]) {
            if (!l48[u] && (l >> 28)) {
                r->status = DRV_ERR_IO;
                return;
            }
            rc = cmd(u, write ? (l48[u] ? 0x35 : 0xCA) : (l48[u] ? 0x25 : 0xC8),
                     l, n, buf, (unsigned long)n << 9, write, 1000);
        } else {
            unsigned i;
            for (i = 0; i < 12; i++)
                cdbu[u][i] = 0;
            cdbu[u][0] = write ? 0x2A : 0x28;   /* WRITE(10) / READ(10)  */
            cdbu[u][2] = (unsigned char)(l >> 24);
            cdbu[u][3] = (unsigned char)(l >> 16);
            cdbu[u][4] = (unsigned char)(l >> 8);
            cdbu[u][5] = (unsigned char)l;
            cdbu[u][8] = (unsigned char)n;
            rc = run(u, buf, (unsigned long)n << (bsz[u] == 512 ? 9 : 11), write, 1000);
        }
        if (rc == R_OK) {
            r->status = DRV_OK;
            return;
        }
        if (rc == R_CHECK) {
            sk = req_sense(u);
            if (rem[u] && (sk >> 8) == 6) {     /* gewechselt               */
                chg[u] = 1;
                r->status = DRV_ERR_CHANGED;
                return;
            }
            if ((sk >> 8) == 6 && tries == 0)   /* CD: Unit Attention       */
                continue;
            if (sk == 0x23A && (rem[u] || tries > 0)) {
                chg[u] = 1;
                r->status = DRV_ERR_NOTREADY;   /* keine CD / kein Medium   */
                return;
            }
            if (sk == 0x204 && spin < 20) {     /* wird gerade bereit       */
                spin++;
                tries--;
                cli();
                proc_block(0xFFFF, 50);
            }
        }
    }
    r->status = DRV_ERR_IO;
}

/* Wechselplatte (ATAPI): Medium da? gewechselt? */
static void media(struct drv_req __far *r)
{
    unsigned u = r->unit, k, sk, spin = 0;
    unsigned long cap;

    r->count = 0;
    r->status = DRV_ERR_IO;
    if (u >= nunits || !rem[u])
        return;
    for (k = 0; k < 6; k++) {
        cdb6(u, 0x00, 0);
        if (run(u, 0, 0, 0, 300) == R_OK)
            break;
        sk = req_sense(u);
        if ((sk >> 8) == 6)
            chg[u] = 1;
        else if (sk == 0x23A) {
            chg[u] = 1;
            r->status = DRV_ERR_NOTREADY;
            return;
        } else if (sk == 0x204 && spin < 20) {
            spin++;
            k--;
            cli();
            proc_block(0xFFFF, 50);
        }
    }
    if (k == 6)
        return;
    if (chg[u]) {
        if ((cap = capacity(u)) == 0)
            return;
        r->lba = cap;
        r->count = 1;
        chg[u] = 0;
    }
    r->status = DRV_OK;
}

/* Auswerfen (nur ATAPI): Entnahme erlauben, dann START STOP UNIT LoEj */
static void eject(struct drv_req __far *r)
{
    unsigned u = r->unit;
    r->status = DRV_ERR_CMD;
    if (u >= nunits || !pkt[u])
        return;
    cdb6(u, 0x1E, 0);
    run(u, 0, 0, 0, 300);
    cdb6(u, 0x1B, 0x02);
    r->status = run(u, 0, 0, 0, 1000) == R_OK ? DRV_OK : DRV_ERR_IO;
    chg[u] = 1;
}

static void flush(struct drv_req __far *r)
{
    unsigned u;
    for (u = 0; u < nunits; u++)
        if (!pkt[u])
            cmd(u, l48[u] ? 0xEA : 0xE7, 0, 0, 0, 0, 0, 1000);
    r->status = DRV_OK;
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
        flush(r);
        break;
    case DRV_MEDIA:
        media(r);
        break;
    case DRV_EJECT:
        eject(r);
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: Ports anhalten -    */
        wr(GHC, rd(GHC) & ~2UL);        /* danach gehoert der Speicher der */
        for (u = 0; u < nunits; u++) {  /* Listen nicht mehr dem Treiber  */
            wr(P(uport[u], PIE), 0);
            port_stop(uport[u]);
        }
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
