/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AIC7XXX.SYS - Adaptec PCI-SCSI mit AIC-7870/7880: AHA-2940, 2940W,
 * 2940 Ultra (UW), 2944UW und der Chip auf Hauptplatinen - AT-DOS 6.0,
 * braucht einen 386
 *
 * CONFIG.SYS:  DEVICE=AIC7XXX.SYS [/ID:7] [/NOIRQ] [/NOWIDE]
 *
 * Der Chip hat einen eigenen kleinen Prozessor (Sequenzer), dessen
 * Programm sonst der Treiber laedt. Dieser Treiber laesst ihn angehalten
 * (HCNTRL PAUSE) und fuehrt den Bus selbst: Auswahl mit ATN, IDENTIFY
 * 80h (LUN 0, ohne Trennen), Befehl, Status und Meldungen Byte fuer Byte
 * ueber SCSIDATL (automatisches PIO, SPIOEN), die Daten per Busmaster-
 * DMA des Chips direkt in den Puffer des Kernels (STCNT/HCNT/HADDR,
 * DFCNTRL). Mehr Daten als erwartet nimmt BITBUCKET ab. Warten: kurz
 * pausenlos nachsehen, danach schlafen bis zum PCI-Interrupt des Chips
 * (SIMODE0/1 je nach erwartetem Ereignis), mit /NOIRQ je Takt.
 *
 * Wide (2940W/UW, 2944UW): die Karte meldet ihren 16-Bit-Anschluss nach
 * dem Chip-Reset in SBLKCTL (SELWIDE). Dann werden die IDs 0-15
 * abgesucht und mit jedem Geraet beim ersten Befehl die Breite
 * verhandelt (WDTR hinter IDENTIFY); sagt es ja, gilt fuer seine
 * Datenphasen SCSIRATE WIDEXFER. /NOWIDE bleibt schmal (IDs 0-7).
 * Register und Ablauf: AIC-7870-Datenbuch bzw. die Nachbildung in 86Box
 * (src/scsi/scsi_aic7xxx.c, nur gelesen); Linux aic7xxx (GPLv2/BSD)
 * nicht uebernommen.
 *
 * Gebaut mit -zu wie alle Treiber: keine Adressen lokaler Variablen.
 */
#include "drvlib.h"

/* ---- AIC-7xxx-Register (I/O, BAR 0) ------------------------------------ */
#define SCSISEQ     0x00
#define  ENSELO     0x40
#define  ENRSELI    0x10
#define  ENAUTOATNO 0x08
#define  SCSIRSTO   0x01
#define SXFRCTL0    0x01
#define  SPIOEN     0x08
#define  CLRSTCNT   0x10
#define  CLRCHN     0x02
#define SXFRCTL1    0x02
#define  BITBUCKET  0x80
#define  ENSTIMER   0x04
#define  ACTNEGEN   0x02
#define  STPWEN     0x01
#define SCSISIG     0x03                /* lesen: Bus, schreiben: Phase   */
#define  REQI       0x02
#define  BSYI       0x04
#define  PHASE_MASK 0xE0
#define  P_DATAOUT  0x00
#define  P_DATAIN   0x40
#define  P_COMMAND  0x80
#define  P_MESGOUT  0xA0
#define  P_STATUS   0xC0
#define  P_MESGIN   0xE0
#define SCSIRATE    0x04
#define  WIDEXFER   0x80
#define SCSIID      0x05
#define SCSIDATL    0x06
#define STCNT       0x08                /* 3 Byte                          */
#define SSTAT0      0x0B                /* schreiben: CLRSINT0             */
#define  SELDO      0x40
#define  SELDI      0x20
#define  DMADONE    0x01
#define SSTAT1      0x0C                /* schreiben: CLRSINT1             */
#define  SELTO      0x80
#define  CLRATNO    0x40
#define  BUSFREE    0x08
#define  PHASEMIS   0x10
#define  SCSIRSTI   0x20
#define SIMODE0     0x10
#define SIMODE1     0x11
#define SELID       0x19                /* Bits 4-7: wer uns auswaehlt     */
#define SBLKCTL     0x1F
#define  SELWIDE    0x02
#define  SELBUSB    0x08
#define HCNTRL      0x87
#define  INTEN      0x02
#define  PAUSE      0x04
#define  CHIPRST    0x01
#define HADDR       0x88                /* 4 Byte                          */
#define HCNT        0x8C                /* 3 Byte                          */
#define INTSTAT     0x91
#define CLRINT      0x92
#define DFCNTRL     0x93
#define  SCSIEN     0x20
#define  SDMAEN     0x10
#define  HDMAEN     0x08
#define  DIRECTION  0x04                /* Speicher -> SCSI                */
#define  FIFOFLUSH  0x02
#define  FIFORESET  0x01
#define DFSTATUS    0x94
#define  FIFOEMP    0x01

