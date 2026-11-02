/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IDE.SYS - Treiber fuer IDE-Platten (ATA) an beiden Kanaelen
 *
 * Primaerer Kanal 1F0h/3F6h, sekundaerer 170h/376h, je Master und Slave:
 * bis zu vier Geraete, Einheiten in dieser Reihenfolge. ATAPI-CD-ROMs
 * sind Einheiten mit 2048-Byte-Bloecken (PACKET + READ(10), nur lesend);
 * ein Dateisystem bekommen sie ueber IFS=ISO9660.IFS.
 *
 * PIO-Modus mit Interrupt: IRQ 14 (primaer) und 15 (sekundaer) per DevHlp
 * SetIRQ. Waehrend die Platte arbeitet, schlaeft der Task (ProcBlock), und
 * andere laufen weiter; die Interruptroutine weckt ihn (ProcRun). Der
 * Status wird danach trotzdem geprueft - der Interrupt beschleunigt nur.
 * Bleibt er aus, obwohl das Geraet fertig ist (Frist 2 Takte), zaehlt das
 * als Fehltreffer; nach dreien fragt der Kanal wieder nur ab wie frueher.
 * /NOIRQ in der CONFIG.SYS schaltet die Interrupts ab. LBA28, wenn die Platte es kann, sonst CHS mit der
 * aktuellen Geometrie der Platte (die das BIOS beim Start eingestellt hat) -
 * Platten aus der 286-Zeit kennen oft kein LBA.
 *
 * Erkennung liest zuerst: Ein unbelegter Kanal liefert FFh (oder 7Fh) im
 * Status, erst danach wird ein Laufwerk ausgewaehlt und IDENTIFY gesendet.
 *
 * Aeltere Controller mit denselben Registern: Der WD1003 (MFM/RLL) kennt
 * kein IDENTIFY und bricht es ab (ERR, Fehlerregister ABRT). Die Geometrie
 * steht dann nur in der Tabelle des BIOS (INT 41h/46h, vom Kernel beim
 * Start gesichert, DH_BIOSDISK) - nur am primaeren Kanal, nur fuer die
 * Platten, die das BIOS kennt. Sie wird mit zwei Lesezugriffen geprueft
 * (erster und letzter Sektor der ersten Spur). Der WD1007 (ESDI) liefert
 * IDENTIFY, aber mit der Rohgeometrie; das BIOS stellt oft eine kleinere
 * ein (eine Spur oder einen Sektor weniger). Bei CHS ohne gueltige
 * "aktuelle Geometrie" (Wort 53) gilt deshalb die des BIOS, wenn sie in die
 * Platte passt. Die gewaehlte Geometrie wird mit INITIALIZE DRIVE
 * PARAMETERS (91h) eingestellt, damit Treiber und Controller sie teilen.
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
    unsigned char  ch;                  /* Kanal 0/1                       */
    unsigned char  sel;                 /* 00h Master, 10h Slave           */
    unsigned char  chs;
    unsigned char  atapi;               /* CD-ROM: Paketbefehle            */
    unsigned char  old;                 /* WD1003: kein IDENTIFY, kein FLUSH */
    unsigned       cyls, heads, spt;
};

static struct unit unit[NUNIT];
static int nunit;
static unsigned short id[256];
static char line[128];                  /* Zeile fuers Startprotokoll   */
static const unsigned ports[2][2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };
static const unsigned char irqs[2] = { 14, 15 };

/* Interrupt je Kanal */
#define IRQ_WAIT    2                   /* Takte bis zum Blick auf den Status */
static struct chan {
    unsigned char  on;                  /* Interrupt benutzen              */
    unsigned char  miss;                /* Fehltreffer in Folge            */
    unsigned char  units;               /* Einheiten an diesem Kanal       */
    volatile unsigned char done;        /* Interrupt kam                   */
} chan[2];

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

/* warten, bis BSY weg ist - vor einem neuen Befehl. Ein gesetztes ERR
   stammt vom vorigen Befehl (z. B. FLUSH CACHE, den alte Platten
   abbrechen) und bleibt stehen, bis der naechste kommt: hier egal. */
