/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.0 - physische Speicherverwaltung in 4-KB-Seiten
 *
 * Seitennummern sind 32 Bit (Seite p = linear p * 4096), bis MEM_MAX_PG
 * (knapp 4 GB). Belegung als Bitmap, 1 Bit je Seite, 1 = belegt: bis
 * 256 MB 8 KB fest bei linear 9000h, darueber (bis 128 KB, zwei
 * Selektoren) am Anfang des Speichers ab 1 MB. Ueber MEM_LOW_PG (2 GB -
 * 4 MB) nur mem_alloc_hi, siehe mem.h.
 *
 * Was frei ist, sagt die Speicherkarte des BIOS (INT 15h E820h, von
 * start.asm im Real Mode abgefragt): nur Bereiche vom Typ 1 (RAM), mit
 * Luecken. Ohne E820h E801h (bis 16 MB in KB, darueber in 64-KB-
 * Bloecken), zuletzt 88h (16 Bit KB). Unter 640 KB frei ab low (hinter
 * dem Kernel), das Loch 640 KB - 1 MB bleibt belegt.
 */
#include <i86.h>
#include "k2.h"                         /* im zweiten Codesegment       */

/* Bitmap bis 128 KB (4 GB): zwei Selektoren, KSEG_IDX(0) und (1) */
#define BMB(b)      (*(unsigned char __far *)MK_FP(MEMMAP_SEL + \
                       ((unsigned)((b) >> 16) << 3), (unsigned)(b)))

static pg_t          max_pg;            /* Seiten, die die Bitmap abdeckt */
static pg_t          free_pg, total_pg, ram_pg;
static pg_t          hint;              /* ab hier suchen (unter MEM_LOW_PG) */
static pg_t          hint_hi;           /* ab hier suchen (darueber)      */
unsigned char        mem_hi_ok;         /* Fenster bereit (paging_init)   */

#define BYTE(p)     BMB((p) >> 3)
#define BIT(p)      (1 << ((unsigned)(p) & 7))
#define USED(p)     (BYTE(p) & BIT(p))

static void release_range(pg_t from, pg_t to)          /* [from, to) */
{
    pg_t p;
    if (to > max_pg)
        to = max_pg;
    for (p = from; p < to; p++)
        if (USED(p)) {
            BYTE(p) &= (unsigned char)~BIT(p);
            free_pg++;
        }
}

static void reserve_range(pg_t from, pg_t to)
{
    pg_t p;
    for (p = from; p < to && p < max_pg; p++)
        if (!USED(p)) {
            BYTE(p) |= (unsigned char)BIT(p);
            free_pg--;
        }
}

/* RAM-Bereiche oberhalb 1 MB nacheinander liefern: -> 1, Anfang a und
   Ende e in Seiten */
static unsigned rg_i;
static int next_range(pg_t *a, pg_t *e)
{
    struct e820 *m;
    unsigned long s, l;

    if (e820_n) {
        while (rg_i < e820_n) {
            m = &e820_map[rg_i++];
            if (m->type != 1 || m->base_hi || (m->len_lo == 0 && m->len_hi == 0))
                continue;
            s = m->base_lo;
            l = m->len_hi ? 0xFFFFFFFFUL - s : m->len_lo;
            if (s + l < s)
                l = 0xFFFFFFFFUL - s;   /* ueber 4 GB: abschneiden       */
            if (s + l <= 0x100000UL)
                continue;               /* unter 1 MB: macht conv_kb     */
            if (s < 0x100000UL) {
                l -= 0x100000UL - s;
                s = 0x100000UL;
            }
            *a = (s + 4095) >> 12;      /* nur ganze Seiten              */
            *e = (s + l) >> 12;
            if (*a >= MEM_MAX_PG)
                continue;
            if (*e > MEM_MAX_PG)
                *e = MEM_MAX_PG;
            if (*e > *a)
                return 1;
        }
        return 0;
    }
    /* ohne Karte: E801h als zwei Bereiche (1-16 MB, ab 16 MB), sonst 88h */
    if (rg_i == 0) {
        rg_i = 1;
        *a = 0x100;
        *e = 0x100 + ((pg_t)(e801_lo || e801_hi ? e801_lo : mem_ext_kb) >> 2);
        if (*e > *a)
            return 1;
    }
    if (rg_i == 1) {
        rg_i = 2;
        *a = 0x1000;
        *e = 0x1000 + ((pg_t)e801_hi << 4);
        if (*e > MEM_MAX_PG)
            *e = MEM_MAX_PG;
        if (*e > *a)
            return 1;
    }
    return 0;
}