static unsigned io, irq, host = 7;
static char line[128];
static unsigned long lin;               /* lineare = physische Basis      */
static const char *chip;

static unsigned char rd8(unsigned r) { return inp(io + r); }
static void wr8(unsigned r, unsigned char v) { outp(io + r, v); }

static unsigned char cdb[12];           /* wird gebaut, run() kopiert ihn  */

/* Breite: wide = Karte mit 16-Bit-Bus (und nicht /NOWIDE), wd[id] =
   Stand der Verhandlung je Ziel: 0 offen, 1 Wide, 2 schmal */
static unsigned char wide, probed, wd[16];

#define SCSI_OK     0
#define SCSI_NODEV  1                   /* Auswahl-Zeitueberschreitung     */
#define SCSI_CHECK  2                   /* Check Condition                 */
#define SCSI_FAIL   3

/* ---- Befehle je Ziel, Trennen und Wiederauswahl ------------------------
   Jedes Ziel hat hoechstens einen Befehl (der Kernel sperrt je Einheit,
   jede Einheit hat ihre eigene Sperrgruppe), sein Stand steht ganz in
   cm[id]. Mit Trennerlaubnis (IDENTIFY C0h) gibt ein Geraet den Bus frei,
   waehrend es sucht (SAVE DATA POINTER, DISCONNECT), und waehlt uns
   spaeter wieder aus (SELDI, SELID). Den Bus fuehrt immer nur einer
   (own); wer gerade im Treiber laeuft und den Bus frei findet, bedient
   eine anstehende Wiederauswahl - auch fuer einen anderen Task, der
   schlaeft: alles Noetige steht in cm[], die Daten gehen per DMA an die
   physische Adresse. Der Treiber laeuft nie gleichzeitig (der Kernel
   unterbricht ihn nicht), Wartepunkte sind nur wait_ev/proc_block. */
#define ST_WANT 1                       /* will auf den Bus                */
#define ST_ON   2                       /* auf dem Bus                     */
#define ST_DISC 3                       /* getrennt, wartet auf das Geraet */
#define ST_DONE 4

static struct cmd {
    unsigned char cdb[12], cdblen, st, rc;
    unsigned char stat, msg, ok, dmsg, idm;
    unsigned char mo[5], mon, moi, mi[8], mii;
    unsigned      ci, t0, tmo, spins;
    unsigned long buf, len, done, sdp;
} cm[16];

static unsigned own;                    /* Ziel + 1, das den Bus hat       */
static unsigned ndisc;                  /* getrennte Befehle               */
static unsigned char own_sm0, own_sm1;  /* worauf der Besitzer wartet      */
static unsigned char discon;            /* Trennen erlauben (nach INIT)    */

static unsigned long phys(const void *p)
{
    return lin + (unsigned)p;
}

/* Warten: mit IRQ, bis der Chip eines der Ereignisse meldet - die des
   Busbesitzers (own_sm0/1) oder, bei freiem Bus und getrennten Befehlen,
   eine Wiederauswahl (SELDI) -, sonst je Takt; geschlafen wird hoechstens
   2 Takte (verlorener oder geteilter IRQ haelt nichts auf). Die Interrupt-
   routine nimmt INTEN weg und weckt alle Wartenden; jeder legt vor dem
   naechsten Schlaf neu an. 0 = Frist (t0, tmo in Takten) um */
static volatile unsigned char irq_on, woke;
#define EV_AIC 1

static void aic_irq(unsigned n)
{
    (void)n;
    if (!(rd8(HCNTRL) & INTEN) || !(rd8(INTSTAT) & 0x0F))
        return;                         /* nicht unserer (geteilter IRQ)  */
    wr8(HCNTRL, PAUSE);                 /* Leitung frei, Sequenzer steht  */
    woke = 1;
    proc_run(EV_AIC);
}

static void rearm(void)                 /* mit CLI                        */
{
    unsigned char m0 = own ? own_sm0 : 0, m1 = own ? own_sm1 : 0;
    if (!own && ndisc)
        m0 |= SELDI;                    /* ENSELDI = Bit von SELDI         */
    wr8(CLRINT, 0x04);                  /* altes SCSIINT weg, die Masken  */
    wr8(SIMODE0, m0);                   /* melden, was jetzt schon gilt   */
    wr8(SIMODE1, m1);
    wr8(HCNTRL, (m0 | m1) && irq_on ? PAUSE | INTEN : PAUSE);
}