static int wait_idle(unsigned base, unsigned t)
{
    unsigned start = ticks();
    while (inp(base + R_STATUS) & ST_BSY)
        if ((unsigned)(ticks() - start) > t)
            return -1;
    return 0;
}

/* Interruptroutine: Status lesen quittiert den Interrupt am Laufwerk */
static void ide_irq(unsigned n)
{
    int ch = n == irqs[1];
    inp(ports[ch][0] + R_STATUS);
    chan[ch].done = 1;
    proc_run(ch + 1);
}

/* wie wait_status, aber der Task schlaeft bis zum Interrupt des Kanals.
   Vorher (bei ausgegebenem Befehl bzw. vor dem Datentransfer) muss
   chan[].done geloescht sein. */
static int wait_irq(struct unit *u, unsigned char mask, unsigned char want,
                    unsigned t)
{
    struct chan *c = &chan[u->ch];
    unsigned start = ticks(), r;
    unsigned char s;

    while (c->on) {
        s = inp(u->base + R_STATUS);
        if (!(s & ST_BSY) && ((s & ST_ERR) || (s & mask) == want))
            break;
        if ((unsigned)(ticks() - start) > t)
            return -1;
        cli();
        if (c->done) {                  /* kam schon: nicht schlafen    */
            sti();
            r = 0;
        } else
            r = proc_block(u->ch + 1, IRQ_WAIT);
        if (r == 0)
            c->miss = 0;
        else {                          /* Frist um: fertig ohne Interrupt? */
            s = inp(u->base + R_STATUS);
            if (!c->done && !(s & ST_BSY) && ((s & ST_ERR) || (s & mask) == want) &&
                ++c->miss >= 3) {
                c->on = 0;              /* kommt nicht: wieder abfragen */
                unset_irq(irqs[u->ch]);
                dputs(u->ch ? "IDE.SYS: IRQ 15 bleibt aus, Abfragebetrieb\n"
                            : "IDE.SYS: IRQ 14 bleibt aus, Abfragebetrieb\n");
            }
        }
        c->done = 0;
    }
    return wait_status(u->base, mask, want, t);
}

/* Modellname (Bytes vertauscht) an p anhaengen */
static char *model(char *p)
{
    char *q = p;
    int i;
    for (i = 27; i < 47; i++) {
        *p++ = (char)(id[i] >> 8);
        *p++ = (char)id[i];
    }
    while (p > q && (p[-1] == ' ' || p[-1] == 0))
        p--;
    *p = 0;
    if (p == q)                             /* ESDI: oft ohne Namen     */
        p = scat(p, "Platte ohne Namen");
    return p;
}

static char *where(int ch, int dev)
{
    return scat(line, ch == 0 ? (dev ? "  IDE prim\x84r Slave: " : "  IDE prim\x84r Master: ")
                              : (dev ? "  IDE sekund\x84r Slave: " : "  IDE sekund\x84r Master: "));
}

/* Sektoren von cyls x heads x spt (ohne __U4M) */
static unsigned long chs_size(unsigned cyls, unsigned heads, unsigned spt)
{
    unsigned long n = 0, t = 0;
    unsigned i;
    for (i = 0; i < spt; i++)
        t += heads;
    for (i = 0; i < cyls; i++)
        n += t;
    return n;
}

/* Befehl fuer n Sektoren ab l ausgeben (Register setzen, Befehl schreiben);
   0 = ok, -1 = Laufwerk belegt oder Adresse ausserhalb */
static int issue(struct unit *u, unsigned long l, unsigned n, unsigned char cmd)
{
    static unsigned sec, head;              /* static: SS ist nicht DS (-zu) */
    unsigned long track;
    unsigned cyl = 0, b = u->base;

    if (u->chs) {
        track = div32(l, u->spt, &sec);
        cyl = (unsigned)div32(track, u->heads, &head);
        if (cyl >= u->cyls)
            return -1;
    }
    outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel));
    settle(u->ctl);
    if (wait_idle(b, 300))
        return -1;
    if (u->chs) {
        outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel | head));
        outp(b + R_COUNT, (unsigned char)n);
        outp(b + R_SECTOR, (unsigned char)(sec + 1));   /* ab 1      */
        outp(b + R_CYL_LO, (unsigned char)cyl);
        outp(b + R_CYL_HI, (unsigned char)(cyl >> 8));
    } else {
        outp(b + R_DRIVE, (unsigned char)(0xE0 | u->sel |
                                           ((unsigned)(l >> 24) & 0x0F)));
        outp(b + R_COUNT, (unsigned char)n);
        outp(b + R_SECTOR, (unsigned char)l);
        outp(b + R_CYL_LO, (unsigned char)(l >> 8));
        outp(b + R_CYL_HI, (unsigned char)(l >> 16));
    }
    settle(u->ctl);
    chan[u->ch].done = 0;
    outp(b + R_CMD, cmd);
    settle(u->ctl);
    return 0;
}

