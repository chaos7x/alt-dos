/*
 * IDE.SYS - Treiber fuer IDE-Platten (ATA) an beiden Kanaelen
 *
 * Primaerer Kanal 1F0h/3F6h, sekundaerer 170h/376h, je Master und Slave:
 * bis zu vier Geraete, Einheiten in dieser Reihenfolge. ATAPI-CD-ROMs
 * sind Einheiten mit 2048-Byte-Bloecken (PACKET + READ(10), nur lesend);
 * ein Dateisystem bekommen sie ueber IFS=ISO9660.IFS.
 *
 * PIO-Modus, ohne Interrupt (IRQ 14/15 bleiben maskiert, der Treiber fragt
 * den Status ab). LBA28, wenn die Platte es kann, sonst CHS mit der
 * aktuellen Geometrie der Platte (die das BIOS beim Start eingestellt hat) -
 * Platten aus der 286-Zeit kennen oft kein LBA.
 *
 * Erkennung liest zuerst: Ein unbelegter Kanal liefert FFh (oder 7Fh) im
 * Status, erst danach wird ein Laufwerk ausgewaehlt und IDENTIFY gesendet.
 */
#include "drvlib.h"

#define R_DATA      0
#define R_ERROR     1
#define R_COUNT     2
#define R_SECTOR    3
#define R_CYL_LO    4
#define R_CYL_HI    5
#define R_DRIVE     6
#define R_STATUS    7
#define R_CMD       7

#define ST_BSY      0x80
#define ST_DRDY     0x40
#define ST_DRQ      0x08
#define ST_ERR      0x01

#define NUNIT       4

struct unit {
    unsigned       base, ctl;           /* Befehlsregister, Steuerregister */
    unsigned char  sel;                 /* 00h Master, 10h Slave           */
    unsigned char  chs;
    unsigned char  atapi;               /* CD-ROM: Paketbefehle            */
    unsigned       cyls, heads, spt;
};

static struct unit unit[NUNIT];
static int nunit;
static unsigned short id[256];
static char line[80];
static const unsigned ports[2][2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };

/* (x / d) in einem Schritt, x >> 16 muss kleiner als d sein;
   -> Rest << 16 | Quotient (DIV ohne Watcom-Hilfsroutinen) */
unsigned long divstep(unsigned long x, unsigned d);
#pragma aux divstep = "div bx" parm [dx ax] [bx] value [dx ax];

/* 32 Bit durch 16 Bit; -> Quotient, *rem = Rest (rem muss static sein) */
static unsigned long div32(unsigned long x, unsigned d, unsigned *rem)
{
    unsigned hi = (unsigned)(x >> 16);
    unsigned long t = divstep(((unsigned long)(hi % d) << 16) | (unsigned)x, d);
    *rem = (unsigned)(t >> 16);
    return ((unsigned long)(hi / d) << 16) | (unsigned)t;
}

/* ca. 400 ns nach einem Laufwerkswechsel: Steuerregister viermal lesen */
static void settle(unsigned ctl)
{
    inp(ctl); inp(ctl); inp(ctl); inp(ctl);
}

