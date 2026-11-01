/*
 * AT-DOS 5.0 - Treiber fuer den Diskettencontroller (NEC 765 / Intel 82077)
 *
 * Laufwerke A: und B:, Typ aus dem CMOS (Register 10h: A oben, B unten;
 * 1 = 360 KB, 2 = 1,2 MB, 3 = 720 KB, 4 = 1,44 MB, 5/6 = 2,88 MB).
 * Formate 360 KB bis 2,88 MB. Beim Einlegen erkennt der Treiber das
 * Format ueber die Datenrate: READ ID gelingt nur mit der Rate, mit der
 * die Diskette beschrieben ist (250 kbit/s DD, 300 kbit/s DD im 1,2-MB-
 * Laufwerk mit Doppelschritt, 500 kbit/s HD, 1 Mbit/s ED mit
 * Perpendicular-Modus). Gewechselt? Die Wechselleitung (3F7h Bit 7) sagt
 * es; 360-KB-Laufwerke haben keine, dort wird jedes Mal neu erkannt.
 *
 * Spurpuffer (wie die alten DOS-Treiber): Gelesen wird immer die ganze
 * Spur einer Seite mit einem Befehl in einen DMA-Puffer (18 KB, ohne
 * 64-KB-Grenze) - Sektor fuer Sektor kostete je eine Umdrehung.
 * Geschrieben wird in den Puffer und am Ende jedes FAPI-Aufrufs (fd_sync,
 * auch aus fat_flush und DASD-Schreiben) in zusammenhaengenden Stuecken
 * auf die Diskette - nach jedem Aufruf ist die Diskette stimmig.
 *
 * Waehrend der Controller arbeitet (Suchen, Lesen, Motoranlauf), schlaeft
 * der Task (proc_block), und andere laufen weiter - die Kernelsperre
 * haelt er dabei. IRQ 6 weckt ihn (start.asm -> fdc_isr). Die Motoren
 * gehen 3 s nach dem letzten Zugriff aus (fd_tick aus dem Scheduler).
 */
#include "kern.h"

#define DOR     0x3F2           /* Digital Output Register             */
#define MSR     0x3F4           /* Main Status Register                */
#define FIFO    0x3F5
#define CCR     0x3F7           /* schreiben: Datenrate                */
#define DIR     0x3F7           /* lesen: Bit 7 = Diskette gewechselt  */

#define DOR_RUN 0x0C            /* nicht Reset, DMA/IRQ an             */
#define MAXSPT  36

/* Formate: Sektoren, je Spur, Zylinder, Rate (CCR), GAP3 lesen/schreiben,
   GAP3 formatieren, Doppelschritt, Perpendicular (ED) */
struct ffmt {
    unsigned       blocks;
    unsigned char  spt, cyls, rate, gap, fgap, dstep, perp;
};
static const struct ffmt fmts[] = {
    {  720,  9, 40, 2, 0x2A, 0x50, 0, 0 },  /* 0: 360 KB, 250 kbit/s     */
    {  720,  9, 40, 1, 0x23, 0x50, 1, 0 },  /* 1: 360 KB im 1,2-MB-Lw.   */
    { 2400, 15, 80, 0, 0x1B, 0x54, 0, 0 },  /* 2: 1,2 MB, 500 kbit/s     */
    { 1440,  9, 80, 2, 0x2A, 0x50, 0, 0 },  /* 3: 720 KB, 250 kbit/s     */
    { 2880, 18, 80, 0, 0x1B, 0x6C, 0, 0 },  /* 4: 1,44 MB, 500 kbit/s    */
    { 5760, 36, 80, 3, 0x1B, 0x54, 0, 1 },  /* 5: 2,88 MB, 1 Mbit/s      */
};
/* je Laufwerkstyp die moeglichen Formate, das eigentliche zuerst */
static const signed char cand[6][4] = {
    { -1 }, { 0, -1 }, { 2, 1, -1 }, { 3, -1 }, { 4, 3, -1 }, { 5, 4, 3, -1 } };

static struct {
    unsigned char type;         /* 0 = kein Laufwerk                   */
    unsigned char fi;           /* Format (Index in fmts)              */
    unsigned char known;        /* Format erkannt / gesetzt            */
    unsigned char pcyl;         /* Kopf steht auf (physisch), FF = ?   */
} fd[2];

