/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Busdienste fuer Treiber: PCI und Belegungsliste
 *
 * PCI (Konfigurationsmechanismus 1, Ports CF8h/CFCh mit 32 Bit): beim
 * Start einmal durchsuchen (Bus 0, dahinter liegende Busse ueber die
 * PCI-PCI-Bruecken), Geraete in pci_tab merken. Treiber finden ihre
 * Karte ueber DevHlp DH_PCIFIND/DH_PCICLASS und lesen/schreiben die
 * Konfiguration mit DH_PCIREAD/DH_PCIWRITE - kein Treiber spricht CF8h
 * selbst an (wie OEMHLP$ bei OS/2). Mechanismus 2 (nur fruehe PCI-
 * Boards 1992/93, z. B. ASUS PCI/I-486SP3 mit Intel 420TX/ZX): Byte an
 * CF8h = Schluessel (Bit 4-7) + Funktion (Bit 1-3), CFAh = Bus, Register
 * von Geraet d bei C000h + d * 100h (16 Geraete je Bus).
 *
 * Belegungsliste: Treiber melden Ports, DMA-Kanaele und IRQs an
 * (DH_CLAIM, DH_SETIRQ traegt IRQs selbst ein). Eine zweite Anmeldung
 * desselben Bereichs durch einen anderen Treiber wird abgelehnt; MSD
 * zeigt die Liste (FAPI_BUSINFO).
 *
 * Laeuft im zweiten Codesegment (KTEXT2, siehe k2.h).
 */
#include "kern.h"
#include "../include/drv.h"
#include "../include/fapi_abi.h"
#include "k2.h"

/* ---- 32-Bit-Ports ------------------------------------------------------- */
static unsigned long k2_inl(unsigned port);
#pragma aux k2_inl = "in eax,dx" "mov edx,eax" "shr edx,16" \
    __parm [__dx] __value [__dx __ax] __modify __exact [__ax __dx];
static void k2_outl(unsigned port, unsigned lo, unsigned hi);
#pragma aux k2_outl = "push ebx" "shl ebx,16" "mov bx,ax" "xchg eax,ebx" \
    "out dx,eax" "xchg eax,ebx" "pop ebx" \
    __parm [__dx] [__ax] [__bx] __modify __exact [];

/* ---- PCI ---------------------------------------------------------------- */
#define NPCI    32
static struct {
    unsigned short loc;                 /* Bus << 8 | Geraet << 3 | Funktion */
    unsigned short vendor, device;
    unsigned char  cls, sub, progif, irq;
} pci_tab[NPCI];
static unsigned npci;
static unsigned char pci_ok;            /* 1 Mechanismus 1, 2 Mechanismus 2 */
static unsigned long k2_inl_p(unsigned port) { return k2_inl(port); }

static void cfg_addr(unsigned loc, unsigned reg)
{
    k2_outl(0xCF8, ((loc & 0xFF) << 8) | (reg & 0xFC), 0x8000 | (loc >> 8));
}

/* Mechanismus 2: Konfigurationsraum ein (Funktion, Bus) -> Portbasis */
static unsigned m2_on(unsigned loc)
{
    outb(0xCF8, 0xF0 | ((loc & 7) << 1));
    outb(0xCFA, loc >> 8);
    return 0xC000 | (((loc >> 3) & 15) << 8);
}

unsigned long __far pci_read(unsigned loc, unsigned reg)
{
    unsigned long v;
    unsigned f;
    if (!pci_ok)
        return 0xFFFFFFFFUL;
    f = irq_off();                      /* Adresse + Daten nicht trennen */
    if (pci_ok == 2) {
        v = k2_inl_p(m2_on(loc) | (reg & 0xFC));
        outb(0xCF8, 0);
    } else {
        cfg_addr(loc, reg);
        v = k2_inl(0xCFC);
    }
    irq_restore(f);
    return v;
}