/* warten, bis BSY weg ist und (mask & status) == want; 0 = ok */
static int wait_status(unsigned base, unsigned char mask, unsigned char want,
                       unsigned t)
{
    unsigned start = ticks();
    unsigned char s;
    for (;;) {
        s = inp(base + R_STATUS);
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

/* Modellname (Bytes vertauscht) an p anhaengen */
static char *model(char *p)
{
    int i;
    for (i = 27; i < 47; i++) {
        *p++ = (char)(id[i] >> 8);
        *p++ = (char)id[i];
    }
    while (p > line && p[-1] == ' ')
        p--;
    *p = 0;
    return p;
}

static char *where(int ch, int dev)
{
    return scat(line, ch == 0 ? (dev ? "  IDE prim\x84r Slave: " : "  IDE prim\x84r Master: ")
                              : (dev ? "  IDE sekund\x84r Slave: " : "  IDE sekund\x84r Master: "));
}

/* ein Geraet pruefen; meldet Platten als Einheit, ATAPI nur im Protokoll */
static void probe(int ch, int dev)
{
    struct drv_req __far *r = REQ;
    unsigned base = ports[ch][0], ctl = ports[ch][1];
    struct unit *u;
    unsigned long n;
    unsigned char s, sig1, sig2;
    char *p;
    int i;

    outp(base + R_DRIVE, (unsigned char)(0xA0 | (dev << 4)));
    settle(ctl);
    s = inp(base + R_STATUS);
    if (s == 0x00 || s == 0xFF || s == 0x7F)
        return;                             /* kein Geraet              */
    if (wait_status(base, 0, 0, 100) && !(inp(base + R_STATUS) & ST_ERR))
        return;

    outp(base + R_CMD, 0xEC);               /* IDENTIFY DEVICE          */
    settle(ctl);
    if (inp(base + R_STATUS) == 0)
        return;
    if (wait_status(base, ST_DRQ, ST_DRQ, 300)) {
        /* abgelehnt: ATAPI meldet sich mit der Signatur 14h/EBh */
        sig1 = inp(base + R_CYL_LO);
        sig2 = inp(base + R_CYL_HI);
        if (sig1 != 0x14 || sig2 != 0xEB)
            return;
        outp(base + R_CMD, 0xA1);           /* IDENTIFY PACKET DEVICE   */
        settle(ctl);
        if (wait_status(base, ST_DRQ, ST_DRQ, 300))
            return;
        insw_far(base + R_DATA, (void __far *)id, 256);
        p = where(ch, dev);
        if (((id[0] >> 8) & 0x1F) != 5 || nunit == NUNIT) {
            p = scat(p, "ATAPI-Ger\x84t ");
            p = model(p);
            scat(p, " (nicht unterst\x81tzt)\n");
            dputs(line);
            return;
        }
        u = &unit[nunit];
        u->base = base;
        u->ctl = ctl;
        u->sel = (unsigned char)(dev << 4);
        u->chs = 0;
        u->atapi = 1;
        r->blocks[nunit] = 0xFFFFFFFFUL;    /* Groesse haengt an der CD */
        r->bsize[nunit] = 2048;
        nunit++;
        p = scat(p, "CD-ROM ");
        p = model(p);
        scat(p, "\n");
        dputs(line);
        return;
    }
    insw_far(base + R_DATA, (void __far *)id, 256);
    if (nunit == NUNIT || id[0] & 0x8000)   /* Bit 15: kein ATA-Geraet  */
        return;

    u = &unit[nunit];
    u->base = base;
    u->ctl = ctl;
    u->sel = (unsigned char)(dev << 4);
    u->chs = 0;
    u->atapi = 0;
    n = id[60] | ((unsigned long)id[61] << 16);     /* LBA-Sektoren */
    if (!(id[49] & 0x0200) || n == 0) {
        /* kein LBA: aktuelle Geometrie (Wort 54-56), sonst Grundgeometrie */
        u->chs = 1;
        if ((id[53] & 1) && id[54] && id[55] && id[56]) {
            u->cyls = id[54]; u->heads = id[55]; u->spt = id[56];
        } else {
            u->cyls = id[1]; u->heads = id[3]; u->spt = id[6];
        }
        if (u->cyls == 0 || u->heads == 0 || u->heads > 16 ||
            u->spt == 0 || u->spt > 255)
            return;
        n = 0;
        for (i = 0; i < (int)(u->heads * u->spt); i++)  /* ohne __U4M */
            n += u->cyls;
    }
    r->blocks[nunit] = n;
    nunit++;

    p = where(ch, dev);
    p = model(p);
    p = scat(p, ", ");
    p = udec(p, (unsigned)(n >> 11));
    scat(p, u->chs ? " MB (CHS)\n" : " MB\n");
    dputs(line);
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char s;
    int ch;

    nunit = 0;
    for (ch = 0; ch < 2; ch++) {
        s = inp(ports[ch][0] + R_STATUS);
        if (s == 0xFF || s == 0x7F)         /* Kanal nicht vorhanden    */
            continue;
        probe(ch, 0);
        probe(ch, 1);
    }
    r->units = (unsigned char)nunit;
    r->status = nunit ? DRV_OK : DRV_ERR_NODEV;
}

/* ATAPI: einen 2048-Byte-Block mit READ(10) per PACKET lesen; 0 = ok */
static unsigned char pkt[12];

static int atapi_read(struct unit *u, unsigned long l, unsigned sel, unsigned off)
{
    void __far *pp = (void __far *)pkt;
    unsigned b = u->base, got = 0, cnt, start;
    int i;

    outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel));
    settle(u->ctl);
    start = ticks();
    while (inp(b + R_STATUS) & ST_BSY)
        if ((unsigned)(ticks() - start) > 300)
            return -1;
    outp(b + R_ERROR, 0);                   /* Features: PIO            */
    outp(b + R_CYL_LO, 0x00);               /* hoechstens 2048 Bytes    */
    outp(b + R_CYL_HI, 0x08);               /* je DRQ-Block             */
    outp(b + R_CMD, 0xA0);                  /* PACKET                   */
    settle(u->ctl);
    if (wait_status(b, ST_DRQ, ST_DRQ, 300))
        return -1;
    for (i = 0; i < 12; i++)
        pkt[i] = 0;
    pkt[0] = 0x28;                          /* READ(10)                 */
    pkt[2] = (unsigned char)(l >> 24);
    pkt[3] = (unsigned char)(l >> 16);
    pkt[4] = (unsigned char)(l >> 8);
    pkt[5] = (unsigned char)l;
    pkt[8] = 1;                             /* ein Block                */
    outsw_far(b + R_DATA, FP_SEG(pp), FP_OFF(pp), 6);
    while (got < 2048) {
        settle(u->ctl);
        if (wait_status(b, ST_DRQ, ST_DRQ, 1000))   /* CD laeuft evtl. an */
            return -1;
        cnt = inp(b + R_CYL_LO) | ((unsigned)inp(b + R_CYL_HI) << 8);
        if (cnt == 0 || (cnt & 1) || got + cnt > 2048)
            return -1;
        insw_far(b + R_DATA, MK_FP(sel, off + got), cnt >> 1);
        got += cnt;
    }
    settle(u->ctl);
    return wait_status(b, 0, 0, 300);
}