static int wait_ev(unsigned t0, unsigned tmo)
{
    if ((unsigned)(devhlp(DH_TICKS, 0, 0, 0) - t0) > tmo)
        return 0;
    cli();
    if (irq_on) {
        woke = 0;
        rearm();
        if (!woke)
            proc_block(EV_AIC, 2);      /* kehrt mit STI zurueck         */
        else
            sti();
    } else
        proc_block(0xFFFF, 1);
    return 1;
}

/* Besitzer wartet auf ein Bus-Ereignis: erst kurz pausenlos (der Chip
   antwortet meist in Mikrosekunden), dann schlafen */
static int later2(struct cmd *c, unsigned char sm0, unsigned char sm1)
{
    if (++c->spins < 2000)
        return 1;
    own_sm0 = sm0;
    own_sm1 = sm1;
    return wait_ev(c->t0, c->tmo);
}

#define later(c) later2(c, 0, BUSFREE | SCSIRSTI | 0x01)    /* REQINIT  */

/* Bus freigeben: Wiederauswahl annehmen, solange jemand getrennt ist,
   und die Wartenden wecken (sie brauchen den Bus) */
static void bus_release(void)
{
    own = 0;
    own_sm0 = own_sm1 = 0;
    wr8(SXFRCTL0, 0);
    wr8(SSTAT0, 0xFF & ~SELDI);
    wr8(SSTAT1, 0xFF);
    wr8(SCSISEQ, ndisc ? ENRSELI : 0);
    cli();
    rearm();
    sti();
    proc_run(EV_AIC);
}

static void chip_reset(void)
{
    unsigned k;
    wr8(HCNTRL, CHIPRST | PAUSE);
    for (k = 0; k < 100; k++)
        inp(0x80);
    wr8(HCNTRL, PAUSE);                 /* Sequenzer bleibt angehalten    */
    for (k = 0; k < 1000 && !(rd8(HCNTRL) & PAUSE); k++)
        ;
    if (!probed) {                      /* Anschluss der Karte: 02h = Wide */
        probed = 1;
        if ((rd8(SBLKCTL) & (SELWIDE | SELBUSB)) != SELWIDE)
            wide = 0;
    }
    wr8(SBLKCTL, wide ? SELWIDE : 0);   /* Kanal A                        */
    for (k = 0; k < 16; k++)            /* nach einem Reset neu verhandeln */
        if (cm[k].st != ST_DISC)        /* (nicht, wer gleich zurueckkommt) */
            wd[k] = 0;
    wr8(SCSISEQ, 0);
    wr8(SXFRCTL0, CLRSTCNT | CLRCHN);
    wr8(SXFRCTL1, ENSTIMER | ACTNEGEN | STPWEN);    /* Auswahlfrist ~256 ms */
    wr8(SCSIRATE, 0);                   /* asynchron                      */
    wr8(SCSIID, (unsigned char)host);
    wr8(SIMODE0, 0);
    wr8(SIMODE1, 0);
    wr8(DFCNTRL, FIFORESET);
    wr8(SSTAT0, 0xFF);                  /* CLRSINT0/1: alles loeschen     */
    wr8(SSTAT1, 0xFF);
    wr8(CLRINT, 0x1F);
    wr8(SCSISEQ, ndisc ? ENRSELI : 0);
}

/* Bus-Reset (Geraet meldet sich zurueck, ohne dass ein Befehl auf es
   wartet): alle getrennten Befehle sind verloren */
static void bus_reset(void)
{
    unsigned k;
    wr8(SCSISEQ, SCSIRSTO);
    for (k = 0; k < 200; k++)
        inp(0x80);
    wr8(SCSISEQ, 0);
    for (k = 0; k < 16; k++)
        if (cm[k].st == ST_DISC) {
            cm[k].st = ST_DONE;
            cm[k].rc = SCSI_FAIL;
        }
    ndisc = 0;
    chip_reset();
    for (k = 0; k < 16; k++)
        wd[k] = 0;
}

static void put24(unsigned r, unsigned long v)
{
    wr8(r, (unsigned char)v);
    wr8(r + 1, (unsigned char)(v >> 8));
    wr8(r + 2, (unsigned char)(v >> 16));
}

