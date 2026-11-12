/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - ISA Plug and Play (Intel/Microsoft, Version 1.0a)
 *
 * Laeuft einmal beim Start, vor der ersten DEVICE=-Zeile: Karten
 * isolieren (Initiation Key, serielle Isolation ueber den Lese-Port),
 * jeder eine CSN geben, die Ressourcendaten lesen und jedes logische
 * Geraet mit der ersten passenden Konfiguration einschalten. Die Treiber
 * (z. B. SB16.SYS) finden die Karte danach wie eine feste ISA-Karte.
 * Hat ein PnP-BIOS ein Geraet schon eingeschaltet, bleibt seine
 * Einstellung. CONFIG.SYS PNP=OFF schaltet alles ab.
 *
 * Ports: 279h (Adresse) und A79h (Daten) sind nach der Spezifikation
 * reine Schreibports - 279h ist das Statusregister von LPT2, Schreiben
 * dorthin ist harmlos (deshalb hat die Spezifikation es gewaehlt). Freie
 * Portbereiche fuer die Karten erkennt der Kernel nur lesend (alle Bytes
 * FFh, Karte noch aus).
 *
 * Nach der Spezifikation geschrieben; das Verhalten der Nachbildung in
 * 86Box (src/device/isapnp.c, GPL) nur gelesen.
 *
 * Laeuft im zweiten Codesegment (KTEXT2). PNP_HOST: Uebersetzung auf dem
 * Entwicklungsrechner gegen eine nachgebildete Karte (Test).
 */
#ifndef PNP_HOST
#include "kern.h"
#include "../include/drv.h"
#include "../include/fapi_abi.h"
#include "k2.h"
#define FAR __far
extern unsigned short bios_ports[7];     /* start.asm: COM1-4, LPT1-3     */
#else
#include "pnp_host.h"
#endif

#define PNP_ADDR    0x279
#define PNP_WDATA   0xA79

#define NPNP        8                   /* logische Geraete              */
#define NIO         4
static struct pnpdev {
    unsigned char  csn, ldn, act, rsv;  /* act: 1 eingeschaltet, 2 vom BIOS */
    unsigned char  vid[4];              /* Karte (EISA-Kennung)          */
    unsigned char  lid[4];              /* logisches Geraet              */
    unsigned short io[NIO];
    unsigned char  irq[2], dma[2];      /* 0 / 4 = keiner                */
} pnp[NPNP];
static unsigned npnp, pnp_rdp;
static unsigned char pass;              /* 0: nur Eingeschaltetes merken */

/* ---- Grundfunktionen ---------------------------------------------------- */
static void udelay(unsigned us)         /* ISA-Lesezugriff ~1 us, auch auf */
{                                       /* schnellen CPUs                 */
    while (us--)
        (void)inb(0x80);
}

static void wr(unsigned reg, unsigned v)
{
    outb(PNP_ADDR, reg);
    outb(PNP_WDATA, v);
}

static unsigned rd(unsigned reg)
{
    outb(PNP_ADDR, reg);
    return inb(pnp_rdp);
}

static void key(void)                   /* Initiation Key (LFSR ab 6Ah)  */
{
    unsigned code = 0x6A, i;
    outb(PNP_ADDR, 0);
    outb(PNP_ADDR, 0);
    for (i = 0; i < 32; i++) {
        outb(PNP_ADDR, code);
        code = (code >> 1) | (((code ^ (code >> 1)) & 1) << 7);
    }
}

/* naechstes Byte der Ressourcendaten (Status 05h Bit 0) -> 0..FFh, -1 */
static int rbyte(void)
{
    unsigned j;
    for (j = 0; j < 100; j++) {
        if (rd(0x05) & 1)
            return (int)rd(0x04);
        udelay(10);
    }
    return -1;
}

/* ---- Isolation ---------------------------------------------------------- */
/* eine Karte isolieren -> 1 gefunden (Kennung in id), 0 keine, -1 Stoerung */
static int isolate_one(unsigned char *id)
{
    unsigned i, b1, b2, bit, cs = 0x6A, got = 0, any = 0;
    for (i = 0; i < 9; i++)
        id[i] = 0;
    outb(PNP_ADDR, 0x01);               /* Serial Isolation              */
    udelay(1000);
    for (i = 0; i < 72; i++) {
        b1 = inb(pnp_rdp);
        udelay(250);
        b2 = inb(pnp_rdp);
        udelay(250);
        bit = b1 == 0x55 && b2 == 0xAA;
        if (b1 != 0xFF || b2 != 0xFF)
            any = 1;
        if (bit)
            id[i >> 3] |= (unsigned char)(1 << (i & 7));
        if (i < 64)
            cs = (cs >> 1) | ((((cs ^ (cs >> 1)) & 1) ^ bit) << 7);
        got |= bit;
    }
    if (!got)
        return any ? -1 : 0;
    return id[8] == (unsigned char)cs ? 1 : -1;
}

