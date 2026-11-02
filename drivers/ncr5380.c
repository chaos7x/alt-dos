/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NCR5380.SYS - SCSI-Karten mit NCR 5380 / 53C400 (ISA, ohne Busmaster)
 *
 * Diese Fassung kennt den Trantor T130B (53C400, I/O-Ports 350h, 340h,
 * 250h oder 240h; die 5380-Register liegen ab Basis+8). Befehl, Status
 * und Meldungen laufen per PIO (jedes Byte mit dem REQ/ACK-Handshake des
 * 5380), Daten in Vielfachen von 128 Byte im Blockbetrieb des 53C400: Der
 * Chip macht den Handshake selbst, der Treiber holt bzw. fuellt nur seinen
 * 128-Byte-Puffer (Basis+4) am Stueck. Steuerregister Basis+0 (Bit 6 =
 * Richtung lesen; Status Bit 2 = Puffer nicht bereit, Bit 7 = 5380
 * erreichbar), Blockzaehler Basis+1. Scheitert der Blockbetrieb, schaltet
 * der Treiber auf PIO zurueck (Meldung); /PIO erzwingt PIO. Laesst ein
 * Geraet auf sich warten (Platte sucht, CD laeuft an), schlaeft der Task
 * kurz (ProcBlock mit Frist), andere laufen weiter. Geplant: die Karten
 * mit Registern im
 * Speicherfenster (Longshine LCS-6821N, Rancho RT1000B) ueber eine DevHlp
 * fuer physischen Speicher (wie PhysToVirt bei OS/2).
 *
 * Erkennung, erst lesen: Ein freier Port liefert FFh. Beim 5380 sind im
 * Moduswert Bit 6 (Zielmodus) und im Zielbefehlsregister die Bits 4-6
 * gelesen 0. Erst dann dient das Zielbefehlsregister als Probe: Es wirkt
 * nur im Zielmodus auf den Bus, als Initiator ist Schreiben harmlos.
 * DEVICE=NCR5380.SYS /P:340 prueft nur diese Adresse.
 */
#include "drvlib.h"

/* 5380-Register (ab Basis+8 beim T130B) */
#define R_CSD   0                       /* lesen: Daten auf dem Bus     */
#define R_ODR   0                       /* schreiben: Ausgabedaten      */
#define R_ICR   1                       /* Initiator-Befehl             */
#define R_MR    2                       /* Modus                        */
#define R_TCR   3                       /* Zielbefehl (Phase)           */
#define R_CSBS  4                       /* lesen: Busleitungen          */
#define R_BSR   5                       /* lesen: Bus und Status        */
#define R_RPI   7                       /* lesen: Paritaet/IRQ loeschen */

#define ICR_RST   0x80
#define ICR_AIP   0x40                  /* Arbitrierung laeuft          */
#define ICR_LA    0x20                  /* Arbitrierung verloren        */
#define ICR_ACK   0x10
#define ICR_BSY   0x08
#define ICR_SEL   0x04
#define ICR_ATN   0x02
#define ICR_DBUS  0x01                  /* Daten auf den Bus legen      */
#define MR_ARB    0x01
#define MR_DMA    0x02
#define MR_MBSY   0x04                  /* BSY ueberwachen              */
#define R_SDS     5                     /* schreiben: DMA senden starten */
#define R_SDIR    7                     /* schreiben: DMA empfangen starten */

/* 53C400 (T130B: ab Basis+0) */
#define C4_CTL    0                     /* Steuerung / Status           */
#define C4_BLK    1                     /* Blockzaehler (je 128 Byte)   */
#define C4_BUF    4                     /* Puffer                       */
#define C4_DIR    0x40                  /* Richtung: zum Rechner        */
#define C4_NRDY   0x04                  /* Puffer nicht bereit          */
#define C4_5380   0x80                  /* 5380-Register erreichbar     */
#define CS_RST    0x80
#define CS_BSY    0x40
#define CS_REQ    0x20
#define CS_SEL    0x02
#define BSR_PHSM  0x08                  /* Phase passt                  */

