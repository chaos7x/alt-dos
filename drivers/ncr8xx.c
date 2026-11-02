/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NCR8XX.SYS - PCI-SCSI mit NCR/Symbios 53C8xx (810, 815, 820, 825, 860,
 * 875, 895; in QEMU der LSI 53C895A) - AT-DOS 6.0, braucht einen 386
 *
 * CONFIG.SYS:  DEVICE=NCR8XX.SYS [/ID:7]
 *
 * Die Karte wird ueber die PCI-Konfiguration gesucht (Mechanismus 1,
 * Ports CF8h/CFCh mit 32 Bit), I/O-Basis aus BAR 0, I/O und Busmaster
 * werden eingeschaltet. Der SCRIPTS-Prozessor der Karte arbeitet ein
 * kleines Programm aus dem Treiber ab (scr[]): Ziel auswaehlen (ohne ATN,
 * keine Nachrichten), dann je nach Phase Befehl, Daten, Status und die
 * Abschlussmeldung uebertragen, am Ende ein SCRIPTS-Interrupt. Die Daten
 * gehen per Busmaster direkt in den Puffer des Kernels (physische Adresse
 * aus der Anforderung - ohne Paging gleich der linearen). Erste Stufe:
 * Abfragebetrieb (ISTAT), noch ohne IRQ.
 *
 * Gebaut mit -zu wie alle Treiber: keine Adressen lokaler Variablen.
 */
#include "drvlib.h"

/* ---- 32-Bit-Ports (386) ---------------------------------------------- */
unsigned long inl(unsigned port);
#pragma aux inl = "in eax,dx" "mov edx,eax" "shr edx,16" \
    __parm [__dx] __value [__dx __ax] __modify __exact [__ax __dx];
void outl(unsigned port, unsigned lo, unsigned hi);
#pragma aux outl = "push ebx" "shl ebx,16" "mov bx,ax" "xchg eax,ebx" \
    "out dx,eax" "xchg eax,ebx" "pop ebx" \
    __parm [__dx] [__ax] [__bx] __modify __exact [];

/* ---- PCI ---------------------------------------------------------------- */
static unsigned long pci_rd(unsigned bus, unsigned dev, unsigned fn, unsigned reg)
{
    outl(0xCF8, (dev << 11) | (fn << 8) | (reg & 0xFC), 0x8000 | bus);
    return inl(0xCFC);
}

static void pci_wr16(unsigned bus, unsigned dev, unsigned fn, unsigned reg, unsigned v)
{
    outl(0xCF8, (dev << 11) | (fn << 8) | (reg & 0xFC), 0x8000 | bus);
    outp(0xCFC + (reg & 2), (unsigned char)v);
    outp(0xCFD + (reg & 2), (unsigned char)(v >> 8));
}

/* Mechanismus 1 vorhanden? Erst lesen, dann den Adressport probieren */
static int pci_present(void)
{
    unsigned long old = inl(0xCF8), v;
    outl(0xCF8, 0x0000, 0x8000);
    v = inl(0xCF8);
    outl(0xCF8, (unsigned)old, (unsigned)(old >> 16));
    return v == 0x80000000UL;
}

/* ---- 53C8xx-Register (I/O) --------------------------------------------- */
#define SCNTL0  0x00
#define SCNTL1  0x01
#define SCID    0x04
#define SXFER   0x05
#define DSTAT   0x0C
#define SSTAT0  0x0D
#define ISTAT   0x14
#define CTEST3  0x1B
#define DFIFO   0x20
#define CTEST4  0x21
#define DSP     0x2C
#define DSPS    0x30
#define DMODE   0x38
#define DIEN    0x39
#define DCNTL   0x3B
#define SIEN0   0x40
#define SIEN1   0x41
#define SIST0   0x42
#define SIST1   0x43
#define RESPID  0x4A
#define STIME0  0x48
#define STEST3  0x4F

#define I_SRST  0x40                    /* ISTAT: Chip zuruecksetzen      */
#define I_SIP   0x02
#define I_DIP   0x01
#define D_SIR   0x04                    /* DSTAT: SCRIPTS-Interrupt       */

static unsigned io, irq, host = 7;
static char line[128];
static unsigned long lin;               /* lineare = physische Basis      */
static const char *chip;

