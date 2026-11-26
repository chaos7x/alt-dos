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

/* Zielflaeche aller Zeichenroutinen: der Bildschirm (Schattenbild) oder
   eine Bitmap im selben Ebenenformat (1 Ebene = einfarbig, 4 = 16 Farben),
   gesetzt von tgt() am Anfang jeder DC-Operation */
static BYTE FAR *tb[4];
static WORD tpitch = 80;
static short tw = SCR_W, th = SCR_H;
static BYTE tnp = 4, tscr = 1;
static BYTE monob[16];                  /* Farbe -> Bit einfarbig (hell = 1) */

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
    if (!tscr)
        return;
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
    WORD o = y * tpitch, b0 = x0 >> 3, b1 = (x1 - 1) >> 3, b, p;
    BYTE m0 = 0xFF >> (x0 & 7), m1 = 0xFF << (7 - ((x1 - 1) & 7)), m;
    for (p = 0; p < tnp; p++) {
        BYTE FAR *d = tb[p] + o;
        BYTE on = tnp == 1 ? monob[c & 15] : (c >> p) & 1;
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

static void tgt_scr(void)
{
    short p;
    for (p = 0; p < 4; p++)
        tb[p] = MKFP(pl[p], 0);
    tpitch = 80; tw = SCR_W; th = SCR_H; tnp = 4; tscr = 1;
}

void gfx_fill(short x0, short y0, short x1, short y1, BYTE c)
{
    short y;
    tgt_scr();
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
    WORD o = y * tpitch + (x >> 3);
    if (tnp == 1)
        return (tb[0][o] & m) ? 15 : 0;
    for (p = 0; p < 4; p++)
        if (tb[p][o] & m)
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
        WORD o = (y + r) * tpitch;
        for (k = 0; k < 2; k++) {
            BYTE fm, cm, cell;
            short bx = (x >> 3) + k, px0 = bx * 8, px1 = px0 + 8, a, e;
            if (k == 1 && !sh)
                break;
            if (bx < 0 || bx >= (short)tpitch)
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
            for (p = 0; p < tnp; p++) {
                BYTE FAR *d = tb[p] + o + bx;
                BYTE set = 0, clr = 0;
                BYTE fon = tnp == 1 ? monob[fg & 15] : (fg >> p) & 1;
                BYTE bon = tnp == 1 ? monob[bg & 15] : (bg >> p) & 1;
                if (fon) set |= fm; else clr |= fm;
                if (opaque) {
                    BYTE bm = cell & ~fm;
                    if (bon) set |= bm; else clr |= bm;
                }
                *d = (*d & ~clr) | set;
            }
        }
    }
}