/* Phasen (MSG C/D I/O, wie TCR Bit 2..0) */
#define PH_DATAOUT  0
#define PH_DATAIN   1
#define PH_CMD      2
#define PH_STATUS   3
#define PH_MSGOUT   6
#define PH_MSGIN    7

#define HOST_ID     7

static const unsigned t130b_ports[] = { 0x350, 0x340, 0x250, 0x240 };

static unsigned base, r5;               /* Kartenbasis, 5380-Register   */
static unsigned char blockmode;         /* 53C400-Puffer benutzen       */
static unsigned char target[DRV_MAXUNITS];
static unsigned bsz[DRV_MAXUNITS];
static unsigned nunits;
static char line[128];
static unsigned char cdb[12];


#define IN(r)       inp(r5 + (r))
#define OUT(r, v)   outp(r5 + (r), (unsigned char)(v))

/* kurz warten, dann schlafen: bis (CSBS & m) == v; t Takte; 0 = ok */
static int wait_bus(unsigned char m, unsigned char v, unsigned t)
{
    unsigned start = ticks(), spin;
    for (;;) {
        for (spin = 0; spin < 2000; spin++)
            if ((IN(R_CSBS) & m) == v)
                return 0;
        if ((unsigned)(ticks() - start) > t)
            return -1;
        cli();
        proc_block(0xFFFF, 1);          /* niemand weckt: 1 Takt schlafen */
    }
}

/* Lesen: sieht base wie ein T130B aus? */
static int looks_like_t130b(unsigned b)
{
    unsigned r = b + 8;
    unsigned char mr = inp(r + R_MR), tcr = inp(r + R_TCR);
    unsigned char cs = inp(r + R_CSBS), bs = inp(r + R_BSR);

    if (mr == 0xFF || tcr == 0xFF || cs == 0xFF || bs == 0xFF)
        return 0;
    if ((mr & 0x40) || (tcr & 0x70))
        return 0;
    outp(r + R_TCR, 0x05);              /* Probe: nur im Zielmodus wirksam */
    if ((inp(r + R_TCR) & 0x0F) != 0x05)
        return 0;
    outp(r + R_TCR, 0x0A);
    if ((inp(r + R_TCR) & 0x0F) != 0x0A)
        return 0;
    outp(r + R_TCR, 0x00);
    return 1;
}

/* Status des 53C400: kurz warten, dann schlafen, bis (Status & m) == v */
static int c4_wait(unsigned char m, unsigned char v, unsigned t)
{
    unsigned start = ticks(), spin;
    for (;;) {
        for (spin = 0; spin < 2000; spin++)
            if ((inp(base + C4_CTL) & m) == v)
                return 0;
        if ((unsigned)(ticks() - start) > t)
            return -1;
        cli();
        proc_block(0xFFFF, 1);
    }
}

/* len Bytes (Vielfaches von 128) im Blockbetrieb; Phase steht im TCR.
   0 = ok, sonst ist der Bus zurueckgesetzt (Aufrufer wiederholt per PIO) */
static int block_xfer(int out, unsigned sel, unsigned off, unsigned len)
{
    unsigned n = len >> 7, i;

    /* Reihenfolge wie im Datenblatt (und bei Linux): erst den 5380 in den
       DMA-Betrieb und starten, dann Richtung und Blockzaehler - der 53C400
       beginnt beim Laden des Zaehlers. Busueberwachung: faellt BSY weg,
       endet der Blockbetrieb von selbst (sonst bleibt der Chip haengen) */
    OUT(R_MR, MR_DMA | MR_MBSY);
    if (out) {
        OUT(R_ICR, ICR_DBUS);
        OUT(R_SDS, 0);
    } else {
        OUT(R_ICR, 0);
        OUT(R_SDIR, 0);
    }
    outp(base + C4_CTL, out ? 0 : C4_DIR);
    outp(base + C4_BLK, (unsigned char)n);
    for (i = 0; i < n; i++) {
        if (c4_wait(C4_NRDY, 0, 300))
            goto fail;
        if (out)
            outsb_far(base + C4_BUF, sel, off + (i << 7), 128);
        else
            insb_far(base + C4_BUF, MK_FP(sel, off + (i << 7)), 128);
    }
    if (out && c4_wait(C4_NRDY, 0, 300))    /* letzter Block gesendet  */
        goto fail;
    if (c4_wait(C4_5380, C4_5380, 300))
        goto fail;
    OUT(R_MR, 0);
    OUT(R_ICR, 0);
    IN(R_RPI);
    return 0;
fail:
    OUT(R_MR, 0);
    outp(base + C4_CTL, 0);
    OUT(R_ICR, ICR_RST);                /* Bus zuruecksetzen            */
    delay(1);
    OUT(R_ICR, 0);
    IN(R_RPI);
    return -1;
}