static unsigned char motors;    /* Bit 0/1: Motor A:/B: an             */
static unsigned char cur_rate = 0xFF, cur_perp, sel_drv;
static unsigned char busy;      /* Befehl laeuft (fd_tick: Motor an lassen) */
static unsigned long last_use;

/* Spurpuffer: eine Seite einer Spur */
static unsigned tb_sel;
static unsigned long tb_lin;
static signed char tb_drv = -1;
static unsigned char tb_cyl, tb_head, tb_ndirty;
static unsigned char tb_valid[MAXSPT], tb_dirty[MAXSPT];

#define F(d) (&fmts[fd[d].fi])

static int fdc_out(unsigned char b)
{
    unsigned i;
    for (i = 0; i < 60000U; i++) {
        if ((inb(MSR) & 0xC0) == 0x80) {        /* bereit, Richtung CPU->FDC */
            outb(FIFO, b);
            return 0;
        }
    }
    return -1;
}

static int fdc_in(void)
{
    unsigned i;
    for (i = 0; i < 60000U; i++) {
        if ((inb(MSR) & 0xD0) == 0xD0)          /* bereit, FDC->CPU, belegt */
            return inb(FIFO);
    }
    return -1;
}

/* bis IRQ 6 kommt (fdc_irq vor dem Befehl loeschen); 0 = ok */
static int wait_irq(unsigned timeout)
{
    unsigned long until = get_ticks() + timeout, now;
    for (;;) {
        irq_off();
        if (fdc_irq)
            break;
        now = get_ticks();
        if (now >= until) {
            enable_ints();
            return -1;
        }
        proc_block(FD_EVENT, (unsigned)(until - now));  /* kehrt mit STI */
    }
    fdc_irq = 0;
    enable_ints();
    return 0;
}

static void sense_int(int *st0, int *cyl)
{
    fdc_out(0x08);
    *st0 = fdc_in();
    *cyl = fdc_in();
}

/* Controller zuruecksetzen (auch nach einem haengenden Befehl, z. B.
   READ ID ohne Diskette: ohne Indeximpuls kommt kein Ende) */
static int ctl_reset(void)
{
    int st0, cyl, i;
    fdc_irq = 0;
    outb(DOR, 0x00);
    outb(DOR, DOR_RUN | sel_drv | (motors << 4));
    if (wait_irq(100))
        return -1;
    for (i = 0; i < 4; i++)                     /* je Laufwerk quittieren */
        sense_int(&st0, &cyl);
    cur_rate = 0xFF;
    cur_perp = 0;
    fdc_out(0x03);                              /* SPECIFY                */
    fdc_out(0xDF);                              /* SRT 3 ms, HUT 240 ms   */
    fdc_out(0x02);                              /* HLT 4 ms, DMA-Modus    */
    fd[0].pcyl = fd[1].pcyl = 0xFF;
    return 0;
}

static void dor(void)
{
    outb(DOR, DOR_RUN | sel_drv | (motors << 4));
}

/* Laufwerk waehlen, Motor an (Anlauf 300 ms schlafend), Datenrate */
static void select(int d)
{
    const struct ffmt *f = F(d);
    busy = 1;
    last_use = get_ticks();
    sel_drv = (unsigned char)d;
    if (!(motors & (1 << d))) {
        motors |= (unsigned char)(1 << d);
        dor();
        irq_off();
        proc_block(SLEEP_EVENT, 30);            /* andere laufen weiter  */
    } else
        dor();
    if (f->rate != cur_rate) {
        outb(CCR, f->rate);
        cur_rate = f->rate;
    }
    if (f->perp != cur_perp) {                  /* 82077: ED braucht     */
        fdc_out(0x12);                          /* PERPENDICULAR MODE    */
        fdc_out(f->perp ? 0x03 : 0x00);         /* GAP + WGATE (1 Mbit/s) */
        cur_perp = f->perp;
    }
}

static void done(void)
{
    last_use = get_ticks();
    busy = 0;
}

void fd_motor_off(void)
{
    motors = 0;
    dor();
}

/* aus dem Scheduler: Motoren nach 3 s ohne Zugriff aus */
void fd_tick(void)
{
    if (motors && !busy && get_ticks() - last_use > 300)
        fd_motor_off();
}

static int recalibrate(int d)
{
    int st0, cyl, n;
    fd[d].pcyl = 0xFF;
    for (n = 0; n < 2; n++) {                   /* bis zu 80 Spuren: 2x */
        fdc_irq = 0;
        fdc_out(0x07);
        fdc_out((unsigned char)d);
        if (wait_irq(300))
            return -1;
        sense_int(&st0, &cyl);
        if (cyl == 0) {
            fd[d].pcyl = 0;
            return 0;
        }
    }
    return -1;
}

