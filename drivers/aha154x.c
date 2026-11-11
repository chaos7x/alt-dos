/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AHA154X.SYS - Adaptec AHA-1540/1542 (ISA, Busmaster), auch BusLogic im
 *               154x-Modus und AHA-174x im Standard-Modus
 *
 * Mailbox-Schnittstelle mit 24-Bit-Adressen: Der Treiber legt Mailboxen
 * und Befehlsbloecke (CCB) im eigenen Datensegment ab und gibt der Karte
 * deren physische Adressen. Die Karte holt sich alles per Busmaster-DMA
 * selbst, auch die Daten. Fertig ist ein Befehl, wenn die Karte die
 * Eingangs-Mailbox beschrieben hat. Darauf wartet der Task schlafend
 * (DevHlp ProcBlock), andere laufen weiter; der Interrupt der Karte (IRQ
 * aus der Konfiguration, Befehl 0Bh) weckt ihn. Bleibt der Interrupt aus,
 * obwohl die Mailbox voll ist (Frist 2 Takte), zaehlt das als Fehltreffer;
 * nach dreien wird wieder abgefragt. /NOIRQ schaltet den Interrupt ab.
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
#define I_INTV      0x80            /* irgendein Interrupt steht an     */

#define CMD_MBINIT      0x01
#define CMD_START       0x02
#define CMD_INQUIRY     0x04            /* Board-ID, Optionen, Firmware */
#define CMD_CONFIG      0x0B
#define CMD_BLMODEL     0x8B            /* nur BusLogic: Modellnummer    */
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
static unsigned bsz[DRV_MAXUNITS];      /* 512 Platte, 2048 CD-ROM      */
static unsigned char rem[DRV_MAXUNITS]; /* Wechselplatte (INQUIRY RMB)  */
static unsigned char chg[DRV_MAXUNITS]; /* Unit Attention gesehen       */
static unsigned nunits;
static char line[128];                  /* 80 war bei IDE.SYS zu knapp  */
static unsigned irq;                    /* 0 = keiner                    */
static unsigned char irq_on, miss;
#define IRQ_WAIT    2                   /* Takte bis zum Blick auf die Mailbox */
#define EV_MBOX     1
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

/* ---- Interrupt -------------------------------------------------------- */
/* Quittieren (IRST), sonst bleibt die ISA-Leitung oben und kein weiterer
   Interrupt kommt an; Abfragen und Interrupt duerfen sich ueberholen. */
static void aha_irq(unsigned n)
{
    (void)n;
    if (inp(INTR(base)) & I_INTV) {
        outp(CTRL(base), C_IRST);
        proc_run(EV_MBOX);
    }
}

/* bis die Karte die Eingangs-Mailbox beschrieben hat; 0 = ok */
static int wait_mbox(unsigned t)
{
    unsigned start = ticks(), r;

    while (mb[1].status == 0) {
        if ((unsigned)(ticks() - start) > t)
            return -1;
        if (!irq_on)
            continue;
        cli();
        if (mb[1].status) {
            sti();
            break;
        }
        r = proc_block(EV_MBOX, IRQ_WAIT);
        if (r == 0)
            miss = 0;
        else if (mb[1].status && ++miss >= 3) {     /* fertig ohne Interrupt */
            irq_on = 0;
            unset_irq(irq);
            dputs("AHA154X.SYS: Interrupt bleibt aus, Abfragebetrieb\n");
        }
    }
    return 0;
}

/* ---- Welche Karte? ------------------------------------------------------
   Die Board-ID (Befehl 04h) unterscheidet Adaptec und BusLogic nicht: der
   AHA-1542B meldet 'A', die BusLogic-Karten im Adaptec-Modus ebenfalls.
   Befehl 8Bh (Modellnummer, ein Laengenbyte) kennt nur BusLogic - ein
   Adaptec lehnt ihn sofort ab (HACC + ungueltig). Dann darf das Laengen-
   byte nicht mehr folgen, sonst gilt es als naechster Befehl. */