/* ---- Busphasen --------------------------------------------------------- */
/* ein Byte als Initiator senden (Phase steht im TCR); 0 = ok */
static int put_byte(unsigned char c)
{
    if (wait_bus(CS_REQ, CS_REQ, 300) || !(IN(R_BSR) & BSR_PHSM))
        return -1;
    OUT(R_ODR, c);
    OUT(R_ICR, ICR_DBUS);
    OUT(R_ICR, ICR_DBUS | ICR_ACK);
    if (wait_bus(CS_REQ, 0, 300))
        return -1;
    OUT(R_ICR, 0);
    return 0;
}

static int get_byte(void)
{
    unsigned char c;
    if (wait_bus(CS_REQ, CS_REQ, 300) || !(IN(R_BSR) & BSR_PHSM))
        return -1;
    c = IN(R_CSD);
    OUT(R_ICR, ICR_ACK);
    if (wait_bus(CS_REQ, 0, 300))
        return -1;
    OUT(R_ICR, 0);
    return c;
}

/* Arbitrierung und Auswahl; 0 = Ziel hat geantwortet, 1 = kein Geraet */
static int select(unsigned id)
{
    unsigned i;

    if (wait_bus(CS_BSY | CS_SEL, 0, 100))
        return -1;                      /* Bus bleibt belegt            */
    OUT(R_TCR, 0);
    OUT(R_ODR, 1 << HOST_ID);
    OUT(R_MR, MR_ARB);
    for (i = 0; i < 10000 && !(IN(R_ICR) & ICR_AIP); i++)
        ;
    if (!(IN(R_ICR) & ICR_AIP) || (IN(R_ICR) & ICR_LA)) {
        OUT(R_MR, 0);
        return -1;
    }
    OUT(R_ICR, ICR_SEL);                /* gewonnen                     */
    OUT(R_ODR, (1 << HOST_ID) | (1 << id));
    OUT(R_ICR, ICR_SEL | ICR_BSY | ICR_DBUS);
    OUT(R_MR, 0);
    OUT(R_ICR, ICR_SEL | ICR_DBUS);     /* BSY los: Ziel waehlen        */
    if (wait_bus(CS_BSY, CS_BSY, 25)) { /* 250 ms                       */
        OUT(R_ICR, 0);
        return 1;
    }
    OUT(R_ICR, 0);                      /* SEL los, ohne ATN: Befehl folgt */
    return 0;
}

#define SCSI_OK     0
#define SCSI_NODEV  1
#define SCSI_CHECK  2
#define SCSI_FAIL   3

