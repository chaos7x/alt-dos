/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - GDI-Kern fuer VGA 640x480x16 (docs/GUI-WIN16.md, Stufe 2).
 *
 * Gezeichnet wird in ein Schattenbild im Speicher (vier Ebenen wie die
 * Karte, je 80 Byte x 480 Zeilen = 38.400 Byte). gdi_flush bringt den
 * geaenderten Bereich ueber VioSetPlane/VioGetPhysBuf auf die Karte - so
 * braucht der GUI-Prozess keine Ports, Lesen fuer XOR geht ohne Latches,
 * und nach einem Sitzungswechsel ist das Bild sofort wieder da. Den
 * Mauszeiger zeichnet der Kernel.
 *
 * Farben: Palette wie die 16 Grundfarben von Windows (Nummern wie dort),
 * RGB-Werte gehen auf die naechste davon. Schrift: 8x16 aus der Codepage-
 * Datei CP850.FNT (FreeDOS cpidos, GPL; \DOS\SYSTEM), Texte von Win16-
 * Programmen sind ANSI (1252) und werden auf 850 umgesetzt.
 */
#include "gui.h"

static WORD pl[4];                      /* Schattenbild, Ebenen         */
static WORD phys;                       /* Bildspeicher (VioGetPhysBuf) */
static short dx0 = 32767, dy0, dx1, dy1;   /* geaenderter Bereich       */
static WORD fsel;                       /* Schrift 8x16, 256 Zeichen    */

/* Windows-16: Nummer -> RGB (0x00BBGGRR wie COLORREF) */
DWORD vga_rgb[16] = {
    0x000000UL, 0x000080UL, 0x008000UL, 0x008080UL,
    0x800000UL, 0x800080UL, 0x808000UL, 0xC0C0C0UL,
    0x808080UL, 0x0000FFUL, 0x00FF00UL, 0x00FFFFUL,
    0xFF0000UL, 0xFF00FFUL, 0xFFFF00UL, 0xFFFFFFUL
};

/* ANSI 1252 -> CP850 (80h-FFh) */
const BYTE a2o[128] = {
    0x3F,0x3F,0x27,0x9F,0x22,0x2E,0xC5,0xCE,0x5E,0x25,0x53,0x3C,0x4F,0x3F,0x5A,0x3F,
    0x3F,0x60,0x27,0x22,0x22,0x07,0x2D,0x2D,0x7E,0x54,0x73,0x3E,0x6F,0x3F,0x7A,0x59,
    0xFF,0xAD,0xBD,0x9C,0xCF,0xBE,0xDD,0xF5,0xF9,0xB8,0xA6,0xAE,0xAA,0xF0,0xA9,0xEE,
    0xF8,0xF1,0xFD,0xFC,0xEF,0xE6,0xF4,0xFA,0xF7,0xFB,0xA7,0xAF,0xAC,0xAB,0xF3,0xA8,
    0xB7,0xB5,0xB6,0xC7,0x8E,0x8F,0x92,0x80,0xD4,0x90,0xD2,0xD3,0xDE,0xD6,0xD7,0xD8,
    0xD1,0xA5,0xE3,0xE0,0xE2,0xE5,0x99,0x9E,0x9D,0xEB,0xE9,0xEA,0x9A,0xED,0xE8,0xE1,
    0x85,0xA0,0x83,0xC6,0x84,0x86,0x91,0x87,0x8A,0x82,0x88,0x89,0x8D,0xA1,0x8C,0x8B,
    0xD0,0xA4,0x95,0xA2,0x93,0xE4,0x94,0xF6,0x9B,0x97,0xA3,0x96,0x81,0xEC,0xE7,0x98
};

BYTE rgb2c(DWORD rgb)
{
    int i, best = 0;
    long bd = 0x7FFFFFFFL;
    for (i = 0; i < 16; i++) {
        int dr = (int)(rgb & 0xFF) - (int)(vga_rgb[i] & 0xFF);
        int dg = (int)((rgb >> 8) & 0xFF) - (int)((vga_rgb[i] >> 8) & 0xFF);
        int db = (int)((rgb >> 16) & 0xFF) - (int)((vga_rgb[i] >> 16) & 0xFF);
        long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return (BYTE)best;
}

void gdi_dirty(short x0, short y0, short x1, short y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (dx0 > dx1) {                    /* leer                         */
        dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1;
        return;
    }
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

/* Mauszeiger (Pfeil 11x16, eigener Entwurf): vom GUI-Prozess selbst
   gezeichnet - auf die Karte nach dem Schattenbild, beim Bewegen oder
   Neuzeichnen kommt der Bereich aus dem Schatten zurueck */
static const WORD cur_and[16] = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x10FF, 0x30FF, 0xF87F, 0xF87F, 0xFC7F
};
static const WORD cur_xor[16] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000
};
static short cux = -1, cuy, cnx, cny;
static BYTE cur_on;