static unsigned char rd8(unsigned r) { return inp(io + r); }
static void wr8(unsigned r, unsigned char v) { outp(io + r, v); }

/* ---- SCRIPTS ------------------------------------------------------------ */
static unsigned long scr[66];           /* Programm (je Befehl 2 Worte)    */
static unsigned char cdb[12], stat, msg, ident = 0x80;

#define SCSI_OK     0
#define SCSI_NODEV  1                   /* Auswahl-Zeitueberschreitung     */
#define SCSI_CHECK  2                   /* Check Condition                 */
#define SCSI_FAIL   3

static unsigned long phys(const void *p)
{
    return lin + (unsigned)p;
}

/* Befehl i: zwei Doppelworte */
static void op(unsigned i, unsigned long a, unsigned long b)
{
    scr[2 * i] = a;
    scr[2 * i + 1] = b;
}

#define L_LOOP  1
#define L_CMD   8
#define L_DOUT  10
#define L_DIN   12
#define L_ST    14
#define L_MI    16
#define L_MO    21
#define L_DONE  23
#define L_DISC  26
#define L_ERR   32

/* Programm fuer einen Befehl aufbauen: Ziel id, CDB (cdblen), Daten
   (len Bytes ab der physischen Adresse buf). Auswahl mit ATN, dann
   IDENTIFY (80h: LUN 0, ohne Trennen) - SCSI-2 verlangt es, und QEMU geht
   nach der Auswahl immer in MSG OUT. DISCONNECT: auf die Wiederauswahl
   warten (QEMU trennt trotz 80h vor den Daten; ein Trennen mitten in den
   Daten kaeme nur mit Trennerlaubnis vor). Andere Nachrichten werden
   quittiert und uebergangen. */
static void build(unsigned id, unsigned cdblen, unsigned long buf, unsigned long len)
{
    unsigned long base = phys(scr);
#define AT(l) (base + ((unsigned long)(l) << 3))
    op(0, 0x41000000UL | ((unsigned long)id << 16), AT(L_ERR));  /* SELECT ATN id */
    op(1, 0x820B0000UL, AT(L_CMD));     /* JUMP cmd,  WHEN COMMAND             */
    op(2, 0x800B0000UL, AT(L_DOUT));    /* JUMP dout, WHEN DATA_OUT            */
    op(3, 0x810B0000UL, AT(L_DIN));     /* JUMP din,  WHEN DATA_IN             */
    op(4, 0x830B0000UL, AT(L_ST));      /* JUMP st,   WHEN STATUS              */
    op(5, 0x870B0000UL, AT(L_MI));      /* JUMP mi,   WHEN MSG_IN              */
    op(6, 0x860B0000UL, AT(L_MO));      /* JUMP mo,   WHEN MSG_OUT             */
    op(7, 0x98080000UL, 2);             /* INT 2: unerwartete Phase            */
    op(8, 0x0A000000UL | cdblen, phys(cdb));    /* MOVE cdblen, COMMAND        */
    op(9, 0x80080000UL, AT(L_LOOP));    /* JUMP loop                           */
    op(10, 0x08000000UL | (len & 0xFFFFFFUL), buf);  /* MOVE len, DATA_OUT     */
    op(11, 0x80080000UL, AT(L_LOOP));
    op(12, 0x09000000UL | (len & 0xFFFFFFUL), buf);  /* MOVE len, DATA_IN      */
    op(13, 0x80080000UL, AT(L_LOOP));
    op(14, 0x0B000001UL, phys(&stat));  /* MOVE 1, STATUS                      */
    op(15, 0x80080000UL, AT(L_LOOP));
    op(16, 0x0F000001UL, phys(&msg));   /* MOVE 1, MSG_IN                      */
    op(17, 0x800C0000UL, AT(L_DONE));   /* JUMP done, IF 00h (COMMAND COMPLETE) */
    op(18, 0x800C0004UL, AT(L_DISC));   /* JUMP disc, IF 04h (DISCONNECT)      */
    op(19, 0x60000040UL, 0);            /* CLEAR ACK (SAVE DATA POINTER usw.)  */
    op(20, 0x80080000UL, AT(L_LOOP));
    op(21, 0x0E000001UL, phys(&ident)); /* MOVE 1, MSG_OUT (IDENTIFY)          */
    op(22, 0x80080000UL, AT(L_LOOP));
    op(23, 0x60000040UL, 0);            /* CLEAR ACK                           */
    op(24, 0x48000000UL, 0);            /* WAIT DISCONNECT                     */
    op(25, 0x98080000UL, 1);            /* INT 1: fertig                       */
    op(26, 0x60000040UL, 0);            /* disc: CLEAR ACK                     */
    op(27, 0x48000000UL, 0);            /* WAIT DISCONNECT                     */
    op(28, 0x50000000UL, AT(L_ERR));    /* WAIT RESELECT                       */
    op(29, 0x0F000001UL, phys(&msg));   /* MOVE 1, MSG_IN (IDENTIFY des Ziels) */
    op(30, 0x60000040UL, 0);            /* CLEAR ACK                           */
    op(31, 0x80080000UL, AT(L_LOOP));   /* weiter, wo es aufgehoert hat        */
    op(32, 0x98080000UL, 3);            /* INT 3: als Ziel ausgewaehlt (nie)   */
#undef AT
}

