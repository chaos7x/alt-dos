/*
 * AHA154X.SYS - Adaptec AHA-1540/1542 (ISA, Busmaster), nur lesend
 *
 * Mailbox-Schnittstelle mit 24-Bit-Adressen: Der Treiber legt Mailboxen
 * und Befehlsbloecke (CCB) im eigenen Datensegment ab und gibt der Karte
 * deren physische Adressen. Die Karte holt sich alles per Busmaster-DMA
 * selbst, auch die Daten. Gewartet wird ohne Interrupt durch Abfragen
 * der Eingangs-Mailbox.
 *
 * Erkennung: Die ueblichen Adressen werden der Reihe nach nur gelesen.
 * Bei einem Adaptec sind im Interrupt-Register (Basis+2) die Bits 4-6
 * immer 0; ein unbelegter Port oder ein MPU-401 (330h!) liefert dort FFh.
 * Erst wenn das passt, bekommt die Karte einen Reset.
 */
#include "drvlib.h"

/* Register */
#define CTRL(b)     (b)             /* schreiben: Steuerung             */
#define STAT(b)     (b)             /* lesen: Status                    */
#define DATA(b)     ((b) + 1)       /* Befehl/Daten                     */
#define INTR(b)     ((b) + 2)       /* lesen: Interrupt-Flags           */

#define C_SRST      0x40            /* Soft-Reset                       */
#define C_IRST      0x20            /* Interrupt quittieren             */
#define S_STST      0x80            /* Selbsttest laeuft                */
#define S_DIAGF     0x40            /* Diagnose fehlgeschlagen          */
#define S_INIT      0x20            /* Mailbox-Initialisierung noetig   */
#define S_IDLE      0x10
#define S_CDF       0x08            /* Befehls-/Datenregister voll      */
#define S_DF        0x04            /* Daten fuer uns bereit            */
#define S_INVDCMD   0x01            /* ungueltiger Befehl               */
#define I_HACC      0x04            /* Befehl abgeschlossen             */

#define CMD_MBINIT      0x01
#define CMD_START       0x02
#define CMD_CONFIG      0x0B
#define CMD_EXTBIOS     0x28
#define CMD_MBENABLE    0x29

struct mbox { unsigned char status, addr[3]; };

struct ccb {
    unsigned char op, idlun, cdblen, rsalen;
    unsigned char datalen[3], dataptr[3], linkptr[3];
    unsigned char linkid, hastat, tarstat, rsv[2];
    unsigned char cdb[12];
    unsigned char sense[14];
};

static const unsigned ports[] = { 0x330, 0x334, 0x230, 0x234, 0x130, 0x134 };

static unsigned base;
static unsigned long lin;               /* lineare Basis dieses Treibers */
static volatile struct mbox mb[2];      /* [0] ausgehend, [1] eingehend  */
static struct ccb ccb;
static unsigned char target[DRV_MAXUNITS];
static char line[80];
static unsigned char cdb[12];

static void put3(unsigned char *d, unsigned long v)    /* big endian */
{
    d[0] = (unsigned char)(v >> 16);
    d[1] = (unsigned char)(v >> 8);
    d[2] = (unsigned char)v;
}

static unsigned long phys(const void *p)
{
    return lin + (unsigned)p;
}

/* ---- Befehle an die Karte ---------------------------------------------- */
static int cmd_out(unsigned char b)
{
    unsigned start = ticks();
    while (inp(STAT(base)) & S_CDF)
        if ((unsigned)(ticks() - start) > 100)
            return -1;
    outp(DATA(base), b);
    return 0;
}

static int data_in(void)
{
    unsigned start = ticks();
    while (!(inp(STAT(base)) & S_DF))
        if ((unsigned)(ticks() - start) > 100)
            return -1;
    return inp(DATA(base));
}

static int cmd_done(void)
{
    unsigned start = ticks();
    while (!(inp(INTR(base)) & I_HACC))
        if ((unsigned)(ticks() - start) > 100)
            return -1;
    outp(CTRL(base), C_IRST);
    return (inp(STAT(base)) & S_INVDCMD) ? -1 : 0;
}