void gfx_text(short x, short y, LPCSTR s, WORD n, BYTE fg, BYTE bg, BYTE opaque)
{
    WORD i;
    tgt_scr();
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
#define OB_BITMAP 4                     /* color = Platz in bm[]         */
#define OB_RGN   5                      /* color = Platz in rg[]         */
#define NOBJ     192
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

/* ---- Regionen: nur ein Rechteck (Vereinigung = umschliessendes Rechteck);
   reicht fuer PROGMAN & Co., die damit Bereiche neu zeichnen lassen ---- */
#define NRGN 24
static RECT rg[NRGN];
static BYTE rgu[NRGN];
static RECT *rptr(WORD h)
{
    struct gobj *g = optr(h);
    return g && g->type == OB_RGN ? &rg[(WORD)g->color] : 0;
}
static short rkind(const RECT *r)
{
    return r->left < r->right && r->top < r->bottom ? 2 : 1;   /* SIMPLE/NULL */
}
static void rset(RECT *r, short l, short t, short rr, short b)
{
    if (l >= rr || t >= b)
        l = t = rr = b = 0;
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
}
WORD API CreateRectRgn(short l, short t, short r, short b)
{
    WORD i;
    HGDIOBJ o;
    for (i = 0; i < NRGN && rgu[i]; i++)
        ;
    if (i == NRGN || !(o = onew(OB_RGN, 0, 0, i)))
        return 0;
    rgu[i] = 1;
    rset(&rg[i], l, t, r, b);
    return o;
}
WORD API CreateRectRgnIndirect(const RECT FAR *r)
{
    return CreateRectRgn(r->left, r->top, r->right, r->bottom);
}
void API SetRectRgn(WORD h, short l, short t, short r, short b)
{
    RECT *p = rptr(h);
    if (p)
        rset(p, l, t, r, b);
}
short API CombineRgn(WORD d, WORD a, WORD b, short mode)
{
    static RECT x;
    RECT *pd = rptr(d), *pa = rptr(a), *pb = rptr(b);
    if (!pd || !pa || (mode != 5 && !pb))
        return 0;
    fmemcpy(&x, pa, sizeof x);
    if (mode == 1)                              /* AND                  */
        rset(&x, pa->left > pb->left ? pa->left : pb->left,
             pa->top > pb->top ? pa->top : pb->top,
             pa->right < pb->right ? pa->right : pb->right,
             pa->bottom < pb->bottom ? pa->bottom : pb->bottom);
    else if (mode == 2 || mode == 3) {          /* OR, XOR              */
        if (rkind(pa) == 1)
            fmemcpy(&x, pb, sizeof x);
        else if (rkind(pb) == 2)
            rset(&x, pa->left < pb->left ? pa->left : pb->left,
                 pa->top < pb->top ? pa->top : pb->top,
                 pa->right > pb->right ? pa->right : pb->right,
                 pa->bottom > pb->bottom ? pa->bottom : pb->bottom);
    } else if (mode == 4 && rkind(pb) == 2 &&    /* DIFF: nur ganz bedeckt */
               pb->left <= pa->left && pb->right >= pa->right &&
               pb->top <= pa->top && pb->bottom >= pa->bottom)
        rset(&x, 0, 0, 0, 0);
    fmemcpy(pd, &x, sizeof x);
    return rkind(pd);
}
short API GetRgnBox(WORD h, LPRECT r)
{
    RECT *p = rptr(h);
    if (!p)
        return 0;
    fmemcpy(r, p, sizeof *r);
    return rkind(p);
}
short API OffsetRgn(WORD h, short x, short y)
{
    RECT *p = rptr(h);
    if (!p)
        return 0;
    if (rkind(p) == 2) {
        p->left += x; p->right += x; p->top += y; p->bottom += y;
    }
    return rkind(p);
}
BOOL API PtInRegion(WORD h, short x, short y)
{
    RECT *p = rptr(h);
    return p && x >= p->left && x < p->right && y >= p->top && y < p->bottom;
}
int gdi_rgnbox(WORD h, RECT FAR *r)
{
    RECT *p = rptr(h);
    if (!p || rkind(p) == 1)
        return 0;
    fmemcpy(r, p, sizeof *r);
    return 1;
}

/* ---- Bitmaps -------------------------------------------------------- */
/* Ebenen hintereinander in einem Segment (Ebene p ab p * pitch * h),
   Zeilen oben zuerst, Zeile = pitch Byte (gerade wie bei Windows) */
#define NBM 128
static HGDIOBJ bm_stock;
static struct bmp { BYTE used, np; WORD w, h, pitch, sel; } bm[NBM];
static struct bmp *bptr(HGDIOBJ o)
{
    struct gobj *g = optr(o);
    if (!g || g->type != OB_BITMAP)
        return 0;
    return &bm[(WORD)g->color];
}
static HGDIOBJ bm_new(WORD w, WORD h, BYTE np)
{
    WORD i, pitch;
    unsigned long sz;
    HGDIOBJ o;
    if (!w) w = 1;
    if (!h) h = 1;
    pitch = ((w + 15) >> 4) << 1;
    sz = (unsigned long)pitch * h * np;
    if (sz > 0xFFF0UL)
        return 0;
    for (i = 0; i < NBM && bm[i].used; i++)
        ;
    if (i == NBM)
        return 0;
    bm[i].sel = seg_new((WORD)sz);
    if (!bm[i].sel)
        return 0;
    fmemset(MKFP(bm[i].sel, 0), 0, (WORD)sz);
    o = onew(OB_BITMAP, 0, 0, i);
    if (!o) {
        seg_del(bm[i].sel);
        return 0;
    }
    bm[i].used = 1; bm[i].np = np; bm[i].w = w; bm[i].h = h; bm[i].pitch = pitch;
    return o;
}

/* ---- Geraetekontexte ---------------------------------------------- */
#define NDC 24
struct dcs {
    BYTE  used, paint, ropx, mem;
    BYTE  client;                       /* Ursprung/Clip: Arbeitsbereich */
    WORD  gen;                          /* win_gen bei der letzten Rechnung */
    HGDIOBJ bmo;                        /* Speicher-DC: gewaehlte Bitmap */
    HWND  hwnd;
    short ox, oy;                       /* Ursprung (Bildschirm)        */
    short wox, woy;                     /* SetWindowOrg                  */
    short vox, voy;                     /* SetViewportOrg                */
    short wex, wey, vex, vey;           /* Ausdehnungen                  */
    BYTE  mm;                           /* Abbildungsmodus               */
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
    struct dcs *d;
    if (h < 0x7000u || i >= NDC || !dcv[i].used)
        return 0;
    d = &dcv[i];
    /* ein gehaltener DC (Write holt ihn einmal beim Start, CS_OWNDC):
       haben sich Fenster bewegt oder gezeigt, Ursprung und Clip neu */
    if (d->gen != win_gen && d->hwnd && !d->paint && !d->mem) {
        struct wnd *w = wptr(d->hwnd);
        d->gen = win_gen;
        if (w) {
            d->ox = d->client ? w->cr.left : w->wr.left;
            d->oy = d->client ? w->cr.top : w->wr.top;
            clip_window(d->hwnd, d->client, &d->c);
        }
    }
    return d;
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

/* Zielflaeche des DC setzen */
static void tgt(struct dcs *d)
{
    struct bmp *b;
    short p;
    if (!d->mem || !(b = bptr(d->bmo))) {
        tgt_scr();
        return;
    }
    for (p = 0; p < 4; p++)
        tb[p] = MKFP(b->sel, (WORD)(p < b->np ? p : 0) * b->pitch * b->h);
    tpitch = b->pitch; tw = b->w; th = b->h; tnp = b->np; tscr = 0;
}
static void dc_memclip(struct dcs *d)
{
    struct bmp *b = bptr(d->bmo);
    d->c.n = 1;
    d->c.r[0].left = 0; d->c.r[0].top = 0;
    d->c.r[0].right = b ? b->w : 1; d->c.r[0].bottom = b ? b->h : 1;
}

/* logisch -> Geraet (Bildschirm bzw. Bitmap) */
static short LX(struct dcs *d, short x)
{
    long v = (long)x - d->wox;
    if (d->mm != 1 && d->wex)
        v = v * d->vex / d->wex;
    return (short)(v + d->vox + d->ox);
}
static short LY(struct dcs *d, short y)
{
    long v = (long)y - d->woy;
    if (d->mm != 1 && d->wey)
        v = v * d->vey / d->wey;
    return (short)(v + d->voy + d->oy);
}
static short DX_(struct dcs *d, short x)
{
    long v = (long)x - d->ox - d->vox;
    if (d->mm != 1 && d->vex)
        v = v * d->wex / d->vex;
    return (short)(v + d->wox);
}
static short DY_(struct dcs *d, short y)
{
    long v = (long)y - d->oy - d->voy;
    if (d->mm != 1 && d->vey)
        v = v * d->wey / d->vey;
    return (short)(v + d->woy);
}
/* MM_ISOTROPIC: gleicher Massstab in beiden Richtungen (die kleinere
   Viewport-Ausdehnung gilt, Vorzeichen bleiben) */
static void iso_fix(struct dcs *d)
{
    long ax, ay, wx, wy, vx, vy;
    if (d->mm != 7 || !d->wex || !d->wey)
        return;
    wx = d->wex < 0 ? -d->wex : d->wex;  wy = d->wey < 0 ? -d->wey : d->wey;
    vx = d->vex < 0 ? -d->vex : d->vex;  vy = d->vey < 0 ? -d->vey : d->vey;
    ax = vx * wy; ay = vy * wx;          /* vx/wx gegen vy/wy             */
    if (ax < ay) {
        vy = vx * wy / wx;
        d->vey = (short)(d->vey < 0 ? -vy : vy);
    } else if (ay < ax) {
        vx = vy * wx / wy;
        d->vex = (short)(d->vex < 0 ? -vx : vx);
    }
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
    d->client = (BYTE)client;
    d->gen = win_gen;
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
    d->mm = 1;                          /* MM_TEXT                      */
    d->wex = d->wey = d->vex = d->vey = 1;
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
    tgt(d);
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
    case OB_BITMAP:                     /* nur in Speicher-DCs          */
        if (!d->mem)
            return 0;
        old = d->bmo; d->bmo = o;
        dc_memclip(d);
        break;
    }
    return old;
}
BOOL API DeleteObject(HGDIOBJ o)
{
    struct gobj *g = optr(o);
    if (!g || g->stock)
        return 0;
    if (g->type == OB_BITMAP) {
        struct bmp *b = &bm[(WORD)g->color];
        seg_del(b->sel);
        b->used = 0;
    }
    if (g->type == OB_RGN)
        rgu[(WORD)g->color] = 0;
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
    if (g->type == OB_BITMAP && n >= 14) {   /* BITMAP                */
        struct bmp *b = &bm[(WORD)g->color];
        WORD FAR *w = buf;
        w[0] = 0; w[1] = b->w; w[2] = b->h; w[3] = b->pitch;
        ((BYTE FAR *)buf)[8] = b->np; ((BYTE FAR *)buf)[9] = 1;
        return 14;
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
short API SetMapMode(HDC h, short m)
{
    struct dcs *d = dptr(h);
    short o;
    if (!d) return 0;
    o = d->mm;
    d->mm = (BYTE)m;
    if (m == 1) {                       /* MM_TEXT                      */
        d->wex = d->wey = d->vex = d->vey = 1;
    } else if (m >= 2 && m <= 6) {      /* feste Einheiten: 96 dpi      */
        static const short per_in[7] = { 0, 254, 2540, 100, 1000, 1440, 0 };
        d->wex = per_in[m]; d->wey = per_in[m];
        d->vex = 96; d->vey = -96;      /* y nach oben                  */
    }
    return o;
}
short API GetMapMode(HDC h) { struct dcs *d = dptr(h); return d ? d->mm : 0; }
DWORD API SetWindowExt(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->wex, d->wey);
    if (d->mm == 7 || d->mm == 8) {     /* nur ISO-/ANISOTROPIC          */
        if (x) d->wex = x;
        if (y) d->wey = y;
        iso_fix(d);
    }
    return o;
}
DWORD API SetViewportExt(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->vex, d->vey);
    if (d->mm == 7 || d->mm == 8) {
        if (x) d->vex = x;
        if (y) d->vey = y;
        iso_fix(d);
    }
    return o;
}
DWORD API GetWindowExt(HDC h) { struct dcs *d = dptr(h); return d ? MAKELONG(d->wex, d->wey) : 0; }
DWORD API GetViewportExt(HDC h) { struct dcs *d = dptr(h); return d ? MAKELONG(d->vex, d->vey) : 0; }
DWORD API GetWindowOrg(HDC h) { struct dcs *d = dptr(h); return d ? MAKELONG(d->wox, d->woy) : 0; }
DWORD API GetViewportOrg(HDC h) { struct dcs *d = dptr(h); return d ? MAKELONG(d->vox, d->voy) : 0; }
DWORD API OffsetViewportOrg(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->vox, d->voy);
    d->vox += x; d->voy += y;
    return o;
}
DWORD API OffsetWindowOrg(HDC h, short x, short y)
{
    struct dcs *d = dptr(h);
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->wox, d->woy);
    d->wox += x; d->woy += y;
    return o;
}
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
    DWORD o;
    if (!d) return 0;
    o = MAKELONG(d->vox, d->voy);
    d->vox = x; d->voy = y;
    return o;
}
/* Punkte des Programms: Geraet = Client-Koordinaten (ohne ox/oy) */
BOOL API DPtoLP(HDC h, POINT FAR *p, short n)
{
    struct dcs *d = dptr(h);
    short i;
    if (!d) return 0;
    for (i = 0; i < n; i++) {
        p[i].x = DX_(d, p[i].x + d->ox);
        p[i].y = DY_(d, p[i].y + d->oy);
    }
    return 1;
}
BOOL API LPtoDP(HDC h, POINT FAR *p, short n)
{
    struct dcs *d = dptr(h);
    short i;
    if (!d) return 0;
    for (i = 0; i < n; i++) {
        p[i].x = LX(d, p[i].x) - d->ox;
        p[i].y = LY(d, p[i].y) - d->oy;
    }
    return 1;
}
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
    case 0: return 0x030A;              /* DRIVERVERSION */
    case 2: return 1;                   /* TECHNOLOGY: Rasterbild */
    case 16: return -1;                 /* NUMBRUSHES */
    case 18: return 16;                 /* NUMPENS */
    case 26: return 0x23;               /* PDEVICESIZE */
    case 30: return 0x0022;             /* LINECAPS: Polylinie, breit */
    case 32: return 0x0008;             /* POLYGONALCAPS: Rechteck */
    case 34: return 0x0004;             /* TEXTCAPS: Strich-genau */
    case 36: return 1;                  /* CLIPCAPS: Rechteck */
    case 38: return 0x0001 | 0x0008 | 0x0080 | 0x0400;  /* RASTERCAPS */
    case 40: case 42: return 36;        /* ASPECTX/Y (quadratische Punkte) */
    case 44: return 51;                 /* ASPECTXY */
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
    tgt(d);
    if (r && (opt & 2))                 /* ETO_OPAQUE                   */
        cfill(d, LX(d, r->left), LY(d, r->top), LX(d, r->right), LY(d, r->bottom), bg, 0);
    tgt(d);
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
/* TEXTMETRIC (Win16, 31 Byte): 7 Worte Masse, tmWeight(14), Bytes
   tmItalic(16) .. tmCharSet(24), tmOverhang(25), tmDigitizedAspectX/Y(27/29) */