static void chip_reset(void)
{
    unsigned k;
    wr8(ISTAT, I_SRST);
    for (k = 0; k < 100; k++)
        inp(0x80);
    wr8(ISTAT, 0);
    wr8(SCID, (unsigned char)host);
    wr8(RESPID, (unsigned char)(1 << host));
    wr8(STIME0, 0x0C);                  /* Auswahl-Zeitueberschreitung ~100 ms */
    wr8(SXFER, 0);                      /* asynchron                       */
    wr8(SIEN0, 0x8F);                   /* Phasenfehler, Fehler, Abbruch,
                                           Reset, Paritaet                */
    wr8(SIEN1, 0x04);                   /* Auswahl-Zeitueberschreitung     */
    wr8(DIEN, 0x7D);                    /* SCRIPTS-Interrupt, Fehler       */
    rd8(DSTAT);                         /* alte Meldungen abholen          */
    rd8(SIST0);
    rd8(SIST1);
}

/* Befehl ausfuehren (Abfragebetrieb) */
static int run(unsigned id, unsigned cdblen, unsigned long buf, unsigned long len,
               unsigned tmo)
{
    unsigned t = devhlp(DH_TICKS, 0, 0, 0), code = 0;
    unsigned char is, ds = 0, s0 = 0, s1 = 0;
    unsigned long base;

    stat = 0xFF;
    msg = 0xFF;
    build(id, cdblen, buf, len);
    base = phys(scr);
    outl(io + DSP, (unsigned)base, (unsigned)(base >> 16));   /* los         */
    for (;;) {
        is = rd8(ISTAT);
        if (is & (I_SIP | I_DIP))
            break;
        if ((unsigned)(devhlp(DH_TICKS, 0, 0, 0) - t) > tmo) {
            chip_reset();               /* haengt: zuruecksetzen           */
            return SCSI_FAIL;
        }
    }
    if (is & I_DIP) {
        ds = rd8(DSTAT);
        if (ds & D_SIR)
            code = (unsigned)inl(io + DSPS);
    }
    if (is & I_SIP) {
        s0 = rd8(SIST0);
        inp(0x80);                      /* kurze Pause zwischen den Lesungen */
        s1 = rd8(SIST1);
    }
    if (s1 & 0x04)                      /* STO: kein Geraet                */
        return SCSI_NODEV;
    if (code != 1 || (s0 & 0x0F)) {
        if (s0 || (ds & ~D_SIR))
            chip_reset();               /* Busfehler/Abbruch: sauber neu   */
        return SCSI_FAIL;
    }
    if (stat == 0)
        return SCSI_OK;
    return stat == 2 ? SCSI_CHECK : SCSI_FAIL;
}

static void cdb6(unsigned char o, unsigned char alloc)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        cdb[i] = 0;
    cdb[0] = o;
    cdb[4] = alloc;
}

/* REQUEST SENSE: Sense Key << 8 | ASC, 0 = keine Antwort */
static unsigned char sense[18];

static unsigned req_sense(unsigned id)
{
    cdb6(0x03, sizeof(sense));
    if (run(id, 6, phys(sense), sizeof(sense), 300) != SCSI_OK)
        return 0;
    return ((sense[2] & 0x0F) << 8) | sense[12];
}

