/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Symbole (Icons) wie Windows 3.x: LoadIcon (RT_GROUP_ICON),
 * ExtractIcon aus NE-Dateien und .ICO, CreateIcon, DrawIcon, DestroyIcon.
 * Eigener Code.
 *
 * Ein Symbol = Farbteil (XOR, 16 Farben oder einfarbig) + AND-Maske
 * (einfarbig, 1 = durchsichtig). DrawIcon: Ziel AND Maske, dann XOR
 * Farbteil - wie Windows.
 */
#include "gui.h"

HBITMAP gdi_dib(WORD w, WORD h, WORD bpp, const BYTE FAR *map,
                const BYTE FAR *bits, BYTE np);
const BYTE FAR *gdi_dibhdr(const BYTE FAR *bi, WORD FAR *w, WORD FAR *h,
                           WORD FAR *bpp, BYTE FAR *map);
HDC API CreateCompatibleDC(HDC h);
HGDIOBJ API SelectObject(HDC h, HGDIOBJ o);
BOOL API DeleteObject(HGDIOBJ o);
BOOL API BitBlt(HDC hd, short x, short y, short w, short h, HDC hs, short sx, short sy, DWORD rop);
HBITMAP API CreateBitmap(short w, short h, BYTE planes, BYTE bpp, const void FAR *bits);
DWORD API SetTextColor(HDC h, DWORD c);
DWORD API SetBkColor(HDC h, DWORD c);
BOOL API PatBlt(HDC h, short x, short y, short w, short hh, DWORD rop);
HBRUSH API CreateSolidBrush(DWORD color);
BOOL API DeleteDC(HDC h);

#define NICON   64
#define HICO(i) ((WORD)(0x5800u + ((i) << 2)))
static struct icn { BYTE used; HBITMAP xb, ab; WORD w, h; } ic[NICON];
static BYTE imap[256];

static struct icn *iptr(WORD h)
{
    WORD i = (h - 0x5800u) >> 2;
    if (h < 0x5800u || i >= NICON || !ic[i].used || HICO(i) != h)
        return 0;
    return &ic[i];
}
static WORD icon_new(HBITMAP xb, HBITMAP ab, WORD w, WORD h)
{
    WORD i;
    for (i = 0; i < NICON && ic[i].used; i++)
        ;
    if (i == NICON || !xb || !ab) {
        if (xb) DeleteObject(xb);
        if (ab) DeleteObject(ab);
        return 0;
    }
    ic[i].used = 1; ic[i].xb = xb; ic[i].ab = ab; ic[i].w = w; ic[i].h = h;
    return HICO(i);
}

/* Symbol aus einem Icon-DIB (Hoehe doppelt: Farbteil, dann AND-Maske) */
static WORD icon_dib(const BYTE FAR *bi)
{
    WORD w, h, bpp, lp;
    static BYTE amap[2] = { 0, 15 };
    const BYTE FAR *bits = gdi_dibhdr(bi, &w, &h, &bpp, imap);
    HBITMAP xb, ab;
    h /= 2;
    lp = (WORD)((((DWORD)w * bpp + 31) >> 5) << 2);
    xb = gdi_dib(w, h, bpp, imap, bits, (BYTE)(bpp == 1 ? 1 : 4));
    ab = gdi_dib(w, h, 1, amap, bits + (DWORD)lp * h, 1);
    return icon_new(xb, ab, w, h);
}

/* beste Eintragsnummer in einem GRPICONDIR (32x32, dann 16 Farben) */
static WORD grp_best(const BYTE FAR *g, WORD esz, WORD idoff)
{
    WORD n = *(const WORD FAR *)(g + 4), i, best = 0, bs = 0;
    for (i = 0; i < n; i++) {
        const BYTE FAR *e = g + 6 + i * esz;
        WORD s = 0;
        if (e[0] == 32 && e[1] == 32) s += 4;
        if (e[2] == 16 || *(const WORD FAR *)(e + 6) == 4) s += 2;
        if (e[2] == 2 || *(const WORD FAR *)(e + 6) == 1) s += 1;
        if (s > bs || !i) { bs = s; best = i; }
    }
    return *(const WORD FAR *)(g + 6 + best * esz + idoff);
}

/* eingebautes Symbol (IDI_*): Fenster mit blauer Titelleiste, eigener
   Entwurf */
