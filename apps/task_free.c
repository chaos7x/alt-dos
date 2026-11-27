/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FREE.EXE - Speicher kurz, wie free (procps):  FREE [-k | -m | -h]
 * Zeilen Speicher (gesamt, belegt, frei, Cache, verfuegbar) und
 * Auslagerung. Ausfuehrlich zeigt es MEM.
 */
#include "fapi.h"
#include "util.h"

static char args[FAPI_ARGLEN];
static struct fapi_meminfo mi;
static struct fapi_cacheinfo ci;
static int unit;                        /* 0 KB, 1 MB, 2 lesbar        */

static void v(unsigned long kb)
{
    if (unit == 1)
        put_num((kb + 512) >> 10, 12, 0);
    else if (unit == 2) {
        if (kb >= 10240) {
            put_num((kb + 512) >> 10, 11, 0);
            putc('M');
        } else {
            put_num(kb, 11, 0);
            putc('K');
        }
    } else
        put_num(kb, 12, 0);
}

void main(void)
{
    char *a;
    unsigned long tot, fr, cache, used, avail;

    DosGetArgs(args);
    for (a = skip(args); *a; a = next_word(a)) {
        if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt den Speicher kurz (ausf\x81hrlich: MEM).\r\n\r\nFREE [-k | -m | -h]\r\n");
            return;
        }
        if (a[0] == '-' && a[1] == 'm') unit = 1;
        if (a[0] == '-' && a[1] == 'h') unit = 2;
        if (a[0] == '-' && a[1] == 'k') unit = 0;
    }
    DosMemInfo(&mi);
    tot = mi.total_kb32 ? mi.total_kb32 : mi.total_kb;
    fr = mi.total_kb32 ? mi.free_kb32 : mi.free_kb;
    cache = 0;
    if (!FAPI_FAILED(DosCacheInfo(&ci)))
        cache = ci.kb;
    used = tot - fr - cache;
    if (used > tot)
        used = tot - fr;
    avail = fr + (cache > ci.min_kb ? cache - ci.min_kb : 0);
    puts("                  gesamt      belegt        frei       Cache  verf\x81gbar\r\n");
    puts("Speicher:   ");
    v(tot); v(used); v(fr); v(cache); v(avail);
    puts("\r\nAuslagerung:");
    v(mi.swap_kb); v(mi.swapped_kb); v(mi.swap_kb - mi.swapped_kb);
    puts("\r\n");
}
