/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VDEMO.EXE - VESA-Demo (6.0): 8-Bit-Modus mit linearem Bildspeicher,
 * gezeichnet ueber das 64-KB-Fenster (VioSetPlane = Abschnitt)
 *
 *   VDEMO [Breite [Bit]]  (Vorgabe 640 8; nimmt den ersten Modus mit
 *                          dieser Breite und Farbtiefe; bei 15-32 Bit
 *                          ergeben die Bytes ein Muster statt Streifen)
 *
 * Zeichnet Farbstreifen und einen Rahmen; jede Taste wechselt die Palette
 * (Farben rotieren), Esc beendet. Im Hintergrund zeichnet es weiter.
 * Mit Maus: Zeiger des Kernels, linke Taste malt weisse Punkte 3x3.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static struct fapi_vesamode m;
static char args[FAPI_ARGLEN];
static unsigned char pal[768];
static struct fapi_mouevent ev;

static unsigned long mul16(unsigned a, unsigned b);
#pragma aux mul16 = "mul dx" __parm [__ax] [__dx] __value [__dx __ax] __modify [__ax __dx];

/* Punkt 3x3 bei (x,y), weiss, ueber die Abschnitte */
static void dot(unsigned sel, unsigned x, unsigned y)
{
    unsigned i, j, k, bb = (m.bpp + 7) >> 3;
    unsigned long a;
    for (j = 0; j < 3; j++)
        for (i = 0; i < 3; i++) {
            if (x + i >= m.width || y + j >= m.height)
                continue;
            a = mul16(y + j, m.pitch) + mul16(x + i, bb);
            for (k = 0; k < bb; k++, a++) {     /* 8 Bit: Farbe 15, sonst weiss */
                VioSetPlane((unsigned)(a >> 16));
                *(unsigned char __far *)MK_FP(sel, (unsigned)a) = bb == 1 ? 15 : 0xFF;
            }
        }
}

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
    unsigned want = 640, bits, n, sel, i, shift = 0, k, mouse;

    DosGetArgs(args);
    a = skip(args);
    for (want = 0; *a >= '0' && *a <= '9'; a++)
        want = want * 10 + (*a - '0');
    if (!want)
        want = 640;
    for (a = skip(a), bits = 0; *a >= '0' && *a <= '9'; a++)
        bits = bits * 10 + (*a - '0');
    if (!bits)
        bits = 8;
    for (n = 0; !VioVesa(VESA_MODE, n, &m); n++)
        if (m.bpp == bits && m.width == want)
            break;
    if (VioVesa(VESA_MODE, n, &m)) {
        puts("Kein passender VESA-Modus.\r\n");
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
    mouse = MouOpen();
    if (mouse)
        MouShowPtr(1);
    for (;;) {
        while (mouse && MouReadEvent(&ev, MOU_XY) == 0)
            if (ev.buttons & MOU_LEFT)
                dot(sel, ev.x, ev.y);
        if (!KbdPeek()) {
            DosSleep(20);
            continue;
        }
        k = KbdCharIn();
        if ((k & 0xFF) == 27)
            break;
        shift += 16;
        draw(sel, shift);
    }
    VioSetGfx(0);
}