static WORD sys_icon;
static WORD icon_sys(void)
{
    static BYTE andb[32 * 4];
    HBITMAP xb, ab;
    HDC dc;
    HGDIOBJ o;
    HBRUSH b;
    WORD i;
    if (sys_icon && iptr(sys_icon))
        return sys_icon;
    for (i = 0; i < sizeof(andb); i++)
        andb[i] = 0xFF;
    for (i = 4; i < 28; i++) {                  /* deckend 26x24 in der Mitte */
        andb[i * 4] = 0xE0; andb[i * 4 + 1] = 0; andb[i * 4 + 2] = 0; andb[i * 4 + 3] = 0x07;
    }
    ab = CreateBitmap(32, 32, 1, 1, andb);
    xb = CreateBitmap(32, 32, 1, 4, 0);
    dc = CreateCompatibleDC(0);
    if (!dc || !xb || !ab)
        return 0;
    o = SelectObject(dc, xb);
    PatBlt(dc, 0, 0, 32, 32, 0x00000042UL);             /* schwarz     */
    b = CreateSolidBrush(0xFFFFFFUL);
    SelectObject(dc, b);
    PatBlt(dc, 4, 5, 24, 22, 0x00F00021UL);             /* weiss       */
    DeleteObject(SelectObject(dc, CreateSolidBrush(0x800000UL)));
    PatBlt(dc, 4, 5, 24, 5, 0x00F00021UL);              /* Titel blau  */
    DeleteObject(SelectObject(dc, b));
    SelectObject(dc, o);
    DeleteDC(dc);
    sys_icon = icon_new(xb, ab, 32, 32);
    return sys_icon;
}

WORD API LoadIcon(WORD hinst, LPCSTR name)
{
    int t = task_of_inst(hinst), m = t >= 0 ? wt[t].hmod - 1 : -1;
    DWORD r;
    WORD sel, id, h;
    if (hinst > 0 && hinst <= NMOD)
        m = hinst - 1;
    if (m < 0 || !hinst)
        return icon_sys();
    r = w16_res_find(m, MKFP(0, 14), name);     /* RT_GROUP_ICON         */
    if (!r || !(sel = w16_res_load(m, r)))
        return icon_sys();
    id = grp_best(MKFP(sel, 0), 14, 12);
    seg_del(sel);
    r = w16_res_find(m, MKFP(0, 3), MKFP(0, id));   /* RT_ICON           */
    if (!r || !(sel = w16_res_load(m, r)))
        return icon_sys();
    h = icon_dib(MKFP(sel, 0));
    seg_del(sel);
    return h ? h : icon_sys();
}

WORD API CreateIcon(WORD hinst, short w, short h, BYTE planes, BYTE bpp,
                    const void FAR *andb, const void FAR *xorb)
{
    (void)hinst;
    return icon_new(CreateBitmap(w, h, planes, bpp, xorb),
                    CreateBitmap(w, h, 1, 1, andb), w, h);
}

BOOL API DestroyIcon(WORD h)
{
    struct icn *c = iptr(h);
    if (!c || h == sys_icon)
        return 0;
    DeleteObject(c->xb);
    DeleteObject(c->ab);
    c->used = 0;
    return 1;
}

BOOL API DrawIcon(HDC hdc, short x, short y, WORD h)
{
    static HDC mdc;
    struct icn *c = iptr(h);
    HGDIOBJ o;
    DWORD tc, bc;
    if (!c)
        return 0;
    if (!mdc && !(mdc = CreateCompatibleDC(0)))
        return 0;
    tc = SetTextColor(hdc, 0);                  /* einfarbig: 0 schwarz, */
    bc = SetBkColor(hdc, 0xFFFFFFUL);           /* 1 weiss               */
    o = SelectObject(mdc, c->ab);
    BitBlt(hdc, x, y, c->w, c->h, mdc, 0, 0, 0x008800C6UL);     /* SRCAND    */
    SelectObject(mdc, c->xb);
    BitBlt(hdc, x, y, c->w, c->h, mdc, 0, 0, 0x00660046UL);     /* SRCINVERT */
    SelectObject(mdc, o);
    SetTextColor(hdc, tc);
    SetBkColor(hdc, bc);
    return 1;
}

/* ---- ExtractIcon: Symbol aus einer anderen Datei (NE oder .ICO) ---- */
static WORD xh;
static unsigned long xpos;
static int xseek(unsigned long p) { xpos = p; return FAPI_FAILED(DosSeek(xh, &xpos)); }
static WORD xsel;                               /* 16 KB Arbeitspuffer   */
#define XB ((BYTE FAR *)MKFP(xsel, 0))
static int xread(WORD off, WORD n)
{
    WORD r = fapi_sel(FAPI_READ, xsel, xh, off, n);
    return FAPI_FAILED(r) || r != n;
}