/* einen Sektor lesen oder schreiben */
static void xfer(int write)
{
    static unsigned sec, head;              /* static: SS ist nicht DS (-zu) */
    struct drv_req __far *r = REQ;
    struct unit *u = &unit[r->unit];
    unsigned long l = r->lba, track;
    unsigned cyl = 0, b = u->base;
    int tries;

    if (u->atapi) {
        if (write) {
            r->status = DRV_ERR_WP;
            return;
        }
        for (tries = 0; tries < 3; tries++)     /* 1. Versuch: Unit Attention */
            if (atapi_read(u, l, r->buf_sel, r->buf_off) == 0) {
                r->status = DRV_OK;
                return;
            }
        r->status = DRV_ERR_IO;
        return;
    }

    if (u->chs) {
        track = div32(l, u->spt, &sec);
        cyl = (unsigned)div32(track, u->heads, &head);
        if (cyl >= u->cyls) {
            r->status = DRV_ERR_IO;
            return;
        }
    }
    for (tries = 0; tries < 3; tries++) {
        if (wait_status(b, 0, 0, 300))
            continue;
        if (u->chs) {
            outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel | head));
            outp(b + R_COUNT, 1);
            outp(b + R_SECTOR, (unsigned char)(sec + 1));   /* ab 1      */
            outp(b + R_CYL_LO, (unsigned char)cyl);
            outp(b + R_CYL_HI, (unsigned char)(cyl >> 8));
        } else {
            outp(b + R_DRIVE, (unsigned char)(0xE0 | u->sel |
                                               ((unsigned)(l >> 24) & 0x0F)));
            outp(b + R_COUNT, 1);
            outp(b + R_SECTOR, (unsigned char)l);
            outp(b + R_CYL_LO, (unsigned char)(l >> 8));
            outp(b + R_CYL_HI, (unsigned char)(l >> 16));
        }
        settle(u->ctl);
        outp(b + R_CMD, write ? 0x30 : 0x20);   /* WRITE / READ SECTORS */
        settle(u->ctl);
        if (wait_status(b, ST_DRQ, ST_DRQ, 300))
            continue;
        if (!write) {
            insw_far(b + R_DATA, MK_FP(r->buf_sel, r->buf_off), 256);
        } else {
            outsw_far(b + R_DATA, r->buf_sel, r->buf_off, 256);
            settle(u->ctl);
            if (wait_status(b, 0, 0, 300))      /* Platte hat geschrieben */
                continue;
        }
        r->status = DRV_OK;
        return;
    }
    r->status = DRV_ERR_IO;
}

/* FLUSH CACHE an alle Platten; aeltere Platten kennen den Befehl nicht
   und brechen ihn ab - die haben dann auch keinen Schreib-Cache */
static void flush(void)
{
    struct drv_req __far *r = REQ;
    int i;
    for (i = 0; i < nunit; i++) {
        if (unit[i].atapi)
            continue;
        if (wait_status(unit[i].base, 0, 0, 300) && !(inp(unit[i].base + R_STATUS) & ST_ERR))
            continue;
        outp(unit[i].base + R_DRIVE, (unsigned char)(0xA0 | unit[i].sel));
        settle(unit[i].ctl);
        outp(unit[i].base + R_CMD, 0xE7);
        settle(unit[i].ctl);
        wait_status(unit[i].base, 0, 0, 3000);
    }
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
    case DRV_READ:
        if (r->unit >= nunit)
            r->status = DRV_ERR_IO;
        else
            xfer(0);
        break;
    case DRV_WRITE:
        if (r->unit >= nunit)
            r->status = DRV_ERR_IO;
        else
            xfer(1);
        break;
    case DRV_FLUSH:
        flush();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