/* ---- Konfiguration wählen ----------------------------------------------- */
struct cfg {
    unsigned short io[NIO], iolen[NIO];
    unsigned char irq[2], dma[2], irqt[2];
    unsigned char nio, nirq, ndma, ok;
};
static struct cfg base, cur, best;      /* unabhaengig / laufend / gewaehlt */
static unsigned char have_best, in_dep, saw_dep;
static unsigned irq_used, dma_used;     /* Bitmasken (System + vergeben) */

static int io_taken(unsigned b, unsigned len, const struct cfg *c)
{
    unsigned i, k;
    for (i = 0; i < npnp; i++)
        for (k = 0; k < NIO; k++)
            if (pnp[i].io[k] && b < pnp[i].io[k] + 8 && pnp[i].io[k] < b + len)
                return 1;
    for (k = 0; k < c->nio; k++)
        if (c->io[k] && b < c->io[k] + c->iolen[k] && c->io[k] < b + len)
            return 1;
    for (k = 0; k < len; k++)           /* nur lesen: frei = alles FFh   */
        if (inb(b + k) != 0xFF)
            return 1;
    return 0;
}

/* E/A-Beschreibung: Basis min..max in Schritten align, Laenge len */
static void want_io(struct cfg *c, unsigned min, unsigned max, unsigned align, unsigned len)
{
    unsigned long b;
    if (c->nio >= NIO) {
        c->ok = 0;
        return;
    }
    if (!align)
        align = 1;
    if (len == 0) {                     /* Bereich nicht benutzt         */
        c->io[c->nio] = 0;
        c->iolen[c->nio++] = 0;
        return;
    }
    for (b = min; b <= max; b += align)
        if (b >= 0x100 && b + len <= 0x10000UL && !io_taken((unsigned)b, len, c)) {
            c->io[c->nio] = (unsigned short)b;
            c->iolen[c->nio++] = (unsigned short)len;
            return;
        }
    c->ok = 0;
}

static const unsigned char irq_pref[] = { 5, 7, 10, 9, 11, 15, 3, 4, 12, 14 };

static void want_irq(struct cfg *c, unsigned mask, unsigned info)
{
    unsigned i, k, n;
    if (c->nirq >= 2) {
        c->ok = 0;
        return;
    }
    if (!mask) {                        /* kein IRQ                      */
        c->irq[c->nirq] = 0;
        c->irqt[c->nirq++] = 2;
        return;
    }
    for (i = 0; i < sizeof(irq_pref); i++) {
        n = irq_pref[i];
        if (!(mask & (1u << n)) || (irq_used & (1u << n)))
            continue;
        for (k = 0; k < c->nirq; k++)
            if (c->irq[k] == n)
                break;
        if (k < c->nirq)
            continue;
        c->irq[c->nirq] = (unsigned char)n;
        /* 71h: Bit 1 Pegel hoch, Bit 0 pegelgesteuert; Vorgabe ISA =
           Flanke steigend (02h) */
        c->irqt[c->nirq++] = (unsigned char)(!(info & 1) && (info & 4) ? 3 :
                                             !(info & 1) && (info & 8) ? 1 :
                                             !(info & 1) && (info & 2) ? 0 : 2);
        return;
    }
    c->ok = 0;
}

static const unsigned char dma_pref[] = { 1, 3, 5, 6, 7, 0 };

static void want_dma(struct cfg *c, unsigned mask)
{
    unsigned i, n, k;
    if (c->ndma >= 2) {
        c->ok = 0;
        return;
    }
    if (!mask) {
        c->dma[c->ndma++] = 4;
        return;
    }
    for (i = 0; i < sizeof(dma_pref); i++) {
        n = dma_pref[i];                /* 0 zuletzt: Refresh auf alten PCs */
        if (!(mask & (1u << n)) || (dma_used & (1u << n)))
            continue;
        for (k = 0; k < c->ndma; k++)
            if (c->dma[k] == n)
                break;
        if (k < c->ndma)
            continue;
        c->dma[c->ndma++] = (unsigned char)n;
        return;
    }
    c->ok = 0;
}

