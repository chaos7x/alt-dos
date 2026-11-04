/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Paging (1:1 und Kernel-Alias)
 *
 * Der 386 uebersetzt lineare Adressen ueber ein Seitenverzeichnis und
 * Seitentabellen (je 1024 Eintraege zu 4 Byte, eine Tabelle deckt 4 MB).
 * Hier zunaechst 1:1: linear = physisch fuer alles von 0 bis zur
 * hoechsten RAM-Seite, mindestens 4 MB (das erste MB mit Bildspeicher und
 * ROMs immer). Alles bleibt, wie es war - Segmente, Busmaster-DMA mit
 * physischen Adressen, Fenster -, aber die Grundlage fuer Schutz (Seiten
 * nicht vorhanden/nur lesen), DOS-Boxen (V86 braucht je Box ein eigenes
 * erstes MB) und virtuellen Speicher ist da.
 *
 * Alle Tasks teilen sich dieses Seitenverzeichnis (CR3 in ihrem 386-TSS),
 * nur DOS-Boxen haben ein eigenes (v86.c). Seiten tragen U/S = 1 und
 * R/W = 1; geschuetzt wird vorerst weiter ueber die Segmente.
 *
 * Alias (Phase 3): Eintraege 512+i des Seitenverzeichnisses zeigen auf
 * dieselben Seitentabellen wie i - der Speicher erscheint ein zweites Mal
 * ab linear KALIAS (80000000h). Danach rechnet paging_init alle
 * Deskriptoren der GDT dorthin um (put_desc tut es fuer alle spaeteren)
 * und laedt GDTR/IDTR neu. Die Boxen uebernehmen die Eintraege ab 512,
 * der Kernel laeuft also in jedem Adressraum gleich.
 *
 * Laeuft im zweiten Codesegment (KTEXT2), nur beim Start.
 */
#include "k2.h"

#define PTE_P   1                       /* vorhanden                     */
#define PTE_W   2                       /* schreibbar                    */
#define PTE_U   4                       /* auch fuer Ring 3              */

static pg_t pd_pg;                      /* Seitenverzeichnis             */
static unsigned n_pt;                   /* Seitentabellen                */

/* Fenster (Phase 5): ueber MEM_LOW_PG (2 GB - 4 MB) spiegelt der Alias
   nichts mehr. Seitenverzeichnis-Eintrag 1023 (linear FFC00000h, in
   allen Adressraeumen gleich - die Boxen uebernehmen die Eintraege ab
   512) zeigt auf die Seitentabelle win_pt; je Fenster-Selektor ein
   Platz mit 32 Seiten (128 KB, reicht fuer 64 KB an beliebiger Stelle).
   pg_win legt die Seiten um und laedt CR3 neu (kein INVLPG: 386), aber
   nur, wenn sich die Abbildung aendert. Wer ohne Kernelsperre ueber
   SCRATCH/SCRATCH2 geht (vio, v86_poll), darf nur Speicher unter
   MEM_LOW_PG abbilden - win_restore stellt nur den Deskriptor zurueck,
   nicht die Seiten des Platzes. */
#define WIN_PDE   1023
#define WIN_LIN   0xFFC00000UL
#define WIN_SLOTS 4                     /* SCRATCH, SCRATCH2, Cache, Rest */
#define WIN_PG    32
static pg_t win_pt;
static pg_t win_first[WIN_SLOTS];       /* abgebildete erste Seite (0 = keine) */

static void tlb_flush(void);
#pragma aux tlb_flush = "mov eax,cr3" "mov cr3,eax" __modify [__ax];

/* CR3 laden und CR0.PG setzen; danach ein naher Sprung (Warteschlange) */
static void pg_enable(unsigned lo, unsigned hi);
#pragma aux pg_enable = \
    "shl edx,16" "mov dx,ax" "mov cr3,edx" \
    "mov eax,cr0" "or eax,80000000h" "mov cr0,eax" \
    "db 0EBh, 0" \
    __parm [__ax] [__dx] __modify [__ax __dx];

static void k2_lgdt(unsigned short *p);
#pragma aux k2_lgdt = 0x66 0x0F 0x01 0x17 __parm [__bx];    /* o32 lgdt [bx] */
static void k2_lidt(unsigned short *p);
#pragma aux k2_lidt = 0x66 0x0F 0x01 0x1F __parm [__bx];    /* 32 Bit Basis */
/* DS, ES, SS neu aus der GDT laden (Basis jetzt im Alias) */
static void k2_reload(void);
#pragma aux k2_reload = "mov ax,ds" "mov ds,ax" "mov es,ax" "mov ax,ss" \
    "mov ss,ax" __modify [__ax];

extern unsigned short gdtr[3], idtr[3];

static void add_alias(unsigned short *r)
{
    unsigned long b = (r[1] | ((unsigned long)r[2] << 16)) + KALIAS;
    r[1] = (unsigned short)b;
    r[2] = (unsigned short)(b >> 16);
}

/* alle Speicherdeskriptoren der GDT (Code, Daten, TSS, LDT; keine Gates)
   in den Alias verschieben */