/* einen Sektor nach id[] lesen (Pruefung der Geometrie); 0 = ok */
static int test_read(struct unit *u, unsigned long l)
{
    if (issue(u, l, 1, 0x20) || wait_status(u->base, ST_DRQ, ST_DRQ, 300))
        return -1;
    insw_far(u->base + R_DATA, (void __far *)id, 256);
    return 0;
}

/* INITIALIZE DRIVE PARAMETERS: Koepfe und Sektoren je Spur einstellen */
static int set_geometry(struct unit *u)
{
    unsigned b = u->base;
    outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel));
    settle(u->ctl);
    if (wait_idle(b, 300))
        return -1;
    outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel | ((u->heads - 1) & 0x0F)));
    outp(b + R_COUNT, (unsigned char)u->spt);
    settle(u->ctl);
    outp(b + R_CMD, 0x91);
    settle(u->ctl);
    return wait_status(b, 0, 0, 300);
}

/* Geometrie aus der BIOS-Tabelle (INT 41h/46h) der Platte dev am
   primaeren Kanal; 0 = gueltig. Uebersetzende BIOS (Signatur A0h im
   Byte 3) legen dort die echte Geometrie ab. */
static unsigned char fdpt[16];

static int bios_geometry(int ch, int dev, unsigned *cyls, unsigned *heads,
                         unsigned *spt)
{
    if (ch != 0 || devhlp(DH_BIOSDISK, 0, (unsigned)dev, (unsigned)fdpt))
        return -1;
    if (fdpt[3] == 0xA0) {
        *cyls = fdpt[9] | (fdpt[10] << 8);
        *heads = fdpt[11];
        *spt = fdpt[4];
    } else {
        *cyls = fdpt[0] | (fdpt[1] << 8);
        *heads = fdpt[2];
        *spt = fdpt[14];
    }
    if (*cyls == 0 || *cyls > 4096 || *heads == 0 || *heads > 16 ||
        *spt == 0 || *spt > 63)
        return -1;
    return 0;
}

/* Platte ohne IDENTIFY (WD1003, MFM/RLL) */
static void old_disk(int ch, int dev)
{
    struct drv_req __far *r = REQ;
    struct unit *u;
    unsigned long n;
    char *p;

    if (nunit == NUNIT)
        return;
    u = &unit[nunit];
    p = where(ch, dev);
    if (bios_geometry(ch, dev, &u->cyls, &u->heads, &u->spt)) {
        scat(p, "kein IDENTIFY, Geometrie unbekannt\n");
        dputs(line);
        return;
    }
    u->base = ports[ch][0];
    u->ctl = ports[ch][1];
    u->ch = (unsigned char)ch;
    u->sel = (unsigned char)(dev << 4);
    u->chs = 1;
    u->atapi = 0;
    u->old = 1;
    /* Geometrie pruefen: Sektor 1 und letzter Sektor der ersten Spur
       (falsche Kopf- oder Sektorzahl -> Fehler) - nur lesen */
    if (set_geometry(u) || test_read(u, 0) ||
        test_read(u, (unsigned long)(u->heads * u->spt - 1))) {
        scat(p, "kein IDENTIFY, BIOS-Geometrie passt nicht\n");
        dputs(line);
        return;
    }
    n = chs_size(u->cyls, u->heads, u->spt);
    r->blocks[nunit] = n;
    nunit++;
    chan[ch].units++;
    /* echte WD1003-Platten: 17 Sektoren je Spur (MFM) oder 26 (RLL);
       mehr gibt es nur im Emulator */
    p = scat(p, u->spt <= 17 ? "MFM-Platte" : u->spt <= 26 ? "RLL-Platte" : "MFM/RLL-Platte");
    p = scat(p, " (WD1003), ");
    p = udec(p, (unsigned)(n >> 11));
    p = scat(p, " MB, CHS ");
    p = udec(p, u->cyls);
    p = scat(p, "/");
    p = udec(p, u->heads);
    p = scat(p, "/");
    p = udec(p, u->spt);
    scat(p, "\n");
    dputs(line);
}