void gdi_cursor(short x, short y)
{
    cnx = x;
    cny = y;
    cur_on = 1;
}

/* Zeilen y0..y1-1, Bytes b0..b1-1 aus dem Schatten auf die Karte */
static void blit(short y0, short y1, short b0, short b1)
{
    short y, p;
    if (y0 < 0) y0 = 0;
    if (y1 > SCR_H) y1 = SCR_H;
    if (b0 < 0) b0 = 0;
    if (b1 > 80) b1 = 80;
    if (y0 >= y1 || b0 >= b1)
        return;
    for (p = 0; p < 4; p++) {
        VioSetPlane(p);
        for (y = y0; y < y1; y++)
            fmemcpy(MKFP(phys, y * 80 + b0), MKFP(pl[p], y * 80 + b0), b1 - b0);
    }
}

/* Zeiger bei cnx/cny auf die Karte (Schatten bleibt unberuehrt) */
static void cur_draw(void)
{
    short r, p, b, x = cnx, y = cny, sh = x & 7;
    for (p = 0; p < 4; p++) {
        VioSetPlane(p);
        for (r = 0; r < 16 && y + r < SCR_H; r++) {
            WORD o = (y + r) * 80 + (x >> 3);
            unsigned long am = (unsigned long)(WORD)~cur_and[r] << 8 >> sh;
            unsigned long xm = (unsigned long)cur_xor[r] << 8 >> sh;
            for (b = 0; b < 3; b++) {
                BYTE m, v, d;
                if ((x >> 3) + b >= 80)
                    break;
                m = (BYTE)(am >> (16 - 8 * b));
                v = (BYTE)(xm >> (16 - 8 * b));
                if (!m && !v)
                    continue;
                d = *(BYTE FAR *)MKFP(pl[p], o + b);
                d &= ~m;                        /* Umriss schwarz      */
                d |= v;                         /* Fuellung weiss (15) */
                *(BYTE FAR *)MKFP(phys, o + b) = d;
            }
        }
    }
    cux = x;
    cuy = y;
}

void gdi_flush(void)
{
    int moved = cur_on && (cnx != cux || cny != cuy);
    if (dx0 >= dx1 && !moved)
        return;
    if (cux >= 0 && (moved || (dx0 < dx1 && cux < dx1 + 8 && cux + 16 > dx0 &&
                               cuy < dy1 && cuy + 16 > dy0))) {
        blit(cuy, cuy + 16, cux >> 3, (cux >> 3) + 3);   /* alter Zeiger weg */
        cux = -1;
    }
    if (dx0 < dx1)
        blit(dy0, dy1, dx0 >> 3, (dx1 + 7) >> 3);
    if (cur_on && cux < 0)
        cur_draw();
    dx0 = 32767;
    dx1 = -1;
}

/* waagerechte Strecke x0..x1-1 in Zeile y, Farbe c (rop: 0 setzen,
   1 XOR, 2 NOT Ziel) - ohne Clipping */
static void span(short x0, short x1, short y, BYTE c, BYTE rop)
{
    WORD o = y * 80, b0 = x0 >> 3, b1 = (x1 - 1) >> 3, b, p;
    BYTE m0 = 0xFF >> (x0 & 7), m1 = 0xFF << (7 - ((x1 - 1) & 7)), m;
    for (p = 0; p < 4; p++) {
        BYTE FAR *d = MKFP(pl[p], o);
        BYTE on = (c >> p) & 1;
        for (b = b0; b <= b1; b++) {
            m = 0xFF;
            if (b == b0) m &= m0;
            if (b == b1) m &= m1;
            if (rop == 1) {
                if (on) d[b] ^= m;
            } else if (rop == 2)
                d[b] ^= m;
            else if (on)
                d[b] |= m;
            else
                d[b] &= ~m;
        }
    }
}

void gfx_fill(short x0, short y0, short x1, short y1, BYTE c)
{
    short y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (y = y0; y < y1; y++)
        span(x0, x1, y, c, 0);
    gdi_dirty(x0, y0, x1, y1);
}

static BYTE getpx(short x, short y)
{
    BYTE c = 0, p, m = 0x80 >> (x & 7);
    for (p = 0; p < 4; p++)
        if (*(BYTE FAR *)MKFP(pl[p], y * 80 + (x >> 3)) & m)
            c |= 1 << p;
    return c;
}