/* Datenphase per DMA: n Bytes ab buf, out = zum Geraet. -> uebertragen */
static unsigned long dma(struct cmd *c, unsigned long buf, unsigned long n, int out)
{
    unsigned char st;
    unsigned long left;
    unsigned k;
    wr8(SXFRCTL0, 0);
    wr8(DFCNTRL, FIFORESET);
    put24(STCNT, n);
    put24(HCNT, n);
    wr8(HADDR, (unsigned char)buf);
    wr8(HADDR + 1, (unsigned char)(buf >> 8));
    wr8(HADDR + 2, (unsigned char)(buf >> 16));
    wr8(HADDR + 3, (unsigned char)(buf >> 24));
    wr8(SSTAT1, PHASEMIS | 0x01);       /* PHASEMIS/REQINIT loeschen      */
    wr8(DFCNTRL, SCSIEN | SDMAEN | HDMAEN | (out ? DIRECTION : 0));
    for (;;) {
        st = rd8(SSTAT0);
        if (st & DMADONE)
            break;
        if (rd8(SSTAT1) & (PHASEMIS | BUSFREE | SCSIRSTI))
            break;
        if (!(rd8(SCSISIG) & BSYI) || !later2(c, DMADONE, PHASEMIS | BUSFREE | SCSIRSTI))
            break;
    }
    if (!out) {                         /* Rest im FIFO in den Speicher   */
        wr8(DFCNTRL, HDMAEN | FIFOFLUSH);
        for (k = 0; !(rd8(DFSTATUS) & FIFOEMP) && k < 20000; k++)
            ;
    }
    wr8(DFCNTRL, 0);
    for (k = 0; (rd8(DFCNTRL) & (SCSIEN | SDMAEN | HDMAEN)) && k < 20000; k++)
        ;
    left = rd8(STCNT) | ((unsigned)rd8(STCNT + 1) << 8) |
           ((unsigned long)rd8(STCNT + 2) << 16);
    return n - left;
}

/* Phasen des Befehls auf dem Bus (nach Auswahl oder Wiederauswahl), bis
   das Geraet den Bus freigibt; danach ist er fertig, getrennt oder
   gescheitert. Gibt den Bus frei. */
static void serve(struct cmd *c, unsigned id)
{
    unsigned char ph, s1;

    c->st = ST_ON;
    c->spins = 0;
    c->dmsg = 0;
    for (;;) {                          /* Phasen, wie das Geraet sie will */
        s1 = rd8(SSTAT1);
        if (!(rd8(SCSISIG) & BSYI) || (s1 & SCSIRSTI))
            break;                      /* Bus frei                       */
        if (!(rd8(SCSISIG) & REQI)) {
            if (!later(c))
                break;
            continue;
        }
        ph = rd8(SCSISIG) & PHASE_MASK;
        wr8(SCSISIG, ph);               /* Phase uebernehmen              */
        wr8(SXFRCTL1, ENSTIMER | ACTNEGEN | STPWEN);
        switch (ph) {
        case P_MESGOUT:                 /* IDENTIFY [+ WDTR], dann NOP     */
            if (c->moi + 1 >= c->mon)
                wr8(SSTAT1, CLRATNO);   /* letztes Byte: ATN weg           */
            wr8(SXFRCTL0, SPIOEN);
            wr8(SCSIDATL, c->moi < c->mon ? c->mo[c->moi] : 0x08);
            c->moi++;
            break;
        case P_COMMAND:
            wr8(SXFRCTL0, SPIOEN);
            wr8(SCSIDATL, c->ci < c->cdblen ? c->cdb[c->ci] : 0);
            c->ci++;
            break;
        case P_DATAIN:
        case P_DATAOUT:
            if (c->done < c->len)
                c->done += dma(c, c->buf + c->done, c->len - c->done, ph == P_DATAOUT);
            else {                      /* mehr als erwartet: verwerfen    */
                wr8(SXFRCTL0, 0);
                wr8(SXFRCTL1, BITBUCKET | ENSTIMER | ACTNEGEN | STPWEN);
                while ((rd8(SCSISIG) & (BSYI | PHASE_MASK)) == (BSYI | ph) && later(c))
                    ;
                wr8(SXFRCTL1, ENSTIMER | ACTNEGEN | STPWEN);
            }
            break;
        case P_STATUS:
            wr8(SXFRCTL0, SPIOEN);
            c->stat = rd8(SCSIDATL);
            break;
        case P_MESGIN:
            wr8(SXFRCTL0, SPIOEN);
            ph = rd8(SCSIDATL);
            if (c->mii) {               /* erweiterte Meldung: Rest        */
                if (c->mii < sizeof(c->mi))
                    c->mi[c->mii] = ph;
                if (++c->mii >= 2 && c->mii >= 2u + c->mi[1]) {
                    if (c->mi[1] == 2 && c->mi[2] == 0x03 && c->mon > 1) {  /* WDTR */
                        wd[id] = c->mi[3] ? 1 : 2;
                        wr8(SCSIRATE, c->mi[3] ? WIDEXFER : 0);
                    }
                    c->mii = 0;
                }
            } else if (ph == 0x01)      /* EXTENDED MESSAGE                */
                c->mii = 1;
            else if (ph & 0x80)
                ;                       /* IDENTIFY der Wiederauswahl      */
            else if (ph == 0x02)        /* SAVE DATA POINTER               */
                c->sdp = c->done;
            else if (ph == 0x03)        /* RESTORE POINTERS                */
                c->done = c->sdp;
            else if (ph == 0x04)        /* DISCONNECT                      */
                c->dmsg = 1;
            else {
                c->msg = ph;
                if (ph == 0)
                    c->ok = 1;          /* COMMAND COMPLETE               */
                else if (ph == 0x07 && c->mon > 1 && !wd[id])
                    wd[id] = 2;         /* MESSAGE REJECT auf WDTR         */
            }
            break;
        default:
            wr8(SXFRCTL0, SPIOEN);      /* unbekannt: Byte abnehmen        */
            rd8(SCSIDATL);
        }
        c->spins = 0;
    }
    wr8(SXFRCTL0, 0);
    if (c->ok && c->mon > 1 && !wd[id])
        wd[id] = 2;                     /* WDTR ohne Antwort: schmal       */
    if (c->ok) {
        c->rc = c->stat == 0 ? SCSI_OK : c->stat == 2 ? SCSI_CHECK : SCSI_FAIL;
        c->st = ST_DONE;
    } else if (c->dmsg && !(rd8(SCSISIG) & BSYI)) {
        c->st = ST_DISC;                /* kommt per Wiederauswahl zurueck */
        c->t0 = devhlp(DH_TICKS, 0, 0, 0);
        ndisc++;
    } else {
        c->rc = SCSI_FAIL;
        c->st = ST_DONE;
        chip_reset();
    }
    bus_release();
}