/* ein Geraet pruefen; meldet Platten als Einheit, ATAPI nur im Protokoll */
static void probe(int ch, int dev)
{
    struct drv_req __far *r = REQ;
    unsigned base = ports[ch][0], ctl = ports[ch][1];
    struct unit *u;
    unsigned long n;
    unsigned char s, sig1, sig2;
    static unsigned c, h, sp;               /* static: SS ist nicht DS (-zu) */
    char *p;
    int bios;

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
        if (sig1 != 0x14 || sig2 != 0xEB) {
            /* WD1003: IDENTIFY sauber abgebrochen -> Geometrie vom BIOS */
            if ((inp(base + R_STATUS) & (ST_BSY | ST_ERR)) == ST_ERR &&
                (inp(base + R_ERROR) & 0x04))
                old_disk(ch, dev);
            return;
        }
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
        u->ch = (unsigned char)ch;
        u->sel = (unsigned char)(dev << 4);
        u->chs = 0;
        u->atapi = 1;
        r->blocks[nunit] = 0xFFFFFFFFUL;    /* Groesse haengt an der CD */
        r->bsize[nunit] = 2048;
        nunit++;
        chan[ch].units++;
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
    u->ch = (unsigned char)ch;
    u->sel = (unsigned char)(dev << 4);
    u->chs = 0;
    u->atapi = 0;
    u->old = 0;
    bios = 0;
    n = id[60] | ((unsigned long)id[61] << 16);     /* LBA-Sektoren */
    if (!(id[49] & 0x0200) || n == 0) {
        /* kein LBA: aktuelle Geometrie (Wort 54-56); sonst die des BIOS,
           wenn sie in die Platte passt (ESDI: BIOS stellt weniger ein),
           sonst die Grundgeometrie - dann selbst einstellen */
        u->chs = 1;
        if ((id[53] & 1) && id[54] && id[55] && id[56]) {
            u->cyls = id[54]; u->heads = id[55]; u->spt = id[56];
        } else {
            u->cyls = id[1]; u->heads = id[3]; u->spt = id[6];
            if (bios_geometry(ch, dev, &c, &h, &sp) == 0 && h <= 16 &&
                chs_size(c, h, sp) <= chs_size(u->cyls, u->heads, u->spt)) {
                u->cyls = c; u->heads = h; u->spt = sp;
                bios = 1;
            }
            if (u->cyls == 0 || u->heads == 0 || u->heads > 16 ||
                u->spt == 0 || u->spt > 255 || set_geometry(u))
                return;
        }
        if (u->cyls == 0 || u->heads == 0 || u->heads > 16 ||
            u->spt == 0 || u->spt > 255)
            return;
        n = chs_size(u->cyls, u->heads, u->spt);
    }
    r->blocks[nunit] = n;
    nunit++;
    chan[ch].units++;

    p = where(ch, dev);
    p = model(p);
    p = scat(p, ", ");
    p = udec(p, (unsigned)(n >> 11));
    scat(p, !u->chs ? " MB\n" : bios ? " MB (CHS, BIOS)\n" : " MB (CHS)\n");
    dputs(line);
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char s;
    int ch;
    char *p;

    nunit = 0;
    for (ch = 0; ch < 2; ch++) {
        s = inp(ports[ch][0] + R_STATUS);
        if (s == 0xFF || s == 0x7F)         /* Kanal nicht vorhanden    */
            continue;
        probe(ch, 0);
        probe(ch, 1);
    }
    /* Interrupts fuer Kanaele mit Geraeten. Steuerregister: nIEN = 0
       (Interrupt an), Bit 3 gesetzt wie beim AT-BIOS (WD1003: mehr als
       8 Koepfe) */
    for (ch = 0; ch < 2; ch++) {
        chan[ch].on = 0;
        if (!chan[ch].units || has_opt(r->args, "/NOIRQ"))
            continue;
        outp(ports[ch][1], 0x08);
        chan[ch].done = 0;
        p = scat(line, ch ? "  IDE sekund\x84r: IRQ " : "  IDE prim\x84r: IRQ ");
        p = udec(p, irqs[ch]);
        if (set_irq(irqs[ch], ide_irq) == 0) {
            chan[ch].on = 1;
            scat(p, "\n");
        } else
            scat(p, " belegt, Abfragebetrieb\n");
        dputs(line);
    }
    r->units = (unsigned char)nunit;
    r->count = 16;                          /* READ SECTORS mit Sektorzahl */
    r->status = nunit ? DRV_OK : DRV_ERR_NODEV;
}