/* Kopf auf den (logischen) Zylinder; Doppelschritt bei 360 KB im 1,2 MB */
static int seek(int d, unsigned cyl, unsigned head)
{
    int st0, c;
    unsigned p = cyl << F(d)->dstep;
    if (fd[d].pcyl == p)
        return 0;
    fdc_irq = 0;
    fdc_out(0x0F);
    fdc_out((unsigned char)((head << 2) | d));
    fdc_out((unsigned char)p);
    if (wait_irq(300))
        return -1;
    sense_int(&st0, &c);
    if (c != (int)p) {
        fd[d].pcyl = 0xFF;
        return -1;
    }
    fd[d].pcyl = (unsigned char)p;
    return 0;
}

static void dma_setup(unsigned long a, unsigned bytes, int write)
{
    unsigned n = bytes - 1;
    outb(0x0A, 0x06);                           /* Kanal 2 maskieren      */
    outb(0x0C, 0x00);                           /* Flip-Flop zuruecksetzen */
    outb(0x0B, write ? 0x4A : 0x46);            /* Einzel, RAM->FDC bzw. FDC->RAM, Kanal 2 */
    outb(0x04, (unsigned)a & 0xFF);
    outb(0x04, ((unsigned)a >> 8) & 0xFF);
    outb(0x81, (unsigned)(a >> 16) & 0xFF);     /* Seitenregister         */
    outb(0x0C, 0x00);
    outb(0x05, n & 0xFF);
    outb(0x05, n >> 8);
    outb(0x0A, 0x02);                           /* Kanal 2 freigeben      */
}

#define WP_ERR (-2)                             /* schreibgeschuetzt      */

/* Ergebnisphase (7 Bytes) auswerten */
static int result(void)
{
    int r[7], i;
    for (i = 0; i < 7; i++)
        r[i] = fdc_in();
    if ((r[0] & 0xC0) && (r[1] & 0x02))
        return WP_ERR;                          /* ST1.NW                 */
    return (r[0] & 0xC0) ? -1 : 0;
}

/* Sektoren s..e (1-basiert) einer Seite zwischen Spurpuffer und Diskette */
static int xfer_once(int d, unsigned cyl, unsigned head, unsigned s, unsigned e,
                     int write)
{
    const struct ffmt *f = F(d);
    if (seek(d, cyl, head))
        return -1;
    dma_setup(tb_lin + ((unsigned long)(s - 1) << 9), (e - s + 1) << 9, write);
    fdc_irq = 0;
    fdc_out(write ? 0x45 : 0x46);               /* WRITE / READ DATA, MFM */
    fdc_out((unsigned char)((head << 2) | d));
    fdc_out((unsigned char)cyl);
    fdc_out((unsigned char)head);
    fdc_out((unsigned char)s);
    fdc_out(2);                                 /* 512 Bytes              */
    fdc_out((unsigned char)e);                  /* letzter Sektor         */
    fdc_out(f->gap);
    fdc_out(0xFF);
    if (wait_irq(f->spt > 18 ? 150 : 300)) {
        ctl_reset();
        return -1;
    }
    return result();
}

static int xfer(int d, unsigned cyl, unsigned head, unsigned s, unsigned e, int write)
{
    int tries, rc = -1;
    for (tries = 0; tries < 3; tries++) {
        rc = xfer_once(d, cyl, head, s, e, write);
        if (rc == 0 || rc == WP_ERR)
            return rc;
        recalibrate(d);
    }
    return rc;
}

/* Geaendertes aus dem Spurpuffer auf die Diskette; 0 = ok, -1 Fehler,
   -2 schreibgeschuetzt. Im Fehlerfall ist der Puffer verworfen. */
int fd_sync(void)
{
    unsigned s, e, spt;
    int d = tb_drv, rc = 0;

    if (d < 0 || !tb_ndirty)
        return 0;
    select(d);
    spt = F(d)->spt;
    for (s = 0; s < spt && rc == 0; s = e) {
        if (!tb_dirty[s]) {
            e = s + 1;
            continue;
        }
        for (e = s; e < spt && tb_dirty[e]; e++)
            ;
        rc = xfer(d, tb_cyl, tb_head, s + 1, e, 1);
    }
    for (s = 0; s < MAXSPT; s++)
        tb_dirty[s] = 0;
    tb_ndirty = 0;
    if (rc)
        tb_drv = -1;                            /* Inhalt stimmt nicht    */
    done();
    return rc;
}

