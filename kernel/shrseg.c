/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.5 - gemeinsame Segmente wie OS/2 1.x (KTEXT4, FAPI_SHRSEG 69h,
 * docs/FAPI-IPC.md Stufe 3): DosAllocShrSeg/DosGetShrSeg (\SHAREMEM\name)
 * und DosGiveSeg.
 *
 * Der Speicher kommt wie bei DosAllocSeg; jeder Task hat einen eigenen
 * LDT-Eintrag darauf (die LDTs sind je Task, die Selektoren also
 * verschieden). Die Eintraege tragen D_ALIAS: seg_free gibt dann nur den
 * Eintrag frei und meldet es hier (shr_unref), der letzte Verweis gibt den
 * Speicher zurueck; seg_compact/seg_out lassen Alias-Eintraege in Ruhe
 * (sonst stimmten die Deskriptoren der anderen nicht mehr).
 *
 * Laeuft mit Kernelsperre wie DosAllocSeg (die Speicherverwaltung steht
 * noch darunter, docs/BIGLOCK.md).
 */
#include "kern.h"
#include "ifs.h"
#include "mem.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4: Hilfen ueber Thunks    */

#define NSHR    32

extern int current;
unsigned __far kf_seg_alloc(int t, unsigned size);              /* kernel.c */
unsigned __far kf_seg_alias(int t, unsigned long base, unsigned limit);
void __far kf_shr_memfree(pg_t pg, unsigned units);
int __far ipc_name(unsigned usel, unsigned off, char *nm, const char __far *pre, unsigned pl);

struct kshr {
    unsigned char used, rsv;
    unsigned      refs;                 /* LDT-Eintraege darauf           */
    pg_t          pg;                   /* erste Seite                    */
    unsigned      units, limit;
    char          name[FAPI_PATHLEN];   /* "" = ohne Namen (nur GIVE)     */
};

static unsigned shr_sel;                /* Tabelle, 0 = noch keine        */
static char sh_name[FAPI_PATHLEN];

#define S(i)    ((struct kshr __far *)MK_FP(shr_sel, (i) * sizeof(struct kshr)))

static pg_t sel_pg(int t, unsigned sel)
{
    struct desc __far *d = LDT(t, sel >> 3);
    unsigned long b = d->base_lo | ((unsigned long)d->base_hi << 16) |
                      ((unsigned long)(d->rsv & 0xFF00) << 16);
    return (b - (k_alias ? KALIAS : 0)) >> 12;
}

static int by_pg(pg_t pg)
{
    int i;
    for (i = 0; shr_sel && i < NSHR; i++)
        if (S(i)->used && S(i)->pg == pg)
            return i;
    return -1;
}

static int by_name(const char *nm)
{
    struct kshr __far *k;
    unsigned j;
    int i;

    for (i = 0; shr_sel && i < NSHR; i++) {
        k = S(i);
        if (!k->used || !k->name[0])
            continue;
        for (j = 0; nm[j] && nm[j] == k->name[j]; j++)
            ;
        if (!nm[j] && !k->name[j])
            return i;
    }
    return -1;
}

/* Segment des Aufrufers: gemeinsamer Eintrag? -> Platz, -1 */
static int own_seg(unsigned sel)
{
    unsigned idx = sel >> 3;
    struct desc __far *d;

    if (!(sel & 4) || idx < 2 || idx >= NLDT)
        return -1;
    d = LDT(current, idx);
    if (!(d->access & 0x80) || !(d->rsv & D_ALIAS))
        return -1;
    return by_pg(sel_pg(current, sel));
}

/* seg_free (kernel.c) hat einen Alias-Eintrag freigegeben, der auf Seite
   pg zeigte: war es ein gemeinsames Segment, Verweis abziehen */
void __far shr_unref(pg_t pg)
{
    struct kshr __far *k;
    int i = by_pg(pg);

    if (i < 0)
        return;
    k = S(i);
    if (--k->refs)
        return;
    k->used = 0;
    kf_shr_memfree(k->pg, k->units);
}

/* FAPI_SHRSEG: a1 = SHRSEG_*, a2/a3 siehe fapi_abi.h */
unsigned __far shr_fapi(unsigned usel, unsigned a1, unsigned a2, unsigned a3)
{
    static const char pre[] = "\\SHAREMEM\\";
    struct kshr __far *k;
    unsigned sel, j;
    int i;

    switch (a1) {
    case SHRSEG_ALLOC:                  /* a2 = Groesse, a3 = Name oder 0 */
        sh_name[0] = 0;
        if (a3) {
            if (ipc_name(usel, a3, sh_name, pre, 10))
                return FAPI_ERROR;
            if (by_name(sh_name) >= 0)
                return FAPI_ERR_EXIST;
        }
        if (!shr_sel) {
            shr_sel = kseg_alloc((unsigned long)NSHR * sizeof(struct kshr));
            if (!shr_sel)
                return FAPI_ERR_FULL;
            zero_seg(shr_sel, NSHR * sizeof(struct kshr) / 2);
        }
        for (i = 0; i < NSHR && S(i)->used; i++)
            ;
        if (i == NSHR)
            return FAPI_ERR_FULL;
        sel = kf_seg_alloc(current, a2);
        if (sel == FAPI_ERROR)
            return FAPI_ERR_FULL;
        k = S(i);                       /* ab hier kein Schlafpunkt mehr  */
        LDT(current, sel >> 3)->rsv |= D_ALIAS;
        k->used = 1;
        k->refs = 1;
        k->pg = sel_pg(current, sel);
        k->units = a2 ? PAGES(a2) : 16;
        k->limit = a2 ? a2 - 1 : 0xFFFF;
        for (j = 0; (k->name[j] = sh_name[j]) != 0; j++)
            ;
        return sel;

    case SHRSEG_GET:                    /* a3 = Name                      */
        if (ipc_name(usel, a3, sh_name, pre, 10) || (i = by_name(sh_name)) < 0)
            return FAPI_ERROR;
        k = S(i);
        sel = kf_seg_alias(current, PG_LIN(k->pg), k->limit);
        if (sel == FAPI_ERROR)
            return FAPI_ERR_FULL;
        k->refs++;
        return sel;

    case SHRSEG_GIVE:                   /* a2 = Selektor, a3 = Task       */
        if ((i = own_seg(a2)) < 0 || a3 >= NTASK ||
            task[a3].state == T_FREE || task[a3].v86)
            return FAPI_ERROR;
        k = S(i);
        sel = kf_seg_alias((int)a3, PG_LIN(k->pg), k->limit);
        if (sel == FAPI_ERROR)
            return FAPI_ERR_FULL;
        k->refs++;
        return sel;
    }
    return FAPI_ERROR;
}