/* ---- Geraete ------------------------------------------------------------ */
static unsigned char target[DRV_MAXUNITS];
static unsigned bsz[DRV_MAXUNITS];
static unsigned nunits;

static const struct { unsigned id; const char *name; } chips[] = {
    { 0x0001, "53C810" }, { 0x0004, "53C815" }, { 0x0002, "53C820" },
    { 0x0003, "53C825" }, { 0x0006, "53C860" }, { 0x000F, "53C875" },
    { 0x008F, "53C875J" }, { 0x000C, "53C895" }, { 0x0012, "53C895A" },
    { 0x000B, "53C896" }
};

static unsigned pbus, pdev;             /* gefundene Karte                 */

static int find_card(void)
{
    unsigned bus, dev, k;
    unsigned long v;
    for (bus = 0; bus < 4; bus++)
        for (dev = 0; dev < 32; dev++) {
            v = pci_rd(bus, dev, 0, 0);
            if ((unsigned)v != 0x1000)  /* Hersteller NCR/Symbios/LSI      */
                continue;
            for (k = 0; k < sizeof(chips) / sizeof(chips[0]); k++)
                if ((unsigned)(v >> 16) == chips[k].id) {
                    chip = chips[k].name;
                    pbus = bus;
                    pdev = dev;
                    return 0;
                }
        }
    return -1;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    const char __far *a;
    unsigned bus, dev, id, k, n = 0, rc;
    unsigned long v;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    lin = r->base;
    if ((a = has_opt(r->args, "/ID:") ? r->args : 0) != 0) {
        for (; *a && !(a[0] == ':' && a[-1] == 'D'); a++)
            ;
        if (*a == ':' && a[1] >= '0' && a[1] <= '7')
            host = a[1] - '0';
    }
    if (!pci_present()) {
        dputs("  NCR8XX.SYS: kein PCI-Bus (Mechanismus 1)\n");
        return;
    }
    if (find_card()) {
        dputs("  NCR8XX.SYS: kein NCR 53C8xx gefunden\n");
        return;
    }
    bus = pbus;
    dev = pdev;
    v = pci_rd(bus, dev, 0, 0x10);      /* BAR 0: I/O                      */
    if (!(v & 1)) {
        dputs("  NCR8XX.SYS: BAR 0 ist kein I/O-Bereich\n");
        return;
    }
    io = (unsigned)v & 0xFFFC;
    irq = (unsigned)pci_rd(bus, dev, 0, 0x3C) & 0xFF;
    v = pci_rd(bus, dev, 0, 0x04);
    pci_wr16(bus, dev, 0, 0x04, (unsigned)v | 0x0005);  /* I/O + Busmaster */
    chip_reset();

    p = scat(line, "  NCR ");
    p = scat(p, chip);
    p = scat(p, " (PCI ");
    p = hex(p, bus, 2);
    p = scat(p, ":");
    p = hex(p, dev, 2);
    p = scat(p, ") an ");
    p = hex(p, io, 4);
    p = scat(p, "h, IRQ ");
    p = udec(p, irq);
    p = scat(p, ", SCSI-ID ");
    p = udec(p, host);
    scat(p, ", Abfragebetrieb\n");
    dputs(line);

    for (id = 0; id < 8; id++) {
        if (id == host)
            continue;
        cdb6(0x12, 36);                 /* INQUIRY                         */
        if (run(id, 6, r->buf_phys, 36, 100) != SCSI_OK)
            continue;
        p = scat(line, "  SCSI-ID ");
        *p++ = (char)('0' + id);
        p = scat(p, (buf[0] & 0x1F) == 0 ? ": Festplatte " :
                    (buf[0] & 0x1F) == 5 ? ": CD-ROM " : ": Ger\x84t ");
        for (k = 8; k < 32; k++)
            *p++ = buf[k] >= 32 ? (char)buf[k] : ' ';
        while (p[-1] == ' ')
            p--;
        *p = 0;
        if ((buf[0] & 0x1F) == 0 && n < DRV_MAXUNITS) {
            for (k = 0; k < 4; k++) {   /* Unit Attention abholen          */
                cdb6(0x00, 0);
                if (run(id, 6, 0, 0, 300) == SCSI_OK)
                    break;
                req_sense(id);
            }
            for (k = 0; k < 12; k++)
                cdb[k] = 0;
            cdb[0] = 0x25;              /* READ CAPACITY                   */
            rc = run(id, 10, r->buf_phys, 8, 300);
            if (rc == SCSI_OK && buf[6] == 2 && buf[7] == 0) {
                unsigned long last = ((unsigned long)buf[0] << 24) |
                    ((unsigned long)buf[1] << 16) | ((unsigned)buf[2] << 8) | buf[3];
                r->blocks[n] = last + 1;
                bsz[n] = 512;
                target[n++] = (unsigned char)id;
                p = scat(p, ", ");
                p = udec(p, (unsigned)((last + 1) >> 11));
                p = scat(p, " MB");
            } else
                p = scat(p, " (nicht bereit)");
        } else if ((buf[0] & 0x1F) == 5 && n < DRV_MAXUNITS) {
            for (k = 0; k < 4; k++) {
                cdb6(0x00, 0);
                if (run(id, 6, 0, 0, 300) == SCSI_OK)
                    break;
                req_sense(id);
            }
            r->blocks[n] = 0xFFFFFFFFUL;
            r->bsize[n] = 2048;
            bsz[n] = 2048;
            target[n++] = (unsigned char)id;
        }
        scat(p, "\n");
        dputs(line);
    }
    nunits = n;
    r->units = (unsigned char)n;
    r->count = 16;                      /* Bloecke je READ(10)             */
    r->status = n ? DRV_OK : DRV_ERR_NODEV;
}