/* size 1, 2 oder 4 Byte; reg darf bei 1/2 auf das Byte zeigen */
void __far pci_write(unsigned loc, unsigned reg, unsigned long v, unsigned size)
{
    unsigned f;
    if (!pci_ok)
        return;
    f = irq_off();
    if (pci_ok == 2) {
        unsigned b = m2_on(loc) | (reg & 0xFC);
        if (size == 4)
            k2_outl(b, (unsigned)v, (unsigned)(v >> 16));
        else {
            outb(b + (reg & 3), (unsigned char)v);
            if (size == 2)
                outb(b + (reg & 3) + 1, (unsigned char)(v >> 8));
        }
        outb(0xCF8, 0);
    } else {
        cfg_addr(loc, reg);
        if (size == 4)
            k2_outl(0xCFC, (unsigned)v, (unsigned)(v >> 16));
        else {
            outb(0xCFC + (reg & 3), (unsigned char)v);
            if (size == 2)
                outb(0xCFD + (reg & 3), (unsigned char)(v >> 8));
        }
    }
    irq_restore(f);
}

static void scan_bus(unsigned bus, unsigned depth)
{
    unsigned dev, fn, nfn, loc;
    unsigned long id, cl, hd;

    for (dev = 0; dev < (pci_ok == 2 ? 16u : 32u); dev++) {
        nfn = 1;
        for (fn = 0; fn < nfn; fn++) {
            loc = (bus << 8) | (dev << 3) | fn;
            id = pci_read(loc, 0);
            if ((unsigned)id == 0xFFFF || (unsigned)id == 0)
                continue;
            hd = pci_read(loc, 0x0C);
            if (fn == 0 && (hd & 0x00800000UL))
                nfn = 8;                /* Mehrfunktionsgeraet           */
            cl = pci_read(loc, 0x08);
            if (npci < NPCI) {
                pci_tab[npci].loc = loc;
                pci_tab[npci].vendor = (unsigned)id;
                pci_tab[npci].device = (unsigned)(id >> 16);
                pci_tab[npci].cls = (unsigned char)(cl >> 24);
                pci_tab[npci].sub = (unsigned char)(cl >> 16);
                pci_tab[npci].progif = (unsigned char)(cl >> 8);
                pci_tab[npci].irq = (unsigned char)pci_read(loc, 0x3C);
                npci++;
            }
            /* PCI-PCI-Bruecke: Bus dahinter (Register 19h) durchsuchen */
            if (((hd >> 16) & 0x7F) == 1 && depth < 4) {
                unsigned sec = (unsigned)(pci_read(loc, 0x18) >> 8) & 0xFF;
                if (sec > bus)
                    scan_bus(sec, depth + 1);
            }
        }
    }
}

/* Mechanismus 1 da? Erst lesen, dann das Freigabebit probieren. Beim
   PCI-Host lesen sich die Bits 0-1 von CF8h immer als 0; ein ISA-Rechner
   liefert dort FFh (offener Bus) - dann nichts schreiben, denn ISA-Karten
   dekodieren nur 10 Adressbits: CF8h waere 0F8h, der Koprozessor. */
unsigned __far pci_init(void)
{
    unsigned long old, v;
    unsigned f = irq_off();
    old = k2_inl(0xCF8);
    if (!(old & 3)) {                   /* 32-Bit-Register da?           */
        k2_outl(0xCF8, 0x0000, 0x8000);
        v = k2_inl(0xCF8);
        k2_outl(0xCF8, (unsigned)old, (unsigned)(old >> 16));
        if (v == 0x80000000UL)
            pci_ok = 1;
    }
    if (pci_ok)
        ;
    /* sonst Mechanismus 2 (erst danach, damit auf Mechanismus-1-Boards
       nie bei C000h gelesen wird; ein 32-Bit-Lesen von CF8h liefert dort
       oft FFFFFFFFh): CF8h (Schluessel/Funktion) und CFAh (Bus) lesen
       sich byteweise als 00h (ISA: FFh) - erst dann den Raum
       einschalten und die Kennung der Host-Bruecke (Geraet 0) pruefen */
    else if (inb(0xCF8) == 0 && inb(0xCFA) == 0) {
        outb(0xCF8, 0xF0);
        outb(0xCFA, 0);
        v = k2_inl(0xC000);
        outb(0xCF8, 0);
        if ((unsigned)v != 0xFFFF && (unsigned)v != 0 && (unsigned)(v >> 16) != 0xFFFF)
            pci_ok = 2;
    }
    irq_restore(f);
    if (!pci_ok)
        return 0;
    scan_bus(0, 0);
    return npci;
}