/* ein Zeichen (8 Punkte breit) bei x/y, nur innerhalb cx0..cx1/cy0..cy1 */
static void glyph(short x, short y, BYTE ch, BYTE fg, BYTE bg, BYTE opaque,
                  short cx0, short cy0, short cx1, short cy1)
{
    short r, r0 = 0, r1 = FONT_H, k, sh = x & 7;
    BYTE FAR *g;
    WORD p;
    if (x >= cx1 || x + 8 <= cx0 || y >= cy1 || y + FONT_H <= cy0)
        return;
    if (y < cy0) r0 = cy0 - y;
    if (y + FONT_H > cy1) r1 = cy1 - y;
    g = fsel ? MKFP(fsel, ch * 16) : 0;
    for (r = r0; r < r1; r++) {
        BYTE bits = g ? g[r] : (BYTE)((r == 0 || r == 15) ? 0xFF : 0x81);
        WORD o = (y + r) * 80;
        for (k = 0; k < 2; k++) {
            BYTE fm, cm, cell;
            short bx = (x >> 3) + k, px0 = bx * 8, px1 = px0 + 8, a, e;
            if (k == 1 && !sh)
                break;
            if (bx < 0 || bx >= 80)
                continue;
            fm = k ? (BYTE)(bits << (8 - sh)) : (BYTE)(bits >> sh);
            cell = k ? (BYTE)(0xFF << (8 - sh)) : (BYTE)(0xFF >> sh);
            a = px0 < cx0 ? cx0 : px0;
            e = px1 > cx1 ? cx1 : px1;
            if (a >= e)
                continue;
            cm = (BYTE)((0xFF >> (a - px0)) & (0xFF << (px1 - e)));
            cell &= cm;
            fm &= cell;
            for (p = 0; p < 4; p++) {
                BYTE FAR *d = MKFP(pl[p], o + bx);
                BYTE set = 0, clr = 0;
                if ((fg >> p) & 1) set |= fm; else clr |= fm;
                if (opaque) {
                    BYTE bm = cell & ~fm;
                    if ((bg >> p) & 1) set |= bm; else clr |= bm;
                }
                *d = (*d & ~clr) | set;
            }
        }
    }
}

void gfx_text(short x, short y, LPCSTR s, WORD n, BYTE fg, BYTE bg, BYTE opaque)
{
    WORD i;
    for (i = 0; i < n; i++) {
        BYTE c = s[i];
        if (c >= 0x80)
            c = a2o[c - 0x80];
        glyph(x + i * 8, y, c, fg, bg, opaque, 0, 0, SCR_W, SCR_H);
    }
    gdi_dirty(x, y, x + n * 8, y + FONT_H);
}
WORD gfx_textw(LPCSTR s, WORD n) { (void)s; return n * FONT_W; }

/* ---- Objekte ---------------------------------------------------- */
#define OB_PEN   1
#define OB_BRUSH 2
#define OB_FONT  3
#define NOBJ     64
static struct gobj { BYTE type, stock, style, width; DWORD color; } ob[NOBJ];
#define HO(i)   ((HGDIOBJ)(0x6000u + ((i) << 2)))
#define OI(h)   ((((WORD)(h)) - 0x6000u) >> 2)
static struct gobj *optr(HGDIOBJ h)
{
    WORD i = OI(h);
    if (h < 0x6000u || i >= NOBJ || !ob[i].type)
        return 0;
    return &ob[i];
}
static HGDIOBJ onew(BYTE type, BYTE style, BYTE width, DWORD color)
{
    int i;
    for (i = 17; i < NOBJ; i++)
        if (!ob[i].type) {
            ob[i].type = type; ob[i].stock = 0; ob[i].style = style;
            ob[i].width = width; ob[i].color = color;
            return HO(i);
        }
    return 0;
}

/* ---- Geraetekontexte ---------------------------------------------- */
#define NDC 24
struct dcs {
    BYTE  used, paint, ropx;
    HWND  hwnd;
    short ox, oy;                       /* Ursprung (Bildschirm)        */
    short wox, woy;                     /* SetWindowOrg                  */
    struct clip c;
    HGDIOBJ pen, brush, font;
    DWORD tcol, bcol;
    BYTE  bkmode, rop2;
    short cx, cy;                       /* MoveTo                        */
    WORD  talign;
    struct dcs *saved;
};
static struct dcs dcv[NDC];
#define HDCV(i)  ((HDC)(0x7000u + ((i) << 2)))
static struct dcs *dptr(HDC h)
{
    WORD i = (((WORD)h) - 0x7000u) >> 2;
    if (h < 0x7000u || i >= NDC || !dcv[i].used)
        return 0;
    return &dcv[i];
}

static void rect_and(RECT FAR *a, const RECT FAR *b)
{
    if (b->left > a->left) a->left = b->left;
    if (b->top > a->top) a->top = b->top;
    if (b->right < a->right) a->right = b->right;
    if (b->bottom < a->bottom) a->bottom = b->bottom;
}

void clip_and(struct clip FAR *c, const RECT FAR *r)
{
    short i, k = 0;
    for (i = 0; i < c->n; i++) {
        RECT t = c->r[i];
        rect_and(&t, r);
        if (t.left < t.right && t.top < t.bottom)
            c->r[k++] = t;
    }
    c->n = k;
}