/* Ein Geraet waehlt uns wieder aus (Bus frei, SELDI): seinen Befehl
   fortsetzen - wem er auch gehoert */
static void resel(void)
{
    unsigned id = rd8(SELID) >> 4;
    struct cmd *c = &cm[id];
    wr8(SSTAT0, SELDI);
    if (c->st != ST_DISC) {             /* niemand erwartet es             */
        bus_reset();
        bus_release();
        return;
    }
    ndisc--;
    own = id + 1;
    c->done = c->sdp;                   /* Zeiger wie zuletzt gesichert    */
    wr8(SCSIRATE, wd[id] == 1 ? WIDEXFER : 0);
    wr8(SXFRCTL1, ENSTIMER | ACTNEGEN | STPWEN);
    serve(c, id);
}

/* Auswahl des Ziels; der Bus ist frei. Kommt dabei eine Wiederauswahl
   dazwischen, wird abgebrochen (der Befehl bleibt ST_WANT). */
static void select(struct cmd *c, unsigned id)
{
    own = id + 1;
    c->spins = 0;
    wr8(SSTAT0, 0xFF & ~SELDI);
    wr8(SSTAT1, 0xFF);
    wr8(SXFRCTL1, ENSTIMER | ACTNEGEN | STPWEN);
    wr8(SCSIRATE, wd[id] == 1 ? WIDEXFER : 0);
    wr8(SCSIID, (unsigned char)((id << 4) | host));
    wr8(SCSISEQ, ENSELO | ENAUTOATNO | (ndisc ? ENRSELI : 0));  /* mit ATN */
    for (;;) {
        if (rd8(SSTAT0) & SELDO)
            break;
        if (rd8(SSTAT0) & SELDI) {      /* ein getrenntes Geraet war schneller */
            wr8(SCSISEQ, ENRSELI);
            own = 0;
            resel();
            return;
        }
        if (rd8(SSTAT1) & SELTO) {
            wr8(SCSISEQ, 0);
            wr8(SSTAT1, SELTO);
            c->rc = SCSI_NODEV;
            c->st = ST_DONE;
            bus_release();
            return;
        }
        if (!later2(c, SELDO | SELDI, SELTO | SCSIRSTI)) {
            c->rc = SCSI_NODEV;
            c->st = ST_DONE;
            chip_reset();
            bus_release();
            return;
        }
    }
    wr8(SCSISEQ, 0);
    wr8(SSTAT0, SELDO);
    serve(c, id);
}