/* n-tes Geraet mit Hersteller/Geraet (dev FFFFh = jedes) -> loc, FFFFh */
unsigned __far pci_find(unsigned vendor, unsigned device, unsigned n)
{
    unsigned i;
    for (i = 0; i < npci; i++)
        if (pci_tab[i].vendor == vendor && (device == 0xFFFF || pci_tab[i].device == device))
            if (n-- == 0)
                return pci_tab[i].loc;
    return 0xFFFF;
}

/* n-tes Geraet der Klasse cls << 8 | sub (sub FFh = jede) -> loc, FFFFh */
unsigned __far pci_findclass(unsigned cs, unsigned n)
{
    unsigned i;
    for (i = 0; i < npci; i++)
        if (pci_tab[i].cls == (cs >> 8) && ((cs & 0xFF) == 0xFF || pci_tab[i].sub == (cs & 0xFF)))
            if (n-- == 0)
                return pci_tab[i].loc;
    return 0xFFFF;
}

/* fuer FAPI_BUSINFO: Eintrag i -> 0, sonst 1 */
int __far pci_info(unsigned i, struct fapi_pcidev __far *p)
{
    if (i >= npci)
        return 1;
    p->loc = pci_tab[i].loc;
    p->vendor = pci_tab[i].vendor;
    p->device = pci_tab[i].device;
    p->cls = pci_tab[i].cls;
    p->sub = pci_tab[i].sub;
    p->progif = pci_tab[i].progif;
    p->irq = pci_tab[i].irq;
    return 0;
}

/* ---- Belegungsliste ------------------------------------------------------ */
#define NRES    32
static struct {
    signed char   drv;                  /* Treibernummer, -1 = frei      */
    unsigned char type;                 /* RES_IO, RES_DMA, RES_IRQ      */
    unsigned short start, len;
} res[NRES];
static unsigned char res_init_done;

static void res_init(void)
{
    unsigned i;
    for (i = 0; i < NRES; i++)
        res[i].drv = -1;
    res_init_done = 1;
}

/* -> 0 ok (auch: derselbe Treiber meldet erneut), 1 belegt, 2 Liste voll */
unsigned __far res_claim(int drv, unsigned type, unsigned start, unsigned len)
{
    unsigned i, fr = NRES;
    unsigned long e = (unsigned long)start + (len ? len : 1);
    if (!res_init_done)
        res_init();
    for (i = 0; i < NRES; i++) {
        if (res[i].drv < 0) {
            if (fr == NRES)
                fr = i;
            continue;
        }
        if (res[i].type != type)
            continue;
        if ((unsigned long)res[i].start < e &&
            start < (unsigned long)res[i].start + (res[i].len ? res[i].len : 1)) {
            if (res[i].drv != drv)
                return 1;
            if (res[i].start == start && res[i].len == len)
                return 0;               /* schon eingetragen             */
        }
    }
    if (fr == NRES)
        return 2;
    res[fr].drv = (signed char)drv;
    res[fr].type = (unsigned char)type;
    res[fr].start = start;
    res[fr].len = len;
    return 0;
}

/* Eintraege eines Treibers loeschen (type 0 = alle, sonst nur diesen Typ
   mit diesem Anfang) */
void __far res_free(int drv, unsigned type, unsigned start)
{
    unsigned i;
    if (!res_init_done)
        res_init();
    for (i = 0; i < NRES; i++)
        if (res[i].drv == drv && (!type || (res[i].type == type && res[i].start == start)))
            res[i].drv = -1;
}

/* Eintrag i (nur belegte zaehlen) -> Treibernummer, -1 = Ende */
int __far res_get(unsigned i, unsigned *type, unsigned *start, unsigned *len)
{
    unsigned k;
    if (!res_init_done)
        res_init();
    for (k = 0; k < NRES; k++)
        if (res[k].drv >= 0 && i-- == 0) {
            *type = res[k].type;
            *start = res[k].start;
            *len = res[k].len;
            return res[k].drv;
        }
    return -1;
}
