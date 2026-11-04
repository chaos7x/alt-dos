/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VESA.EXE - VESA-Modi mit linearem Bildspeicher auflisten (6.0)
 *
 *   VESA
 */
#include "fapi.h"
#include "util.h"

static struct fapi_vesamode m;
static char args[FAPI_ARGLEN];

static void put_hex(unsigned v)
{
    static char h[5];
    unsigned i;
    for (i = 0; i < 4; i++)
        h[i] = "0123456789ABCDEF"[(v >> (12 - 4 * i)) & 15];
    h[4] = 0;
    puts(h);
}

void main(void)
{
    unsigned n;
    char *a;

    DosGetArgs(args);
    a = skip(args);
    if (a[0] == '/' && a[1] == '?') {
        puts("Zeigt die VESA-Grafikmodi der Karte, die AT-DOS mit linearem\r\n"
             "Bildspeicher nutzen kann.\r\n\r\nVESA\r\n");
        return;
    }
    if (VioVesa(VESA_MODE, 0, &m)) {
        puts("Keine VESA-Karte mit linearem Bildspeicher (VBE 2.0) gefunden.\r\n");
        DosExit(1);
        return;
    }
    puts("VBE ");
    put_num(m.version >> 8, 1, 0);
    puts(".");
    put_num((m.version >> 4) & 15, 1, 0);
    puts(", Bildspeicher ");
    put_num(m.mem_kb, 1, 1);
    puts(" KB\r\n\r\n  Modus   Aufl\x94sung       Farben   Byte/Zeile\r\n");
    for (n = 0; !VioVesa(VESA_MODE, n, &m); n++) {
        puts("   ");
        put_hex(m.mode);
        puts("h  ");
        put_num(m.width, 4, 0);
        puts(" x ");
        put_num(m.height, 4, 0);
        puts("    ");
        put_num(m.bpp, 2, 0);
        puts(" Bit   ");
        put_num(m.pitch, 6, 0);
        puts("\r\n");
    }
}