/* Lesen: passt das Register-Muster zu einem Adaptec? */
static int looks_like_aha(unsigned b)
{
    unsigned char s = inp(STAT(b)), i = inp(INTR(b));
    if (s == 0xFF || i == 0xFF)
        return 0;
    if (i & 0x70)                       /* reservierte Bits immer 0     */
        return 0;
    if (s & 0x02)                       /* reserviertes Statusbit       */
        return 0;
    return 1;
}

static int reset(void)
{
    unsigned start;
    outp(CTRL(base), C_SRST);
    start = ticks();
    for (;;) {
        unsigned char s = inp(STAT(base));
        if (!(s & S_STST) && (s & (S_IDLE | S_INIT)) == (S_IDLE | S_INIT)) {
            if (s & S_DIAGF)
                return -1;
            return 0;
        }
        if ((unsigned)(ticks() - start) > 300)
            return -1;
    }
}

/* ---- SCSI-Befehl ausfuehren -------------------------------------------- */
#define SCSI_OK     0
#define SCSI_NODEV  1               /* Auswahl-Zeitueberschreitung      */
#define SCSI_CHECK  2               /* Check Condition (z. B. Unit Attention) */
#define SCSI_FAIL   3

static int scsi(unsigned id, unsigned len, unsigned long buf, unsigned cdblen)
{
    unsigned start, i;
    unsigned char st;

    ccb.op = 0;                             /* SCSI-Initiator           */
    ccb.idlun = (unsigned char)((id << 5) | 0x08);  /* Daten zum Rechner */
    ccb.cdblen = (unsigned char)cdblen;
    ccb.rsalen = sizeof(ccb.sense);
    put3(ccb.datalen, len);
    put3(ccb.dataptr, buf);
    put3(ccb.linkptr, 0);
    ccb.linkid = ccb.hastat = ccb.tarstat = 0;
    for (i = 0; i < 12; i++)
        ccb.cdb[i] = i < cdblen ? cdb[i] : 0;

    mb[1].status = 0;
    put3((unsigned char *)mb[0].addr, phys(&ccb));
    mb[0].status = 1;                       /* Befehl starten           */
    if (cmd_out(CMD_START))
        return SCSI_FAIL;

    start = ticks();
    while (mb[1].status == 0)
        if ((unsigned)(ticks() - start) > 300)
            return SCSI_FAIL;
    st = mb[1].status;
    mb[1].status = 0;
    outp(CTRL(base), C_IRST);

    if (ccb.hastat == 0x11)
        return SCSI_NODEV;
    if (st == 1 && ccb.hastat == 0 && ccb.tarstat == 0)
        return SCSI_OK;
    if (ccb.tarstat == 2)
        return SCSI_CHECK;
    return SCSI_FAIL;
}

static void cdb6(unsigned char op, unsigned char alloc)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        cdb[i] = 0;
    cdb[0] = op;
    cdb[4] = alloc;
}