HDC dc_get(HWND h, int client, int paint)
{
    int i;
    struct dcs *d;
    struct wnd *w = wptr(h);
    for (i = 0; i < NDC && dcv[i].used; i++)
        ;
    if (i == NDC)
        return 0;
    d = &dcv[i];
    fmemset((void FAR *)d, 0, sizeof(*d));
    d->used = 1;
    d->hwnd = h;
    d->paint = (BYTE)paint;
    if (w) {
        d->ox = client ? w->cr.left : w->wr.left;
        d->oy = client ? w->cr.top : w->wr.top;
    }
    clip_window(h, client, &d->c);
    d->pen = HO(7);                     /* BLACK_PEN                    */
    d->brush = HO(0);                   /* WHITE_BRUSH                  */
    d->font = HO(13);                   /* SYSTEM_FONT                  */
    d->tcol = 0;
    d->bcol = 0xFFFFFFUL;
    d->bkmode = 2;                      /* OPAQUE                       */
    d->rop2 = 13;                       /* R2_COPYPEN                   */
    return HDCV(i);
}

void dc_release(HDC h)
{
    struct dcs *d = dptr(h);
    if (d)
        d->used = 0;
}

void dc_clip_update(HDC h, const RECT FAR *r)  /* r in Bildschirmkoordinaten */
{
    struct dcs *d = dptr(h);
    if (d)
        clip_and(&d->c, r);
}

/* Rechteck (Bildschirm) mit Clipping fuellen */
static void cfill(struct dcs *d, short x0, short y0, short x1, short y1, BYTE c, BYTE rop)
{
    short i, y;
    RECT r;
    for (i = 0; i < d->c.n; i++) {
        r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
        rect_and(&r, &d->c.r[i]);
        if (r.left >= r.right || r.top >= r.bottom)
            continue;
        for (y = r.top; y < r.bottom; y++)
            span(r.left, r.right, y, c, rop);
        gdi_dirty(r.left, r.top, r.right, r.bottom);
    }
}

static void cpixel(struct dcs *d, short x, short y, BYTE c, BYTE rop)
{
    cfill(d, x, y, x + 1, y + 1, c, rop);
}

static BYTE ropof(struct dcs *d)
{
    return d->rop2 == 7 ? 1 : d->rop2 == 6 ? 2 : 0;   /* XORPEN, NOT */
}

#define LX(d, x)  ((short)((x) - (d)->wox + (d)->ox))
#define LY(d, y)  ((short)((y) - (d)->woy + (d)->oy))