static void cfg_copy(struct cfg *d, const struct cfg *s)
{
    const unsigned char *a = (const unsigned char *)s;
    unsigned char *b = (unsigned char *)d;
    unsigned i;
    for (i = 0; i < sizeof(struct cfg); i++)
        b[i] = a[i];
}

static void dep_end(void)               /* abhaengige Funktion zu Ende   */
{
    if (in_dep && cur.ok && !have_best) {
        cfg_copy(&best, &cur);
        have_best = 1;
    }
    in_dep = 0;
}

/* ---- logisches Geraet einschalten --------------------------------------- */
static void ld_done(unsigned csn, unsigned ldn, const unsigned char *vid,
                    const unsigned char *lid)
{
    struct pnpdev *d;
    const struct cfg *c;
    unsigned k;
    dep_end();
    if (saw_dep && !have_best)          /* ohne Ende-Eintrag: nichts passte */
        cur.ok = 0;
    if (npnp >= NPNP)
        return;
    d = &pnp[npnp];
    d->csn = (unsigned char)csn;
    d->ldn = (unsigned char)ldn;
    for (k = 0; k < 4; k++) {
        d->vid[k] = vid[k];
        d->lid[k] = lid[k];
    }
    wr(0x07, ldn);
    if (rd(0x30) & 1) {                 /* vom BIOS eingeschaltet: lassen */
        if (pass)
            return;                     /* schon im ersten Durchgang     */
        d->act = 2;
        for (k = 0; k < NIO; k++)
            d->io[k] = (unsigned short)(rd(0x60 + 2 * k) << 8 | rd(0x61 + 2 * k));
        for (k = 0; k < 2; k++) {
            d->irq[k] = (unsigned char)(rd(0x70 + 2 * k) & 15);
            d->dma[k] = (unsigned char)(rd(0x74 + k) & 7);
            if (d->irq[k])
                irq_used |= 1u << d->irq[k];
            if (d->dma[k] < 4)
                dma_used |= 1u << d->dma[k];
        }
        npnp++;
        return;
    }
    if (!pass)
        return;
    c = have_best ? &best : &cur;       /* ohne abhaengige Funktionen    */
    if (!c->ok)
        return;                         /* nichts passt: aus lassen      */
    if (!have_best && !c->nio && !c->nirq && !c->ndma)
        return;                         /* braucht nichts (nichts tun)   */
    for (k = 0; k < NIO; k++) {
        d->io[k] = k < c->nio ? c->io[k] : 0;
        wr(0x60 + 2 * k, d->io[k] >> 8);
        wr(0x61 + 2 * k, d->io[k] & 0xFF);
    }
    for (k = 0; k < 2; k++) {
        d->irq[k] = k < c->nirq ? c->irq[k] : 0;
        d->dma[k] = k < c->ndma ? c->dma[k] : 4;
        wr(0x70 + 2 * k, d->irq[k]);
        wr(0x71 + 2 * k, k < c->nirq ? c->irqt[k] : 2);
        wr(0x74 + k, d->dma[k]);
        if (d->irq[k])
            irq_used |= 1u << d->irq[k];
        if (d->dma[k] < 4)
            dma_used |= 1u << d->dma[k];
    }
    wr(0x31, 0);                        /* keine Bereichspruefung        */
    wr(0x30, 1);                        /* einschalten                   */
    d->act = 1;
    npnp++;
}

static void cfg_reset(void)
{
    unsigned char *b = (unsigned char *)&base;
    unsigned i;
    for (i = 0; i < sizeof(struct cfg); i++)
        b[i] = 0;
    base.ok = 1;
    cfg_copy(&cur, &base);
    have_best = in_dep = saw_dep = 0;
}