/* ATAPI: Paketbefehl in pkt[] schicken und len Bytes nach sel:off lesen
   (READ(10): 2048, REQUEST SENSE: 18); 0 = ok */
static unsigned char pkt[12];

static int atapi_pkt(struct unit *u, unsigned len, unsigned sel, unsigned off)
{
    void __far *pp = (void __far *)pkt;
    unsigned b = u->base, got = 0, cnt, start;

    outp(b + R_DRIVE, (unsigned char)(0xA0 | u->sel));
    settle(u->ctl);
    start = ticks();
    while (inp(b + R_STATUS) & ST_BSY)
        if ((unsigned)(ticks() - start) > 300)
            return -1;
    outp(b + R_ERROR, 0);                   /* Features: PIO            */
    outp(b + R_CYL_LO, (unsigned char)len);     /* hoechstens len Bytes */
    outp(b + R_CYL_HI, (unsigned char)(len >> 8));  /* je DRQ-Block     */
    outp(b + R_CMD, 0xA0);                  /* PACKET                   */
    settle(u->ctl);
    if (wait_status(b, ST_DRQ, ST_DRQ, 300))
        return -1;
    chan[u->ch].done = 0;
    outsw_far(b + R_DATA, FP_SEG(pp), FP_OFF(pp), 6);
    while (got < len) {
        settle(u->ctl);
        if (wait_irq(u, ST_DRQ, ST_DRQ, 1000))      /* CD laeuft evtl. an */
            return -1;
        cnt = inp(b + R_CYL_LO) | ((unsigned)inp(b + R_CYL_HI) << 8);
        if (cnt == 0 || (cnt & 1) || got + cnt > len)
            return -1;
        chan[u->ch].done = 0;
        insw_far(b + R_DATA, MK_FP(sel, off + got), cnt >> 1);
        got += cnt;
    }
    settle(u->ctl);
    return wait_irq(u, 0, 0, 300);
}

static void pkt_clear(unsigned char op)
{
    int i;
    for (i = 0; i < 12; i++)
        pkt[i] = 0;
    pkt[0] = op;
}

/* einen 2048-Byte-Block mit READ(10) lesen; 0 = ok */
static int atapi_read(struct unit *u, unsigned long l, unsigned sel, unsigned off)
{
    pkt_clear(0x28);                        /* READ(10)                 */
    pkt[2] = (unsigned char)(l >> 24);
    pkt[3] = (unsigned char)(l >> 16);
    pkt[4] = (unsigned char)(l >> 8);
    pkt[5] = (unsigned char)l;
    pkt[8] = 1;                             /* ein Block                */
    return atapi_pkt(u, 2048, sel, off);
}

/* REQUEST SENSE nach einem Fehler: Sense Key << 8 | ASC, -1 = keine Antwort */
static unsigned char sense[18];

static int atapi_sense(struct unit *u)
{
    void __far *sp = (void __far *)sense;
    pkt_clear(0x03);
    pkt[4] = sizeof(sense);
    if (atapi_pkt(u, sizeof(sense), FP_SEG(sp), FP_OFF(sp)))
        return -1;
    return ((sense[2] & 0x0F) << 8) | sense[12];
}

/* gescheiterten Versuch ins Startprotokoll (Sitzung 1) melden: Schritt
   (1 Befehl, 2 Daten, 3 Ende), Status- und Fehlerregister. Kommt nur bei
   Fehlern vor - hilft, Zeitprobleme auf echter Hardware zu finden. */