void tm_fill(BYTE FAR *tm)
{
    short FAR *t = (short FAR *)tm;
    fmemset(tm, 0, 31);
    t[0] = FONT_H;      /* tmHeight */
    t[1] = 12;          /* tmAscent */
    t[2] = 4;           /* tmDescent */
    t[3] = 0;           /* tmInternalLeading */
    t[4] = 2;           /* tmExternalLeading */
    t[5] = FONT_W;      /* tmAveCharWidth */
    t[6] = FONT_W;      /* tmMaxCharWidth */
    t[7] = 400;         /* tmWeight */
    tm[19] = 0x20;      /* tmFirstChar */
    tm[20] = 0xFF;      /* tmLastChar */
    tm[21] = 0x80;      /* tmDefaultChar */
    tm[22] = 0x20;      /* tmBreakChar */
    tm[23] = 0x30;      /* tmPitchAndFamily: FF_MODERN, feste Breite */
    tm[24] = 0;         /* tmCharSet: ANSI */
    *(short FAR *)(tm + 27) = 96;   /* tmDigitizedAspectX */
    *(short FAR *)(tm + 29) = 96;
}
BOOL API GetTextMetrics(HDC h, void FAR *tm)
{
    (void)h;
    tm_fill(tm);
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
/* gefuelltes Vieleck (Pinsel), Gerade-Ungerade-Regel; Punkte in
   Geraetekoordinaten, hoechstens PMAX Ecken */
#define PMAX 64
static short pfx[PMAX], pfy[PMAX], pxs[PMAX];
static void pfill(struct dcs *d, short n, BYTE c)
{
    short y0 = 32767, y1 = -32767, y, i, j, k, m;
    for (i = 0; i < n; i++) {
        if (pfy[i] < y0) y0 = pfy[i];
        if (pfy[i] > y1) y1 = pfy[i];
    }
    for (y = y0; y < y1; y++) {
        m = 0;
        for (i = 0, j = n - 1; i < n; j = i++) {
            short ya = pfy[j], yb = pfy[i];
            if ((ya <= y && yb > y) || (yb <= y && ya > y)) {
                long t = (long)(y - ya) * (pfx[i] - pfx[j]) * 2 / (yb - ya);
                pxs[m++] = pfx[j] + (short)((t + (t < 0 ? -1 : 1)) / 2);
            }
        }
        for (i = 1; i < m; i++)               /* einfuegen sortieren   */
            for (k = i; k > 0 && pxs[k - 1] > pxs[k]; k--) {
                short t = pxs[k]; pxs[k] = pxs[k - 1]; pxs[k - 1] = t;
            }
        for (i = 0; i + 1 < m; i += 2)
            if (pxs[i] < pxs[i + 1])
                cfill(d, pxs[i], y, pxs[i + 1], y + 1, c, 0);
    }
}
BOOL API Polygon(HDC h, POINT FAR *p, short n)
{
    struct dcs *d = dptr(h);
    struct gobj *br;
    short i;
    if (!d || n < 2)
        return 0;
    br = optr(d->brush);
    if (br && br->style != 1 && n <= PMAX) {
        for (i = 0; i < n; i++) {
            pfx[i] = LX(d, p[i].x);
            pfy[i] = LY(d, p[i].y);
        }
        pfill(d, n, rgb2c(br->color));
    }
    if (!Polyline(h, p, n))
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
    tgt(d);
    if (X < 0 || Y < 0 || X >= tw || Y >= th)
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
    tgt(d);
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
    for (p = 0; p < 16; p++)            /* hell (Summe >= 384) -> 1      */
        monob[p] = (BYTE)(((vga_rgb[p] & 0xFF) + ((vga_rgb[p] >> 8) & 0xFF) +
                           ((vga_rgb[p] >> 16) & 0xFF)) >= 384);
    bm_stock = bm_new(1, 1, 1);
    if (bm_stock)
        ob[OI(bm_stock)].stock = 1;
    tgt_scr();
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
BOOL API GetTextExtentPoint(HDC h, LPCSTR s, short n, POINT FAR *sz)
{
    DWORD e = GetTextExtent(h, s, n);
    if (sz) { sz->x = LOWORD(e); sz->y = HIWORD(e); }
    return 1;
}

/* ---- Bitmaps und Speicher-DCs (Stufe 2 fuer die osFree-Programme) ---- */

HDC API CreateCompatibleDC(HDC h)
{
    HDC n = dc_get(0, 0, 0);
    struct dcs *d = dptr(n);
    (void)h;
    if (!d)
        return 0;
    d->mem = 1;
    d->bmo = bm_stock;
    d->ox = d->oy = 0;
    dc_memclip(d);
    return n;
}
BOOL API DeleteDC(HDC h)
{
    struct dcs *d = dptr(h);
    if (!d)
        return 0;
    dc_release(h);
    return 1;
}
HBITMAP API CreateCompatibleBitmap(HDC h, short w, short hh)
{
    struct dcs *d = dptr(h);
    struct bmp *b = d && d->mem ? bptr(d->bmo) : 0;
    return bm_new(w, hh, (BYTE)(b && b->np == 1 ? 1 : 4));
}
HBITMAP API CreateDiscardableBitmap(HDC h, short w, short hh)
{
    return CreateCompatibleBitmap(h, w, hh);
}

/* Bits im Format von Windows (Zeilen oben zuerst, je Zeile gerade
   Byteanzahl): einfarbig = 1 Ebene wie bei uns, 4 Bit = gepackt */
static void bm_setbits(struct bmp *b, BYTE bpp, const BYTE FAR *bits)
{
    WORD y, x, p;
    if (bpp == 1) {
        fmemcpy(MKFP(b->sel, 0), bits, b->pitch * b->h);
        return;
    }
    if (bpp == 4) {
        WORD lp = ((b->w * 4 + 15) >> 4) << 1;
        for (y = 0; y < b->h; y++)
            for (x = 0; x < b->w; x++) {
                BYTE v = bits[y * lp + (x >> 1)];
                BYTE c = (x & 1) ? (v & 15) : (v >> 4);
                for (p = 0; p < b->np; p++) {
                    BYTE FAR *q = MKFP(b->sel, p * b->pitch * b->h + y * b->pitch + (x >> 3));
                    BYTE on = b->np == 1 ? monob[c] : (c >> p) & 1;
                    if (on) *q |= 0x80 >> (x & 7); else *q &= ~(0x80 >> (x & 7));
                }
            }
    }
}
HBITMAP API CreateBitmap(short w, short h, BYTE planes, BYTE bpp, const void FAR *bits)
{
    BYTE np = (BYTE)(planes * bpp == 1 ? 1 : 4);
    HBITMAP o = bm_new(w, h, np);
    struct bmp *b = bptr(o);
    if (b && bits)
        bm_setbits(b, (BYTE)(planes * bpp), bits);
    return o;
}
HBITMAP API CreateBitmapIndirect(const void FAR *lp)
{
    const WORD FAR *w = lp;
    const BYTE FAR *c = lp;
    return CreateBitmap(w[1], w[2], c[8], c[9], *(void FAR * FAR *)(c + 10));
}
DWORD API SetBitmapBits(HBITMAP o, DWORD n, const void FAR *bits)
{
    struct bmp *b = bptr(o);
    (void)n;
    if (!b || b->np != 1)
        return 0;
    bm_setbits(b, 1, bits);
    return (DWORD)b->pitch * b->h;
}
/* Farbteil eines Symbols: Bitmap -> 4 Bit gepackt (Zeile lp Byte) */
void gdi_bm4(HBITMAP o, BYTE FAR *dst, WORD lp)
{
    struct bmp *b = bptr(o);
    WORD x, y, p;
    if (!b)
        return;
    for (y = 0; y < b->h; y++)
        for (x = 0; x < b->w; x++) {
            BYTE c = 0, FAR *q = dst + y * lp + (x >> 1);
            for (p = 0; p < b->np; p++)
                if (*(BYTE FAR *)MKFP(b->sel, p * b->pitch * b->h + y * b->pitch + (x >> 3))
                    & (0x80 >> (x & 7)))
                    c |= 1 << p;
            if (b->np == 1 && c)
                c = 15;
            if (x & 1) *q = (*q & 0xF0) | c; else *q = (*q & 0x0F) | (c << 4);
        }
}
DWORD API GetBitmapBits(HBITMAP o, DWORD n, void FAR *bits)
{
    struct bmp *b = bptr(o);
    WORD k;
    if (!b || b->np != 1)
        return 0;
    k = b->pitch * b->h;
    if (n < k) k = (WORD)n;
    fmemcpy(bits, MKFP(b->sel, 0), k);
    return k;
}

/* Zeilen der aktuellen Flaeche als Farbnummern lesen/schreiben */
static BYTE rbs[SCR_W], rbd[SCR_W];
static void rowget(short x, short y, short n, BYTE *o)
{
    short i;
    for (i = 0; i < n; i++, x++)
        o[i] = (x < 0 || y < 0 || x >= tw || y >= th) ? 0 : getpx(x, y);
}
static void rowput(short x, short y, short n, const BYTE *v)
{
    short i, p;
    for (i = 0; i < n; i++, x++) {
        WORD o = y * tpitch + (x >> 3);
        BYTE m = 0x80 >> (x & 7);
        for (p = 0; p < tnp; p++) {
            BYTE on = tnp == 1 ? monob[v[i] & 15] : (v[i] >> p) & 1;
            if (on) tb[p][o] |= m; else tb[p][o] &= ~m;
        }
    }
}

/* BitBlt/StretchBlt: Rasteroperation (Ternaercode von Windows) Bit fuer
   Bit auf den Farbnummern; einfarbig <-> Farbe wie Windows: Quelle
   einfarbig: 1 = Hintergrund-, 0 = Textfarbe des Ziels; Ziel einfarbig:
   Quellfarbe = Hintergrundfarbe der Quelle -> 1 */
static BOOL blt(HDC hd, short x, short y, short w, short h,
                HDC hs, short sx, short sy, short sw, short sh, DWORD rop)
{
    struct dcs *d = dptr(hd), *s = hs ? dptr(hs) : 0;
    struct gobj *br;
    BYTE r3 = (BYTE)(rop >> 16), useS, useD, P, sbk, dtx, dbk, smono, dmono;
    short X, Y, W, H, SX, SY, SW, SH, k, yy, i;
    if (!d)
        return 0;
    useS = (BYTE)(((r3 >> 2) ^ r3) & 0x33);
    useD = (BYTE)(((r3 >> 1) ^ r3) & 0x55);
    if (useS && !s)
        return 0;
    X = LX(d, x); Y = LY(d, y);
    W = LX(d, x + w) - X; H = LY(d, y + h) - Y;
    if (W < 0) { X += W; W = -W; }
    if (H < 0) { Y += H; H = -H; }
    if (!W || !H)
        return 1;
    if (s) {
        SX = LX(s, sx); SY = LY(s, sy);
        SW = LX(s, sx + sw) - SX; SH = LY(s, sy + sh) - SY;
        if (SW < 0) { SX += SW; SW = -SW; }
        if (SH < 0) { SY += SH; SH = -SH; }
        tgt(s);
        smono = tnp == 1;
        sbk = rgb2c(s->bcol);
    } else {
        SX = SY = 0; SW = W; SH = H; smono = 0; sbk = 0;
    }
    br = optr(d->brush);
    P = br && br->style != 1 ? rgb2c(br->color) : 15;
    dtx = rgb2c(d->tcol); dbk = rgb2c(d->bcol);
    tgt(d);
    dmono = tnp == 1;
    if (dmono)
        P = monob[P] ? 15 : 0;
    for (k = 0; k < d->c.n; k++) {
        RECT c = d->c.r[k], q;
        q.left = X; q.top = Y; q.right = X + W; q.bottom = Y + H;
        rect_and(&c, &q);
        if (c.left >= c.right || c.top >= c.bottom)
            continue;
        if (c.right - c.left > SCR_W)
            c.right = c.left + SCR_W;
        for (yy = c.top; yy < c.bottom; yy++) {
            short n = c.right - c.left;
            if (useS) {
                short ry = SY + (short)((long)(yy - Y) * SH / H);
                tgt(s);
                for (i = 0; i < n; i++) {
                    short rx = SX + (short)((long)(c.left + i - X) * SW / W);
                    BYTE v = (rx < 0 || ry < 0 || rx >= tw || ry >= th) ? 0 : getpx(rx, ry);
                    if (smono && !dmono)
                        v = v ? dbk : dtx;
                    else if (!smono && dmono)
                        v = v == sbk ? 15 : 0;
                    rbs[i] = v;
                }
                tgt(d);
            }
            if (useD)
                rowget(c.left, yy, n, rbd);
            for (i = 0; i < n; i++) {
                BYTE S = useS ? rbs[i] : 0, D = useD ? rbd[i] : 0, rv = 0, b;
                for (b = 0; b < 4; b++) {
                    BYTE ix = (BYTE)((((P >> b) & 1) << 2) | (((S >> b) & 1) << 1) | ((D >> b) & 1));
                    rv |= (BYTE)(((r3 >> ix) & 1) << b);
                }
                rbd[i] = rv;
            }
            rowput(c.left, yy, n, rbd);
        }
        gdi_dirty(c.left, c.top, c.right, c.bottom);
    }
    return 1;
}
BOOL API BitBlt(HDC hd, short x, short y, short w, short h, HDC hs, short sx, short sy, DWORD rop)
{
    return blt(hd, x, y, w, h, hs, sx, sy, w, h, rop);
}
BOOL API StretchBlt(HDC hd, short x, short y, short w, short h, HDC hs,
                    short sx, short sy, short sw, short sh, DWORD rop)
{
    return blt(hd, x, y, w, h, hs, sx, sy, sw, sh, rop);
}

/* DIB-Bits (Zeilen unten zuerst, je 4 Byte ausgerichtet) -> Bitmap mit
   np Ebenen; map: Nummer im DIB -> Grundfarbe (bei 24 Bit nicht benutzt) */
HBITMAP gdi_dib(WORD w, WORD h, WORD bpp, const BYTE FAR *map,
                const BYTE FAR *bits, BYTE np)
{
    WORD lp = (WORD)((((DWORD)w * bpp + 31) >> 5) << 2), x, y, p;
    HBITMAP o = bm_new(w, h, np);
    struct bmp *b = bptr(o);
    if (!b)
        return 0;
    for (y = 0; y < h; y++) {
        const BYTE FAR *row = bits + (DWORD)(h - 1 - y) * lp;
        for (x = 0; x < w; x++) {
            BYTE c;
            if (bpp == 1) c = map[(row[x >> 3] >> (7 - (x & 7))) & 1];
            else if (bpp == 4) c = map[(x & 1) ? row[x >> 1] & 15 : row[x >> 1] >> 4];
            else if (bpp == 8) c = map[row[x]];
            else if (bpp == 24) {
                const BYTE FAR *e = row + x * 3;
                c = rgb2c((DWORD)e[2] | ((DWORD)e[1] << 8) | ((DWORD)e[0] << 16));
            } else c = 0;
            for (p = 0; p < b->np; p++) {
                BYTE FAR *q = MKFP(b->sel, p * b->pitch * b->h + y * b->pitch + (x >> 3));
                BYTE on = b->np == 1 ? monob[c & 15] : (c >> p) & 1;
                if (on) *q |= 0x80 >> (x & 7); else *q &= ~(0x80 >> (x & 7));
            }
        }
    }
    return o;
}

/* Kopf eines DIB lesen (BITMAPINFOHEADER oder -COREHEADER): Masse, Bits
   je Punkt, Farbtabelle -> map, -> Stelle der Bits */
const BYTE FAR *gdi_dibhdr(const BYTE FAR *bi, WORD FAR *w, WORD FAR *h, WORD FAR *bpp, BYTE FAR *map)
{
    WORD hs = *(const WORD FAR *)bi, ncol, i;
    BYTE core = hs == 12;
    *w = *(const WORD FAR *)(bi + 4);
    *h = core ? *(const WORD FAR *)(bi + 6) : *(const WORD FAR *)(bi + 8);
    *bpp = core ? *(const WORD FAR *)(bi + 10) : *(const WORD FAR *)(bi + 14);
    ncol = *bpp <= 8 ? (WORD)(1 << *bpp) : 0;
    if (!core && *(const WORD FAR *)(bi + 32) && *bpp <= 8)
        ncol = *(const WORD FAR *)(bi + 32);
    for (i = 0; i < ncol && i < 256; i++) {
        const BYTE FAR *e = bi + hs + i * (core ? 3 : 4);
        map[i] = rgb2c((DWORD)e[2] | ((DWORD)e[1] << 8) | ((DWORD)e[0] << 16));
    }
    return bi + hs + ncol * (core ? 3 : 4);
}

/* LoadBitmap: DIB-Ressource (RT_BITMAP) 1/4/8/24 Bit -> Bitmap; 1 Bit
   bleibt einfarbig, sonst 16 Farben (naechste Grundfarbe) */
HBITMAP API LoadBitmap(WORD hinst, LPCSTR name)
{
    int t = task_of_inst(hinst), m = t >= 0 ? wt[t].hmod - 1 : -1;
    DWORD r;
    WORD sel, w, h, bpp;
    static BYTE map[256];
    const BYTE FAR *bits;
    HBITMAP o;
    if (hinst > 0 && hinst <= NMOD)
        m = hinst - 1;
    if (m < 0) {
        glog("LoadBitmap: Systembitmaps (OBM_) gibt es noch nicht");
        return 0;
    }
    r = w16_res_find(m, MKFP(0, 2), name);
    if (!r || !(sel = w16_res_load(m, r)))
        return 0;
    bits = gdi_dibhdr(MKFP(sel, 0), &w, &h, &bpp, map);
    o = gdi_dib(w, h, bpp, map, bits, (BYTE)(bpp == 1 ? 1 : 4));
    seg_del(sel);
    return o;
}

/* ---- fuer Write & Co.: Kleinkram ohne eigene Wirkung ---- */
short API SetStretchBltMode(HDC h, short m) { (void)h; (void)m; return 1; }
short API SetTextJustification(HDC h, short extra, short n)
{
    (void)h; (void)extra; (void)n;
    return 1;
}
/* NULL = Beschneidung zuruecksetzen (Regionen gibt es nicht) */
short API SelectClipRgn(HDC h, WORD rgn)
{
    struct dcs *d = dptr(h);
    struct wnd *w;
    if (!d)
        return 0;
    if (!rgn && !d->mem) {
        w = wptr(d->hwnd);
        clip_window(d->hwnd, w && d->ox == w->cr.left && d->oy == w->cr.top, &d->c);
    }
    return d->c.n ? 2 : 1;
}
/* sichtbar = schneidet eines der Clip-Rechtecke (logisch -> Geraet grob
   ueber den Ursprung; Abbildungsmodi spielen hier keine Rolle) */
BOOL API RectVisible(HDC h, const RECT FAR *r)
{
    struct dcs *d = dptr(h);
    short i, l, t, rr, b;
    if (!d)
        return 0;
    l = r->left + d->ox; t = r->top + d->oy; rr = r->right + d->ox; b = r->bottom + d->oy;
    for (i = 0; i < d->c.n; i++)
        if (l < d->c.r[i].right && rr > d->c.r[i].left &&
            t < d->c.r[i].bottom && b > d->c.r[i].top)
            return 1;
    return 0;
}
BOOL API PlayMetaFile(HDC h, WORD mf) { (void)h; (void)mf; return 1; }
short API MulDiv(short a, short b, short c)
{
    long p;
    if (!c)
        return -32768;
    p = (long)a * b;
    p = p >= 0 ? (p + (c > 0 ? c : -c) / 2) / c : (p - (c > 0 ? c : -c) / 2) / c;
    if (p > 32767 || p < -32767)
        return -32768;
    return (short)p;
}
DWORD API SetBitmapDimension(HBITMAP b, short x, short y)
{
    (void)b; (void)x; (void)y;
    return 0;
}
BOOL API GetCharWidth(HDC h, WORD first, WORD last, short FAR *buf)
{
    (void)h;
    while (first <= last) {
        *buf++ = FONT_W;
        if (first++ == 0xFFFF)
            break;
    }
    return 1;
}
