/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - physische Speicherverwaltung
 *
 * Der Adressraum des 286 (16 MB) wird in Einheiten zu 256 Byte verwaltet.
 * Damit passt jede Einheitennummer in 16 Bit (Einheit u = linear u * 256),
 * und ein Segment von hoechstens 64 KB umfasst hoechstens 256 Einheiten.
 * Belegung als Bitmap: 65536 Einheiten = 8 KB, 1 = belegt.
 *
 * Nutzbar sind zwei Stuecke: unter 640 KB (ab 128 KB, darunter liegen
 * BIOS-Daten, Bootbereich, DMA-Puffer und Kernel) und ab 1 MB. Das Loch
 * dazwischen (Video, ROMs) bleibt dauerhaft belegt.
 */
#include <i86.h>
#include "mem.h"

/* Die Bitmap liegt nicht im Kernelsegment (dort ist der Platz knapp),
   sondern bei linear 9000h-AFFFh bzw. hinter dem DMA-Puffer ueber 1 MB
   (memmap_lin), Selektor MEMMAP_SEL (kern.h) */
#define bitmap ((unsigned char __far *)MK_FP(MEMMAP_SEL, 0))
#define BITMAP_SIZE 8192U
static unsigned      free_units;

#define USED(u)     (bitmap[(u) >> 3] & (1 << ((u) & 7)))
#define SET(u)      (bitmap[(u) >> 3] |= (unsigned char)(1 << ((u) & 7)))
#define CLR(u)      (bitmap[(u) >> 3] &= (unsigned char)~(1 << ((u) & 7)))

static void release_range(unsigned from, unsigned to)   /* [from, to) */
{
    unsigned u;
    for (u = from; u < to; u++) {
        if (USED(u)) {
            CLR(u);
            free_units++;
        }
    }
}

void mem_init(unsigned low, unsigned conv_kb, unsigned ext_kb)
{
    unsigned i;
    unsigned long end;

    for (i = 0; i < BITMAP_SIZE; i++)
        bitmap[i] = 0xFF;
    free_units = 0;

    if (conv_kb > 640)
        conv_kb = 640;
    release_range(low, conv_kb * 4);

    end = MEM_HIGH_START + (unsigned long)ext_kb * 4;
    if (end > 0xFFFFUL)
        end = 0xFFFFUL;                 /* letzte Einheit opfern wir     */
    release_range(MEM_HIGH_START, (unsigned)end);
}

unsigned (*mem_reclaim)(unsigned units);

/* erst ab 1 MB suchen, dann darunter: der konventionelle Speicher bleibt
   moeglichst frei (5.1: er gehoert der Kompatibilitaetsbox) */
static unsigned try_range(unsigned units, unsigned from, unsigned to)
{
    unsigned u, run = 0, start = 0;

    for (u = from; u != to; u++) {
        if ((u & 7) == 0 && bitmap[u >> 3] == 0xFF) {
            run = 0;                    /* ganzes Byte belegt: ueberspringen */
            u += 7;
            continue;
        }
        if (USED(u)) {
            run = 0;
            continue;
        }
        if (run == 0)
            start = u;
        if (++run == units) {
            for (u = start; u < start + units; u++)
                SET(u);
            free_units -= units;
            return start;
        }
    }
    return 0;
}

static unsigned try_alloc(unsigned units)
{
    unsigned u;
    if (units == 0 || units > free_units)
        return 0;
    u = try_range(units, MEM_HIGH_START, 0);
    return u ? u : try_range(units, 1, MEM_HIGH_START);
}

/* Kein Platz? Dann gibt der Platten-Cache Stuecke ab (wie der Cache-
   Manager bei NT, hier noch in festen 16-KB-Stuecken), bis es passt */
unsigned mem_alloc(unsigned units)
{
    unsigned u;
    if (units == 0)
        return 0;
    while ((u = try_alloc(units)) == 0 && mem_reclaim && mem_reclaim(units))
        ;
    return u;
}

int mem_is_free(unsigned from, unsigned to)
{
    unsigned u;
    for (u = from; u < to; u++)
        if (USED(u))
            return 0;
    return 1;
}

void mem_reserve(unsigned from, unsigned to)
{
    unsigned u;
    for (u = from; u < to; u++)
        if (!USED(u)) {
            SET(u);
            free_units--;
        }
}

void mem_free(unsigned start, unsigned units)
{
    release_range(start, start + units);
}

unsigned mem_free_kb(void)
{
    return free_units / 4;
}
