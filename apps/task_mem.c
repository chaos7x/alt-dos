/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MEM.EXE: zeigt den Arbeitsspeicher, ueber DosMemInfo(). */
#include "fapi.h"

static void putc(char c)
{
    DosWrite(STDOUT, &c, 1);
}

/* v rechtsbuendig mit Tausenderpunkt in w Zeichen */
static void put_kb(unsigned v, int w)
{
    char d[8], o[10];
    int n = 0, k = 0, j;

    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (j = n - 1; j >= 0; j--) {
        o[k++] = d[j];
        if (j % 3 == 0 && j != 0)
            o[k++] = '.';
    }
    o[k] = 0;
    for (j = k; j < w; j++)
        putc(' ');
    puts(o);
}

void main(void)
{
    static struct fapi_meminfo mi;
    static struct fapi_cacheinfo ci;
    unsigned used;

    if (DosMemInfo(&mi) == FAPI_ERROR) {
        puts("DosMemInfo fehlgeschlagen.\n");
        return;
    }
    used = mi.total_kb - mi.free_kb;
    puts("\nArbeitsspeicher      ");  put_kb(mi.total_kb, 6); puts(" KB\n");
    puts("  belegt             ");   put_kb(used, 6);        puts(" KB\n");
    puts("    Kernel           ");   put_kb(mi.kernel_kb, 6); puts(" KB\n");
    puts("    Programme        ");   put_kb(mi.apps_kb, 6);  puts(" KB\n");
    puts("    angefordert      ");   put_kb(mi.alloc_kb, 6); puts(" KB\n");
    if (DosCacheInfo(&ci) != 0)
        ci.kb = 0;
    puts("    Platten-Cache    ");   put_kb(ci.kb, 6);       puts(" KB\n");
    puts("    BIOS, Boot, Puffer "); put_kb(used - mi.kernel_kb - mi.apps_kb - mi.alloc_kb - ci.kb, 4); puts(" KB\n");
    puts("  frei               ");   put_kb(mi.free_kb, 6);  puts(" KB\n");
    puts("Systembereich 640 KB - 1 MB (Video, ROMs) ausgespart\n");
    if (ci.kb) {                        /* Trefferquote ohne 32-Bit-Division */
        unsigned long h = ci.hits, t = ci.hits + ci.misses;
        puts("\nPlatten-Cache (CACHE=): ");
        put_kb(ci.kb, 0);
        puts(" KB, Schreiben ");
        if (ci.delay_s) {
            puts("verz\x94gert ");
            put_kb(ci.delay_s, 0);
            puts(" s");
        } else
            puts("sofort");
        while (t > 600) {
            h >>= 1;
            t >>= 1;
        }
        puts(", Treffer ");
        put_kb(t ? (unsigned)h * 100 / (unsigned)t : 0, 0);
        puts(" %\n");
    }
    puts("\n");
}