/* Ressourcendaten einer Karte lesen und ihre Geraete einschalten */
static void card(unsigned csn)
{
    unsigned char vid[9], lid[4], t[8];
    int c, i, len, tag, ldn = -1;
    unsigned k;
    wr(0x03, csn);                      /* Wake[CSN]                     */
    udelay(100);
    for (i = 0; i < 9; i++) {           /* Kennung kommt noch einmal     */
        if ((c = rbyte()) < 0)
            return;
        vid[i] = (unsigned char)c;
    }
    for (k = 0; k < 4; k++)
        lid[k] = 0;
    cfg_reset();
    for (;;) {
        if ((c = rbyte()) < 0)
            break;
        if (c & 0x80) {                 /* langer Eintrag: Laenge 16 Bit */
            int l1 = rbyte(), l2 = rbyte();
            if (l1 < 0 || l2 < 0)
                break;
            len = l1 | l2 << 8;
            tag = 0x100 | (c & 0x7F);
        } else {
            len = c & 7;
            tag = (c >> 3) & 0x0F;
        }
        for (i = 0; i < len; i++) {
            if ((c = rbyte()) < 0)
                goto out;
            if (i < 8)
                t[i] = (unsigned char)c;
        }
        switch (tag) {
        case 0x02:                      /* logisches Geraet              */
            if (ldn >= 0)
                ld_done(csn, (unsigned)ldn, vid, lid);
            ldn++;
            for (k = 0; k < 4; k++)
                lid[k] = t[k];
            cfg_reset();
            break;
        case 0x04:                      /* IRQ                           */
            want_irq(in_dep ? &cur : &base, t[0] | t[1] << 8, len >= 3 ? t[2] : 1);
            if (!in_dep)
                cfg_copy(&cur, &base);
            break;
        case 0x05:                      /* DMA                           */
            want_dma(in_dep ? &cur : &base, t[0]);
            if (!in_dep)
                cfg_copy(&cur, &base);
            break;
        case 0x06:                      /* Anfang abhaengige Funktion    */
            dep_end();
            cfg_copy(&cur, &base);
            in_dep = saw_dep = 1;
            break;
        case 0x07:                      /* Ende abhaengige Funktionen    */
            dep_end();
            if (have_best)              /* danach unabhaengig weiter     */
                cfg_copy(&base, &best);
            else
                base.ok = 0;            /* keine Funktion passt          */
            cfg_copy(&cur, &base);
            have_best = saw_dep = 0;
            break;
        case 0x08:                      /* E/A-Bereich (Basis variabel)  */
            want_io(in_dep ? &cur : &base, t[1] | t[2] << 8, t[3] | t[4] << 8, t[5], t[6]);
            if (!in_dep)
                cfg_copy(&cur, &base);
            break;
        case 0x09:                      /* fester E/A-Bereich (10 Bit)   */
            k = (t[0] | t[1] << 8) & 0x3FF;
            want_io(in_dep ? &cur : &base, k, k, 1, t[2]);
            if (!in_dep)
                cfg_copy(&cur, &base);
            break;
        case 0x0F:                      /* Ende                          */
            goto out;
        default:                        /* Speicher, Namen, Kennungen    */
            break;
        }
    }
out:
    if (ldn >= 0)
        ld_done(csn, (unsigned)ldn, vid, lid);
}

/* ---- Einstieg ----------------------------------------------------------- */
/* IRQs/DMA, die schon jemand hat (PCI, COM, feste Systemleitungen) */
static void used_init(void)
{
    unsigned i;
    irq_used = 0x0001 | 0x0002 | 0x0004 | 0x0040 | 0x0100 | 0x2000;  /* 0 1 2 6 8 13 */
    irq_used |= 0x1000 | 0x4000 | 0x8000;  /* 12 PS/2-Maus, 14/15 IDE   */
    if (bios_ports[0] || bios_ports[2])
        irq_used |= 0x0010;             /* COM1/COM3: IRQ 4             */
    if (bios_ports[1] || bios_ports[3])
        irq_used |= 0x0008;             /* COM2/COM4: IRQ 3             */
    for (i = 0; i < 32; i++) {          /* PCI-Geraete                   */
        struct fapi_pcidev pd;
        if (pci_info(i, &pd))
            break;
        if (pd.irq && pd.irq < 16)
            irq_used |= 1u << pd.irq;
    }
    dma_used = 0x0004 | 0x0010;         /* 2 Diskette, 4 Kaskade        */
}