static void cline(struct dcs *d, short x0, short y0, short x1, short y1)
{
    struct gobj *p = optr(d->pen);
    short dx, dy, sx, sy, e, e2;
    BYTE c;
    if (!p || p->style == 5)            /* PS_NULL                      */
        return;
    c = rgb2c(p->color);
    if (y0 == y1) {
        if (x1 < x0) { e = x0; x0 = x1 + 1; x1 = e + 1; }
        cfill(d, x0, y0, x1, y0 + 1, c, ropof(d));
        return;
    }
    if (x0 == x1) {
        if (y1 < y0) { e = y0; y0 = y1 + 1; y1 = e + 1; }
        cfill(d, x0, y0, x0 + 1, y1, c, ropof(d));
        return;
    }
    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y0 - y1 : y1 - y0;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    e = dx + dy;
    for (;;) {
        if (x0 == x1 && y0 == y1)
            break;                      /* letzter Punkt nicht (Windows) */
        cpixel(d, x0, y0, c, ropof(d));
        e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

/* ---- Exporte GDI ---------------------------------------------------- */
HGDIOBJ API GetStockObject(short i)
{
    if (i < 0 || i > 16 || !ob[i].type)
        return 0;
    return HO(i);
}
HGDIOBJ API SelectObject(HDC h, HGDIOBJ o)
{
    struct dcs *d = dptr(h);
    struct gobj *g = optr(o);
    HGDIOBJ old = 0;
    if (!d || !g)
        return 0;
    switch (g->type) {
    case OB_PEN: old = d->pen; d->pen = o; break;
    case OB_BRUSH: old = d->brush; d->brush = o; break;
    case OB_FONT: old = d->font; d->font = o; break;
    }
    return old;
}
BOOL API DeleteObject(HGDIOBJ o)
{
    struct gobj *g = optr(o);
    if (!g || g->stock)
        return 0;
    g->type = 0;
    return 1;
}
BOOL API UnrealizeObject(HGDIOBJ o) { (void)o; return 1; }
HPEN API CreatePen(short style, short width, DWORD color)
{
    return onew(OB_PEN, (BYTE)style, (BYTE)width, color);
}
HPEN API CreatePenIndirect(void FAR *lp)
{
    WORD FAR *w = lp;
    return onew(OB_PEN, (BYTE)w[0], (BYTE)w[1], *(DWORD FAR *)(w + 3));
}
HBRUSH API CreateSolidBrush(DWORD color) { return onew(OB_BRUSH, 0, 0, color); }
HBRUSH API CreateHatchBrush(short st, DWORD color) { (void)st; return onew(OB_BRUSH, 0, 0, color); }
HBRUSH API CreateBrushIndirect(void FAR *lp)
{
    WORD FAR *w = lp;
    return onew(OB_BRUSH, (BYTE)w[0], 0, *(DWORD FAR *)(w + 1));
}
HFONT API CreateFont(short h, short w, short e, short o, short wt, BYTE it, BYTE ul,
                     BYTE so, BYTE cs, BYTE op, BYTE cp, BYTE q, BYTE pf, LPCSTR face)
{
    (void)h; (void)w; (void)e; (void)o; (void)wt; (void)it; (void)ul; (void)so;
    (void)cs; (void)op; (void)cp; (void)q; (void)pf; (void)face;
    return onew(OB_FONT, 0, 0, 0);
}
HFONT API CreateFontIndirect(void FAR *lf) { (void)lf; return onew(OB_FONT, 0, 0, 0); }
short API GetObject(HGDIOBJ o, short n, void FAR *buf)
{
    struct gobj *g = optr(o);
    if (!g || !buf)
        return 0;
    fmemset(buf, 0, n);
    if (g->type == OB_BRUSH && n >= 8) {
        *(WORD FAR *)buf = g->style;
        *(DWORD FAR *)((char FAR *)buf + 2) = g->color;
        return 8;
    }
    if (g->type == OB_PEN && n >= 10) {
        WORD FAR *w = buf;
        w[0] = g->style; w[1] = g->width;
        *(DWORD FAR *)(w + 3) = g->color;
        return 10;
    }
    if (g->type == OB_FONT && n >= 2) {
        *(WORD FAR *)buf = FONT_H;
        return n > 50 ? 50 : n;
    }
    return 0;
}

DWORD API SetTextColor(HDC h, DWORD c)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = d->tcol; d->tcol = c;
    return o;
}
DWORD API SetBkColor(HDC h, DWORD c)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = d->bcol; d->bcol = c;
    return o;
}
DWORD API GetTextColor(HDC h) { struct dcs *d = dptr(h); return d ? d->tcol : 0; }
DWORD API GetBkColor(HDC h) { struct dcs *d = dptr(h); return d ? d->bcol : 0; }
short API SetBkMode(HDC h, short m)
{
    struct dcs *d = dptr(h);
    short o;
    if (!d) return 0;
    o = d->bkmode; d->bkmode = (BYTE)m;
    return o;
}
short API GetBkMode(HDC h) { struct dcs *d = dptr(h); return d ? d->bkmode : 0; }
short API SetROP2(HDC h, short r)
{
    struct dcs *d = dptr(h);
    short o;
    if (!d) return 0;
    o = d->rop2; d->rop2 = (BYTE)r;
    return o;
}
WORD API SetTextAlign(HDC h, WORD a)
{
    struct dcs *d = dptr(h);
    WORD o;
    if (!d) return 0;
    o = d->talign; d->talign = a;
    return o;
}
short API SetMapMode(HDC h, short m) { (void)h; (void)m; return 1; }
short API GetMapMode(HDC h) { (void)h; return 1; }
DWORD API SetWindowOrg(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->wox, d->woy);
    d->wox = x; d->woy = y;
    return o;
}
DWORD API SetViewportOrg(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    if (!d) return 0;
    d->wox = -x; d->woy = -y;
    return 0;
}
BOOL API DPtoLP(HDC h, void FAR *p, short n) { (void)h; (void)p; (void)n; return 1; }
BOOL API LPtoDP(HDC h, void FAR *p, short n) { (void)h; (void)p; (void)n; return 1; }
DWORD API GetNearestColor(HDC h, DWORD c) { (void)h; return vga_rgb[rgb2c(c)]; }

short API GetDeviceCaps(HDC h, short i)
{
    (void)h;
    switch (i) {
    case 8: return SCR_W;               /* HORZRES */
    case 10: return SCR_H;              /* VERTRES */
    case 4: return 208;                 /* HORZSIZE mm */
    case 6: return 156;
    case 12: return 1;                  /* BITSPIXEL */
    case 14: return 4;                  /* PLANES */
    case 24: return 16;                 /* NUMCOLORS */
    case 88: case 90: return 96;        /* LOGPIXELSX/Y */
    case 2: return 1;                   /* TECHNOLOGY: Rasterbild */
    case 38: return 0x0001 | 0x0008;    /* RASTERCAPS: BITBLT */
    default: return 0;
    }
}