/* Spurpuffer auf (d, cyl, head) umstellen; load = 1: Spur lesen */
static int tb_track(int d, unsigned cyl, unsigned head)
{
    unsigned i;
    int rc;
    if (tb_drv == d && tb_cyl == cyl && tb_head == head)
        return 0;
    if ((rc = fd_sync()) != 0)
        return rc;
    tb_drv = (signed char)d;
    tb_cyl = (unsigned char)cyl;
    tb_head = (unsigned char)head;
    for (i = 0; i < MAXSPT; i++)
        tb_valid[i] = 0;
    return 0;
}

static int split(int d, unsigned lba, unsigned *cyl, unsigned *head, unsigned *sec)
{
    unsigned spt = F(d)->spt, t;
    if (d > 1 || !fd[d].type || lba >= F(d)->blocks)
        return -1;
    t = lba / spt;
    *sec = lba % spt;
    *head = t & 1;
    *cyl = t >> 1;
    return 0;
}

/* einen Sektor in den DMA-Puffer (DMABUF, Offset 0); 0 = ok */
int fd_read(int d, unsigned lba)
{
    unsigned cyl, head, sec, spt, i;
    int rc = 0;

    if (split(d, lba, &cyl, &head, &sec) || tb_track(d, cyl, head))
        return -1;
    if (!tb_valid[sec]) {
        if (tb_ndirty && fd_sync())             /* Spur lesen ueberschriebe */
            return -1;                          /* Geaendertes             */
        select(d);
        spt = F(d)->spt;
        if (xfer(d, cyl, head, 1, spt, 0) == 0) {
            for (i = 0; i < spt; i++)           /* ganze Spur             */
                tb_valid[i] = 1;
        } else if (xfer(d, cyl, head, sec + 1, sec + 1, 0) == 0)
            tb_valid[sec] = 1;                  /* Spur mit Fehler: einzeln */
        else
            rc = -1;
        done();
    }
    if (rc == 0)
        far_copy(DMABUF, 0, tb_sel, sec << 9, 512);
    return rc;
}

/* einen Sektor aus DMABUF schreiben (in den Spurpuffer, fd_sync bringt
   ihn auf die Diskette) */
int fd_write(int d, unsigned lba)
{
    unsigned cyl, head, sec;
    if (split(d, lba, &cyl, &head, &sec) || tb_track(d, cyl, head))
        return -1;
    far_copy(tb_sel, sec << 9, DMABUF, 0, 512);
    tb_valid[sec] = 1;
    if (!tb_dirty[sec]) {
        tb_dirty[sec] = 1;
        tb_ndirty++;
    }
    return 0;
}

static void set_fmt(int d, unsigned fi)
{
    if (fd[d].fi != fi)
        fd[d].pcyl = 0xFF;                      /* Doppelschritt anders   */
    fd[d].fi = (unsigned char)fi;
}

/* Format bestimmen: READ ID mit jeder moeglichen Datenrate */
static int detect(int d)
{
    const signed char *c = cand[fd[d].type];
    int i;

    for (i = 0; c[i] >= 0; i++) {
        set_fmt(d, (unsigned)c[i]);
        select(d);
        if (recalibrate(d))
            break;
        fdc_irq = 0;
        fdc_out(0x4A);                          /* READ ID, MFM           */
        fdc_out((unsigned char)d);
        if (wait_irq(60)) {                     /* kein Index: keine Diskette */
            ctl_reset();
            break;
        }
        /* Rate passt; dazu muss der letzte Sektor der Spur lesbar sein
           (unterscheidet 1,44 MB von 720 KB auch, wo READ ID die Rate
           nicht prueft, etwa in Emulatoren) */
        if (result() == 0 &&
            xfer_once(d, 0, 0, fmts[(unsigned)c[i]].spt, fmts[(unsigned)c[i]].spt, 0) == 0) {
            fd[d].known = 1;
            return 0;
        }
    }
    set_fmt(d, (unsigned)c[0]);                 /* unbekannt: eigenes Format */
    fd[d].known = 0;
    return -1;
}

/* Vor einem Zugriff: gewechselt oder unbekannt? Dann neu erkennen.
   -> 0 unveraendert, 1 neu (Caches verwerfen), -1 kein Laufwerk */