/* SCSI-Befehl cdb[0..cdblen) mit len Bytes Daten bei sel:off */
static int scsi(unsigned id, unsigned cdblen, unsigned sel, unsigned off,
                unsigned len)
{
    unsigned char __far *p = MK_FP(sel, off);
    unsigned ci = 0, di = 0, ph;
    int c, status = -1, rc;

    rc = select(id);
    if (rc)
        return rc > 0 ? SCSI_NODEV : SCSI_FAIL;
    for (;;) {
        /* auf REQ warten (lange: Platte sucht); Bus frei = Ende */
        if (wait_bus(CS_REQ | CS_BSY, CS_REQ | CS_BSY, 1000)) {
            if (!(IN(R_CSBS) & CS_BSY))
                break;
            OUT(R_ICR, ICR_RST);        /* haengt: Bus zuruecksetzen    */
            delay(1);
            OUT(R_ICR, 0);
            IN(R_RPI);
            return SCSI_FAIL;
        }
        ph = (IN(R_CSBS) >> 2) & 7;
        OUT(R_TCR, ph);
        switch (ph) {
        case PH_CMD:
            c = put_byte(ci < cdblen ? cdb[ci] : 0);
            ci++;
            break;
        case PH_DATAOUT:
        case PH_DATAIN:
            if (blockmode && di == 0 && len >= 128 && !(len & 127)) {
                if (block_xfer(ph == PH_DATAOUT, sel, off, len)) {
                    blockmode = 0;      /* zurueck auf PIO, Aufrufer wiederholt */
                    dputs("NCR5380.SYS: Blockbetrieb gescheitert, weiter mit PIO\n");
                    return SCSI_FAIL;
                }
                di = len;
                c = 0;
                break;
            }
            if (ph == PH_DATAOUT)
                c = put_byte(di < len ? p[di] : 0);
            else {
                c = get_byte();
                if (c >= 0 && di < len)
                    p[di] = (unsigned char)c;
            }
            di++;
            break;
        case PH_STATUS:
            c = get_byte();
            status = c;
            break;
        case PH_MSGIN:
            c = get_byte();             /* 0 = fertig; Rest uebergehen  */
            if (c == 0) {
                OUT(R_TCR, 0);
                wait_bus(CS_BSY, 0, 25);
                return status == 0 ? SCSI_OK : status == 2 ? SCSI_CHECK : SCSI_FAIL;
            }
            break;
        case PH_MSGOUT:
            c = put_byte(0x08);         /* NOP (wir setzen kein ATN)    */
            break;
        default:
            c = -1;
        }
        if (c < 0)
            break;
    }
    OUT(R_TCR, 0);
    OUT(R_ICR, 0);
    return status == 0 ? SCSI_OK : status == 2 ? SCSI_CHECK : SCSI_FAIL;
}

static void cdb6(unsigned char op, unsigned char alloc)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        cdb[i] = 0;
    cdb[0] = op;
    cdb[4] = alloc;
}

/* REQUEST SENSE nach Check Condition (der 5380 kennt kein Auto-Sense):
   Sense Key << 8 | ASC, 0 = keine Antwort */
static unsigned char sense[18];

static unsigned req_sense(unsigned id)
{
    void __far *sp = (void __far *)sense;
    cdb6(0x03, sizeof(sense));
    if (scsi(id, 6, FP_SEG(sp), FP_OFF(sp), sizeof(sense)) != SCSI_OK)
        return 0;
    return ((sense[2] & 0x0F) << 8) | sense[12];
}

static char *hex3(char *p, unsigned v)
{
    return hex(p, v, 3);
}

