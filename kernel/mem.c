/*
 * DOS 5.0 - physische Speicherverwaltung
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
#include "mem.h"

static unsigned char bitmap[8192];
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

void mem_init(unsigned conv_kb, unsigned ext_kb)
{
    unsigned i;
    unsigned long end;

    for (i = 0; i < sizeof(bitmap); i++)
        bitmap[i] = 0xFF;
    free_units = 0;

    if (conv_kb > 640)
        conv_kb = 640;
    release_range(MEM_LOW_START, conv_kb * 4);

    end = MEM_HIGH_START + (unsigned long)ext_kb * 4;
    if (end > 0xFFFFUL)
        end = 0xFFFFUL;                 /* letzte Einheit opfern wir     */
    release_range(MEM_HIGH_START, (unsigned)end);
}

unsigned mem_alloc(unsigned units)
{
    unsigned u, run = 0, start = 0;

    if (units == 0 || units > free_units)
        return 0;
    for (u = 1; u != 0; u++) {
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

void mem_free(unsigned start, unsigned units)
{
    release_range(start, start + units);
}

unsigned mem_free_kb(void)
{
    return free_units / 4;
}