static void xfer(int write)
{
    struct drv_req __far *r = REQ;
    unsigned long l = r->lba;
    unsigned n = write || r->count == 0 ? 1 : r->count;
    int tries, rc, spin = 0;
    unsigned sk;

    if (r->unit >= nunits) {
        r->status = DRV_ERR_IO;
        return;
    }
    if (write && bsz[r->unit] != 512) {
        r->status = DRV_ERR_WP;
        return;
    }
    if (bsz[r->unit] != 512 || n > 16)
        n = bsz[r->unit] != 512 ? 1 : 16;
    for (tries = 0; tries < 3; tries++) {
        unsigned i;
        for (i = 0; i < 12; i++)
            cdb[i] = 0;
        cdb[0] = write ? 0x2A : 0x28;   /* WRITE(10) / READ(10)            */
        cdb[2] = (unsigned char)(l >> 24);
        cdb[3] = (unsigned char)(l >> 16);
        cdb[4] = (unsigned char)(l >> 8);
        cdb[5] = (unsigned char)l;
        cdb[8] = (unsigned char)n;
        rc = run(target[r->unit], 10, r->buf_phys,
                 (unsigned long)n << (bsz[r->unit] == 512 ? 9 : 11), 1000);
        if (rc == SCSI_OK) {
            r->status = DRV_OK;
            return;
        }
        if (rc == SCSI_CHECK) {
            sk = req_sense(target[r->unit]);
            if (sk == 0x23A && bsz[r->unit] != 512 && tries > 0) {
                r->status = DRV_ERR_NOTREADY;   /* keine CD drin           */
                return;
            }
            if (sk == 0x204 && spin < 20) {     /* wird gerade bereit      */
                spin++;
                tries--;
                cli();
                proc_block(0xFFFF, 50);
            }
        }
    }
    r->status = DRV_ERR_IO;
}

static void flush(void)
{
    unsigned u, i;
    for (u = 0; u < nunits; u++) {
        if (bsz[u] != 512)
            continue;
        for (i = 0; i < 12; i++)
            cdb[i] = 0;
        cdb[0] = 0x35;                  /* SYNCHRONIZE CACHE               */
        run(target[u], 10, 0, 0, 1000);
    }
    REQ->status = DRV_OK;
}

void strategy(void)
{
    struct drv_req __far *r = REQ;

    switch (r->cmd) {
    case DRV_INIT:
        devhlp_ptr = MK_FP(r->devhlp_sel, r->devhlp_off);
        init();
        break;
    case DRV_READ:
        xfer(0);
        break;
    case DRV_WRITE:
        xfer(1);
        break;
    case DRV_FLUSH:
        flush();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