void __far mem_init(pg_t low, unsigned conv_kb)
{
    pg_t a, e, bm_pg = 0, bm_n = 0;
    unsigned long bytes, i;

    /* 1. Groesse: hoechste RAM-Seite */
    max_pg = 0x100;
    rg_i = 0;
    while (next_range(&a, &e))
        if (e > max_pg)
            max_pg = e;
    max_pg = (max_pg + 7) & ~7UL;
    bytes = max_pg >> 3;

    /* 2. Platz fuer die Bitmap: bis 8 KB fest, sonst im ersten passenden
          Bereich ab 1 MB */
    if (bytes <= MEMMAP_FIX) {
        set_desc(MEMMAP_SEL >> 3, MEMMAP_LIN, (unsigned)(bytes - 1), ACC_KDATA);
    } else {
        bm_n = (bytes + 4095) >> 12;
        rg_i = 0;
        while (next_range(&a, &e))
            if (e - a > bm_n && a + bm_n <= MEM_LOW_PG) {
                bm_pg = a;
                break;
            }
        if (!bm_pg) {                   /* geht nicht: nur bis 256 MB    */
            max_pg = (pg_t)MEMMAP_FIX << 3;
            bytes = MEMMAP_FIX;
            bm_n = 0;
            set_desc(MEMMAP_SEL >> 3, MEMMAP_LIN, MEMMAP_FIX - 1, ACC_KDATA);
        } else {
            set_desc(MEMMAP_SEL >> 3, PG_LIN(bm_pg),
                     bytes > 0x10000UL ? 0xFFFF : (unsigned)(bytes - 1), ACC_KDATA);
            if (bytes > 0x10000UL)      /* zweite Haelfte (ueber 2 GB)   */
                set_desc((MEMMAP_SEL >> 3) + 1, PG_LIN(bm_pg) + 0x10000UL,
                         (unsigned)(bytes - 0x10001UL), ACC_KDATA);
        }
    }
    for (i = 0; i < bytes; i++)
        BMB(i) = 0xFF;
    free_pg = 0;

    /* 3. frei: konventionell ab low, RAM ab 1 MB */
    if (conv_kb > 640)
        conv_kb = 640;
    release_range(low, (pg_t)conv_kb >> 2);
    ram_pg = (pg_t)(mem_conv_kb > 640 ? 640 : mem_conv_kb) >> 2;
    rg_i = 0;
    while (next_range(&a, &e)) {
        release_range(a, e);
        ram_pg += e - a;
    }
    if (bm_n)
        reserve_range(bm_pg, bm_pg + bm_n);
    total_pg = free_pg;
    hint = low;
    hint_hi = MEM_LOW_PG;
}

/* Seite 0 ist nie frei: 0 heisst "kein Speicher" */
pg_t (*mem_reclaim)(unsigned pages);

/* n Seiten am Stueck; erste Wahl unter limit (Seiten, 0 = egal) */
static pg_t try_alloc(unsigned n, pg_t from, pg_t to)
{
    pg_t p, start = 0;
    unsigned run = 0;

    for (p = from; p < to; p++) {
        if (((unsigned)p & 7) == 0 && BYTE(p) == 0xFF && p + 8 <= to) {
            run = 0;                    /* ganzes Byte belegt            */
            p += 7;
            continue;
        }
        if (USED(p)) {
            run = 0;
            continue;
        }
        if (run == 0)
            start = p;
        if (++run == n) {
            for (p = start; p < start + n; p++)
                BYTE(p) |= (unsigned char)BIT(p);
            free_pg -= n;
            if (start == hint)
                hint = start + n;
            else if (start == hint_hi)
                hint_hi = start + n;
            return start;
        }
    }
    return 0;
}

static pg_t alloc_in(unsigned n, pg_t to)
{
    pg_t p;
    if (n == 0 || n > free_pg)
        return 0;
    if (to > max_pg)
        to = max_pg;
    if (to > MEM_LOW_PG)                /* darueber nur ueber Fenster    */
        to = MEM_LOW_PG;
    p = try_alloc(n, hint, to);         /* ab dem Merker, dann von vorn  */
    if (!p && hint > 1)
        p = try_alloc(n, 1, hint + n < to ? hint + n : to);
    return p;
}

/* ueber MEM_LOW_PG (nur mit Fenstern, also nach paging_init) */
static pg_t alloc_hi(unsigned n)
{
    pg_t p;
    if (!mem_hi_ok || max_pg <= MEM_LOW_PG || n == 0 || n > free_pg)
        return 0;
    p = try_alloc(n, hint_hi, max_pg);
    if (!p && hint_hi > MEM_LOW_PG)
        p = try_alloc(n, MEM_LOW_PG, hint_hi + n < max_pg ? hint_hi + n : max_pg);
    return p;
}

/* Kein Platz? Dann gibt der Platten-Cache Stuecke ab (wie der Cache-
   Manager bei NT), bis es passt */
pg_t __far mem_alloc(unsigned n)
{
    pg_t p;
    while ((p = alloc_in(n, max_pg)) == 0 && kf_reclaim(n))
        ;
    return p;
}

/* Box-Speicher, XMS/EMS/DPMI, Platten-Cache: zuerst ueber 2 GB, damit
   der gespiegelte Bereich fuer Segmente frei bleibt */
pg_t __far mem_alloc_hi(unsigned n)
{
    pg_t p;
    while ((p = alloc_hi(n)) == 0 && (p = alloc_in(n, max_pg)) == 0 &&
           kf_reclaim(n))
        ;
    return p;
}

/* unter 16 MB (ISA-DMA, 386SX-Adressraum) */
pg_t __far mem_alloc_low(unsigned n)
{
    pg_t p;
    while ((p = alloc_in(n, 0x1000)) == 0 && kf_reclaim(n))
        ;
    return p;
}

void __far mem_free(pg_t start, unsigned n)
{
    release_range(start, start + n);
    if (start >= MEM_LOW_PG) {
        if (start < hint_hi)
            hint_hi = start;
    } else if (start < hint)
        hint = start;
}

unsigned long __far mem_free_kb(void)
{
    return free_pg << 2;
}

unsigned long __far mem_total_kb(void)
{
    return total_pg << 2;
}

/* RAM insgesamt (konventionell + ab 1 MB laut BIOS-Karte) */
unsigned long __far mem_ram_kb(void)
{
    return ram_pg << 2;
}

/* hoechste RAM-Seite + 1 (Groesse der Bitmap): Paging bildet bis hier ab */
pg_t __far mem_top_pg(void)
{
    return max_pg;
}
