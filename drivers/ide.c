/*
 * IDE.SYS - Treiber fuer die erste IDE-Festplatte (Port 1F0h), nur lesend
 *
 * PIO-Modus, LBA28, ohne Interrupt (IRQ 14 bleibt maskiert, der Treiber
 * fragt den Status ab). Erkennung liest zuerst: Ein unbelegter Port liefert
 * FFh, erst danach wird das Laufwerk ausgewaehlt und IDENTIFY gesendet.
 */
#include "drvlib.h"

#define BASE    0x1F0
#define DATA    (BASE + 0)
#define COUNT   (BASE + 2)
#define LBA0    (BASE + 3)
#define LBA1    (BASE + 4)
#define LBA2    (BASE + 5)
#define DRIVE   (BASE + 6)
#define STATUS  (BASE + 7)
#define CMD     (BASE + 7)

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

static unsigned short id[256];
static char line[80];

/* warten, bis BSY weg ist und (mask & status) == want; 0 = ok */
static int wait_status(unsigned char mask, unsigned char want, unsigned t)
{
    unsigned start = ticks();
    unsigned char s;
    for (;;) {
        s = inp(STATUS);
        if (!(s & ST_BSY)) {
            if (s & ST_ERR)
                return -1;
            if ((s & mask) == want)
                return 0;
        }
        if ((unsigned)(ticks() - start) > t)
            return -1;
    }
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned long n;
    unsigned mb;
    unsigned char s = inp(STATUS);
    char *p;
    int i;

    r->units = 0;
    if (s == 0xFF || s == 0x7F) {           /* nichts angeschlossen     */
        r->status = DRV_ERR_NODEV;
        return;
    }
    outp(DRIVE, 0xA0);                      /* Master                   */
    if (wait_status(0, 0, 300)) {
        r->status = DRV_ERR_NODEV;
        return;
    }
    outp(CMD, 0xEC);                        /* IDENTIFY                 */
    if (wait_status(ST_DRQ, ST_DRQ, 300)) {
        r->status = DRV_ERR_NODEV;
        return;
    }
    insw_far(DATA, (void __far *)id, 256);

    n = id[60] | ((unsigned long)id[61] << 16);     /* LBA-Sektoren */
    r->units = 1;
    r->blocks[0] = n;
    r->status = DRV_OK;

    p = scat(line, "  IDE Master: ");
    for (i = 27; i < 47; i++) {             /* Modellname, Bytes vertauscht */
        *p++ = (char)(id[i] >> 8);
        *p++ = (char)id[i];
    }
    while (p > line && p[-1] == ' ')
        p--;
    *p = 0;
    mb = (unsigned)(n >> 11);
    p = scat(p, ", ");
    p = udec(p, mb);
    scat(p, " MB\n");
    dputs(line);
}

static void read(void)
{
    struct drv_req __far *r = REQ;
    unsigned long l = r->lba;
    int tries;

    for (tries = 0; tries < 3; tries++) {
        if (wait_status(0, 0, 300))
            continue;
        outp(DRIVE, (unsigned char)(0xE0 | ((unsigned)(l >> 24) & 0x0F)));
        outp(COUNT, 1);
        outp(LBA0, (unsigned char)l);
        outp(LBA1, (unsigned char)(l >> 8));
        outp(LBA2, (unsigned char)(l >> 16));
        outp(CMD, 0x20);                    /* READ SECTORS             */
        if (wait_status(ST_DRQ, ST_DRQ, 300))
            continue;
        insw_far(DATA, MK_FP(r->buf_sel, r->buf_off), 256);
        r->status = DRV_OK;
        return;
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
        if (r->unit != 0)
            r->status = DRV_ERR_IO;
        else
            read();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
