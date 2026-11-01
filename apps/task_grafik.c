/*
 * GRAFIK.EXE - Vorfuehrung des VGA-Modus 13h (320x200, 256 Farben):
 * VioSetGfx, VioGetPhysBuf, VioSetPal. Oben alle 256 Farben, unten ein
 * Balken, der waehrend des Laufs waechst - auch im Hintergrund (nach
 * Alt+Esc zeichnet das Programm in den Puffer der Sitzung weiter).
 * GRAFIK 12: Modus 12h (640x480, 16 Farben, vier Ebenen; VioSetPlane):
 * 16 Farbbalken, unten ein wachsender Balken. Ende mit einer Taste.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static unsigned char pal[16 * 3];
static char args[FAPI_ARGLEN];

/* 12h: 16 Balken zu je 40 Punkten (5 Byte), Ebene p traegt Farbbit p */
static void mode12(void)
{
    unsigned sel, y, b, p, n = 0, x;
    unsigned char __far *v;

    if (VioSetGfx(0x12) != 0) {
        puts("Grafik nicht verf\x81gbar (nur VGA).\r\n");
        DosExit(1);
    }
    sel = VioGetPhysBuf();
    if (FAPI_FAILED(sel)) {
        VioSetGfx(0);
        DosExit(1);
    }
    v = MK_FP(sel, 0);
    for (p = 0; p < 4; p++) {
        VioSetPlane(p);
        for (y = 0; y < 400; y++)
            for (b = 0; b < 80; b++)
                v[y * 80 + b] = (unsigned char)(((b / 5) >> p) & 1 ? 0xFF : 0x00);
    }
    for (;;) {                              /* Balken waechst, jede Runde
                                               in einer anderen Farbe     */
        x = 2 + (n % 76);
        y = 1 + (n / 76) % 15;              /* Farbe                    */
        for (p = 0; p < 4; p++) {
            VioSetPlane(p);
            for (b = 420; b < 460; b++)
                v[b * 80 + x] = (unsigned char)((y >> p) & 1 ? 0xFF : 0x00);
        }
        n++;
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        DosSleep(30);
    }
    VioSetGfx(0);
}

void main(void)
{
    unsigned sel, x, y, i, n = 0;
    unsigned char __far *v;

    DosGetArgs(args);
    if (args[0] == '1' && args[1] == '2' || (args[0] == ' ' && args[1] == '1' && args[2] == '2')) {
        mode12();
        return;
    }
    if (VioSetGfx(0x13) != 0) {
        puts("Grafik nicht verf\x81gbar (nur VGA).\r\n");
        DosExit(1);
    }
    sel = VioGetPhysBuf();
    if (FAPI_FAILED(sel)) {
        VioSetGfx(0);
        puts("Kein Zugriff auf den Bildspeicher.\r\n");
        DosExit(1);
    }
    v = MK_FP(sel, 0);
    for (y = 0; y < 128; y++)               /* alle 256 Farben, 16 x 16 */
        for (x = 0; x < 320; x++)
            v[y * 320 + x] = (unsigned char)((y >> 3) * 16 + x / 20);
    for (x = 0; x < 320; x++) {             /* weisser Rahmen           */
        v[x] = 15;
        v[199U * 320 + x] = 15;
    }
    for (y = 0; y < 200; y++) {
        v[y * 320] = 15;
        v[y * 320 + 319] = 15;
    }
    for (i = 0; i < 16; i++) {              /* eigene Palette 240-255: Rot */
        pal[i * 3] = (unsigned char)(i * 4 + 3);
        pal[i * 3 + 1] = 0;
        pal[i * 3 + 2] = 0;
    }
    VioSetPal(240, 16, pal);
    for (;;) {                              /* Balken waechst           */
        for (y = 150; y < 190; y++)
            v[y * 320 + 10 + (n % 300)] = (unsigned char)(240 + (n / 300) % 16);
        n++;
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        if ((n & 7) == 0)
            DosSleep(10);
    }
    VioSetGfx(0);
}