/* ---- Initialisierung --------------------------------------------------- */
static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    unsigned i, id, host, n = 0;
    int rc, k;
    unsigned char dma, lock;
    char *p;

    lin = r->base;
    r->units = 0;
    base = 0;
    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
        if (looks_like_aha(ports[i])) {
            base = ports[i];
            if (reset() == 0)
                break;
            base = 0;
        }
    if (!base) {
        r->status = DRV_ERR_NODEV;
        return;
    }

    /* Konfiguration: DMA-Kanal, IRQ, eigene SCSI-ID */
    cmd_out(CMD_CONFIG);
    dma = (unsigned char)data_in();
    data_in();
    host = data_in() & 7;
    cmd_done();

    /* 154xC/CF: Mailbox ggf. entsperren (bei erweiterter BIOS-Uebersetzung) */
    cmd_out(CMD_EXTBIOS);
    data_in();
    lock = (unsigned char)data_in();
    if (cmd_done() == 0 && lock) {
        cmd_out(CMD_MBENABLE);
        cmd_out(0);
        cmd_out(lock);
        cmd_done();
    }

    /* Busmaster: DMA-Kanal am zweiten Controller auf Kaskade schalten */
    for (k = 5; k <= 7; k++)
        if (dma & (1 << k)) {
            outp(0xD6, (unsigned char)(0xC0 | (k - 4)));
            outp(0xD4, (unsigned char)(k - 4));
        }

    /* eine Mailbox je Richtung */
    mb[0].status = mb[1].status = 0;
    cmd_out(CMD_MBINIT);
    cmd_out(1);
    {
        static unsigned char a[3];
        put3(a, phys((void *)mb));
        cmd_out(a[0]); cmd_out(a[1]); cmd_out(a[2]);
    }
    if (cmd_done()) {
        dputs("  AHA154X: Mailbox-Initialisierung fehlgeschlagen\n");
        r->status = DRV_ERR_NODEV;
        return;
    }

    p = scat(line, "  Adaptec 154x an Port ");
    *p++ = (char)('0' + (base >> 8));
    *p++ = (char)("0123456789ABCDEF"[(base >> 4) & 15]);
    *p++ = (char)("0123456789ABCDEF"[base & 15]);
    p = scat(p, "h, DMA ");
    for (k = 0; k < 8; k++)
        if (dma & (1 << k))
            *p++ = (char)('0' + k);
    scat(p, "\n");
    dputs(line);

    for (id = 0; id < 8; id++) {
        if (id == host)
            continue;
        cdb6(0x12, 36);                     /* INQUIRY                  */
        if (scsi(id, 36, r->buf_phys, 6) != SCSI_OK)
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
            for (k = 0; k < 4; k++) {       /* Unit Attention abholen   */
                cdb6(0x00, 0);              /* TEST UNIT READY          */
                if (scsi(id, 0, 0, 6) == SCSI_OK)
                    break;
            }
            for (k = 0; k < 12; k++)
                cdb[k] = 0;
            cdb[0] = 0x25;                  /* READ CAPACITY            */
            rc = scsi(id, 8, r->buf_phys, 10);
            if (rc == SCSI_OK && buf[6] == 2 && buf[7] == 0) {   /* 512 Byte */
                unsigned long last = ((unsigned long)buf[0] << 24) |
                    ((unsigned long)buf[1] << 16) | ((unsigned)buf[2] << 8) | buf[3];
                r->blocks[n] = last + 1;
                target[n++] = (unsigned char)id;
                p = scat(p, ", ");
                p = udec(p, (unsigned)((last + 1) >> 11));
                p = scat(p, " MB");
            } else {
                p = scat(p, " (nicht bereit)");
            }
        } else if ((buf[0] & 0x1F) == 5) {
            p = scat(p, " (noch kein Dateisystem)");
        }
        scat(p, "\n");
        dputs(line);
    }
    r->units = (unsigned char)n;
    r->status = n ? DRV_OK : DRV_ERR_NODEV;
}

static void read(void)
{
    struct drv_req __far *r = REQ;
    unsigned long l = r->lba;
    int tries, rc;

    if (r->unit >= DRV_MAXUNITS) {
        r->status = DRV_ERR_IO;
        return;
    }
    for (tries = 0; tries < 3; tries++) {
        cdb[0] = 0x28;                      /* READ(10)                 */
        cdb[1] = 0;
        cdb[2] = (unsigned char)(l >> 24);
        cdb[3] = (unsigned char)(l >> 16);
        cdb[4] = (unsigned char)(l >> 8);
        cdb[5] = (unsigned char)l;
        cdb[6] = 0;
        cdb[7] = 0;
        cdb[8] = 1;                         /* ein Block                */
        cdb[9] = 0;
        rc = scsi(target[r->unit], 512, r->buf_phys, 10);
        if (rc == SCSI_OK) {
            r->status = DRV_OK;
            return;
        }
    }
    r->status = DRV_ERR_IO;
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
        read();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