BOOL API ExtTextOut(HDC h, short x, short y, WORD opt, LPRECT r, LPCSTR s, WORD n, short FAR *dx)
{
    struct dcs *d = dptr(h);
    short i, k, X, Y;
    BYTE fg, bg, op;
    (void)dx;
    if (!d)
        return 0;
    if (d->talign & 1) {                /* TA_UPDATECP                  */
        x = d->cx; y = d->cy;
    }
    X = LX(d, x);
    Y = LY(d, y);
    if ((d->talign & 6) == 2)           /* TA_RIGHT                     */
        X -= n * FONT_W;
    else if ((d->talign & 6) == 6)      /* TA_CENTER                    */
        X -= n * FONT_W / 2;
    if ((d->talign & 24) == 8)          /* TA_BOTTOM                    */
        Y -= FONT_H;
    else if ((d->talign & 24) == 24)    /* TA_BASELINE                  */
        Y -= 12;
    fg = rgb2c(d->tcol);
    bg = rgb2c(d->bcol);
    op = d->bkmode == 2;
    if (r && (opt & 2))                 /* ETO_OPAQUE                   */
        cfill(d, LX(d, r->left), LY(d, r->top), LX(d, r->right), LY(d, r->bottom), bg, 0);
    for (k = 0; k < d->c.n; k++) {
        RECT c = d->c.r[k];
        if (r && (opt & 4)) {           /* ETO_CLIPPED                  */
            RECT q;
            q.left = LX(d, r->left); q.top = LY(d, r->top);
            q.right = LX(d, r->right); q.bottom = LY(d, r->bottom);
            rect_and(&c, &q);
        }
        if (c.left >= c.right || c.top >= c.bottom)
            continue;
        for (i = 0; i < (short)n; i++) {
            BYTE ch = s[i];
            if (ch >= 0x80)
                ch = a2o[ch - 0x80];
            glyph(X + i * FONT_W, Y, ch, fg, bg, op, c.left, c.top, c.right, c.bottom);
        }
        gdi_dirty(c.left, c.top, c.right, c.bottom);
    }
    if (d->talign & 1)
        d->cx += n * FONT_W;
    return 1;
}
BOOL API TextOut(HDC h, short x, short y, LPCSTR s, short n)
{
    return ExtTextOut(h, x, y, 0, 0, s, n, 0);
}
DWORD API GetTextExtent(HDC h, LPCSTR s, short n)
{
    (void)h; (void)s;
    return MAKELONG(n * FONT_W, FONT_H);
}
short API GetTextFace(HDC h, short n, LPSTR buf)
{
    (void)h;
    fstrncpy(buf, "System", n);
    return fstrlen(buf);
}
BOOL API GetTextMetrics(HDC h, void FAR *tm)
{
    short FAR *t = tm;
    (void)h;
    fmemset(tm, 0, 31);
    t[0] = FONT_H;      /* tmHeight */
    t[1] = 12;          /* tmAscent */
    t[2] = 4;           /* tmDescent */
    t[3] = 0;           /* tmInternalLeading */
    t[4] = 2;           /* tmExternalLeading */
    t[5] = FONT_W;      /* tmAveCharWidth */
    t[6] = FONT_W;      /* tmMaxCharWidth */
    t[7] = 400;         /* tmWeight */
    ((BYTE FAR *)tm)[20] = 0x20;    /* tmFirstChar */
    ((BYTE FAR *)tm)[21] = 0xFF;
    ((BYTE FAR *)tm)[22] = '?';
    ((BYTE FAR *)tm)[23] = ' ';
    ((BYTE FAR *)tm)[24] = 0x30;    /* tmPitchAndFamily: fest */
    t[13] = 96; t[14] = 96;         /* tmDigitizedAspect */
    return 1;
}