/* Befehl: Ziel id, CDB (cdblen, aus cdb[]), Daten len Bytes ab der
   physischen Adresse buf (Richtung nach der Phase, die das Geraet waehlt).
   t = Frist in Takten */
static int run(unsigned id, unsigned cdblen, unsigned long buf, unsigned long len,
               unsigned t)
{
    struct cmd *c = &cm[id];
    unsigned k;

    for (k = 0; k < 12; k++)
        c->cdb[k] = cdb[k];
    c->cdblen = (unsigned char)cdblen;
    c->buf = buf;
    c->len = len;
    c->done = c->sdp = 0;
    c->stat = c->msg = 0xFF;
    c->ok = 0;
    c->ci = 0;
    c->moi = c->mii = 0;
    c->mo[0] = discon ? 0xC0 : 0x80;    /* IDENTIFY, LUN 0, Trennen erlaubt? */
    c->mon = 1;
    if (wide && !wd[id]) {              /* WDTR: 16 Bit anbieten           */
        c->mo[1] = 0x01;
        c->mo[2] = 0x02;
        c->mo[3] = 0x03;
        c->mo[4] = 0x01;
        c->mon = 5;
    }
    c->t0 = devhlp(DH_TICKS, 0, 0, 0);
    c->tmo = t;
    c->st = ST_WANT;
    for (;;) {
        if (c->st == ST_DONE)
            break;
        if (!own) {                     /* Bus frei: wer zuerst?          */
            if (rd8(SSTAT0) & SELDI) {
                resel();
                continue;
            }
            if (c->st == ST_WANT) {
                select(c, id);
                continue;
            }
        }
        if (c->st == ST_ON) {           /* ein anderer fuehrt ihn gerade  */
            cli();                      /* (seine eigene Frist gilt)      */
            proc_block(EV_AIC, 2);
            continue;
        }
        if (!wait_ev(c->t0, c->tmo)) {  /* Frist um                       */
            if (c->st == ST_DISC) {     /* Geraet kam nicht zurueck        */
                ndisc--;
                if (!own) {
                    bus_reset();
                    bus_release();
                }
            }
            c->rc = c->st == ST_WANT ? SCSI_NODEV : SCSI_FAIL;
            break;
        }
    }
    c->st = 0;
    return c->rc;
}

static void cdb6(unsigned char o, unsigned char alloc)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        cdb[i] = 0;
    cdb[0] = o;
    cdb[4] = alloc;
}

/* REQUEST SENSE: Sense Key << 8 | ASC, 0 = keine Antwort (Puffer je Ziel:
   ein anderer Task kann derweil dasselbe fuer sein Geraet tun) */
static unsigned char sense[16][18];

static unsigned req_sense(unsigned id)
{
    cdb6(0x03, 18);
    if (run(id, 6, phys(sense[id]), 18, 300) != SCSI_OK)
        return 0;
    return ((sense[id][2] & 0x0F) << 8) | sense[id][12];
}

/* ---- Geraete ------------------------------------------------------------ */
static unsigned char target[DRV_MAXUNITS];
static unsigned bsz[DRV_MAXUNITS];
static unsigned char rem[DRV_MAXUNITS]; /* Wechselplatte (INQUIRY RMB)     */
static unsigned char chg[DRV_MAXUNITS]; /* Unit Attention gesehen          */
static unsigned nunits;

/* READ CAPACITY -> Sektoren (0 = keine Antwort / nicht 512 Byte) */
static unsigned long capacity(unsigned id, unsigned long phys,
                              unsigned char __far *buf)
{
    unsigned k;
    for (k = 0; k < 12; k++)
        cdb[k] = 0;
    cdb[0] = 0x25;
    if (run(id, 10, phys, 8, 300) != SCSI_OK || buf[6] != 2 || buf[7] != 0)
        return 0;
    return (((unsigned long)buf[0] << 24) | ((unsigned long)buf[1] << 16) |
            ((unsigned)buf[2] << 8) | buf[3]) + 1;
}

static const struct { unsigned id; const char *name; } chips[] = {
    { 0x7178, "AHA-2940 (AIC-7870)" }, { 0x7078, "AIC-7870" },
    { 0x7278, "AHA-3940 (AIC-7870)" }, { 0x7478, "AHA-2944 (AIC-7870)" },
    { 0x8178, "AHA-2940 Ultra (AIC-7880)" }, { 0x8078, "AIC-7880" },
    { 0x8278, "AHA-3940 Ultra (AIC-7880)" },
    { 0x8478, "AHA-2944 Ultra (AIC-7880)" }
};