static void report(struct unit *u, int write, unsigned long l, int step, int t)
{
    unsigned char st = inp(u->ctl), er = inp(u->base + R_ERROR);
    char *p = scat(line, write ? "IDE.SYS: Schreibfehler, Einheit " : "IDE.SYS: Lesefehler, Einheit ");
    p = udec(p, (unsigned)(u - unit));
    p = scat(p, ", Sektor ");
    p = hex(p, l, 8);
    p = scat(p, "h, Schritt ");
    p = udec(p, (unsigned)step);
    p = scat(p, ", Status ");
    p = hex(p, st, 2);
    p = scat(p, "h, Fehler ");
    p = hex(p, er, 2);
    p = scat(p, "h, Versuch ");
    p = udec(p, (unsigned)t + 1);
    scat(p, chan[u->ch].on ? ", IRQ\n" : ", Abfrage\n");
    dputs(line);
}

/* count Sektoren (1-16) lesen oder einen schreiben */
static void xfer(int write)
{
    struct drv_req __far *r = REQ;
    struct unit *u = &unit[r->unit];
    unsigned long l = r->lba;
    unsigned b = u->base, n, i;
    int tries;

    n = write || r->count == 0 ? 1 : r->count;
    if (n > 16)
        n = 16;

    if (u->atapi) {
        if (write) {
            r->status = DRV_ERR_WP;
            return;
        }
        /* Nach einem Fehler fragt REQUEST SENSE nach dem Grund: Unit
           Attention (6, Medium gewechselt) - gleich nochmal; nicht bereit
           (2) mit ASC 3Ah - keine CD drin, nach einem zweiten Versuch
           melden (manche Laufwerke, auch QEMU, melden das nach einem
           Wechsel einmal, damit das System ihn bemerkt); ASC 04h - CD
           laeuft an, je halbe Sekunde nochmal (bis 10 s) */
        for (tries = 0; tries < 24; tries++) {
            int sk;
            if (atapi_read(u, l, r->buf_sel, r->buf_off) == 0) {
                r->status = DRV_OK;
                return;
            }
            sk = atapi_sense(u);
            if (sk == 0x23A && tries > 0) {
                r->status = DRV_ERR_NOTREADY;
                return;
            }
            if (sk == 0x204 && tries < 22) {
                cli();
                proc_block(0x7F, 50);   /* wird gerade bereit       */
                continue;
            }
            if ((sk & 0xFF00) != 0x600 && sk != 0x23A && tries >= 2)
                break;
        }
        r->status = DRV_ERR_IO;
        return;
    }

    for (tries = 0; tries < 3; tries++) {
        if (issue(u, l, n, write ? 0x30 : 0x20)) {  /* WRITE / READ SECTORS */
            report(u, write, l, 1, tries);
            continue;
        }
        if (!write) {
            for (i = 0; i < n; i++) {           /* je Sektor auf DRQ warten */
                if (wait_irq(u, ST_DRQ, ST_DRQ, 300))
                    break;
                chan[u->ch].done = 0;           /* naechster kommt nach dem Lesen */
                insw_far(b + R_DATA, MK_FP(r->buf_sel, r->buf_off + (i << 9)), 256);
            }
            if (i < n) {
                report(u, write, l + i, 2, tries);
                continue;
            }
        } else {
            if (wait_status(b, ST_DRQ, ST_DRQ, 300)) {
                report(u, write, l, 2, tries);
                continue;
            }
            chan[u->ch].done = 0;               /* erster DRQ ohne Interrupt */
            outsw_far(b + R_DATA, r->buf_sel, r->buf_off, 256);
            settle(u->ctl);
            if (wait_irq(u, 0, 0, 300)) {       /* Platte hat geschrieben */
                report(u, write, l, 3, tries);
                continue;
            }
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
        if (unit[i].atapi || unit[i].old)
            continue;
        outp(unit[i].base + R_DRIVE, (unsigned char)(0xA0 | unit[i].sel));
        settle(unit[i].ctl);
        if (wait_idle(unit[i].base, 300))
            continue;
        outp(unit[i].base + R_CMD, 0xE7);
        settle(unit[i].ctl);
        wait_idle(unit[i].base, 3000);      /* ERR danach: kennt FLUSH nicht */
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