int fd_check(int d)
{
    int ch;
    if (d > 1 || !fd[d].type)
        return -1;
    select(d);
    ch = fd[d].type == 1 || !fd[d].known || (inb(DIR) & 0x80);
    if (!ch) {
        done();
        return 0;
    }
    if (tb_drv == d) {                          /* Puffer gehoert der alten */
        tb_drv = -1;
        tb_ndirty = 0;
    }
    if (tb_ndirty)                              /* detect liest in den   */
        fd_sync();                              /* Puffer: B: zuerst raus */
    tb_drv = -1;
    if (fd[d].type != 1 && (inb(DIR) & 0x80)) { /* Leitung zuruecksetzen:  */
        recalibrate(d);                         /* Schritt bei eingelegter */
        fd[d].pcyl = 0xFF;                      /* Diskette               */
        fdc_irq = 0;
        fdc_out(0x0F);
        fdc_out((unsigned char)d);
        fdc_out(1);
        wait_irq(300);
        {
            int st0, c;
            sense_int(&st0, &c);
        }
    }
    detect(d);
    done();
    return 1;
}

unsigned fd_size(int d)
{
    return d <= 1 && fd[d].type ? F(d)->blocks : 0;
}

int fd_type(int d)
{
    return d <= 1 ? fd[d].type : 0;
}

/* Spur formatieren (FAPI DosFmtTrack): Format nach Groesse 'blocks', muss
   zum Laufwerk passen. 0 = ok, -1 Fehler, -2 schreibgeschuetzt, -3 Format */
int fd_format(int d, unsigned cyl, unsigned head, unsigned blocks)
{
    const signed char *c;
    unsigned char __far *t = MK_FP(DMABUF, 0);
    const struct ffmt *f;
    int i, rc, tries;

    if (d > 1 || !fd[d].type || head > 1)
        return -3;
    c = cand[fd[d].type];
    for (i = 0; c[i] >= 0 && fmts[(unsigned)c[i]].blocks != blocks; i++)
        ;
    if (c[i] < 0)
        return -3;
    f = &fmts[(unsigned)c[i]];
    if (cyl >= f->cyls)
        return -3;
    if ((rc = fd_sync()) != 0)
        return rc;
    if (tb_drv == d)
        tb_drv = -1;
    set_fmt(d, (unsigned)c[i]);
    fd[d].known = 1;
    select(d);
    for (tries = 0; tries < 3; tries++) {
        for (i = 0; i < f->spt; i++) {          /* C H R N je Sektor      */
            t[i * 4] = (unsigned char)cyl;
            t[i * 4 + 1] = (unsigned char)head;
            t[i * 4 + 2] = (unsigned char)(i + 1);
            t[i * 4 + 3] = 2;
        }
        if (seek(d, cyl, head)) {
            recalibrate(d);
            continue;
        }
        dma_setup(DMABUF_LIN, f->spt * 4, 1);
        fdc_irq = 0;
        fdc_out(0x4D);                          /* FORMAT TRACK, MFM      */
        fdc_out((unsigned char)((head << 2) | d));
        fdc_out(2);
        fdc_out(f->spt);
        fdc_out(f->fgap);
        fdc_out(0xF6);                          /* Fuellbyte              */
        rc = wait_irq(300) ? -1 : result();
        if (rc == 0 || rc == WP_ERR)
            break;
        recalibrate(d);
    }
    done();
    return rc;
}

/* types = CMOS-Register 10h; -> 0 = Controller da */
int fd_init(unsigned types)
{
    int d;

    fd[0].type = (unsigned char)(types >> 4);
    fd[1].type = (unsigned char)(types & 15);
    for (d = 0; d < 2; d++) {
        if (fd[d].type == 6)
            fd[d].type = 5;                     /* manche BIOS: 2,88 = 6  */
        if (fd[d].type > 5)
            fd[d].type = 0;
        fd[d].fi = fd[d].type ? (unsigned char)cand[fd[d].type][0] : 4;
        fd[d].known = 0;
        fd[d].pcyl = 0xFF;
    }
    if (!tb_sel)
        tb_sel = kseg_dma(MAXSPT * 512U, &tb_lin);
    if (!tb_sel)
        return -1;
    motors = 0;
    sel_drv = 0;
    if (ctl_reset())
        return -1;
    for (d = 0; d < 2; d++)
        if (fd[d].type) {
            select(d);
            if (recalibrate(d) && d == 0) {
                done();
                return -1;
            }
            done();
        }
    return 0;
}