/* NE: Ressourcentabelle nach xsel:8192, -> Verschiebung (0 = Fehler) */
static WORD ne_restab(WORD FAR *nt)
{
    unsigned long ne;
    WORD rt, rn;
    if (xseek(0) || xread(0, 64) || XB[0] != 'M' || XB[1] != 'Z')
        return 0;
    ne = *(DWORD FAR *)(XB + 0x3C);
    if (xseek(ne) || xread(0, 64) || XB[0] != 'N' || XB[1] != 'E')
        return 0;
    rt = *(WORD FAR *)(XB + 0x24);
    rn = *(WORD FAR *)(XB + 0x26);             /* = Tabelle der Namen   */
    if (rn <= rt || rn - rt > 8000)
        return 0;
    if (xseek(ne + rt) || xread(8192, rn - rt))
        return 0;
    *nt = rn - rt;
    return 1;
}
/* Eintrag (Typ, n-ter bzw. Kennung) in der Tabelle bei 8192: -> Stelle in
   Datei und Laenge; idx < 0x8000: n-ter, sonst Kennung = idx */
static int ne_find(WORD type, WORD idx, unsigned long FAR *pos, WORD FAR *len)
{
    WORD sh = *(WORD FAR *)(XB + 8192), o = 8194, n, k;
    for (;;) {
        WORD t = *(WORD FAR *)(XB + o);
        if (!t)
            return 0;
        n = *(WORD FAR *)(XB + o + 2);
        o += 8;
        for (k = 0; k < n; k++, o += 12) {
            BYTE FAR *e = XB + o;
            if (t != type)
                continue;
            if ((idx & 0x8000u) ? *(WORD FAR *)(e + 6) == idx : k == idx) {
                *pos = (unsigned long)*(WORD FAR *)e << sh;
                *len = (WORD)((unsigned long)*(WORD FAR *)(e + 2) << sh);
                return 1;
            }
        }
    }
}
static WORD ne_count(WORD type)
{
    WORD o = 8194;
    for (;;) {
        WORD t = *(WORD FAR *)(XB + o), n;
        if (!t)
            return 0;
        n = *(WORD FAR *)(XB + o + 2);
        if (t == type)
            return n;
        o += 8 + n * 12;
    }
}

static char xpath[FAPI_PATHLEN];
WORD API ExtractIcon(WORD hinst, LPCSTR file, WORD idx)
{
    WORD r = 0, nt, len, id;
    unsigned long pos;
    (void)hinst;
    if (!xsel && !(xsel = seg_new(16384)))
        return 0;
    fstrncpy(xpath, file, sizeof(xpath));
    xh = DosOpen(xpath);
    if (FAPI_FAILED(xh))
        return 0;
    if (ne_restab(&nt)) {
        if (idx == 0xFFFF)
            r = ne_count(0x800E);
        else if (ne_find(0x800E, idx, &pos, &len) && len <= 8000 &&
                 !xseek(pos) && !xread(0, len)) {
            id = grp_best(XB, 14, 12) | 0x8000u;
            if (ne_find(0x8003, id, &pos, &len) && len <= 16000 &&
                !xseek(pos) && !xread(0, len))
                r = icon_dib(XB);
        }
    } else if (!xseek(0) && !xread(0, 6) && *(WORD FAR *)XB == 0 &&
               *(WORD FAR *)(XB + 2) == 1) {        /* .ICO              */
        WORD n = *(WORD FAR *)(XB + 4);
        if (idx == 0xFFFF)
            r = 1;
        else if (!idx && n && n <= 64 && !xread(0, 6 + n * 16)) {
            const BYTE FAR *e;
            WORD i, bi = 0, bs = 0;
            for (i = 0; i < n; i++) {               /* wie grp_best, Index */
                WORD s = 0;
                e = XB + 6 + i * 16;
                if (e[0] == 32 && e[1] == 32) s += 4;
                if (e[2] == 16) s += 2;
                if (s > bs || !i) { bs = s; bi = i; }
            }
            e = XB + 6 + bi * 16;
            pos = *(DWORD FAR *)(e + 12);
            len = (WORD)*(DWORD FAR *)(e + 8);
            if (len <= 16000 && !xseek(pos) && !xread(0, len))
                r = icon_dib(XB);
        }
    }
    DosClose(xh);
    return r;
}