static char model[40];

/* ein Byte als Antwort auf Befehl c (ohne Parameter); -1 = keins */
static int ask1(unsigned char c)
{
    int v;
    if (cmd_out(c))
        return -1;
    v = data_in();
    cmd_done();
    return v;
}

static void identify(void)
{
    unsigned start, i;
    int c, id = 0, fw1 = 0, fw2 = 0, bl = 0;
    char *p = model;

    /* BusLogic? */
    if (cmd_out(CMD_BLMODEL) == 0) {
        start = ticks();
        while ((unsigned)(ticks() - start) < 5 && !(inp(INTR(base)) & I_HACC))
            ;
        if (inp(INTR(base)) & I_HACC) {
            outp(CTRL(base), C_IRST);   /* abgelehnt: kein BusLogic      */
        } else if (cmd_out(4) == 0) {
            bl = 1;
            p = scat(model, "BusLogic BT-");
            for (i = 0; i < 4; i++) {
                c = data_in();
                if (c < 0)
                    break;
                if (c > ' ' && c < 127)
                    *p++ = (char)c;
            }
            *p = 0;
            cmd_done();
            /* leer (86Box nimmt das Modell aus dem NVRAM der Karte) */
            if (p == model + 12)
                p = scat(model, "BusLogic BT-54x");
        }
    }
    /* Board-ID und Firmware (Adaptec: Modell aus der Board-ID) */
    if (cmd_out(CMD_INQUIRY) == 0) {
        id = data_in();
        data_in();
        fw1 = data_in();
        fw2 = data_in();
        cmd_done();
    }
    if (bl) {
        if (fw1 >= '0' && fw1 <= '9' && fw2 >= '0' && fw2 <= '9') {
            p = scat(p, ", Firmware ");
            *p++ = (char)fw1;
            *p++ = '.';
            *p++ = (char)fw2;
            c = ask1(0x84);             /* zweite Nebenziffer            */
            if (c >= '0' && c <= '9')
                *p++ = (char)c;
            c = ask1(0x85);             /* Buchstabe, falls vorhanden    */
            if (c > ' ' && c < 127)
                *p++ = (char)c;
            *p = 0;
        }
        return;
    }
    scat(model, id == 'A' ? "Adaptec AHA-1540/1542A/B" :
                id == 'D' ? "Adaptec AHA-1542C" :
                id == 'E' ? "Adaptec AHA-1542CF" :
                id == 'F' ? "Adaptec AHA-1542CP" : "Adaptec 154x");
}

/* ---- SCSI-Befehl ausfuehren -------------------------------------------- */
#define SCSI_OK     0
#define SCSI_NODEV  1               /* Auswahl-Zeitueberschreitung      */
#define SCSI_CHECK  2               /* Check Condition (z. B. Unit Attention) */
#define SCSI_FAIL   3

static unsigned last_sense;             /* Sense Key << 8 | ASC (Check Condition) */

/* dir: 08h Daten zum Rechner (Lesen), 10h zum Geraet (Schreiben) */
static int scsi_dir(unsigned id, unsigned len, unsigned long buf, unsigned cdblen,
                    unsigned char dir)
{
    unsigned i;
    unsigned char st;

    ccb.op = 0;                             /* SCSI-Initiator           */
    ccb.idlun = (unsigned char)((id << 5) | dir);   /* Richtung         */
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

    if (wait_mbox(300))
        return SCSI_FAIL;
    st = mb[1].status;
    mb[1].status = 0;
    outp(CTRL(base), C_IRST);

    if (ccb.hastat == 0x11)
        return SCSI_NODEV;
    if (st == 1 && ccb.hastat == 0 && ccb.tarstat == 0)
        return SCSI_OK;
    if (ccb.tarstat == 2) {
        /* Auto-Sense: der Adapter legt die Daten direkt hinter die CDB
           (Offset 18 + cdblen), nicht an den Anfang von ccb.sense */
        unsigned char *sd = ccb.cdb + cdblen;
        last_sense = ((sd[2] & 0x0F) << 8) | sd[12];
        return SCSI_CHECK;
    }
    return SCSI_FAIL;
}

