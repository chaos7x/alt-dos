/*
 * GRAFIK.EXE - Vorfuehrung des VGA-Modus 13h (320x200, 256 Farben):
 * VioSetGfx, VioGetPhysBuf, VioSetPal. Oben alle 256 Farben, unten ein
 * Balken, der waehrend des Laufs waechst - auch im Hintergrund (nach
 * Alt+Esc zeichnet das Programm in den Puffer der Sitzung weiter).
 * GRAFIK 12: Modus 12h (640x480, 16 Farben, vier Ebenen; VioSetPlane):
 * 16 Farbbalken, unten ein wachsender Balken. Ende mit einer Taste.
 * GRAFIK X: Mode X (320x240, 256 Farben, unverkettet, zwei Seiten):
 * Farbverlauf und springendes Quadrat, je Bild in die verdeckte Seite
 * gezeichnet und mit VioSetPage umgeschaltet - flimmerfrei. Linke
 * Maustaste setzt das Quadrat an den Zeiger.
 * Mit Maus (MOUSE.SYS): Zeiger in Bildpunkten (MouReadEvent mit MOU_XY),
 * linke Taste malt, rechte wechselt die Malfarbe.
 */
#include <i86.h>
#include "fapi.h"
#include "util.h"

static unsigned char pal[256 * 3];
static char args[FAPI_ARGLEN];
static struct fapi_mouevent ev;
static unsigned mouse, color = 15;
static int is12, lastx = -1, lasty;

/* Mausereignisse abholen; liefert 1, solange gemalt werden soll (Punkt
   in ev.x/ev.y) */
static int mouse_paint(void)
{
    static unsigned char last;

    while (mouse && MouReadEvent(&ev, MOU_XY) == 0) {
        if ((ev.buttons & MOU_RIGHT) && !(last & MOU_RIGHT))
            color = (color + 1) & 15;
        last = ev.buttons;
        if (ev.buttons & MOU_LEFT)
            return 1;
        lastx = -1;                         /* losgelassen: neue Linie   */
    }
    return 0;
}

static unsigned char __far *vbuf;

static void putpix(int x, int y)
{
    unsigned p, o;
    unsigned char m;

    if (!is12) {
        if (x < 320 && y < 200)
            vbuf[(unsigned)y * 320 + (unsigned)x] = (unsigned char)color;
        return;
    }
    if (x >= 640 || y >= 480)
        return;
    o = (unsigned)y * 80 + ((unsigned)x >> 3);
    m = (unsigned char)(0x80 >> (x & 7));
    for (p = 0; p < 4; p++) {
        VioSetPlane(p);
        if ((color >> p) & 1)
            vbuf[o] |= m;
        else
            vbuf[o] &= (unsigned char)~m;
    }
}

/* Linie vom letzten Punkt (Bresenham), damit schnelles Ziehen keine
   Luecken laesst */
static void paint_to(int x, int y)
{
    int dx, dy, sx, sy, err, e2;

    if (lastx < 0) {
        lastx = x;
        lasty = y;
    }
    dx = x > lastx ? x - lastx : lastx - x;
    dy = y > lasty ? lasty - y : y - lasty;
    sx = lastx < x ? 1 : -1;
    sy = lasty < y ? 1 : -1;
    err = dx + dy;
    for (;;) {
        putpix(lastx, lasty);
        if (lastx == x && lasty == y)
            break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            lastx += sx;
        }
        if (e2 <= dx) {
            err += dx;
            lasty += sy;
        }
    }
}

static void mouse_on(void)
{
    mouse = MouOpen();
    if (mouse)
        MouShowPtr(1);
}

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
    is12 = 1;
    for (p = 0; p < 4; p++) {
        VioSetPlane(p);
        for (y = 0; y < 400; y++)
            for (b = 0; b < 80; b++)
                v[y * 80 + b] = (unsigned char)(((b / 5) >> p) & 1 ? 0xFF : 0x00);
    }
    vbuf = v;
    mouse_on();
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
        while (mouse_paint())
            paint_to(ev.x, ev.y);
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        DosSleep(30);
    }
    VioSetGfx(0);
}

/* Dreieckswelle 0..63..0 ueber 256 Schritte */
static unsigned char tri(unsigned v)
{
    v &= 255;
    return (unsigned char)(v < 128 ? v >> 1 : (255 - v) >> 1);
}

static void modex(void)
{
    unsigned sel, p, y, x, b, page = 1, n = 0, row, off;
    int bx = 20, by = 30, dx = 3, dy = 2;
    unsigned short __far *w;
    unsigned char __far *v;
    unsigned short c2;

    if (VioSetGfx(VIO_MODEX) != 0) {
        puts("Grafik nicht verf\x81gbar (nur VGA).\r\n");
        DosExit(1);
    }
    sel = VioGetPhysBuf();
    if (FAPI_FAILED(sel)) {
        VioSetGfx(0);
        DosExit(1);
    }
    v = MK_FP(sel, 0);
    for (x = 0; x < 255; x++) {             /* Regenbogen, 255 = weiss  */
        pal[x * 3] = tri(x);
        pal[x * 3 + 1] = tri(x + 85);
        pal[x * 3 + 2] = tri(x + 170);
    }
    pal[255 * 3] = pal[255 * 3 + 1] = pal[255 * 3 + 2] = 63;
    VioSetPal(0, 256, pal);
    mouse_on();
    for (;;) {
        off = page ? 19200U : 0;            /* verdeckte Seite          */
        for (p = 0; p < 4; p++) {
            VioSetPlane(p);
            w = MK_FP(sel, off);
            for (y = 0; y < 240; y++) {     /* Zeile y: Farbe (y + n)   */
                c2 = (unsigned char)((y + n) % 255);
                c2 |= c2 << 8;
                for (b = 0; b < 40; b++)
                    *w++ = c2;
            }
            for (y = 0; y < 40; y++) {      /* Quadrat 40x40, weiss     */
                row = off + (by + y) * 80;
                for (x = (4 + p - (bx & 3)) & 3; x < 40; x += 4)
                    v[row + ((bx + x) >> 2)] = 255;
            }
        }
        VioSetPage(page);
        page ^= 1;
        n += 2;
        while (mouse && MouReadEvent(&ev, MOU_XY) == 0)
            if (ev.buttons & MOU_LEFT) {
                bx = ev.x > 279 ? 279 : ev.x;
                by = ev.y > 199 ? 199 : ev.y;
            }
        bx += dx;
        by += dy;
        if (bx < 0 || bx > 280) {
            dx = -dx;
            bx += 2 * dx;
        }
        if (by < 0 || by > 200) {
            dy = -dy;
            by += 2 * dy;
        }
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
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
    if (args[0] == 'X' || args[0] == 'x' || (args[0] == ' ' && (args[1] == 'X' || args[1] == 'x'))) {
        modex();
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
    vbuf = v;
    mouse_on();
    for (;;) {                              /* Balken waechst           */
        for (y = 150; y < 190; y++)
            v[y * 320 + 10 + (n % 300)] = (unsigned char)(240 + (n / 300) % 16);
        n++;
        while (mouse_paint())
            paint_to(ev.x, ev.y);
        if (KbdPeek()) {
            KbdCharIn();
            break;
        }
        if ((n & 7) == 0)
            DosSleep(10);
    }
    VioSetGfx(0);
}