/* -> Anzahl eingetragener logischer Geraete */
unsigned FAR pnp_init(void)
{
    static const unsigned short rdps[] = { 0x20B, 0x27B, 0x2BB, 0x37B, 0x3BB, 0x213, 0x3E3 };
    unsigned char id[9];
    unsigned r, csn = 0, f;
    int k;
    used_init();
    for (r = 0; r < sizeof(rdps) / sizeof(rdps[0]); r++) {
        pnp_rdp = rdps[r];
        if (inb(pnp_rdp) != 0xFF)       /* belegt: anderen Lese-Port     */
            continue;
        f = irq_off();
        key();
        wr(0x02, 0x04);                 /* alle CSN auf 0                */
        irq_restore(f);
        udelay(2000);
        f = irq_off();
        key();
        wr(0x03, 0x00);                 /* Wake[0]: alle in die Isolation */
        wr(0x00, pnp_rdp >> 2);         /* Lese-Port setzen              */
        irq_restore(f);
        udelay(1000);
        csn = 0;
        for (;;) {
            k = isolate_one(id);
            if (k <= 0)
                break;
            wr(0x06, ++csn);            /* CSN geben -> Config           */
            if (csn == 15)
                break;
            wr(0x03, 0x00);             /* uebrige wieder isolieren      */
        }
        if (k < 0 && csn == 0)
            continue;                   /* Stoerung am Port: naechster   */
        break;
    }
    for (pass = 0; pass < 2; pass++)    /* erst, was das BIOS schon      */
        for (r = 1; r <= csn; r++)      /* eingeschaltet hat, dann den   */
            card(r);                    /* Rest vergeben                 */
    wr(0x02, 0x02);                     /* alle: Wait for Key            */
    return npnp;
}

/* fuer FAPI_BUSINFO / Startmeldung: Eintrag i -> 0, sonst 1 */
int FAR pnp_info(unsigned i, struct fapi_pnpdev FAR *p)
{
    unsigned k;
    if (i >= npnp)
        return 1;
    p->csn = pnp[i].csn;
    p->ldn = pnp[i].ldn;
    p->act = pnp[i].act;
    p->rsv = 0;
    for (k = 0; k < 4; k++) {
        p->vid[k] = pnp[i].vid[k];
        p->lid[k] = pnp[i].lid[k];
        p->io[k] = pnp[i].io[k];
    }
    for (k = 0; k < 2; k++) {
        p->irq[k] = pnp[i].irq[k];
        p->dma[k] = pnp[i].dma[k];
    }
    return 0;
}

/* Startmeldung fuer Eintrag i: "CTL0031 an 220h 330h, IRQ 5, DMA 1 5" */
static char FAR *hx(char FAR *o, unsigned v, unsigned n)
{
    while (n--)
        *o++ = "0123456789ABCDEF"[(v >> (4 * n)) & 15];
    return o;
}

void FAR pnp_line(unsigned i, char FAR *buf)
{
    const struct pnpdev *d = &pnp[i];
    char FAR *o = buf;
    unsigned k;
    /* EISA-Kennung: 3 Buchstaben zu je 5 Bit, dann 4 Hexziffern */
    *o++ = (char)('@' + ((d->lid[0] >> 2) & 31));
    *o++ = (char)('@' + (((d->lid[0] & 3) << 3) | (d->lid[1] >> 5)));
    *o++ = (char)('@' + (d->lid[1] & 31));
    o = hx(o, d->lid[2], 2);
    o = hx(o, d->lid[3], 2);
    for (k = 0; k < NIO; k++)
        if (d->io[k]) {
            *o++ = ' ';
            o = hx(o, d->io[k], d->io[k] > 0xFFF ? 4 : 3);
            *o++ = 'h';
        }
    for (k = 0; k < 2; k++)
        if (d->irq[k]) {
            *o++ = ',';
            *o++ = ' ';
            *o++ = 'I';
            *o++ = 'R';
            *o++ = 'Q';
            *o++ = ' ';
            if (d->irq[k] >= 10)
                *o++ = '1';
            *o++ = (char)('0' + d->irq[k] % 10);
        }
    for (k = 0; k < 2; k++)
        if (d->dma[k] < 4 || (d->dma[k] > 4 && d->dma[k] < 8)) {
            *o++ = ',';
            *o++ = ' ';
            *o++ = 'D';
            *o++ = 'M';
            *o++ = 'A';
            *o++ = ' ';
            *o++ = (char)('0' + d->dma[k]);
        }
    if (d->act == 2) {
        const char *s = " (vom BIOS)";
        while (*s)
            *o++ = *s++;
    }
    *o = 0;
}