/* ---- Initialisierung --------------------------------------------------- */
static void init(void)
{
    struct drv_req __far *r = REQ;
    unsigned char __far *buf = MK_FP(r->buf_sel, r->buf_off);
    unsigned i, id, n = 0, only = 0;
    int k, rc;
    char *p;

    r->units = 0;
    if (has_opt(r->args, "/P:350")) only = 0x350;
    else if (has_opt(r->args, "/P:340")) only = 0x340;
    else if (has_opt(r->args, "/P:250")) only = 0x250;
    else if (has_opt(r->args, "/P:240")) only = 0x240;
    base = 0;
    for (i = 0; i < sizeof(t130b_ports) / sizeof(t130b_ports[0]); i++)
        if ((!only || only == t130b_ports[i]) && looks_like_t130b(t130b_ports[i])) {
            base = t130b_ports[i];
            break;
        }
    if (!base) {
        r->status = DRV_ERR_NODEV;
        return;
    }
    r5 = base + 8;
    OUT(R_MR, 0);
    OUT(R_ICR, 0);
    OUT(R_TCR, 0);
    IN(R_RPI);
    blockmode = !has_opt(r->args, "/PIO");
    outp(base + C4_CTL, 0);             /* 53C400: schreiben, kein IRQ  */
    claim(RES_IO, base, 16);
    p = scat(line, "  NCR 53C400 (Trantor T130B) an Port ");
    p = hex3(p, base);
    scat(p, blockmode ? "h, Blockbetrieb\n" : "h, PIO\n");
    dputs(line);

    for (id = 0; id < 7; id++) {        /* eigene ID 7                  */
        cdb6(0x12, 36);                 /* INQUIRY                      */
        if (scsi(id, 6, r->buf_sel, r->buf_off, 36) != SCSI_OK)
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
            for (k = 0; k < 4; k++) {   /* Unit Attention abholen       */
                cdb6(0x00, 0);
                if (scsi(id, 6, r->buf_sel, r->buf_off, 0) == SCSI_OK)
                    break;
            }
            for (k = 0; k < 12; k++)
                cdb[k] = 0;
            cdb[0] = 0x25;              /* READ CAPACITY                */
            rc = scsi(id, 10, r->buf_sel, r->buf_off, 8);
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
                if (scsi(id, 6, r->buf_sel, r->buf_off, 0) == SCSI_OK)
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
    r->units = (unsigned char)n;
    r->count = 4;                       /* DMABUF: 4 Sektoren (16 Bloecke) */
    r->status = n ? DRV_OK : DRV_ERR_NODEV;
}

static void report(int write, unsigned u, unsigned long l, int rc, int t)
{
    char *p = scat(line, write ? "NCR5380.SYS: Schreibfehler, SCSI-ID " :
                                 "NCR5380.SYS: Lesefehler, SCSI-ID ");
    p = udec(p, target[u]);
    p = scat(p, ", Sektor ");
    p = hex(p, l, 8);
    p = scat(p, rc == SCSI_NODEV ? "h, Ger\x84t antwortet nicht" :
                rc == SCSI_CHECK ? "h, Check Condition" : "h, Busfehler");
    p = scat(p, ", Versuch ");
    p = udec(p, (unsigned)t + 1);
    scat(p, "\n");
    dputs(line);
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
    if (bsz[r->unit] != 512)
        n = 1;
    for (tries = 0; tries < 3; tries++) {
        cdb[0] = write ? 0x2A : 0x28;   /* WRITE(10) / READ(10)         */
        cdb[1] = 0;
        cdb[2] = (unsigned char)(l >> 24);
        cdb[3] = (unsigned char)(l >> 16);
        cdb[4] = (unsigned char)(l >> 8);
        cdb[5] = (unsigned char)l;
        cdb[6] = 0;
        cdb[7] = 0;
        cdb[8] = (unsigned char)n;
        cdb[9] = 0;
        rc = scsi(target[r->unit], 10, r->buf_sel, r->buf_off, n * bsz[r->unit]);
        if (rc == SCSI_OK) {
            r->status = DRV_OK;
            return;
        }
        /* CD: keine drin (2/3Ah, beim zweiten Mal - nach einem Wechsel
           meldet manches Laufwerk das einmal); laeuft an (2/04h): warten */
        sk = 0;
        if (rc == SCSI_CHECK && bsz[r->unit] != 512) {
            sk = req_sense(target[r->unit]);
            if (sk == 0x23A && tries > 0) {
                r->status = DRV_ERR_NOTREADY;
                return;
            }
            if (sk == 0x204 && spin < 20) {
                spin++;
                tries--;
                cli();
                proc_block(0xFFFF, 50);
                continue;
            }
        }
        if (!(rc == SCSI_CHECK && bsz[r->unit] != 512 && (tries == 0 || sk == 0x23A)))
            report(write, r->unit, l, rc, tries);
    }
    r->status = DRV_ERR_IO;
}

static void flush(void)
{
    struct drv_req __far *r = REQ;
    unsigned u, i;
    for (u = 0; u < nunits; u++) {
        if (bsz[u] != 512)
            continue;
        for (i = 0; i < 12; i++)
            cdb[i] = 0;
        cdb[0] = 0x35;                  /* SYNCHRONIZE CACHE            */
        scsi(target[u], 10, r->buf_sel, r->buf_off, 0);
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
    default:
        r->status = DRV_ERR_CMD;
    }
}
