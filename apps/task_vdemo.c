/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VDEMO.EXE - VESA-Demo (6.0): 8-Bit-Modus mit linearem Bildspeicher,
 * gezeichnet ueber das 64-KB-Fenster (VioSetPlane = Abschnitt)
 *
 *   VDEMO [Breite]      (Vorgabe 640; nimmt den ersten 8-Bit-Modus
 *                        mit dieser Breite)
 *
 * Zeichnet Farbstreifen und einen Rahmen; jede Taste wechselt die Palette
 * (Farben rotieren), Esc beendet. Im Hintergrund zeichnet es weiter.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static struct fapi_vesamode m;
static char args[FAPI_ARGLEN];
static unsigned char pal[768];

/* ganzes Bild zeichnen: Farbe = (Zeile + shift) & FFh, Rand weiss (15) */
static void draw(unsigned sel, unsigned shift)
{
    unsigned bank = 0, off = 0, x = 0, y = 0;
    unsigned char __far *p = MK_FP(sel, 0);
    VioSetPlane(0);
    for (;;) {
        p[off] = (unsigned char)(x == 0 || y == 0 || x + 1 == m.width ||
                                 y + 1 == m.height ? 15 : ((y >> 2) + shift) & 0xFF);
        if (++x == m.pitch) {
            x = 0;
            if (++y == m.height)
                break;
        }
        if (++off == 0) {
            VioSetPlane(++bank);
            p = MK_FP(sel, 0);
        }
    }
}

void main(void)
{
    char *a;
    unsigned want = 640, n, sel, i, shift = 0, k;

    DosGetArgs(args);
    a = skip(args);
    for (want = 0; *a >= '0' && *a <= '9'; a++)
        want = want * 10 + (*a - '0');
    if (!want)
        want = 640;
    for (n = 0; !VioVesa(VESA_MODE, n, &m); n++)
        if (m.bpp == 8 && m.width == want)
            break;
    if (VioVesa(VESA_MODE, n, &m)) {
        puts("Kein passender VESA-Modus (8 Bit).\r\n");
        DosExit(1);
        return;
    }
    if (VioVesa(VESA_SET, m.mode, &m)) {
        puts("VESA-Modus l\x84\xE1t sich nicht einstellen.\r\n");
        DosExit(1);
        return;
    }
    sel = VioGetPhysBuf();
    for (i = 0; i < 256; i++) {             /* Regenbogen-Palette        */
        k = i & 63;
        pal[i * 3] = (unsigned char)((i >> 6) == 0 ? 63 : (i >> 6) == 1 ? 63 - k : (i >> 6) == 3 ? k : 0);
        pal[i * 3 + 1] = (unsigned char)((i >> 6) == 0 ? k : (i >> 6) == 1 ? 63 : (i >> 6) == 2 ? 63 - k : 0);
        pal[i * 3 + 2] = (unsigned char)((i >> 6) == 2 ? k : (i >> 6) == 3 ? 63 : 0);
    }
    pal[15 * 3] = pal[15 * 3 + 1] = pal[15 * 3 + 2] = 63;
    VioSetPal(0, 256, pal);
    draw(sel, 0);
    for (;;) {
        k = KbdCharIn();
        if ((k & 0xFF) == 27)
            break;
        shift += 16;
        draw(sel, shift);
    }
    VioSetGfx(0);
}
