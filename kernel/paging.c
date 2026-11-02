/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - Paging (Phase 1b: 1:1)
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
 * Die 286-TSS der Tasks enthalten kein CR3: alle Tasks teilen sich ein
 * Seitenverzeichnis. Seiten tragen U/S = 1 und R/W = 1; geschuetzt wird
 * vorerst weiter ueber die Segmente.
 *
 * Laeuft im zweiten Codesegment (KTEXT2), nur beim Start.
 */
#include "k2.h"

#define PTE_P   1                       /* vorhanden                     */
#define PTE_W   2                       /* schreibbar                    */
#define PTE_U   4                       /* auch fuer Ring 3              */

static pg_t pd_pg;                      /* Seitenverzeichnis             */
static unsigned n_pt;                   /* Seitentabellen                */

/* CR3 laden und CR0.PG setzen; danach ein naher Sprung (Warteschlange) */
static void pg_enable(unsigned lo, unsigned hi);
#pragma aux pg_enable = \
    "shl edx,16" "mov dx,ax" "mov cr3,edx" \
    "mov eax,cr0" "or eax,80000000h" "mov cr0,eax" \
    "db 0EBh, 0" \
    __parm [__ax] [__dx] __modify [__ax __dx];

/* 32-Bit-Wert an Stelle i (Doppelworte) im Fenster sel */
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
    unsigned i, j, n = (unsigned)((top + 1023) >> 10);

    if (n == 0)
        n = 1;
    pd_pg = mem_alloc(1);
    if (!pd_pg)
        return 0;
    scratch_map(PG_LIN(pd_pg), 4095);
    for (i = 0; i < 1024; i++)
        put32(SCRATCH, i, 0);
    for (i = 0; i < n; i++) {
        pt = mem_alloc(1);
        if (!pt)
            return 0;                   /* (Seiten bleiben belegt)       */
        scratch_map(PG_LIN(pd_pg), 4095);
        put32(SCRATCH, i, PG_LIN(pt) | PTE_P | PTE_W | PTE_U);
        scratch2_map(PG_LIN(pt), 4095);
        p = (pg_t)i << 10;
        for (j = 0; j < 1024; j++, p++)
            put32(SCRATCH2, j, p < top || p < 0x400 ?
                  PG_LIN(p) | PTE_P | PTE_W | PTE_U : 0);
    }
    n_pt = n;
    pg_enable((unsigned)PG_LIN(pd_pg), (unsigned)(PG_LIN(pd_pg) >> 16));
    return n;
}

unsigned __far paging_tables(void)
{
    return n_pt;
}