static unsigned ploc;                   /* gefundene Karte (PCI-Ort)       */

/* Karte ueber den PCI-Dienst des Kernels suchen */
static int find_card(void)
{
    unsigned k;
    for (k = 0; k < sizeof(chips) / sizeof(chips[0]); k++)
        if ((ploc = pci_find(0x9004, chips[k].id, 0)) != 0xFFFF) {
            chip = chips[k].name;
            return 0;
        }
    return -1;
}

static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    const char __far *a;
    unsigned bus, dev, id, k, n = 0;
    unsigned long v;
    char *p;

    r->units = 0;
    r->status = DRV_ERR_NODEV;
    lin = r->base;
    if ((a = has_opt(r->args, "/ID:") ? r->args : 0) != 0) {
        for (; *a && !(a[0] == ':' && a[-1] == 'D'); a++)
            ;
        if (*a == ':' && a[1] >= '0' && a[1] <= '9') {
            host = a[1] - '0';
            if (a[2] >= '0' && a[2] <= '9')
                host = host * 10 + a[2] - '0';
        }
    }
    wide = has_opt(r->args, "/NOWIDE") ? 0 : 1;     /* chip_reset prueft */
    if (find_card()) {
        dputs("  AIC7XXX.SYS: kein Adaptec AIC-7870/7880 gefunden\n");
        return;
    }
    bus = ploc >> 8;
    dev = (ploc >> 3) & 31;
    v = pci_rd(ploc, 0x10);             /* BAR 0: I/O                      */
    if (!(v & 1)) {
        dputs("  AIC7XXX.SYS: BAR 0 ist kein I/O-Bereich\n");
        return;
    }
    io = (unsigned)v & 0xFFFC;
    irq = (unsigned)pci_rd(ploc, 0x3C) & 0xFF;
    if (claim(RES_IO, io, 0x100)) {
        dputs("  AIC7XXX.SYS: Ports geh\x94ren schon einem anderen Treiber\n");
        return;
    }
    v = pci_rd(ploc, 0x04);
    pci_wr(ploc, 0x04, (unsigned)v | 0x0005, 2);        /* I/O + Busmaster */
    chip_reset();
    if (host > (wide ? 15u : 7u))
        host = 7;
    wr8(SCSIID, (unsigned char)host);

    p = scat(line, "  Adaptec ");
    p = scat(p, chip);
    p = scat(p, ", PCI ");
    p = hex(p, bus, 2);
    p = scat(p, ":");
    p = hex(p, dev, 2);
    p = scat(p, ", ");
    p = hex(p, io, 4);
    k = irq && irq < 16 ? (has_opt(r->args, "/NOIRQ") ? 2 : set_irq(irq, aic_irq) ? 1 : 0) : 3;
    irq_on = k == 0;
    p = scat(p, "h, IRQ ");
    p = udec(p, irq);
    p = scat(p, k == 1 ? " belegt, Abfragebetrieb" : k == 2 ? " nicht benutzt (/NOIRQ)" :
                k == 3 ? " ung\x81ltig, Abfragebetrieb" : "");
    p = scat(p, wide ? ", Wide" : "");
    p = scat(p, k == 0 && !has_opt(r->args, "/NODISC") ? ", Trennen" : "");
    p = scat(p, ", ID ");
    p = udec(p, host);
    scat(p, "\n");
    dputs(line);

    for (id = 0; id < (wide ? 16u : 8u); id++) {
        if (id == host)
            continue;
        cdb6(0x12, 36);                 /* INQUIRY                         */
        if (run(id, 6, r->buf_phys, 36, 100) != SCSI_OK)
            continue;
        p = scat(line, "  SCSI-ID ");
        p = udec(p, id);
        p = scat(p, (buf[0] & 0x1F) == 0 ? ((buf[1] & 0x80) ? ": Wechselplatte " : ": Festplatte ") :
                    (buf[0] & 0x1F) == 7 ? ": MO-Laufwerk " :
                    (buf[0] & 0x1F) == 5 ? ": CD-ROM " : ": Ger\x84t ");
        for (k = 8; k < 32; k++)
            *p++ = buf[k] >= 32 ? (char)buf[k] : ' ';
        while (p[-1] == ' ')
            p--;
        *p = 0;
        if (wd[id] == 1)
            p = scat(p, " (Wide)");
        if (((buf[0] & 0x1F) == 0 || (buf[0] & 0x1F) == 7) && n < DRV_MAXUNITS) {
            unsigned char rmb = buf[1] & 0x80;  /* ZIP, JAZ, MO, SyQuest  */
            unsigned long cap;
            for (k = 0; k < 4; k++) {   /* Unit Attention abholen          */
                cdb6(0x00, 0);
                if (run(id, 6, 0, 0, 300) == SCSI_OK)
                    break;
                req_sense(id);
            }
            cap = capacity(id, r->buf_phys, buf);
            if (cap || rmb) {
                r->blocks[n] = cap;
                r->bsize[n] = (rmb ? 512 | DRV_BS_REMOVABLE : 0) | DRV_BS_GROUP(n);
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
            r->bsize[n] = 2048 | DRV_BS_GROUP(n);
            bsz[n] = 2048;
            target[n++] = (unsigned char)id;
        }
        scat(p, "\n");
        dputs(line);
    }
    nunits = n;
    /* ab jetzt trennen - nur mit IRQ: im Abfragebetrieb kostete jede
       Wiederauswahl einen Takt Schlaf */
    discon = irq_on && !has_opt(r->args, "/NODISC");
    r->units = (unsigned char)n;
    r->count = 0x90;                    /* 16 Bloecke je READ(10), 80h: auch CD */
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
    if (n > 16)                         /* CD: 16 x 2048 = 32 KB         */
        n = 16;
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
            if (rem[r->unit] && (sk >> 8) == 6) {   /* gewechselt: der Kernel  */
                chg[r->unit] = 1;                   /* soll neu einhaengen     */
                r->status = DRV_ERR_CHANGED;
                return;
            }
            if (sk == 0x23A && (rem[r->unit] || (bsz[r->unit] != 512 && tries > 0))) {
                chg[r->unit] = 1;
                r->status = DRV_ERR_NOTREADY;   /* keine CD/kein Medium    */
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

/* Wechselplatte: Medium da? gewechselt? (TEST UNIT READY; ein
   Wechsel kommt als Unit Attention 6/28h, kein Medium als 2/3Ah) */
static void media(void)
{
    struct drv_req __far *r = REQ;
    unsigned u = r->unit, k, sk, spin = 0;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    unsigned long cap;

    r->count = 0;
    r->status = DRV_ERR_IO;
    if (u >= nunits || !rem[u])
        return;
    for (k = 0; k < 6; k++) {
        cdb6(0x00, 0);
        if (run(target[u], 6, 0, 0, 300) == SCSI_OK)
            break;
        sk = req_sense(target[u]);
        if ((sk >> 8) == 6)
            chg[u] = 1;
        else if (sk == 0x23A) {
            chg[u] = 1;
            r->status = DRV_ERR_NOTREADY;
            return;
        } else if (sk == 0x204 && spin < 20) {  /* laeuft gerade an       */
            spin++;
            k--;
            cli();
            proc_block(0xFFFF, 50);
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

/* Auswerfen: erst Entnahme erlauben (PREVENT ALLOW 1Eh mit 0), dann
   START STOP UNIT (1Bh) mit LoEj = 1, Start = 0 */
static void eject(void)
{
    struct drv_req __far *r = REQ;
    unsigned u = r->unit;
    r->status = DRV_ERR_CMD;
    if (u >= nunits || (!rem[u] && bsz[u] == 512))
        return;
    cdb6(0x1E, 0);
    run(target[u], 6, 0, 0, 300);
    cdb6(0x1B, 0x02);
    r->status = run(target[u], 6, 0, 0, 1000) == SCSI_OK ? DRV_OK : DRV_ERR_IO;
    if (bsz[u] == 512) {                    /* Wechselplatte: dazu Stop  */
        cdb6(0x1B, 0x00);                   /* (Iomega-ZIP wirft so aus, */
        run(target[u], 6, 0, 0, 1000);      /* 86Box nur so; Ergebnis egal) */
    }
    chg[u] = 1;
}

static void flush(void)
{
    struct drv_req __far *r = REQ;      /* REQ gilt nur beim Eintritt     */
    unsigned u, i;
    for (u = 0; u < nunits; u++) {
        if (bsz[u] != 512)
            continue;
        for (i = 0; i < 12; i++)
            cdb[i] = 0;
        cdb[0] = 0x35;                  /* SYNCHRONIZE CACHE               */
        run(target[u], 10, 0, 0, 1000);
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
    case DRV_EJECT:
        eject();
        break;
    case DRV_DEINIT:                    /* DEVLOAD /U: Interrupts aus,    */
        wr8(SCSISEQ, 0);                /* keine Wiederauswahl mehr       */
        wr8(HCNTRL, PAUSE);
        r->status = DRV_OK;
        break;
    default:
        r->status = DRV_ERR_CMD;
    }
}
