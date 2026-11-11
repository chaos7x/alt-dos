/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MEM.EXE: zeigt den Arbeitsspeicher von AT-DOS ueber DosMemInfo() und
 * DosCacheInfo(), als Tabelle im Stil des MEM von FreeDOS/MS-DOS (in der
 * DOS-Box laeuft dessen MEM aus \DOS\MDOS, wie \OS2\MDOS bei OS/2).
 *
 *   MEM [/C]     /C: nur die Tabelle, ohne Platten-Cache
 */
#include "fapi.h"

static void putc(char c)
{
    DosWrite(STDOUT, &c, 1);
}

static void spaces(int n)
{
    while (n-- > 0)
        putc(' ');
}

/* v / d ohne 32-Bit-Hilfsroutine (Programme bleiben 286) */
static unsigned long udiv(unsigned long v, unsigned long d, unsigned long *r)
{
    unsigned long q = 0, m = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        m = (m << 1) | ((v >> i) & 1);
        if (m >= d) {
            m -= d;
            q |= 1UL << i;
        }
    }
    if (r)
        *r = m;
    return q;
}

/* v mit Tausenderpunkt; w > 0 rechtsbuendig in w Zeichen */
static void put_num(unsigned long v, int w)
{
    char d[12], o[16];
    int n = 0, k = 0, j;
    unsigned long r;

    do { v = udiv(v, 10, &r); d[n++] = (char)('0' + (unsigned)r); } while (v);
    for (j = n - 1; j >= 0; j--) {
        o[k++] = d[j];
        if (j % 3 == 0 && j != 0)
            o[k++] = '.';
    }
    o[k] = 0;
    spaces(w - k);
    puts(o);
}

/* Anteil in Prozent, gerundet, rechtsbuendig in 4 Zeichen + " %" */
static void put_pct(unsigned long v, unsigned long total)
{
    unsigned long p;
    if (!total) {
        spaces(6);
        return;
    }
    /* v * 1000 kann ueberlaufen: erst passend verkleinern */
    while (v > 0x400000UL || total > 0x400000UL) {
        v >>= 1;
        total >>= 1;
    }
    p = udiv((v << 10) - (v << 4) - (v << 3) + (total >> 1), total, 0); /* v * 1000 */
    p = udiv(p + 5, 10, 0);
    put_num(p, 4);
    puts(" %");
}

/* Zeile "Name  ....K  ...%": Name auf 24 Zeichen, KB in 10 + "K" */
static void row(const char *name, unsigned long kb, unsigned long total)
{
    int n = 0;
    puts(name);
    while (name[n])
        n++;
    spaces(24 - n);
    put_num(kb, 10);
    putc('K');
    spaces(3);
    put_pct(kb, total);
    puts("\n");
}

static void line(void)
{
    puts("----------------------  -----------   ------\n");
}

void main(void)
{
    static struct fapi_meminfo mi;
    static struct fapi_cacheinfo ci;
    static char args[FAPI_ARGLEN];
    unsigned long used, total, fr, rest, boxes;
    char *a;
    int brief = 0;

    DosGetArgs(args);
    for (a = args; *a; a++)
        if (a[0] == '/' && (a[1] == 'C' || a[1] == 'c'))
            brief = 1;
        else if (a[0] == '/' && a[1] == '?') {
            puts("Zeigt den Arbeitsspeicher von AT-DOS.\n\n"
                 "MEM [/C]\n\n"
                 "  /C  nur die Tabelle, ohne Platten-Cache\n\n"
                 "In der DOS-Box zeigt MEM den DOS-Speicher der Box (MEM von\n"
                 "FreeDOS aus \\DOS\\MDOS).\n");
            return;
        }

    if (DosMemInfo(&mi) == FAPI_ERROR) {
        puts("DosMemInfo fehlgeschlagen.\n");
        return;
    }
#ifdef MEMINFO_V60                      /* 6.0: 32 Bit, DOS-Boxen        */
    total = mi.total_kb32 ? mi.total_kb32 : mi.total_kb;   /* 5.0: 16 Bit */
    fr = mi.total_kb32 ? mi.free_kb32 : mi.free_kb;
    boxes = mi.box_kb;
#else                                   /* 5.x: 16 Bit, keine V86-Boxen  */
    total = mi.total_kb;
    fr = mi.free_kb;
    boxes = 0;
#endif
    used = total - fr;
    if (DosCacheInfo(&ci) != 0)
        ci.kb = 0;

    puts("\nSpeicher von AT-DOS           Gr\x94\xE1" "e   Anteil\n");
    line();
    row("Kernel", mi.kernel_kb, total);
    row("Programme", mi.apps_kb, total);
    row("Angefordert (AllocSeg)", mi.alloc_kb, total);
    row("Platten-Cache", ci.kb, total);
    if (boxes)
        row("DOS-Boxen", boxes, total);
    rest = used - mi.kernel_kb - mi.apps_kb - mi.alloc_kb - ci.kb - boxes;
    row("BIOS, Boot, Puffer", rest, total);
    line();
    row("Belegt", used, total);
    row("Frei", fr, total);
    line();
    row("Gesamt", total, total);
    if (ci.max_kb > ci.min_kb && ci.kb > ci.min_kb) {   /* dynamischer Cache */
        puts("Verf\x81gbar               ");
        put_num(fr + ci.kb - ci.min_kb, 10);
        puts("K   (frei + Cache, den er abgibt)\n");
    }
    puts("\nSystembereich 640 KB - 1 MB (Video, ROMs) ist ausgespart.\n");

    if (!brief && ci.kb) {
        unsigned long h = ci.hits, t = ci.hits + ci.misses;
        puts("\nPlatten-Cache (CACHE=)  ");
        put_num(ci.kb, 0);
        puts("K");
        if (ci.max_kb > ci.min_kb) {    /* dynamisch                     */
            puts(", dynamisch ");
            put_num(ci.min_kb, 0);
            puts("K - ");
            put_num(ci.max_kb, 0);
            puts("K");
        }
        puts("\n  Schreiben             ");
        if (ci.delay_s) {
            puts("verz\x94gert ");
            put_num(ci.delay_s, 0);
            puts(" s");
        } else
            puts("sofort");
        puts("\n  Treffer               ");
        if (t) {
            while (t > 0x400000UL) {
                h >>= 1;
                t >>= 1;
            }
            put_num(udiv((h << 6) + (h << 5) + (h << 2) + (t >> 1), t, 0), 0);
            puts(" %");
        } else
            puts("-");
        puts("\n");
    }
    puts("\n");
}