static void alias_on(void)
{
    struct desc *d;
    unsigned i, f, a;
    unsigned long b;

    f = irq_off();
    for (i = 1; i < NGDT; i++) {
        d = &gdt[i];
        a = d->access;
        if (!(a & 0x80))
            continue;
        if (!(a & 0x10) && (a & 0x0F) != 1 && (a & 0x0F) != 2 &&
            (a & 0x0F) != 3 && (a & 0x0F) != 9 && (a & 0x0F) != 11)
            continue;                           /* Gate                  */
        b = (d->base_lo | ((unsigned long)d->base_hi << 16) |
             ((unsigned long)(d->rsv & 0xFF00) << 16)) + KALIAS;
        d->base_lo = (unsigned short)b;
        d->base_hi = (unsigned char)(b >> 16);
        d->rsv = (d->rsv & 0x00FF) | (unsigned short)((b >> 16) & 0xFF00);
    }
    k_alias = 1;
    add_alias(gdtr);
    add_alias(idtr);
    k2_lgdt(gdtr);
    k2_lidt(idtr);
    k2_reload();
    irq_restore(f);
}

/* 32-Bit-Wert an Stelle i (Doppelworte) im Fenster sel */
unsigned win_csel;                      /* Cache-Fenster (kernel.c bc_wsel) */

static void put32(unsigned sel, unsigned i, unsigned long v)
{
    unsigned __far *p = MK_FP(sel, i << 2);
    p[0] = (unsigned)v;
    p[1] = (unsigned)(v >> 16);
}

/* -> Anzahl Seitentabellen, 0 = kein Speicher (Paging bleibt aus) */
unsigned __far paging_init(void)
{
    pg_t top = mem_top_pg(), pt, p;
    unsigned i, j, n;

    if (top > MEM_LOW_PG)               /* Alias nur bis 2 GB - 4 MB     */
        top = MEM_LOW_PG;
    n = (unsigned)((top + 1023) >> 10);
    if (n == 0)
        n = 1;
    pd_pg = mem_alloc(1);
    win_pt = mem_alloc(1);
    if (!pd_pg || !win_pt)
        return 0;
    scratch_map(PG_LIN(win_pt), 4095);
    for (i = 0; i < 1024; i++)
        put32(SCRATCH, i, 0);
    scratch_map(PG_LIN(pd_pg), 4095);
    for (i = 0; i < 1024; i++)
        put32(SCRATCH, i, 0);
    for (i = 0; i < n; i++) {
        pt = mem_alloc(1);
        if (!pt)
            return 0;                   /* (Seiten bleiben belegt)       */
        scratch_map(PG_LIN(pd_pg), 4095);
        put32(SCRATCH, i, PG_LIN(pt) | PTE_P | PTE_W | PTE_U);
        put32(SCRATCH, 512 + i, PG_LIN(pt) | PTE_P | PTE_W | PTE_U);   /* Alias */
        scratch2_map(PG_LIN(pt), 4095);
        p = (pg_t)i << 10;
        for (j = 0; j < 1024; j++, p++)
            put32(SCRATCH2, j, p < top || p < 0x400 ?
                  PG_LIN(p) | PTE_P | PTE_W | PTE_U : 0);
    }
    scratch_map(PG_LIN(pd_pg), 4095);
    put32(SCRATCH, WIN_PDE, PG_LIN(win_pt) | PTE_P | PTE_W);   /* nur Ring 0 */
    n_pt = n;
    pg_enable((unsigned)PG_LIN(pd_pg), (unsigned)(PG_LIN(pd_pg) >> 16));
    alias_on();
    mem_hi_ok = 1;
    return n;
}

/* Fenster fuer Selektor sel auf [base, base + limit] (physisch) legen:
   -> Basis fuer set_desc (put_desc addiert KALIAS selbst dazu) */
unsigned long __far pg_win(unsigned sel, unsigned long base, unsigned limit)
{
    unsigned slot = sel == SCRATCH ? 0 : sel == SCRATCH2 ? 1 :
                    sel == win_csel ? 2 : 3, i;
    pg_t first = (base >> 12) & ~15UL;  /* auf 64 KB: Nachbarn treffen  */
    (void)limit;                        /* <= 64 KB passt immer (<= 31 Seiten) */
    if (win_first[slot] != first) {
        set_desc(sel >> 3, PG_LIN(win_pt), 4095, ACC_KDATA);
        for (i = 0; i < WIN_PG; i++)
            put32(sel, slot * WIN_PG + i,
                  first + i < MEM_MAX_PG ? PG_LIN(first + i) | PTE_P | PTE_W : 0);
        win_first[slot] = first;
        tlb_flush();
    }
    return WIN_LIN + ((unsigned long)slot * WIN_PG << 12) + (base - PG_LIN(first)) - KALIAS;
}

/* fuer die TSS der Tasks (0: Paging aus - dann stoert CR3 nicht) */
unsigned long __far paging_cr3(void)
{
    return n_pt ? PG_LIN(pd_pg) : 0;
}

unsigned __far paging_tables(void)
{
    return n_pt;
}