static int scsi(unsigned id, unsigned len, unsigned long buf, unsigned cdblen)
{
    return scsi_dir(id, len, buf, cdblen, 0x08);
}

static void cdb6(unsigned char op, unsigned char alloc)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        cdb[i] = 0;
    cdb[0] = op;
    cdb[4] = alloc;
}

/* READ CAPACITY -> Sektoren (0 = keine Antwort / nicht 512 Byte) */
static unsigned long capacity(unsigned id, unsigned long phys,
                              unsigned char __far *buf)
{
    unsigned k;
    for (k = 0; k < 12; k++)
        cdb[k] = 0;
    cdb[0] = 0x25;
    if (scsi(id, 8, phys, 10) != SCSI_OK || buf[6] != 2 || buf[7] != 0)
        return 0;
    return (((unsigned long)buf[0] << 24) | ((unsigned long)buf[1] << 16) |
            ((unsigned)buf[2] << 8) | buf[3]) + 1;
}

/* ---- Initialisierung --------------------------------------------------- */
static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    unsigned i, id, host, n = 0;
    int k;
    unsigned char dma, lock, irqs;
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

    identify();

    /* Konfiguration: DMA-Kanal, IRQ, eigene SCSI-ID */
    cmd_out(CMD_CONFIG);
    dma = (unsigned char)data_in();
    irqs = (unsigned char)data_in();        /* Bit 0-3: IRQ 9-12, 5/6: 14/15 */
    host = data_in() & 7;
    cmd_done();
    irq = 0;
    for (k = 0; k < 7 && !irq; k++)
        if (irqs & (1 << k))
            irq = k < 4 ? 9 + k : k == 5 ? 14 : k == 6 ? 15 : 0;

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

    p = scat(line, "  ");
    p = scat(p, model);
    p = scat(p, " an Port ");
    *p++ = (char)('0' + (base >> 8));
    *p++ = (char)("0123456789ABCDEF"[(base >> 4) & 15]);
    *p++ = (char)("0123456789ABCDEF"[base & 15]);
    p = scat(p, "h, ");
    if (irq) {
        p = scat(p, "IRQ ");
        p = udec(p, irq);
        p = scat(p, ", ");
    }
    p = scat(p, "DMA ");
    for (k = 0; k < 8; k++)
        if (dma & (1 << k)) {
            *p++ = (char)('0' + k);
            claim(RES_DMA, k, 1);
        }
    scat(p, "\n");
    dputs(line);
    claim(RES_IO, base, 4);

    for (id = 0; id < 8; id++) {
        if (id == host)
            continue;
        cdb6(0x12, 36);                     /* INQUIRY                  */
        if (scsi(id, 36, r->buf_phys, 6) != SCSI_OK)
            continue;
        p = scat(line, "  SCSI-ID ");
        *p++ = (char)('0' + id);
        p = scat(p, (buf[0] & 0x1F) == 0 ? ((buf[1] & 0x80) ? ": Wechselplatte " : ": Festplatte ") :
                    (buf[0] & 0x1F) == 7 ? ": MO-Laufwerk " :
                    (buf[0] & 0x1F) == 5 ? ": CD-ROM " : ": Ger\x84t ");
        for (k = 8; k < 32; k++)
            *p++ = buf[k] >= 32 ? (char)buf[k] : ' ';
        while (p[-1] == ' ')
            p--;
        *p = 0;

        if (((buf[0] & 0x1F) == 0 || (buf[0] & 0x1F) == 7) && n < DRV_MAXUNITS) {
            unsigned char rmb = buf[1] & 0x80;  /* ZIP, JAZ, MO, SyQuest */
            unsigned long cap;
            for (k = 0; k < 4; k++) {       /* Unit Attention abholen   */
                cdb6(0x00, 0);              /* TEST UNIT READY          */
                if (scsi(id, 0, 0, 6) == SCSI_OK)
                    break;
            }
            cap = capacity(id, r->buf_phys, buf);
            if (cap || rmb) {               /* Wechselplatte auch leer  */
                r->blocks[n] = cap;
                r->bsize[n] = rmb ? 512 | DRV_BS_REMOVABLE : 0;
                bsz[n] = 512;
                rem[n] = rmb ? 1 : 0;
                chg[n] = 0;
                target[n++] = (unsigned char)id;
                if (cap) {
                    p = scat(p, ", ");
                    p = udec(p, (unsigned)(cap >> 11));
                    p = scat(p, " MB");
                } else
                    p = scat(p, " (kein Medium)");
            } else {
                p = scat(p, " (nicht bereit)");
            }
        } else if ((buf[0] & 0x1F) == 5 && n < DRV_MAXUNITS) {
            /* CD-ROM: Einheit mit 2048-Byte-Bloecken; ob eine CD drin
               ist, prueft erst das Dateisystem beim Zugriff */
            for (k = 0; k < 4; k++) {
                cdb6(0x00, 0);
                if (scsi(id, 0, 0, 6) == SCSI_OK)
                    break;
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
    /* Interrupt erst nach der Erkennung (die fragt ab) */
    irq_on = 0;
    miss = 0;
    if (n && irq && !has_opt(r->args, "/NOIRQ")) {
        if (set_irq(irq, aha_irq) == 0)
            irq_on = 1;
        else {
            p = scat(line, "  AHA154X: IRQ ");
            p = udec(p, irq);
            scat(p, " belegt, Abfragebetrieb\n");
            dputs(line);
        }
    }
    r->units = (unsigned char)n;
    r->count = 16;                          /* Blockzahl in READ(10)    */
    r->status = n ? DRV_OK : DRV_ERR_NODEV;
}

/* gescheiterten Versuch in Sitzung 1 melden (Host- und Zielstatus aus
   dem CCB, Rueckgabe von scsi_dir) */
static void report(int write, unsigned u, unsigned long l, int rc, int t)
{
    char *p = scat(line, write ? "AHA154X.SYS: Schreibfehler, SCSI-ID " :
                                 "AHA154X.SYS: Lesefehler, SCSI-ID ");
    p = udec(p, target[u]);
    p = scat(p, ", Sektor ");
    p = hex(p, l, 8);
    p = scat(p, "h, ");
    p = scat(p, rc == SCSI_NODEV ? "Ger\x84t antwortet nicht" :
                rc == SCSI_CHECK ? "Check Condition" :
                ccb.hastat == 0 && ccb.tarstat == 0 ? "Zeit\x81" "berschreitung" : "Fehler");
    p = scat(p, ", Host ");
    p = hex(p, ccb.hastat, 2);
    p = scat(p, "h, Ziel ");
    p = hex(p, ccb.tarstat, 2);
    p = scat(p, "h, Versuch ");
    p = udec(p, (unsigned)t + 1);
    scat(p, irq_on ? ", IRQ\n" : ", Abfrage\n");
    dputs(line);
}

/* READ(10) / WRITE(10) eines Blocks */
static void xfer(int write)
{
    struct drv_req __far *r = REQ;
    unsigned long l = r->lba;
    unsigned n = write || r->count == 0 ? 1 : r->count;
    int tries, rc, spin = 0;

    if (r->unit >= DRV_MAXUNITS) {
        r->status = DRV_ERR_IO;
        return;
    }
    for (tries = 0; tries < 3; tries++) {
        cdb[0] = write ? 0x2A : 0x28;       /* WRITE(10) / READ(10)     */
        cdb[1] = 0;
        cdb[2] = (unsigned char)(l >> 24);
        cdb[3] = (unsigned char)(l >> 16);
        cdb[4] = (unsigned char)(l >> 8);
        cdb[5] = (unsigned char)l;
        cdb[6] = 0;
        cdb[7] = 0;
        cdb[9] = 0;
        if (write && bsz[r->unit] != 512) {
            r->status = DRV_ERR_WP;
            return;
        }
        if (n > 1 && bsz[r->unit] != 512)
            n = 1;                          /* CD: ein Block (2048)     */
        cdb[8] = (unsigned char)n;
        rc = scsi_dir(target[r->unit], n * bsz[r->unit], r->buf_phys, 10, write ? 0x10 : 0x08);
        if (rc == SCSI_OK) {
            r->status = DRV_OK;
            return;
        }
        if (rc == SCSI_CHECK && rem[r->unit]) {     /* Wechselplatte    */
            chg[r->unit] = 1;
            if ((last_sense >> 8) == 6) {           /* gewechselt       */
                r->status = DRV_ERR_CHANGED;
                return;
            }
            if (last_sense == 0x23A) {              /* kein Medium      */
                r->status = DRV_ERR_NOTREADY;
                return;
            }
        }
        /* CD: keine drin (2/3Ah, beim zweiten Mal - nach einem Wechsel
           meldet manches Laufwerk das einmal); laeuft an (2/04h): warten */
        if (rc == SCSI_CHECK && bsz[r->unit] != 512) {
            if (last_sense == 0x23A && tries > 0) {
                r->status = DRV_ERR_NOTREADY;
                return;
            }
            if (last_sense == 0x204 && spin < 20) {
                spin++;
                tries--;
                cli();
                proc_block(0x7F, 50);
                continue;
            }
        }
        /* CD: Check Condition beim ersten Zugriff nach dem Wechsel (Unit
           Attention) ist normal, nicht melden */
        if (!(rc == SCSI_CHECK && bsz[r->unit] != 512 && (tries == 0 || last_sense == 0x23A)))
            report(write, r->unit, l, rc, tries);
    }
    r->status = DRV_ERR_IO;
}

/* Wechselplatte: Medium da? gewechselt? (TEST UNIT READY; Wechsel =
   Unit Attention 6/28h, kein Medium 2/3Ah) */
static void media(void)
{
    struct drv_req __far *r = REQ;
    unsigned u = r->unit, k, spin = 0;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    unsigned long cap;
    int rc;

    r->count = 0;
    r->status = DRV_ERR_IO;
    if (u >= nunits || !rem[u])
        return;
    for (k = 0; k < 6; k++) {
        cdb6(0x00, 0);
        if ((rc = scsi(target[u], 0, 0, 6)) == SCSI_OK)
            break;
        if (rc != SCSI_CHECK)
            continue;
        if ((last_sense >> 8) == 6)
            chg[u] = 1;
        else if (last_sense == 0x23A) {
            chg[u] = 1;
            r->status = DRV_ERR_NOTREADY;
            return;
        } else if (last_sense == 0x204 && spin < 20) {   /* laeuft an */
            spin++;
            k--;
            cli();
            proc_block(0x7F, 50);
        }
    }
    if (k == 6)
        return;
    if (chg[u]) {
        if ((cap = capacity(target[u], r->buf_phys, buf)) == 0)
            return;
        r->lba = cap;
        r->count = 1;
        chg[u] = 0;
    }
    r->status = DRV_OK;
}

/* SYNCHRONIZE CACHE(10) an alle Platten; Platten ohne Cache lehnen den
   Befehl ab (Check Condition) - das ist kein Fehler */
static void flush(void)
{
    struct drv_req __far *r = REQ;
    unsigned u, i;

    for (u = 0; u < nunits; u++) {
        if (bsz[u] != 512)
            continue;
        for (i = 0; i < 12; i++)
            cdb[i] = 0;
        cdb[0] = 0x35;
        scsi_dir(target[u], 0, 0, 10, 0x18);    /* 18h: keine Daten     */
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
        xfer(0);
        break;
    case DRV_WRITE:
        xfer(1);
        break;
    case DRV_FLUSH:
        flush();
        break;
    case DRV_MEDIA:
        media();
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