DWORD API MoveTo(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->cx, d->cy);
    d->cx = x; d->cy = y;
    return o;
}
BOOL API MoveToEx(HDC h, short x, short y, POINT FAR *p)
{
    DWORD o = MoveTo(h, x, y);
    if (p) { p->x = LOWORD(o); p->y = HIWORD(o); }
    return 1;
}
DWORD API GetCurrentPosition(HDC h)
{
    struct dcs *d = dptr(h);
    return d ? MAKELONG(d->cx, d->cy) : 0;
}
BOOL API LineTo(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    if (!d) return 0;
    cline(d, LX(d, d->cx), LY(d, d->cy), LX(d, x), LY(d, y));
    d->cx = x; d->cy = y;
    return 1;
}
BOOL API Polyline(HDC h, POINT FAR *p, short n)
{
    short i;
    struct dcs *d = dptr(h);
    if (!d || n < 2) return 0;
    for (i = 1; i < n; i++)
        cline(d, LX(d, p[i - 1].x), LY(d, p[i - 1].y), LX(d, p[i].x), LY(d, p[i].y));
    return 1;
}
BOOL API Polygon(HDC h, POINT FAR *p, short n)
{
    struct dcs *d = dptr(h);
    if (!Polyline(h, p, n) || !d)
        return 0;
    cline(d, LX(d, p[n - 1].x), LY(d, p[n - 1].y), LX(d, p[0].x), LY(d, p[0].y));
    return 1;
}
BOOL API Rectangle(HDC h, short l, short t, short r, short b)
{
    struct dcs *d = dptr(h);
    struct gobj *br, *pn;
    if (!d) return 0;
    br = optr(d->brush);
    pn = optr(d->pen);
    l = LX(d, l); r = LX(d, r); t = LY(d, t); b = LY(d, b);
    if (br && br->style != 1)           /* BS_NULL/BS_HOLLOW            */
        cfill(d, l + 1, t + 1, r - 1, b - 1, rgb2c(br->color), 0);
    if (pn && pn->style != 5) {
        BYTE c = rgb2c(pn->color), ro = ropof(d);
        cfill(d, l, t, r, t + 1, c, ro);
        cfill(d, l, b - 1, r, b, c, ro);
        cfill(d, l, t + 1, l + 1, b - 1, c, ro);
        cfill(d, r - 1, t + 1, r, b - 1, c, ro);
    }
    return 1;
}
BOOL API RoundRect(HDC h, short l, short t, short r, short b, short w, short hh)
{
    (void)w; (void)hh;
    return Rectangle(h, l, t, r, b);
}
BOOL API Ellipse(HDC h, short l, short t, short r, short b)
{
    struct dcs *d = dptr(h);
    struct gobj *br, *pn;
    long a, bb, x, y, a2, b2;
    short cx2, cy2, yy;
    if (!d) return 0;
    br = optr(d->brush);
    pn = optr(d->pen);
    l = LX(d, l); r = LX(d, r); t = LY(d, t); b = LY(d, b);
    a = (r - l) / 2; bb = (b - t) / 2;
    if (a <= 0 || bb <= 0)
        return 1;
    cx2 = l + r; cy2 = t + b;           /* doppelte Mitte               */
    a2 = a * a; b2 = bb * bb;
    for (yy = 0; yy <= bb; yy++) {
        /* x = a * sqrt(1 - y^2/b^2) */
        long v = a2 - (a2 * yy * yy) / b2, lo = 0, hi = a;
        while (lo < hi) {
            long m = (lo + hi + 1) / 2;
            if (m * m <= v) lo = m; else hi = m - 1;
        }
        x = lo;
        y = yy;
        if (br && br->style != 1) {
            cfill(d, (short)(cx2 / 2 - x), (short)(cy2 / 2 - y), (short)(cx2 / 2 + x), (short)(cy2 / 2 - y + 1), rgb2c(br->color), 0);
            cfill(d, (short)(cx2 / 2 - x), (short)(cy2 / 2 + y), (short)(cx2 / 2 + x), (short)(cy2 / 2 + y + 1), rgb2c(br->color), 0);
        }
        if (pn && pn->style != 5) {
            BYTE c = rgb2c(pn->color);
            cpixel(d, (short)(cx2 / 2 - x), (short)(cy2 / 2 - y), c, 0);
            cpixel(d, (short)(cx2 / 2 + x - 1), (short)(cy2 / 2 - y), c, 0);
            cpixel(d, (short)(cx2 / 2 - x), (short)(cy2 / 2 + y), c, 0);
            cpixel(d, (short)(cx2 / 2 + x - 1), (short)(cy2 / 2 + y), c, 0);
        }
    }
    return 1;
}
BOOL API PatBlt(HDC h, short x, short y, short w, short hh, DWORD rop)
{
    struct dcs *d = dptr(h);
    struct gobj *br;
    BYTE c = 0, ro = 0;
    if (!d) return 0;
    br = optr(d->brush);
    if (rop == 0x00F00021UL)            /* PATCOPY                      */
        c = br ? rgb2c(br->color) : 15;
    else if (rop == 0x00000042UL)       /* BLACKNESS                    */
        c = 0;
    else if (rop == 0x00FF0062UL)       /* WHITENESS                    */
        c = 15;
    else if (rop == 0x00550009UL)       /* DSTINVERT                    */
        ro = 2;
    else if (rop == 0x005A0049UL) {     /* PATINVERT                    */
        c = br ? rgb2c(br->color) : 15;
        ro = 1;
    } else
        c = br ? rgb2c(br->color) : 15;
    cfill(d, LX(d, x), LY(d, y), LX(d, x + w), LY(d, y + hh), c, ro);
    return 1;
}
DWORD API SetPixel(HDC h, short x, short y, DWORD c)
{
    struct dcs *d = dptr(h);
    if (!d) return 0;
    cpixel(d, LX(d, x), LY(d, y), rgb2c(c), 0);
    return vga_rgb[rgb2c(c)];
}
DWORD API GetPixel(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    short X, Y;
    if (!d) return 0;
    X = LX(d, x); Y = LY(d, y);
    if (X < 0 || Y < 0 || X >= SCR_W || Y >= SCR_H)
        return 0xFFFFFFFFUL;
    return vga_rgb[getpx(X, Y)];
}
short API SaveDC(HDC h) { (void)h; return 1; }
BOOL API RestoreDC(HDC h, short n) { (void)h; (void)n; return 1; }
short API IntersectClipRect(HDC h, short l, short t, short r, short b)
{
    struct dcs *d = dptr(h);
    RECT q;
    if (!d) return 0;
    q.left = LX(d, l); q.top = LY(d, t); q.right = LX(d, r); q.bottom = LY(d, b);
    clip_and(&d->c, &q);
    return d->c.n ? 2 : 1;
}

/* USER-Seite: Rechteck mit Pinsel fuellen (FillRect) und Rahmen */
void dc_fillrect(HDC h, const RECT FAR *r, HBRUSH b)
{
    struct dcs *d = dptr(h);
    struct gobj *g;
    BYTE c;
    if (!d) return;
    if (b > 0 && b <= 31)               /* COLOR_xxx + 1                */
        c = (BYTE)b;                    /* vom Aufrufer schon Farbe     */
    else {
        g = optr(b);
        if (!g) return;
        c = rgb2c(g->color);
    }
    if (b > 0 && b <= 31)
        c = rgb2c(GetSysColor_c(b - 1));
    cfill(d, LX(d, r->left), LY(d, r->top), LX(d, r->right), LY(d, r->bottom), c, 0);
}
void dc_invert(HDC h, const RECT FAR *r)
{
    struct dcs *d = dptr(h);
    if (!d) return;
    cfill(d, LX(d, r->left), LY(d, r->top), LX(d, r->right), LY(d, r->bottom), 15, 2);
}
/* direkt in Bildschirmkoordinaten mit dem Clipping des DC (Rahmen) */
void dc_rawfill(HDC h, short x0, short y0, short x1, short y1, BYTE c)
{
    struct dcs *d = dptr(h);
    if (d)
        cfill(d, x0, y0, x1, y1, c, 0);
}
void dc_rawtext(HDC h, short x, short y, LPCSTR s, WORD n, BYTE fg)
{
    struct dcs *d = dptr(h);
    short k, i;
    if (!d) return;
    for (k = 0; k < d->c.n; k++) {
        RECT c = d->c.r[k];
        for (i = 0; i < (short)n; i++) {
            BYTE ch = s[i];
            if (ch >= 0x80)
                ch = a2o[ch - 0x80];
            glyph(x + i * FONT_W, y, ch, fg, 0, 0, c.left, c.top, c.right, c.bottom);
        }
        gdi_dirty(c.left, c.top, c.right, c.bottom);
    }
}

/* ---- Start/Ende ------------------------------------------------------ */
static char fpath[80];

static void font_load(void)
{
    WORD h, r;
    static char cs[80];
    char d = 'C';
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        d = cs[0];
    fstrncpy(fpath, "C:\\DOS\\SYSTEM\\CP850.FNT", sizeof(fpath));
    fpath[0] = d;
    h = DosOpen(fpath);
    if (FAPI_FAILED(h)) {
        glogs("Schrift fehlt: ", fpath);
        return;
    }
    fsel = seg_new(4096);
    if (fsel) {
        r = fapi_sel(FAPI_READ, fsel, h, 0, 4096);
        if (r != 4096) {
            seg_del(fsel);
            fsel = 0;
        }
    }
    DosClose(h);
}

void gdi_init(void)
{
    int p;
    static BYTE pal[16 * 3];
    for (p = 0; p < 4; p++)
        pl[p] = seg_new(38400U);
    for (p = 0; p < 16; p++) {
        pal[p * 3] = (BYTE)((vga_rgb[p] & 0xFF) >> 2);
        pal[p * 3 + 1] = (BYTE)(((vga_rgb[p] >> 8) & 0xFF) >> 2);
        pal[p * 3 + 2] = (BYTE)(((vga_rgb[p] >> 16) & 0xFF) >> 2);
    }
    font_load();
    VioSetGfx(0x12);
    phys = VioGetPhysBuf();
    VioSetPal(0, 16, pal);
    /* Bestand der Objekte (GetStockObject) */
    ob[0].type = OB_BRUSH; ob[0].color = 0xFFFFFFUL;
    ob[1].type = OB_BRUSH; ob[1].color = 0xC0C0C0UL;
    ob[2].type = OB_BRUSH; ob[2].color = 0x808080UL;
    ob[3].type = OB_BRUSH; ob[3].color = 0x404040UL;
    ob[4].type = OB_BRUSH; ob[4].color = 0;
    ob[5].type = OB_BRUSH; ob[5].style = 1;
    ob[6].type = OB_PEN; ob[6].color = 0xFFFFFFUL;
    ob[7].type = OB_PEN; ob[7].color = 0;
    ob[8].type = OB_PEN; ob[8].style = 5;
    for (p = 10; p <= 16; p++)
        ob[p].type = OB_FONT;
    for (p = 0; p <= 16; p++)
        ob[p].stock = 1;
    MouOpen();
    MouShowPtr(0);
    gdi_cursor(SCR_W / 2, SCR_H / 2);
    dx1 = -1;
}

void gdi_done(void)
{
    int p;
    VioSetGfx(0);
    for (p = 0; p < 4; p++)
        seg_del(pl[p]);
}
