/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - USER fuer Win16-Programme: Fenster, Nachrichten, Eingabe,
 * Rahmen (CUA-Stil, eigener Entwurf), Knoepfe, MessageBox.
 *
 * Nachrichten: je Task eine Schlange (krnl.c struct wtask). WM_PAINT und
 * WM_TIMER entstehen beim Abholen wie unter Windows. Eine Fensterprozedur
 * laeuft immer im Task, dem das Fenster gehoert (auf dessen Stapel - Win16-
 * Programme setzen SS = DS voraus); SendMessage an einen anderen Task geht
 * ueber die Hauptschleife (Task wechselt hin und zurueck).
 *
 * Bedienung wie CUA: Ziehen an der Titelleiste verschiebt (Umriss), Klick
 * holt nach vorn, Doppelklick auf das Systemfeld und Alt+F4 schliessen.
 */
#include "gui.h"

struct wnd ww[NWND];
HWND desktop;
static WORD zmax;
static HWND active, focus, capture;

/* ---- Klassen ------------------------------------------------------- */
#define NCLS 24
struct wcls {
    BYTE used, internal;
    char name[24];
    WNDPROC proc;
    WORD style, hinst, cbwnd;
    HBRUSH bg;
    LPCSTR menu;                /* Menuename aus WNDCLASS           */
};
static struct wcls cl[NCLS];

static DWORD syscol[21] = {
    0xC0C0C0UL, 0x808000UL, 0x800000UL, 0xFFFFFFUL, 0xFFFFFFUL, 0xFFFFFFUL,
    0x000000UL, 0x000000UL, 0x000000UL, 0xFFFFFFUL, 0xC0C0C0UL, 0xC0C0C0UL,
    0xC0C0C0UL, 0x800000UL, 0xFFFFFFUL, 0xC0C0C0UL, 0x808080UL, 0x808080UL,
    0x000000UL, 0x000000UL, 0xFFFFFFUL
};
#define C_DESK   1
#define C_ACAP   2
#define C_ICAP   3
#define C_FRAME  6
#define C_CAPTXT 9
#define C_BTN    15
#define C_SHADOW 16
#define C_BTNTXT 18
#define C_HILITE 20
DWORD GetSysColor_c(WORD i) { return i < 21 ? syscol[i] : 0; }
static BYTE sc(WORD i) { return rgb2c(GetSysColor_c(i)); }

#define WS_CHILD        0x40000000UL
#define WS_POPUP        0x80000000UL
#define WS_VISIBLE      0x10000000UL
#define WS_DISABLED     0x08000000UL
#define WS_CAPTION      0x00C00000UL
#define WS_BORDER       0x00800000UL
#define WS_DLGFRAME     0x00400000UL
#define WS_SYSMENU      0x00080000UL
#define WS_THICKFRAME   0x00040000UL
#define WS_MINIMIZEBOX  0x00020000UL
#define WS_MAXIMIZEBOX  0x00010000UL
#define WS_VSCROLL      0x00200000UL
#define WS_HSCROLL      0x00100000UL

#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_MOVE         0x0003
#define WM_SIZE         0x0005
#define WM_ACTIVATE     0x0006
#define WM_SETFOCUS     0x0007
#define WM_KILLFOCUS    0x0008
#define WM_SETTEXT      0x000C
#define WM_GETTEXT      0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_ERASEBKGND   0x0014
#define WM_SHOWWINDOW   0x0018
#define WM_NCCREATE     0x0081
#define WM_NCDESTROY    0x0082
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_CHAR         0x0102
#define WM_SYSKEYDOWN   0x0104
#define WM_COMMAND      0x0111
#define WM_SYSCOMMAND   0x0112
#define WM_TIMER        0x0113
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN  0x0204
#define WM_RBUTTONUP    0x0205
#define WM_SYSCHAR      0x0106
#define WM_HSCROLL      0x0114
#define WM_VSCROLL      0x0115
#define SC_CLOSE        0xF060
#define SC_MOUSEMENU    0xF090
#define SC_KEYMENU      0xF100

struct wnd *wptr(HWND h)
{
    WORD i = WI(h);
    if (h < 0x4000u || i >= NWND || !ww[i].used || HW(i) != h)
        return 0;
    return &ww[i];
}

static void rset(RECT FAR *r, short l, short t, short rr, short b)
{
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
}
static int rhit(const RECT FAR *r, short x, short y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}
static int rover(const RECT FAR *a, const RECT FAR *b)
{
    return a->left < b->right && b->left < a->right &&
           a->top < b->bottom && b->top < a->bottom;
}

short wnd_border(struct wnd *w)
{
    if (w->style & WS_THICKFRAME) return 4;
    if (w->style & WS_DLGFRAME) return 3;
    if (w->style & WS_BORDER) return 1;
    return 0;
}

static void calc_client(struct wnd *w)
{
    short b = wnd_border(w);
    w->cr = w->wr;
    w->cr.left += b; w->cr.right -= b;
    w->cr.top += b; w->cr.bottom -= b;
    if ((w->style & WS_CAPTION) == WS_CAPTION)
        w->cr.top += CAPH;
    if (w->menu && !(w->style & WS_CHILD))
        w->cr.top += MENUH;
    if (w->style & WS_VSCROLL)
        w->cr.right -= SBW;
    if (w->style & WS_HSCROLL)
        w->cr.bottom -= SBW;
    if (w->cr.right < w->cr.left) w->cr.right = w->cr.left;
    if (w->cr.bottom < w->cr.top) w->cr.bottom = w->cr.top;
}

static struct wnd *top_of(struct wnd *w)
{
    while (w && (w->style & WS_CHILD) && w->parent)
        w = wptr(w->parent);
    return w;
}

/* Clipping: Bereich ohne die Fenster darueber */
static void clip_sub(struct clip FAR *c, const RECT FAR *s)
{
    struct clip o;
    short i;
    o = *c;
    c->n = 0;
    for (i = 0; i < o.n; i++) {
        RECT r = o.r[i], p;
        if (!rover(&r, s)) {
            if (c->n < NCLIP) c->r[c->n++] = r;
            continue;
        }
        if (s->top > r.top) {
            rset(&p, r.left, r.top, r.right, s->top);
            if (c->n < NCLIP) c->r[c->n++] = p;
        }
        if (s->bottom < r.bottom) {
            rset(&p, r.left, s->bottom, r.right, r.bottom);
            if (c->n < NCLIP) c->r[c->n++] = p;
        }
        {
            short t = s->top > r.top ? s->top : r.top;
            short b = s->bottom < r.bottom ? s->bottom : r.bottom;
            if (s->left > r.left) {
                rset(&p, r.left, t, s->left, b);
                if (c->n < NCLIP) c->r[c->n++] = p;
            }
            if (s->right < r.right) {
                rset(&p, s->right, t, r.right, b);
                if (c->n < NCLIP) c->r[c->n++] = p;
            }
        }
    }
}

void clip_and(struct clip FAR *c, const RECT FAR *r);

void clip_window(HWND h, int client, struct clip FAR *c)
{
    struct wnd *w = wptr(h), *t, *p;
    short i;
    c->n = 1;
    rset(&c->r[0], 0, 0, SCR_W, SCR_H);
    if (!w)                                     /* Bildschirm (Desktop) */
        return;
    if (!w->visible) {
        c->n = 0;
        return;
    }
    c->r[0] = client ? w->cr : w->wr;
    for (p = w; (p->style & WS_CHILD) && p->parent; ) {
        struct wnd *q = wptr(p->parent);
        if (!q)
            break;
        /* Geschwister mit hoeherem Rang liegen darueber */
        for (i = 0; i < NWND; i++) {
            struct wnd *o = &ww[i];
            if (o->used && o->visible && (o->style & WS_CHILD) && o != p &&
                o->parent == p->parent && o->z > p->z && rover(&o->wr, &p->wr))
                clip_sub(c, &o->wr);
        }
        p = q;
        if (!p->visible) {
            c->n = 0;
            return;
        }
        clip_and(c, &p->cr);
    }
    t = top_of(w);
    for (i = 0; i < NWND; i++) {
        struct wnd *o = &ww[i];
        if (!o->used || !o->visible || (o->style & WS_CHILD) || o == t || o->z <= t->z)
            continue;
        clip_sub(c, &o->wr);
    }
}

/* ---- Zeichnen des Rahmens ------------------------------------------- */
void dc_rawfill(HDC h, short x0, short y0, short x1, short y1, BYTE c);
void dc_rawtext(HDC h, short x, short y, LPCSTR s, WORD n, BYTE fg);
void dc_fillrect(HDC h, const RECT FAR *r, HBRUSH b);
void dc_invert(HDC h, const RECT FAR *r);

void ui_bevel(HDC dc, short l, short t, short r, short b, int down)
{
    BYTE hi = sc(C_HILITE), sh = sc(C_SHADOW), bk = 0;
    dc_rawfill(dc, l, t, r, b, sc(C_BTN));
    dc_rawfill(dc, l, t, r, t + 1, down ? sh : hi);
    dc_rawfill(dc, l, t, l + 1, b, down ? sh : hi);
    dc_rawfill(dc, l, b - 1, r, b, down ? hi : bk);
    dc_rawfill(dc, r - 1, t, r, b, down ? hi : bk);
    if (!down) {
        dc_rawfill(dc, l + 1, b - 2, r - 1, b - 1, sh);
        dc_rawfill(dc, r - 2, t + 1, r - 1, b - 1, sh);
    }
}

/* Bildlaufleisten im Rahmen: Lage des Schiebers */
static short sb_thumb(struct wnd *w, int v, short len)
{
    short *s = w->sb[v];
    long r = (long)s[1] - s[0];
    if (len <= 0 || r <= 0)
        return 0;
    return (short)((long)(s[2] - s[0]) * len / r);
}
static void arrow(HDC dc, short x, short y, int dir)   /* 0 hoch 1 runter 2 links 3 rechts */
{
    short k;
    for (k = 0; k < 4; k++)
        switch (dir) {
        case 0: dc_rawfill(dc, x + 7 - k, y + 6 + k, x + 8 + k + 1, y + 7 + k, 0); break;
        case 1: dc_rawfill(dc, x + 4 + k, y + 6 + k, x + 12 - k, y + 7 + k, 0); break;
        case 2: dc_rawfill(dc, x + 6 + k, y + 7 - k, x + 7 + k, y + 9 + k, 0); break;
        default: dc_rawfill(dc, x + 6 + k, y + 4 + k, x + 7 + k, y + 12 - k, 0); break;
        }
}
static void sb_paint(HDC dc, struct wnd *w)
{
    short l = w->cr.left, t = w->cr.top, r = w->cr.right, b = w->cr.bottom, p;
    if (w->style & WS_VSCROLL) {
        ui_bevel(dc, r, t, r + SBW, t + SBW, 0); arrow(dc, r, t, 0);
        ui_bevel(dc, r, b - SBW, r + SBW, b, 0); arrow(dc, r, b - SBW, 1);
        dc_rawfill(dc, r, t + SBW, r + SBW, b - SBW, 7);
        if (b - t > 3 * SBW && w->sb[1][1] > w->sb[1][0]) {
            p = t + SBW + sb_thumb(w, 1, b - t - 3 * SBW);
            ui_bevel(dc, r, p, r + SBW, p + SBW, 0);
        }
    }
    if (w->style & WS_HSCROLL) {
        ui_bevel(dc, l, b, l + SBW, b + SBW, 0); arrow(dc, l, b, 2);
        ui_bevel(dc, r - SBW, b, r, b + SBW, 0); arrow(dc, r - SBW, b, 3);
        dc_rawfill(dc, l + SBW, b, r - SBW, b + SBW, 7);
        if (r - l > 3 * SBW && w->sb[0][1] > w->sb[0][0]) {
            p = l + SBW + sb_thumb(w, 0, r - l - 3 * SBW);
            ui_bevel(dc, p, b, p + SBW, b + SBW, 0);
        }
    }
    if ((w->style & WS_VSCROLL) && (w->style & WS_HSCROLL))
        dc_rawfill(dc, r, b, r + SBW, b + SBW, sc(C_BTN));
}

static void nc_paint(struct wnd *w)
{
    HDC dc;
    short b = wnd_border(w), l = w->wr.left, t = w->wr.top, r = w->wr.right, bt = w->wr.bottom;
    int act = top_of(w) && HW(top_of(w) - ww) == active;
    if ((w->style & WS_CHILD) && mdi_is_kid(HW(w - ww)))
        act = act && mdi_kid_active(HW(w - ww));
    if (!w->visible || !b && (w->style & WS_CAPTION) != WS_CAPTION &&
        !(w->style & (WS_VSCROLL | WS_HSCROLL)))
        return;
    dc = dc_get(HW(w - ww), 0, 0);
    if (!dc)
        return;
    if (b) {
        dc_rawfill(dc, l, t, r, t + 1, sc(C_FRAME));
        dc_rawfill(dc, l, bt - 1, r, bt, sc(C_FRAME));
        dc_rawfill(dc, l, t, l + 1, bt, sc(C_FRAME));
        dc_rawfill(dc, r - 1, t, r, bt, sc(C_FRAME));
        if (b > 1) {
            BYTE c = act ? sc(10) : sc(11);
            dc_rawfill(dc, l + 1, t + 1, r - 1, t + b, c);
            dc_rawfill(dc, l + 1, bt - b, r - 1, bt - 1, c);
            dc_rawfill(dc, l + 1, t + 1, l + b, bt - 1, c);
            dc_rawfill(dc, r - b, t + 1, r - 1, bt - 1, c);
            if (w->style & WS_THICKFRAME) {    /* Eckmarken            */
                BYTE f = sc(C_FRAME);
                dc_rawfill(dc, l, t + CAPH + b - 1, l + b, t + CAPH + b, f);
                dc_rawfill(dc, r - b, t + CAPH + b - 1, r, t + CAPH + b, f);
                dc_rawfill(dc, l, bt - CAPH - b, l + b, bt - CAPH - b + 1, f);
                dc_rawfill(dc, r - b, bt - CAPH - b, r, bt - CAPH - b + 1, f);
                dc_rawfill(dc, l + CAPH + b - 1, t, l + CAPH + b, t + b, f);
                dc_rawfill(dc, l + CAPH + b - 1, bt - b, l + CAPH + b, bt, f);
                dc_rawfill(dc, r - CAPH - b, t, r - CAPH - b + 1, t + b, f);
                dc_rawfill(dc, r - CAPH - b, bt - b, r - CAPH - b + 1, bt, f);
            }
        }
    }
    if ((w->style & WS_CAPTION) == WS_CAPTION) {
        short cl = l + b, ct = t + b, cr = r - b, cb = t + b + CAPH, n, tw;
        BYTE bg = act ? sc(C_ACAP) : sc(C_ICAP), fg = act ? sc(C_CAPTXT) : sc(19);
        dc_rawfill(dc, cl, ct, cr, cb - 1, bg);
        dc_rawfill(dc, cl, cb - 1, cr, cb, sc(C_FRAME));
        if (w->style & WS_SYSMENU) {           /* Systemfeld: Balken   */
            ui_bevel(dc, cl, ct, cl + CAPH - 1, cb - 1, 0);
            dc_rawfill(dc, cl + 4, ct + 8, cl + CAPH - 5, ct + 11, sc(C_SHADOW));
            dc_rawfill(dc, cl + 3, ct + 7, cl + CAPH - 6, ct + 10, 15);
            dc_rawfill(dc, cl + CAPH - 1, ct, cl + CAPH, cb - 1, sc(C_FRAME));
            cl += CAPH;
        }
        if (w->style & WS_MAXIMIZEBOX) {        /* Pfeil nach oben      */
            short x = cr - CAPH + 1, k;
            ui_bevel(dc, x, ct, cr, cb - 1, 0);
            for (k = 0; k < 4; k++)
                dc_rawfill(dc, x + 9 - k, ct + 7 + k, x + 10 + k, ct + 8 + k, 0);
            dc_rawfill(dc, x - 1, ct, x, cb - 1, sc(C_FRAME));
            cr = x - 1;
        }
        if (w->style & WS_MINIMIZEBOX) {        /* Pfeil nach unten     */
            short x = cr - CAPH + 1, k;
            ui_bevel(dc, x, ct, cr, cb - 1, 0);
            for (k = 0; k < 4; k++)
                dc_rawfill(dc, x + 6 + k, ct + 7 + k, x + 13 - k, ct + 8 + k, 0);
            dc_rawfill(dc, x - 1, ct, x, cb - 1, sc(C_FRAME));
            cr = x - 1;
        }
        n = fstrlen(w->title);
        tw = n * FONT_W;
        if (tw > cr - cl - 4) {
            n = (cr - cl - 4) / FONT_W;
            tw = n * FONT_W;
        }
        if (n > 0)
            dc_rawtext(dc, cl + (cr - cl - tw) / 2, ct + 2, w->title, n, fg);
    }
    if (w->menu && !(w->style & WS_CHILD))
        menu_bar_paint(dc, w);
    sb_paint(dc, w);
    dc_release(dc);
}

/* ---- Fensterverwaltung --------------------------------------------- */
static int task_win(struct wnd *w) { return w->task; }

static void invalidate(struct wnd *w, const RECT FAR *r, int erase)
{
    RECT c;
    if (!w->visible)
        return;
    rset(&c, 0, 0, w->cr.right - w->cr.left, w->cr.bottom - w->cr.top);
    if (r) {
        if (r->left > c.left) c.left = r->left;
        if (r->top > c.top) c.top = r->top;
        if (r->right < c.right) c.right = r->right;
        if (r->bottom < c.bottom) c.bottom = r->bottom;
    }
    if (c.left >= c.right || c.top >= c.bottom)
        return;
    if (!w->need_paint)
        w->upd = c;
    else {
        if (c.left < w->upd.left) w->upd.left = c.left;
        if (c.top < w->upd.top) w->upd.top = c.top;
        if (c.right > w->upd.right) w->upd.right = c.right;
        if (c.bottom > w->upd.bottom) w->upd.bottom = c.bottom;
    }
    w->need_paint = 1;
    if (erase) {
        short i;
        w->erase = 1;
        /* der Hintergrund loescht auch die Kinder: neu zeichnen */
        for (i = 0; i < NWND; i++)
            if (ww[i].used && ww[i].visible && (ww[i].style & WS_CHILD) &&
                ww[i].parent == HW(w - ww) && rover(&ww[i].wr, &w->cr)) {
                ww[i].need_ncpaint = 1;
                invalidate(&ww[i], 0, 1);
            }
    }
}

/* Bildschirmbereich neu: Desktop und alle Fenster darin */
static RECT desk_upd;
static BYTE desk_need;
static void expose(const RECT FAR *s)
{
    short i;
    if (!desk_need) {
        desk_upd = *s;
        desk_need = 1;
    } else {
        if (s->left < desk_upd.left) desk_upd.left = s->left;
        if (s->top < desk_upd.top) desk_upd.top = s->top;
        if (s->right > desk_upd.right) desk_upd.right = s->right;
        if (s->bottom > desk_upd.bottom) desk_upd.bottom = s->bottom;
    }
    for (i = 0; i < NWND; i++) {
        struct wnd *w = &ww[i];
        RECT r;
        if (!w->used || !w->visible || !rover(&w->wr, s))
            continue;
        w->need_ncpaint = 1;
        r = *s;
        r.left -= w->cr.left; r.right -= w->cr.left;
        r.top -= w->cr.top; r.bottom -= w->cr.top;
        invalidate(w, &r, 1);
    }
}

static void children_move(struct wnd *p, short dx, short dy)
{
    short i;
    for (i = 0; i < NWND; i++) {
        struct wnd *c = &ww[i];
        if (!c->used || !(c->style & WS_CHILD) || c->parent != HW(p - ww))
            continue;
        c->wr.left += dx; c->wr.right += dx; c->wr.top += dy; c->wr.bottom += dy;
        calc_client(c);
        children_move(c, dx, dy);
    }
}

static void win_move(struct wnd *w, short x, short y, short cx, short cy)
{
    RECT old = w->wr;
    short dx, dy;
    struct wnd *p;
    if ((w->style & WS_CHILD) && (p = wptr(w->parent)) != 0) {
        x += p->cr.left;
        y += p->cr.top;
    }
    dx = x - w->wr.left;
    dy = y - w->wr.top;
    rset(&w->wr, x, y, x + cx, y + cy);
    calc_client(w);
    children_move(w, dx, dy);
    if (w->visible) {
        expose(&old);
        expose(&w->wr);
    }
    send_msg(HW(w - ww), WM_MOVE, 0, MAKELONG(w->cr.left, w->cr.top));
    send_msg(HW(w - ww), WM_SIZE, 0,
             MAKELONG(w->cr.right - w->cr.left, w->cr.bottom - w->cr.top));
}

static void activate(HWND h)
{
    struct wnd *w = wptr(h), *o;
    if (w)
        w = top_of(w);
    if (!w)
        return;
    h = HW(w - ww);
    if (h == active && w->z == zmax)
        return;
    o = wptr(active);
    active = h;
    if (w->z != zmax) {
        w->z = ++zmax;
        expose(&w->wr);
    } else
        w->need_ncpaint = 1;
    if (o) {
        o->need_ncpaint = 1;
        post_msg(HW(o - ww), WM_ACTIVATE, 0, 0);
    }
    post_msg(h, WM_ACTIVATE, 1, 0);
    if (!focus || !wptr(focus) || top_of(wptr(focus)) != w) {
        focus = h;
        post_msg(h, WM_SETFOCUS, 0, 0);
    }
}

void user_task_end(int t)
{
    short i;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && ww[i].task == t) {
            if (ww[i].visible)
                expose(&ww[i].wr);
            ww[i].used = 0;
        }
    for (i = 0; i < NCLS; i++)
        if (cl[i].used && !cl[i].internal && task_of_inst(cl[i].hinst) == t)
            cl[i].used = 0;
    menu_task_end(t);
    if (!wptr(active)) active = 0;
    if (!wptr(focus)) focus = 0;
    if (!wptr(capture)) capture = 0;
}

/* ---- Nachrichten ------------------------------------------------------ */
static WORD pq_words[5];

static long call_proc(struct wnd *w, WNDPROC p, HWND h, WORD m, WORD wp, DWORD lp)
{
    if (w->internal)
        return p(h, m, wp, lp);
    pq_words[0] = h; pq_words[1] = m; pq_words[2] = wp;
    pq_words[3] = HIWORD(lp); pq_words[4] = LOWORD(lp);
    return call16((void FAR *)p, w->hinst, 5, pq_words);
}

static void process_sent(int t);

long send_msg(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    int t = cur_task, o;
    if (!w)
        return 0;
    if (w->internal || task_win(w) == t)
        return call_proc(w, w->proc, h, m, wp, lp);
    o = task_win(w);
    if (t < 0 || o < 0 || !wt[o].used) {
        post_msg(h, m, wp, lp);
        return 0;
    }
    /* anderer Task: ueber die Hauptschleife */
    while (wt[o].sent)
        task_yield_to_main();
    wt[o].sent = 1;
    wt[o].s_hwnd = h; wt[o].s_msg = m; wt[o].s_wp = wp; wt[o].s_lp = lp;
    wt[o].s_from = t;
    wt[t].wait_reply = 1;
    while (wt[o].sent == 1 && wt[o].used)
        task_yield_to_main();
    wt[t].wait_reply = 0;
    wt[o].sent = 0;
    return wt[o].s_res;
}

static void process_sent(int t)
{
    struct wnd *w;
    if (wt[t].sent != 1)
        return;
    w = wptr(wt[t].s_hwnd);
    wt[t].s_res = w ? call_proc(w, w->proc, wt[t].s_hwnd, wt[t].s_msg, wt[t].s_wp, wt[t].s_lp) : 0;
    wt[t].sent = 2;
}

static DWORD tick32;
static WORD tick_last;
static DWORD ticks(void)
{
    WORD n = DosQTicks();
    tick32 += (WORD)(n - tick_last);
    tick_last = n;
    return tick32;                      /* 100 Hz                        */
}

static POINT mpos;
BOOL post_msg(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    int t;
    struct wtask *k;
    if (!w)
        return 0;
    t = task_win(w);
    if (t < 0 || !wt[t].used) {
        if (w->internal)                /* Fenster der GUI selbst        */
            call_proc(w, w->proc, h, m, wp, lp);
        return 1;
    }
    k = &wt[t];
    if ((WORD)(k->qt + 1) % NQ == k->qh)
        return 0;                       /* voll                          */
    k->q[k->qt].hwnd = h;
    k->q[k->qt].message = m;
    k->q[k->qt].wParam = wp;
    k->q[k->qt].lParam = lp;
    k->q[k->qt].time = ticks() * 10;
    k->q[k->qt].pt = mpos;
    k->qt = (k->qt + 1) % NQ;
    return 1;
}

/* Zeitgeber */
#define NTIM 16
static struct { HWND h; WORD id, ms; int task; DWORD due; DWORD proc; } tm[NTIM];

static int timer_due(int t, MSG FAR *out)
{
    short i;
    DWORD now = ticks();
    for (i = 0; i < NTIM; i++)
        if (tm[i].ms && tm[i].task == t && (long)(now - tm[i].due) >= 0) {
            tm[i].due = now + (tm[i].ms + 9) / 10;
            if (out) {
                out->hwnd = tm[i].h; out->message = WM_TIMER;
                out->wParam = tm[i].id; out->lParam = tm[i].proc;
                out->time = now * 10; out->pt = mpos;
            }
            return 1;
        }
    return 0;
}

static struct wnd *paint_win(int t)
{
    short i;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && ww[i].task == t && ww[i].need_paint && ww[i].visible)
            return &ww[i];
    return 0;
}

int user_task_has_work(int t)
{
    struct wtask *k = &wt[t];
    if (!k->waiting)
        return 1;
    if (k->wait_reply)
        return 0 || k->sent == 1;
    return k->qh != k->qt || k->quit || k->sent == 1 || paint_win(t) || timer_due_peek(t);
}

int timer_due_peek(int t)
{
    short i;
    DWORD now = ticks();
    for (i = 0; i < NTIM; i++)
        if (tm[i].ms && tm[i].task == t && (long)(now - tm[i].due) >= 0)
            return 1;
    return 0;
}

static DWORD kmsg_lp;                   /* Umschalttasten der letzten Taste */
BOOL user_get_msg(MSG FAR *m, HWND filt, WORD lo, WORD hi, int remove, int wait)
{
    int t = cur_task;
    struct wtask *k;
    struct wnd *w;
    MSG tmsg;
    (void)filt; (void)lo; (void)hi;
    if (t < 0)
        return 0;
    k = &wt[t];
    for (;;) {
        process_sent(t);
        if (k->qh != k->qt) {
            *m = k->q[k->qh];
            if (remove)
                k->qh = (k->qh + 1) % NQ;
            if (m->message >= 0x0100 && m->message <= 0x0108)
                kmsg_lp = m->lParam;
            return m->message != WM_QUIT;
        }
        if (k->quit) {
            m->hwnd = 0; m->message = WM_QUIT; m->wParam = k->quitcode; m->lParam = 0;
            if (remove)
                k->quit = 0;
            return 0;
        }
        if ((w = paint_win(t)) != 0) {
            m->hwnd = HW(w - ww); m->message = WM_PAINT; m->wParam = 0; m->lParam = 0;
            m->time = ticks() * 10; m->pt = mpos;
            return 1;
        }
        if (timer_due(t, &tmsg)) {
            *m = tmsg;
            return 1;
        }
        if (!wait)
            return 0;
        k->waiting = 1;
        task_yield_to_main();
        k->waiting = 0;
    }
}

/* ---- Eingabe (Hauptschleife) ---------------------------------------- */
static BYTE mbtn;
static HWND drag;                       /* Verschieben am Titel          */
static short dgx, dgy;
static RECT dgr;
static BYTE dg_shown;
static DWORD last_click;
static HWND last_click_h;
static BYTE keystate[256];

static HWND child_at(HWND h, short x, short y)
{
    short i;
    struct wnd *b = 0;
    for (i = 0; i < NWND; i++) {
        struct wnd *c = &ww[i];
        if (!c->used || !c->visible || !(c->style & WS_CHILD) || c->parent != h)
            continue;
        if (rhit(&c->wr, x, y) && (!b || c->z > b->z))
            b = c;
    }
    return b ? child_at(HW(b - ww), x, y) : h;
}

static HWND win_at(short x, short y)
{
    short i;
    struct wnd *b = 0;
    for (i = 0; i < NWND; i++) {
        struct wnd *w = &ww[i];
        if (!w->used || !w->visible || (w->style & WS_CHILD) || !rhit(&w->wr, x, y))
            continue;
        if (!b || w->z > b->z)
            b = w;
    }
    return b ? child_at(HW(b - ww), x, y) : 0;
}

#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTMIN 8
#define HTMAX 9
static short hittest(struct wnd *w, short x, short y)
{
    short b = wnd_border(w);
    if (rhit(&w->cr, x, y))
        return HTCLIENT;
    if ((w->style & WS_VSCROLL) && x >= w->cr.right && x < w->cr.right + SBW &&
        y >= w->cr.top && y < w->cr.bottom)
        return HTVSCROLL;
    if ((w->style & WS_HSCROLL) && y >= w->cr.bottom && y < w->cr.bottom + SBW &&
        x >= w->cr.left && x < w->cr.right)
        return HTHSCROLL;
    if (w->menu && !(w->style & WS_CHILD) && menu_bar_hit(w, x, y) != -1)
        return HTMENU;
    if ((w->style & WS_CAPTION) == WS_CAPTION && y >= w->wr.top + b && y < w->wr.top + b + CAPH) {
        if ((w->style & WS_SYSMENU) && x < w->wr.left + b + CAPH)
            return HTSYSMENU;
        return HTCAPTION;
    }
    return 0;
}

static void drag_xor(void)
{
    HDC dc = dc_get(0, 0, 0);
    RECT r;
    if (!dc)
        return;
    SetROP2(dc, 6);                     /* R2_NOT                        */
    rset(&r, dgr.left, dgr.top, dgr.right, dgr.top + 2); dc_invert(dc, &r);
    rset(&r, dgr.left, dgr.bottom - 2, dgr.right, dgr.bottom); dc_invert(dc, &r);
    rset(&r, dgr.left, dgr.top + 2, dgr.left + 2, dgr.bottom - 2); dc_invert(dc, &r);
    rset(&r, dgr.right - 2, dgr.top + 2, dgr.right, dgr.bottom - 2); dc_invert(dc, &r);
    dc_release(dc);
}

/* Bildlaufleiste: Klick, Wiederholen beim Halten, Schieber ziehen */
static struct { HWND h; BYTE v, code, drag; short off; DWORD next; } sbm;
static void sb_post(void)
{
    struct wnd *w = wptr(sbm.h);
    if (w)
        post_msg(sbm.h, sbm.v ? WM_VSCROLL : WM_HSCROLL, sbm.code,
                 sbm.code == 4 || sbm.code == 5 ? MAKELONG(w->sb[sbm.v][2], 0) : 0);
}
static void sb_down(struct wnd *w, int v, short x, short y)
{
    short a = v ? y - w->cr.top : x - w->cr.left;
    short len = v ? w->cr.bottom - w->cr.top : w->cr.right - w->cr.left;
    short th = SBW + sb_thumb(w, v, len - 3 * SBW);
    sbm.h = HW(w - ww);
    sbm.v = (BYTE)v;
    sbm.drag = 0;
    if (a < SBW) sbm.code = 0;                          /* SB_LINEUP     */
    else if (a >= len - SBW) sbm.code = 1;              /* SB_LINEDOWN   */
    else if (w->sb[v][1] <= w->sb[v][0]) { sbm.h = 0; return; }
    else if (a < th) sbm.code = 2;                      /* SB_PAGEUP     */
    else if (a >= th + SBW) sbm.code = 3;               /* SB_PAGEDOWN   */
    else { sbm.code = 5; sbm.drag = 1; sbm.off = a - th; return; }
    sb_post();
    sbm.next = 0;
}
static DWORD ticks(void);
static int sb_mouse(short x, short y, BYTE btn, int moved)
{
    struct wnd *w = wptr(sbm.h);
    if (!sbm.h)
        return 0;
    if (!w) {
        sbm.h = 0;
        return 0;
    }
    if (sbm.drag && moved) {
        short v = sbm.v, len = (v ? w->cr.bottom - w->cr.top : w->cr.right - w->cr.left) - 3 * SBW;
        short a = (v ? y - w->cr.top : x - w->cr.left) - SBW - sbm.off;
        short *s = w->sb[v];
        if (a < 0) a = 0;
        if (a > len) a = len;
        if (len > 0) {
            short np = s[0] + (short)((long)a * (s[1] - s[0]) / len);
            if (np != s[2]) {
                s[2] = np;
                w->need_ncpaint = 1;
                sbm.code = 5;
                sb_post();
            }
        }
    }
    if (!(btn & 1)) {
        if (sbm.drag) {
            sbm.code = 4;                               /* THUMBPOSITION */
            sb_post();
        }
        sbm.code = 8;                                   /* SB_ENDSCROLL  */
        sb_post();
        sbm.h = 0;
    }
    return 1;
}
static void sb_repeat(void)
{
    DWORD now = ticks();
    if (!sbm.h || sbm.drag || !(mbtn & 1))
        return;
    if (!sbm.next) {
        sbm.next = now + 35;
        return;
    }
    if ((long)(now - sbm.next) >= 0) {
        sbm.next = now + 6;
        sb_post();
    }
}

static void mouse_event(short x, short y, BYTE btn)
{
    HWND h;
    struct wnd *w;
    BYTE down = btn & ~mbtn, up = mbtn & ~btn;
    short ht;
    int moved = x != mpos.x || y != mpos.y;
    mpos.x = x; mpos.y = y;
    if (moved)
        gdi_cursor(x, y);
    if (menu_input_mouse(x, y, btn)) {
        mbtn = btn;
        return;
    }
    if (sb_mouse(x, y, btn, moved)) {
        mbtn = btn;
        return;
    }
    mbtn = btn;
    if (drag) {
        w = wptr(drag);
        if (moved && w) {
            if (dg_shown) drag_xor();
            dgr = w->wr;
            dgr.left += x - dgx; dgr.right += x - dgx;
            dgr.top += y - dgy; dgr.bottom += y - dgy;
            drag_xor();
            dg_shown = 1;
        }
        if (up & 1) {
            if (dg_shown) drag_xor();
            dg_shown = 0;
            if (w && (x != dgx || y != dgy)) {
                struct wnd *pp = (w->style & WS_CHILD) ? wptr(w->parent) : 0;
                short ox = pp ? pp->cr.left : 0, oy = pp ? pp->cr.top : 0;
                win_move(w, dgr.left - ox, dgr.top - oy, dgr.right - dgr.left, dgr.bottom - dgr.top);
            }
            drag = 0;
        }
        return;
    }
    h = capture ? capture : win_at(x, y);
    w = wptr(h);
    if (!w)
        return;
    ht = hittest(w, x, y);
    if (capture)
        ht = HTCLIENT;
    if (down) {
        struct wnd *tw = top_of(w);
        /* modale Sperre: deaktivierte Fenster bekommen nichts */
        if (tw && (tw->style & WS_DISABLED)) {
            MessageBeep_c();
            return;
        }
        activate(h);
        {                                       /* MDI-Kind darunter aktivieren */
            struct wnd *q = w;
            while (q && (q->style & WS_CHILD)) {
                if (mdi_is_kid(HW(q - ww))) {
                    send_msg(q->parent, 0x0222, HW(q - ww), 0);   /* WM_MDIACTIVATE */
                    break;
                }
                q = wptr(q->parent);
            }
        }
        if (w->style & WS_CHILD && w->enabled && focus != h) {
            if (focus)
                post_msg(focus, WM_KILLFOCUS, h, 0);
            post_msg(h, WM_SETFOCUS, focus, 0);
            focus = h;
        }
    }
    if (ht == HTCAPTION && (down & 1)) {
        drag = (w->style & WS_CHILD) ? h : HW(top_of(w) - ww);
        dgx = x; dgy = y;
        dg_shown = 0;
        return;
    }
    if (ht == HTMENU && (down & 1)) {
        post_msg(HW(top_of(w) - ww), WM_SYSCOMMAND, SC_MOUSEMENU, MAKELONG(x, y));
        return;
    }
    if ((ht == HTVSCROLL || ht == HTHSCROLL) && (down & 1)) {
        sb_down(w, ht == HTVSCROLL, x, y);
        return;
    }
    if (ht == HTSYSMENU && (down & 1)) {
        DWORD now = ticks();
        if (last_click_h == h && now - last_click < 50)
            post_msg(h, WM_SYSCOMMAND, SC_CLOSE, 0);
        last_click = now;
        last_click_h = h;
        return;
    }
    if (ht != HTCLIENT || (w->style & WS_DISABLED))
        return;
    {
        DWORD lp = MAKELONG(x - w->cr.left, y - w->cr.top);
        WORD kf = (btn & 1) | ((btn & 2) ? 2 : 0);
        if (moved)
            post_msg(h, WM_MOUSEMOVE, kf, lp);
        if (down & 1)
            post_msg(h, WM_LBUTTONDOWN, kf, lp);
        if (up & 1)
            post_msg(h, WM_LBUTTONUP, kf, lp);
        if (down & 2)
            post_msg(h, WM_RBUTTONDOWN, kf, lp);
        if (up & 2)
            post_msg(h, WM_RBUTTONUP, kf, lp);
    }
}

static const BYTE scvk[0x59] = {
    0, 0x1B, 0,0,0,0,0,0,0,0,0,0,0,0, 0x08, 0x09,
    0,0,0,0,0,0,0,0,0,0,0,0, 0x0D, 0x11, 0,0,
    0,0,0,0,0,0,0,0,0,0, 0x10, 0,0,0,0,0,
    0,0,0,0,0,0, 0x10, 0x6A, 0x12, 0x20, 0x14, 0x70,0x71,0x72,0x73,0x74,
    0x75,0x76,0x77,0x78,0x79, 0x90, 0x91, 0x24, 0x26, 0x21, 0x6D, 0x25, 0x0C, 0x27, 0x6B, 0x23,
    0x28, 0x22, 0x2D, 0x2E, 0,0,0, 0x7A, 0x7B
};

/* Alt+Buchstabe liefert die Tastatur nur als Scancode (deutsche Lage) */
static const char altkey[0x23] = "qwertzuiop\0\0\0\0asdfghjkl\0\0\0\0\0yxcvbnm";

static void key_event(WORD k)
{
    BYTE ch = (BYTE)k, scn = k >> 8, vk = 0;
    DWORD lp;
    WORD sh = fapi_call(FAPI_KBDSHIFT, 1, 0, 0);   /* beim Druecken */
    HWND h = focus ? focus : active;
    int ext = KEY_EXT(ch, scn);
    if (ch >= 'a' && ch <= 'z' && !ext)
        vk = ch - 32;
    else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
        vk = ext ? 0 : ch;
    if (!vk && scn < sizeof(scvk))
        vk = scvk[scn];
    if (!vk)
        vk = ch;
    if (!h)
        return;
    if (menu_input_key(k))
        return;
    if ((sh & KBDSHIFT_ALT) && scn == 0x3E) {   /* Alt+F4               */
        post_msg(active, WM_SYSCOMMAND, SC_CLOSE, 0);
        return;
    }
    lp = 1 | ((DWORD)scn << 16);
    if (sh & KBDSHIFT_SHIFT) lp |= 0x02000000UL;    /* eigene Bits 25/26 */
    if (sh & KBDSHIFT_CTRL) lp |= 0x04000000UL;
    if (sh & KBDSHIFT_ALT) lp |= 0x20000000UL;      /* wie Windows       */
    if (ext) lp |= 0x01000000UL;
    keystate[vk] = 0x80;
    post_msg(h, (sh & KBDSHIFT_ALT) || vk == 0x79 ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp);
    if ((sh & KBDSHIFT_ALT) && ch <= ' ' && scn >= 0x10 && scn <= 0x32)
        ch = altkey[scn - 0x10];                /* Alt+Buchstabe: nur Scancode */
    if ((sh & KBDSHIFT_ALT) && ch > ' ')
        post_msg(h, WM_SYSCHAR, ch, lp);
    if (!ext && ch && !(sh & KBDSHIFT_ALT)) {
        BYTE a = ch;
        /* CP850-Eingabe -> ANSI fuer Win16 (nur Umlaute/ss) */
        switch (a) {
        case 0x84: a = 0xE4; break; case 0x94: a = 0xF6; break;
        case 0x81: a = 0xFC; break; case 0x8E: a = 0xC4; break;
        case 0x99: a = 0xD6; break; case 0x9A: a = 0xDC; break;
        case 0xE1: a = 0xDF; break;
        }
        post_msg(h, WM_CHAR, a, lp);
    }
    post_msg(h, (sh & KBDSHIFT_ALT) || vk == 0x79 ? 0x0105 : WM_KEYUP, vk, lp | 0xC0000000UL);
    keystate[vk] = 0;
}

/* Platz fuer eine Taste (KEYDOWN, CHAR, KEYUP + Reserve) in der
   Schlange der Task, deren Fenster den Fokus hat - sonst warten, statt
   Tasten zu verwerfen (auf dem 386 tippt man schneller, als Notepad
   zeichnet) */
static int key_room(void)
{
    struct wnd *w = wptr(focus ? focus : active);
    struct wtask *k;
    int t;
    if (!w)
        return 1;
    t = task_win(w);
    if (t < 0 || !wt[t].used)
        return 1;
    k = &wt[t];
    return (WORD)((k->qt + NQ - k->qh) % NQ) < NQ - 5;
}

void user_input(void)
{
    static struct fapi_mouevent ev;
    WORD k;
    short n = 0;
    while (n++ < 32 && MouReadEvent(&ev, MOU_XY) == 0) {
        mouse_event(ev.x, ev.y, ev.buttons);
    }
    sb_repeat();
    for (n = 0; n < 16; n++) {
        if (!key_room())                /* Taste bleibt im Kernelpuffer   */
            break;
        k = KbdPeek();
        if (!k)
            break;
        k = KbdCharIn();
        key_event(k);
    }
}

void user_paint_pending(void)
{
    short i;
    WORD z;
    if (desk_need) {
        HDC dc = dc_get(0, 0, 0);
        short k;
        if (dc) {
            struct clip c;
            c.n = 1;
            c.r[0] = desk_upd;
            for (k = 0; k < NWND; k++)
                if (ww[k].used && ww[k].visible && !(ww[k].style & WS_CHILD))
                    clip_sub(&c, &ww[k].wr);
            for (k = 0; k < c.n; k++)
                gfx_fill(c.r[k].left, c.r[k].top, c.r[k].right, c.r[k].bottom, sc(C_DESK));
            dc_release(dc);
        }
        desk_need = 0;
    }
    /* Rahmen von unten nach oben */
    for (z = 0; z <= zmax; z++)
        for (i = 0; i < NWND; i++)
            if (ww[i].used && ww[i].need_ncpaint && ww[i].z == z) {
                ww[i].need_ncpaint = 0;
                nc_paint(&ww[i]);
            }
    /* Kinder der GUI selbst (ohne Task) sofort zeichnen */
    for (i = 0; i < NWND; i++)
        if (ww[i].used && ww[i].need_paint && ww[i].task < 0)
            send_msg(HW(i), WM_PAINT, 0, 0);
}

/* ---- Exporte USER ----------------------------------------------------- */
#define NULLH 0

WORD API InitApp(WORD hinst) { (void)hinst; return 1; }

WORD API RegisterClass(void FAR *wcp)
{
    BYTE FAR *p = wcp;
    LPCSTR nm = *(LPCSTR FAR *)(p + 22);
    short i;
    for (i = 0; i < NCLS; i++)
        if (cl[i].used && !fstricmp(cl[i].name, nm))
            return 0;
    for (i = 0; i < NCLS && cl[i].used; i++)
        ;
    if (i == NCLS)
        return 0;
    cl[i].used = 1;
    cl[i].internal = 0;
    cl[i].style = *(WORD FAR *)p;
    cl[i].proc = *(WNDPROC FAR *)(p + 2);
    cl[i].cbwnd = *(WORD FAR *)(p + 8);
    cl[i].hinst = *(WORD FAR *)(p + 10);
    cl[i].bg = *(WORD FAR *)(p + 16);
    cl[i].menu = *(LPCSTR FAR *)(p + 18);
    fstrncpy(cl[i].name, nm, sizeof(cl[i].name));
    return 0xC000 + i;
}
BOOL API UnregisterClass(LPCSTR nm, WORD hinst)
{
    short i;
    (void)hinst;
    for (i = 0; i < NCLS; i++)
        if (cl[i].used && !cl[i].internal && !fstricmp(cl[i].name, nm)) {
            cl[i].used = 0;
            return 1;
        }
    return 0;
}

static short find_class(LPCSTR nm)
{
    short i;
    if (SELOF(nm) == 0) {
        i = OFFOF(nm) - 0xC000;
        return (i >= 0 && i < NCLS && cl[i].used) ? i : -1;
    }
    for (i = 0; i < NCLS; i++)
        if (cl[i].used && !fstricmp(cl[i].name, nm))
            return i;
    return -1;
}

static struct { DWORD param; WORD hinst, hmenu, parent; short cy, cx, y, x; DWORD style;
                LPCSTR name, cls; DWORD exstyle; } cs;
static short cascade;

HWND API CreateWindowEx(DWORD ex, LPCSTR cname, LPCSTR title, DWORD style,
                        short x, short y, short cx, short cy, HWND parent,
                        HMENU menu, WORD hinst, LPVOID param)
{
    short c = find_class(cname), i;
    struct wnd *w, *p;
    HWND h;
    if (c < 0) {
        glogs("Klasse fehlt: ", SELOF(cname) ? cname : "(Atom)");
        if (!SELOF(cname))
            glogn("  Atom ", OFFOF(cname));
        return 0;
    }
    for (i = 0; i < NWND && ww[i].used; i++)
        ;
    if (i == NWND)
        return 0;
    w = &ww[i];
    fmemset((void FAR *)w, 0, sizeof(*w));
    h = HW(i);
    w->used = 1;
    w->cls = c + 1;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = ex;
    w->internal = cl[c].internal;
    w->proc = cl[c].proc;
    w->hinst = cl[c].internal ? gui_ds : hinst;
    w->task = cur_task;
    w->enabled = !(style & WS_DISABLED);
    w->id = menu;
    if (!(style & WS_CHILD)) {
        w->id = 0;
        w->menu = menu;
        if (!menu && cl[c].menu && !cl[c].internal)
            w->menu = LoadMenu(hinst, cl[c].menu);
    }
    w->sb[0][1] = w->sb[1][1] = 100;
    if (title)
        fstrncpy(w->title, title, sizeof(w->title));
    p = parent ? wptr(parent) : 0;
    if (style & WS_CHILD) {
        w->parent = parent;
        w->z = ++zmax;                          /* Rang unter Geschwistern */
        if (p) {
            x += p->cr.left;
            y += p->cr.top;
        }
    } else {
        w->owner = parent;
        if (x == (short)0x8000) {
            x = 20 + cascade * 24;
            y = 20 + cascade * 24;
            cascade = (cascade + 1) % 8;
        }
        if (cx == (short)0x8000) {
            cx = 480;
            cy = 320;
        }
        w->z = ++zmax;
    }
    if (p && !(style & WS_CHILD) && p->task >= 0 && w->task < 0)
        w->task = p->task;
    rset(&w->wr, x, y, x + cx, y + cy);
    calc_client(w);
    cs.param = (DWORD)param; cs.hinst = hinst; cs.hmenu = menu; cs.parent = parent;
    cs.cy = cy; cs.cx = cx; cs.y = y; cs.x = x; cs.style = style;
    cs.name = title; cs.cls = cname; cs.exstyle = ex;
    if (!send_msg(h, WM_NCCREATE, 0, (DWORD)(void FAR *)&cs) && !w->internal) {
        /* DefWindowProc liefert 1 - 0 heisst abbrechen */
    }
    if (send_msg(h, WM_CREATE, 0, (DWORD)(void FAR *)&cs) == -1) {
        w->used = 0;
        return 0;
    }
    send_msg(h, WM_SIZE, 0, MAKELONG(w->cr.right - w->cr.left, w->cr.bottom - w->cr.top));
    if (style & WS_VISIBLE)
        ShowWindow(h, 1);
    return h;
}
HWND API CreateWindow(LPCSTR cname, LPCSTR title, DWORD style, short x, short y,
                      short cx, short cy, HWND parent, HMENU menu, WORD hinst, LPVOID param)
{
    return CreateWindowEx(0, cname, title, style, x, y, cx, cy, parent, menu, hinst, param);
}

BOOL API ShowWindow(HWND h, short cmd)
{
    struct wnd *w = wptr(h);
    BYTE was;
    if (!w)
        return 0;
    was = w->visible;
    if (cmd == 0) {                             /* SW_HIDE              */
        if (was) {
            w->visible = 0;
            expose(&w->wr);
            if (active == h)
                active = 0;
        }
        return was;
    }
    if (!was) {
        w->visible = 1;
        w->style |= WS_VISIBLE;
        w->need_ncpaint = 1;
        invalidate(w, 0, 1);
        if (!(w->style & WS_CHILD) && cmd != 4 && cmd != 8)
            activate(h);
        send_msg(h, WM_SHOWWINDOW, 1, 0);
    }
    return was;
}

void API UpdateWindow(HWND h)
{
    struct wnd *w = wptr(h);
    user_paint_pending();
    if (w && w->need_paint && w->visible && (w->task == cur_task || w->internal))
        send_msg(h, WM_PAINT, 0, 0);
}

BOOL API DestroyWindow(HWND h)
{
    struct wnd *w = wptr(h);
    short i;
    if (!w)
        return 0;
    send_msg(h, WM_DESTROY, 0, 0);
    for (i = 0; i < NWND; i++)
        if (ww[i].used && (ww[i].parent == h || ww[i].owner == h))
            DestroyWindow(HW(i));
    send_msg(h, WM_NCDESTROY, 0, 0);
    if (w->menu && !(w->style & WS_CHILD))
        menu_free(w->menu);
    w->menu = 0;
    if (w->visible)
        expose(&w->wr);
    w->used = 0;
    if (capture == h) capture = 0;
    if (focus == h) focus = 0;
    if (active == h) {
        struct wnd *b = 0;
        active = 0;
        for (i = 0; i < NWND; i++)
            if (ww[i].used && ww[i].visible && !(ww[i].style & WS_CHILD) &&
                (!b || ww[i].z > b->z))
                b = &ww[i];
        if (b)
            activate(HW(b - ww));
    }
    return 1;
}

BOOL API MoveWindow(HWND h, short x, short y, short cx, short cy, BOOL rep)
{
    struct wnd *w = wptr(h);
    (void)rep;
    if (!w) return 0;
    win_move(w, x, y, cx, cy);
    return 1;
}
BOOL API SetWindowPos(HWND h, HWND after, short x, short y, short cx, short cy, WORD f)
{
    struct wnd *w = wptr(h), *p;
    (void)after;
    if (!w) return 0;
    p = (w->style & WS_CHILD) ? wptr(w->parent) : 0;
    if (f & 2) {                                /* SWP_NOMOVE           */
        x = w->wr.left - (p ? p->cr.left : 0);
        y = w->wr.top - (p ? p->cr.top : 0);
    }
    if (f & 1) {                                /* SWP_NOSIZE           */
        cx = w->wr.right - w->wr.left;
        cy = w->wr.bottom - w->wr.top;
    }
    if ((f & 3) != 3)
        win_move(w, x, y, cx, cy);
    if (f & 0x40) ShowWindow(h, 5);             /* SWP_SHOWWINDOW       */
    if (f & 0x80) ShowWindow(h, 0);
    return 1;
}

void API GetClientRect(HWND h, LPRECT r)
{
    struct wnd *w = wptr(h);
    if (!w) {
        r->left = r->top = 0; r->right = SCR_W; r->bottom = SCR_H;
        return;
    }
    r->left = r->top = 0;
    r->right = w->cr.right - w->cr.left;
    r->bottom = w->cr.bottom - w->cr.top;
}
void API GetWindowRect(HWND h, LPRECT r)
{
    struct wnd *w = wptr(h);
    if (!w) {
        r->left = r->top = 0; r->right = SCR_W; r->bottom = SCR_H;
        return;
    }
    *r = w->wr;
}
void API ClientToScreen(HWND h, POINT FAR *p)
{
    struct wnd *w = wptr(h);
    if (w) { p->x += w->cr.left; p->y += w->cr.top; }
}
void API ScreenToClient(HWND h, POINT FAR *p)
{
    struct wnd *w = wptr(h);
    if (w) { p->x -= w->cr.left; p->y -= w->cr.top; }
}

void API InvalidateRect(HWND h, const RECT FAR *r, BOOL erase)
{
    struct wnd *w = wptr(h);
    RECT q;
    short i;
    if (!w) {                                   /* alle                 */
        for (i = 0; i < NWND; i++)
            if (ww[i].used)
                invalidate(&ww[i], 0, erase);
        return;
    }
    if (r) {
        q = *r;
        invalidate(w, &q, erase);
    } else
        invalidate(w, 0, erase);
    for (i = 0; i < NWND; i++)                  /* Kinder im Bereich    */
        if (ww[i].used && (ww[i].style & WS_CHILD) && ww[i].parent == h) {
            ww[i].need_ncpaint = 1;
            invalidate(&ww[i], 0, 1);
        }
}
void API ValidateRect(HWND h, const RECT FAR *r)
{
    struct wnd *w = wptr(h);
    (void)r;
    if (w) w->need_paint = 0;
}

struct paintst { HDC hdc; BOOL erase; RECT rc; BOOL res, inc; BYTE rgb[16]; };
HDC API BeginPaint(HWND h, void FAR *pv)
{
    struct paintst FAR *ps = pv;
    struct wnd *w = wptr(h);
    HDC dc;
    RECT u;
    void dc_clip_update(HDC h, const RECT FAR *r);
    if (!w)
        return 0;
    dc = dc_get(h, 1, 1);
    u = w->upd;
    if (!w->need_paint)
        rset(&u, 0, 0, 0, 0);
    w->need_paint = 0;
    {
        RECT s = u;
        s.left += w->cr.left; s.right += w->cr.left;
        s.top += w->cr.top; s.bottom += w->cr.top;
        dc_clip_update(dc, &s);
    }
    ps->hdc = dc;
    ps->rc = u;
    ps->erase = 0;
    if (w->erase) {
        w->erase = 0;
        if (!send_msg(h, WM_ERASEBKGND, dc, 0))
            ps->erase = 1;
    }
    /* das Elternfenster zeichnet ueber seine Kinder (kein Ausschneiden
       wie ohne WS_CLIPCHILDREN): betroffene Kinder danach neu */
    if (u.left < u.right) {
        short i;
        RECT s = u;
        s.left += w->cr.left; s.right += w->cr.left;
        s.top += w->cr.top; s.bottom += w->cr.top;
        for (i = 0; i < NWND; i++)
            if (ww[i].used && ww[i].visible && (ww[i].style & WS_CHILD) &&
                ww[i].parent == h && rover(&ww[i].wr, &s)) {
                ww[i].need_ncpaint = 1;
                invalidate(&ww[i], 0, 1);
            }
    }
    return dc;
}
BOOL API EndPaint(HWND h, void FAR *pv)
{
    struct paintst FAR *ps = pv;
    (void)h;
    dc_release(ps->hdc);
    return 1;
}
HDC API GetDC(HWND h) { return dc_get(h, 1, 0); }
HDC API GetWindowDC(HWND h) { return dc_get(h, 0, 0); }
short API ReleaseDC(HWND h, HDC dc) { (void)h; dc_release(dc); return 1; }

short API FillRect(HDC dc, const RECT FAR *r, HBRUSH b)
{
    dc_fillrect(dc, r, b);
    return 1;
}
void API InvertRect(HDC dc, const RECT FAR *r) { dc_invert(dc, r); }
short API FrameRect(HDC dc, const RECT FAR *r, HBRUSH b)
{
    RECT q;
    q = *r; q.bottom = q.top + 1; dc_fillrect(dc, &q, b);
    q = *r; q.top = q.bottom - 1; dc_fillrect(dc, &q, b);
    q = *r; q.right = q.left + 1; dc_fillrect(dc, &q, b);
    q = *r; q.left = q.right - 1; dc_fillrect(dc, &q, b);
    return 1;
}
short API DrawText(HDC dc, LPCSTR s, short n, LPRECT r, WORD fmt)
{
    short x, y, w;
    if (n < 0)
        n = fstrlen(s);
    w = n * FONT_W;
    x = r->left;
    y = r->top;
    if (fmt & 1) x = (r->left + r->right - w) / 2;          /* DT_CENTER  */
    else if (fmt & 2) x = r->right - w;                      /* DT_RIGHT   */
    if (fmt & 4) y = (r->top + r->bottom - FONT_H) / 2;      /* DT_VCENTER */
    else if (fmt & 8) y = r->bottom - FONT_H;                /* DT_BOTTOM  */
    if (fmt & 0x400) {                                       /* DT_CALCRECT */
        r->right = r->left + w;
        r->bottom = r->top + FONT_H;
        return FONT_H;
    }
    ExtTextOut(dc, x, y, (fmt & 0x100) ? 0 : 4, r, s, n, 0);
    return FONT_H;
}

DWORD API GetSysColor(short i) { return GetSysColor_c(i); }
short API GetSystemMetrics(short i)
{
    switch (i) {
    case 0: return SCR_W;               /* SM_CXSCREEN */
    case 1: return SCR_H;
    case 2: case 3: return 16;          /* Bildlaufleisten */
    case 4: return CAPH;                /* SM_CYCAPTION */
    case 5: case 6: return 1;           /* Rand */
    case 7: case 8: return 3;           /* DLGFRAME */
    case 11: case 12: return 32;        /* Symbol */
    case 13: case 14: return 32;        /* Zeiger */
    case 15: return CAPH;               /* SM_CYMENU */
    case 32: case 33: return 4;         /* SM_CXFRAME */
    case 34: return 100; case 35: return 60;
    default: return 0;
    }
}

LONG API DefWindowProc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    switch (m) {
    case WM_NCCREATE:
        return 1;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE)
            send_msg(h, WM_CLOSE, 0, 0);
        else if ((wp & 0xFFF0) == SC_MOUSEMENU)
            menu_track(h, menu_bar_hit(w, LOWORD(lp), HIWORD(lp)), 0);
        else if ((wp & 0xFFF0) == SC_KEYMENU) {
            short k = LOWORD(lp) ? menu_mnemonic(w->menu, (BYTE)lp) : -1;
            if (LOWORD(lp) && k < 0)
                MessageBeep_c();
            else
                menu_track(h, k, 1);
        }
        return 0;
    case WM_SYSCHAR:
        if (w->style & WS_CHILD)
            return send_msg(GetParent(h), m, wp, lp);
        if (w->menu)
            send_msg(h, WM_SYSCOMMAND, SC_KEYMENU, wp);
        return 0;
    case WM_ERASEBKGND: {
        struct wcls *c = w->cls ? &cl[w->cls - 1] : 0;
        RECT r;
        if (!c || !c->bg)
            return 0;
        GetClientRect(h, &r);
        dc_fillrect((HDC)wp, &r, c->bg);
        return 1;
    }
    case WM_PAINT: {
        static struct paintst ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETTEXT:
        fstrncpy(w->title, (LPCSTR)lp, sizeof(w->title));
        w->need_ncpaint = 1;
        return 1;
    case WM_GETTEXT:
        fstrncpy((LPSTR)lp, w->title, wp);
        return fstrlen((LPSTR)lp);
    case WM_GETTEXTLENGTH:
        return fstrlen(w->title);
    case WM_SYSKEYDOWN:
        if (wp == 0x79) {                       /* F10                  */
            struct wnd *t = top_of(w);
            if (t && t->menu)
                send_msg(HW(t - ww), WM_SYSCOMMAND, SC_KEYMENU, 0);
        }
        return 0;
    }
    return 0;
}

LONG API CallWindowProc(WNDPROC p, HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    return call_proc(w, p, h, m, wp, lp);
}

BOOL API GetMessage(MSG FAR *m, HWND h, WORD lo, WORD hi) { return user_get_msg(m, h, lo, hi, 1, 1); }
BOOL API PeekMessage(MSG FAR *m, HWND h, WORD lo, WORD hi, WORD rem)
{
    BOOL r = user_get_msg(m, h, lo, hi, rem & 1, 0);
    if (!r && cur_task >= 0 && m->message != WM_QUIT) {
        wt[cur_task].waiting = 0;               /* andere Tasks laufen */
        task_yield_to_main();
    }
    return r || m->message == WM_QUIT;
}
BOOL API TranslateMessage(const MSG FAR *m) { (void)m; return 0; }
LONG API DispatchMessage(const MSG FAR *m)
{
    struct wnd *w;
    if (m->message == WM_TIMER && m->lParam) {
        pq_words[0] = m->hwnd; pq_words[1] = m->message; pq_words[2] = m->wParam;
        pq_words[3] = 0; pq_words[4] = LOWORD(ticks() * 10);
        w = wptr(m->hwnd);
        return call16((void FAR *)m->lParam, w ? w->hinst : wt[cur_task].hinst, 4, pq_words);
    }
    w = wptr(m->hwnd);
    if (!w)
        return 0;
    if (m->message == WM_PAINT && !w->need_paint)
        return 0;
    return send_msg(m->hwnd, m->message, m->wParam, m->lParam);
}
BOOL API PostMessage(HWND h, WORD m, WORD wp, DWORD lp)
{
    if (h == 0xFFFF) {
        short i;
        for (i = 0; i < NWND; i++)
            if (ww[i].used && !(ww[i].style & WS_CHILD))
                post_msg(HW(i), m, wp, lp);
        return 1;
    }
    return post_msg(h, m, wp, lp);
}
LONG API SendMessage(HWND h, WORD m, WORD wp, DWORD lp) { return send_msg(h, m, wp, lp); }
void API PostQuitMessage(short code)
{
    if (cur_task >= 0) {
        wt[cur_task].quit = 1;
        wt[cur_task].quitcode = code;
    }
}

WORD API LoadCursor(WORD hinst, LPCSTR id) { (void)hinst; (void)id; return 0x5001; }
WORD API SetCursor(WORD c) { (void)c; return 0x5001; }
void MessageBeep_c(void) { fapi_call(FAPI_BEEP, 880, 80, 0); }
void API MessageBeep(WORD t) { (void)t; MessageBeep_c(); }

BOOL API SetWindowText(HWND h, LPCSTR s) { return (BOOL)send_msg(h, WM_SETTEXT, 0, (DWORD)s); }
short API GetWindowText(HWND h, LPSTR s, short n) { return (short)send_msg(h, WM_GETTEXT, n, (DWORD)s); }
short API GetWindowTextLength(HWND h) { return (short)send_msg(h, WM_GETTEXTLENGTH, 0, 0); }

HWND API SetFocus(HWND h)
{
    HWND o = focus;
    if (h && !wptr(h))
        return 0;
    if (o && o != h)
        send_msg(o, WM_KILLFOCUS, h, 0);
    focus = h;
    if (h && o != h)
        send_msg(h, WM_SETFOCUS, o, 0);
    return o;
}
HWND API GetFocus(void) { return focus; }
HWND API GetActiveWindow(void) { return active; }
HWND API SetActiveWindow(HWND h) { HWND o = active; activate(h); return o; }
BOOL API IsWindow(HWND h) { return wptr(h) != 0; }
BOOL API IsWindowVisible(HWND h) { struct wnd *w = wptr(h); return w && w->visible; }
BOOL API IsWindowEnabled(HWND h) { struct wnd *w = wptr(h); return w && w->enabled; }
BOOL API EnableWindow(HWND h, BOOL on)
{
    struct wnd *w = wptr(h);
    BYTE o;
    if (!w) return 0;
    o = w->enabled;
    w->enabled = on ? 1 : 0;
    if (on) w->style &= ~WS_DISABLED; else w->style |= WS_DISABLED;
    if (o != w->enabled) {
        w->need_ncpaint = 1;
        invalidate(w, 0, 1);
    }
    return !o;
}
HWND API GetParent(HWND h)
{
    struct wnd *w = wptr(h);
    return w ? ((w->style & WS_CHILD) ? w->parent : w->owner) : 0;
}
BOOL API IsChild(HWND p, HWND h)
{
    struct wnd *w = wptr(h);
    while (w && (w->style & WS_CHILD)) {
        if (w->parent == p) return 1;
        w = wptr(w->parent);
    }
    return 0;
}
HWND API GetDesktopWindow(void) { return desktop; }
HWND API WindowFromPoint(POINT p) { return win_at(p.x, p.y); }
BOOL API BringWindowToTop(HWND h)
{
    struct wnd *w = wptr(h);
    if (w && (w->style & WS_CHILD)) {           /* nur unter Geschwistern */
        if (w->z != zmax) {
            w->z = ++zmax;
            if (w->visible)
                expose(&w->wr);
        }
        return 1;
    }
    activate(h);
    return 1;
}
BOOL API IsIconic(HWND h) { (void)h; return 0; }
BOOL API IsZoomed(HWND h) { (void)h; return 0; }
HMENU API GetMenu(HWND h) { struct wnd *w = wptr(h); return w ? w->menu : 0; }
BOOL API SetMenu(HWND h, HMENU m)
{
    struct wnd *w = wptr(h);
    if (!w || (w->style & WS_CHILD))
        return 0;
    w->menu = m;
    calc_client(w);
    w->need_ncpaint = 1;
    invalidate(w, 0, 1);
    return 1;
}
void API DrawMenuBar(HWND h) { struct wnd *w = wptr(h); if (w) w->need_ncpaint = 1; }

/* Bildlaufleisten */
static short *sbp(HWND h, short bar)
{
    struct wnd *w = wptr(h);
    if (!w || bar < 0 || bar > 1)
        return 0;
    return w->sb[bar];
}
short API SetScrollPos(HWND h, short bar, short pos, BOOL redraw)
{
    short *s = sbp(h, bar), o;
    if (!s) return 0;
    o = s[2];
    if (pos < s[0]) pos = s[0];
    if (pos > s[1]) pos = s[1];
    s[2] = pos;
    if (redraw && o != pos) wptr(h)->need_ncpaint = 1;
    return o;
}
short API GetScrollPos(HWND h, short bar) { short *s = sbp(h, bar); return s ? s[2] : 0; }
void API SetScrollRange(HWND h, short bar, short lo, short hi, BOOL redraw)
{
    short *s = sbp(h, bar);
    if (!s) return;
    s[0] = lo; s[1] = hi;
    if (s[2] < lo) s[2] = lo;
    if (s[2] > hi) s[2] = hi;
    if (redraw) wptr(h)->need_ncpaint = 1;
}
void API GetScrollRange(HWND h, short bar, short FAR *lo, short FAR *hi)
{
    short *s = sbp(h, bar);
    *lo = s ? s[0] : 0;
    *hi = s ? s[1] : 0;
}
void API ShowScrollBar(HWND h, WORD bar, BOOL show) { (void)h; (void)bar; (void)show; }
HMENU API GetSystemMenu(HWND h, BOOL r) { (void)h; (void)r; return 0; }
HWND API GetDlgItem(HWND p, short id)
{
    short i;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && ww[i].parent == p && ww[i].id == (WORD)id)
            return HW(i);
    return 0;
}
LONG API SendDlgItemMessage(HWND p, short id, WORD m, WORD wp, DWORD lp)
{
    return send_msg(GetDlgItem(p, id), m, wp, lp);
}
HWND API SetCapture(HWND h) { HWND o = capture; capture = h; return o; }
void API ReleaseCapture(void) { capture = 0; }
HWND API GetCapture(void) { return capture; }
short API GetKeyState(short vk)
{
    WORD sh = fapi_call(FAPI_KBDSHIFT, 0, 0, 0);
    if (vk == 0x10) return (kmsg_lp & 0x02000000UL) ? 0x8000 : 0;
    if (vk == 0x11) return (kmsg_lp & 0x04000000UL) ? 0x8000 : 0;
    if (vk == 0x12) return (kmsg_lp & 0x20000000UL) ? 0x8000 : 0;
    if (vk == 0x14) return (sh & KBDSHIFT_CAPS) ? 1 : 0;
    if (vk == 1) return (mbtn & 1) ? 0x8000 : 0;
    if (vk == 2) return (mbtn & 2) ? 0x8000 : 0;
    return (vk >= 0 && vk < 256 && keystate[vk]) ? 0x8000 : 0;
}
short API GetAsyncKeyState(short vk) { return GetKeyState(vk); }

WORD API SetTimer(HWND h, WORD id, WORD ms, DWORD proc)
{
    short i, f = -1;
    for (i = 0; i < NTIM; i++) {
        if (tm[i].ms && tm[i].h == h && tm[i].id == id) { f = i; break; }
        if (!tm[i].ms && f < 0) f = i;
    }
    if (f < 0) return 0;
    if (ms < 10) ms = 10;
    tm[f].h = h; tm[f].id = h ? id : (WORD)(f + 1); tm[f].ms = ms;
    tm[f].task = cur_task; tm[f].proc = proc;
    tm[f].due = ticks() + (ms + 9) / 10;
    return tm[f].id ? tm[f].id : 1;
}
BOOL API KillTimer(HWND h, WORD id)
{
    short i;
    for (i = 0; i < NTIM; i++)
        if (tm[i].ms && tm[i].h == h && tm[i].id == id) {
            tm[i].ms = 0;
            return 1;
        }
    return 0;
}
DWORD API GetTickCount(void) { return ticks() * 10; }
DWORD API GetCurrentTime(void) { return ticks() * 10; }

WORD API GetWindowWord(HWND h, short i)
{
    struct wnd *w = wptr(h);
    if (!w) return 0;
    if (i == -6) return w->hinst;               /* GWW_HINSTANCE        */
    if (i == -8) return GetParent(h);
    if (i == -12) return w->id;                 /* GWW_ID               */
    if (i >= 0 && i < 15) return *(WORD *)(w->extra + i);
    return 0;
}
WORD API SetWindowWord(HWND h, short i, WORD v)
{
    struct wnd *w = wptr(h);
    WORD o;
    if (!w || i < 0 || i >= 15) return 0;
    o = *(WORD *)(w->extra + i);
    *(WORD *)(w->extra + i) = v;
    return o;
}
LONG API GetWindowLong(HWND h, short i)
{
    struct wnd *w = wptr(h);
    if (!w) return 0;
    if (i == -4) return (LONG)w->proc;          /* GWL_WNDPROC          */
    if (i == -16) return w->style;
    if (i == -20) return w->exstyle;
    if (i >= 0 && i < 13) return *(LONG *)(w->extra + i);
    return 0;
}
LONG API SetWindowLong(HWND h, short i, LONG v)
{
    struct wnd *w = wptr(h);
    LONG o;
    if (!w) return 0;
    if (i == -4) { o = (LONG)w->proc; w->proc = (WNDPROC)v; return o; }
    if (i == -16) { o = w->style; w->style = v; return o; }
    if (i >= 0 && i < 13) { o = *(LONG *)(w->extra + i); *(LONG *)(w->extra + i) = v; return o; }
    return 0;
}
WORD API GetClassWord(HWND h, short i)
{
    struct wnd *w = wptr(h);
    if (!w || !w->cls) return 0;
    if (i == -10) return cl[w->cls - 1].bg;
    if (i == -16) return cl[w->cls - 1].hinst;
    if (i == -26) return cl[w->cls - 1].style;
    return 0;
}
WORD API SetClassWord(HWND h, short i, WORD v)
{
    struct wnd *w = wptr(h);
    WORD o = 0;
    if (!w || !w->cls) return 0;
    if (i == -10) { o = cl[w->cls - 1].bg; cl[w->cls - 1].bg = v; }
    return o;
}
LONG API GetClassLong(HWND h, short i)
{
    struct wnd *w = wptr(h);
    if (!w || !w->cls) return 0;
    if (i == -24) return (LONG)cl[w->cls - 1].proc;
    return 0;
}
short API GetClassName(HWND h, LPSTR s, short n)
{
    struct wnd *w = wptr(h);
    if (!w || !w->cls) return 0;
    fstrncpy(s, cl[w->cls - 1].name, n);
    return fstrlen(s);
}

/* Rechtecke */
void API SetRect(LPRECT r, short l, short t, short rr, short b) { r->left = l; r->top = t; r->right = rr; r->bottom = b; }
void API SetRectEmpty(LPRECT r) { r->left = r->top = r->right = r->bottom = 0; }
void API CopyRect(LPRECT d, const RECT FAR *s) { *d = *s; }
BOOL API IsRectEmpty(const RECT FAR *r) { return r->left >= r->right || r->top >= r->bottom; }
BOOL API PtInRect(const RECT FAR *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
void API OffsetRect(LPRECT r, short x, short y) { r->left += x; r->right += x; r->top += y; r->bottom += y; }
void API InflateRect(LPRECT r, short x, short y) { r->left -= x; r->right += x; r->top -= y; r->bottom += y; }
BOOL API IntersectRect(LPRECT d, const RECT FAR *a, const RECT FAR *b)
{
    d->left = a->left > b->left ? a->left : b->left;
    d->top = a->top > b->top ? a->top : b->top;
    d->right = a->right < b->right ? a->right : b->right;
    d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (d->left >= d->right || d->top >= d->bottom) {
        d->left = d->top = d->right = d->bottom = 0;
        return 0;
    }
    return 1;
}
BOOL API UnionRect(LPRECT d, const RECT FAR *a, const RECT FAR *b)
{
    d->left = a->left < b->left ? a->left : b->left;
    d->top = a->top < b->top ? a->top : b->top;
    d->right = a->right > b->right ? a->right : b->right;
    d->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return 1;
}

/* Zeichenketten */
static char lo_(char c) { return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
short API lstrcmp(LPCSTR a, LPCSTR b) { return fstrcmp(a, b); }
short API lstrcmpi(LPCSTR a, LPCSTR b) { return fstricmp(a, b); }
LPSTR API AnsiUpper(LPSTR s)
{
    LPSTR p;
    if (!SELOF(s)) return (LPSTR)(DWORD)(BYTE)(OFFOF(s) >= 'a' && OFFOF(s) <= 'z' ? OFFOF(s) - 32 : OFFOF(s));
    for (p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}
LPSTR API AnsiLower(LPSTR s)
{
    LPSTR p;
    if (!SELOF(s)) return (LPSTR)(DWORD)(BYTE)lo_((char)OFFOF(s));
    for (p = s; *p; p++) *p = lo_(*p);
    return s;
}
LPSTR API AnsiNext(LPCSTR s) { return (LPSTR)(*s ? s + 1 : s); }
LPSTR API AnsiPrev(LPCSTR a, LPCSTR s) { return (LPSTR)(s > a ? s - 1 : a); }

short API LoadString(WORD hinst, WORD id, LPSTR buf, short n)
{
    int t = task_of_inst(hinst), m = t >= 0 ? wt[t].hmod - 1 : -1;
    DWORD r;
    WORD sel, o = 0, k;
    BYTE FAR *p;
    if (n <= 0)
        return 0;
    buf[0] = 0;
    r = w16_res_find(m, MKFP(0, 6), MKFP(0, (id >> 4) + 1));
    if (!r || !(sel = w16_res_load(m, r)))
        return 0;
    p = MKFP(sel, 0);
    for (k = 0; k < (id & 15); k++)
        o += p[o] + 1;
    k = p[o];
    if (k > n - 1)
        k = n - 1;
    fmemcpy(buf, p + o + 1, k);
    buf[k] = 0;
    seg_del(sel);
    return k;
}

/* ---- MessageBox ----------------------------------------------------- */
static short mb_result;
static LONG FAR PASCAL mb_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    switch (m) {
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        dc_rawfill((HDC)wp, w->cr.left, w->cr.top, w->cr.right, w->cr.bottom, 15);
        return 1;
    }
    case WM_PAINT: {
        static struct paintst ps;
        HDC dc = BeginPaint(h, &ps);
        LPCSTR s = *(LPCSTR *)w->extra;
        short y = w->cr.top + 14, n;
        while (*s) {
            for (n = 0; s[n] && s[n] != '\n' && s[n] != '\r'; n++)
                ;
            dc_rawtext(dc, w->cr.left + 16, y, s, n, 0);
            y += FONT_H;
            s += n;
            if (*s == '\r') s++;
            if (*s == '\n') s++;
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND:
        mb_result = wp;
        return 0;
    case WM_KEYDOWN:
        if (wp == 0x0D) mb_result = w->extra[4];
        if (wp == 0x1B) mb_result = w->extra[5];
        return 0;
    case WM_CLOSE:
        mb_result = w->extra[5];
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

static const char *mbt[] = { "OK", "Abbrechen", "Abbruch", "Wiederholen", "Ignorieren", "Ja", "Nein" };

short API MessageBox(HWND owner, LPCSTR text, LPCSTR cap, WORD type)
{
    short lines = 1, wmax = 0, n = 0, bw, cx, cy, x, y, i, nb;
    static BYTE ids[3];
    LPCSTR p;
    HWND h, b;
    struct wnd *w, *ow = wptr(owner);
    static MSG msg;
    switch (type & 15) {
    case 1: ids[0] = 1; ids[1] = 2; nb = 2; break;       /* OKCANCEL    */
    case 2: ids[0] = 3; ids[1] = 4; ids[2] = 5; nb = 3; break;
    case 3: ids[0] = 6; ids[1] = 7; ids[2] = 2; nb = 3; break;
    case 4: ids[0] = 6; ids[1] = 7; nb = 2; break;
    case 5: ids[0] = 4; ids[1] = 2; nb = 2; break;
    default: ids[0] = 1; nb = 1; break;
    }
    for (p = text; ; p++) {
        if (!*p || *p == '\n') {
            if (n > wmax) wmax = n;
            n = 0;
            if (!*p) break;
            lines++;
        } else if (*p != '\r')
            n++;
    }
    bw = 88;
    cx = wmax * FONT_W + 40;
    if (cx < nb * (bw + 12) + 24) cx = nb * (bw + 12) + 24;
    if (cap && fstrlen(cap) * FONT_W + 60 > cx) cx = fstrlen(cap) * FONT_W + 60;
    if (cx > SCR_W - 20) cx = SCR_W - 20;
    cy = lines * FONT_H + 28 + 40 + CAPH + 6;
    x = (SCR_W - cx) / 2;
    y = (SCR_H - cy) / 2;
    h = CreateWindowEx(0, MKFP(0, 0xC000 + 2), cap ? cap : "Fehler",
                       WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_DLGFRAME, x, y, cx, cy,
                       owner, 0, 0, 0);
    w = wptr(h);
    if (!w)
        return 0;
    if (w->task < 0)
        w->task = cur_task;
    *(LPCSTR *)w->extra = text;
    w->extra[4] = ids[0];
    w->extra[5] = (type & 15) == 0 ? 1 : (type & 15) == 4 ? 7 : 2;
    if ((type & 15) == 2) w->extra[5] = 3;
    for (i = 0; i < nb; i++) {
        short bx = (cx - 6 - nb * bw - (nb - 1) * 12) / 2 + i * (bw + 12);
        b = CreateWindowEx(0, MKFP(0, 0xC000 + 0), mbt[ids[i] - 1],
                           WS_CHILD | WS_VISIBLE | (i == 0 ? 1 : 0),
                           bx, w->cr.bottom - w->cr.top - 34, bw, 24, h, ids[i], 0, 0);
        if (wptr(b)) wptr(b)->task = w->task;
    }
    if (ow)
        EnableWindow(owner, 0);
    ShowWindow(h, 1);
    focus = h;
    mb_result = 0;
    while (!mb_result && wptr(h)) {
        if (cur_task >= 0) {
            if (!user_get_msg(&msg, 0, 0, 0, 1, 1)) {
                PostQuitMessage(msg.wParam);
                break;
            }
            DispatchMessage(&msg);
        } else {
            user_input();
            user_paint_pending();
            gdi_flush();
            DosSleep(10);
        }
    }
    if (ow)
        EnableWindow(owner, 1);
    DestroyWindow(h);
    if (ow)
        activate(owner);
    return mb_result;
}

void user_init(void)
{
    RECT r;
    cl[0].used = 1; cl[0].internal = 1; fstrncpy(cl[0].name, "BUTTON", 24);
    cl[0].proc = button_proc; cl[0].bg = 0;
    cl[1].used = 1; cl[1].internal = 1; fstrncpy(cl[1].name, "STATIC", 24);
    cl[1].proc = static_proc;
    cl[2].used = 1; cl[2].internal = 1; fstrncpy(cl[2].name, "#MSGBOX", 24);
    cl[2].proc = mb_proc;
    cl[3].used = 1; cl[3].internal = 1; fstrncpy(cl[3].name, "#MENU", 24);
    cl[3].proc = menu_pop_proc;
    cl[4].used = 1; cl[4].internal = 1; fstrncpy(cl[4].name, "EDIT", 24);
    cl[4].proc = edit_proc;
    cl[5].used = 1; cl[5].internal = 1; fstrncpy(cl[5].name, "#32770", 24);
    cl[5].proc = dlg_proc;
    cl[6].used = 1; cl[6].internal = 1; fstrncpy(cl[6].name, "LISTBOX", 24);
    cl[6].proc = listbox_proc;
    cl[7].used = 1; cl[7].internal = 1; fstrncpy(cl[7].name, "MDICLIENT", 24);
    cl[7].proc = mdi_client_proc;
    rset(&r, 0, 0, SCR_W, SCR_H);
    expose(&r);
    user_paint_pending();
    gdi_flush();
}

/* ---- Stufe 1 fuer die osFree-Programme (25.11.2026) ---- */

static WORD en_w[3];
/* Rueckruf BOOL FAR PASCAL f(HWND, LPARAM) im Datensegment der Task */
static BOOL en_call(void FAR *f, HWND h, DWORD lp)
{
    WORD ds = cur_task >= 0 ? wt[cur_task].hinst : 0;
    en_w[0] = h; en_w[1] = HIWORD(lp); en_w[2] = LOWORD(lp);
    return (BOOL)call16(f, ds, 3, en_w);
}

/* oberste Fenster, vorne zuerst; aufgezaehlt wird ein Abzug der Liste,
   damit Rueckrufe Fenster anlegen oder schliessen duerfen */
static HWND en_list[NWND];
static short en_top(void)
{
    short i, n = 0;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && !(ww[i].style & WS_CHILD) && ww[i].cls != 4)
            en_list[n++] = HW(i);
    return n;
}
BOOL API EnumWindows(void FAR *f, DWORD lp)
{
    short i, n = en_top();
    for (i = 0; i < n; i++)
        if (wptr(en_list[i]) && !en_call(f, en_list[i], lp))
            break;
    return 1;
}
BOOL API EnumTaskWindows(WORD task, void FAR *f, DWORD lp)
{
    short i, n = en_top();
    for (i = 0; i < n; i++) {
        struct wnd *w = wptr(en_list[i]);
        if (w && w->task + 1 == (short)task && !en_call(f, en_list[i], lp))
            break;
    }
    return 1;
}
BOOL API EnumChildWindows(HWND par, void FAR *f, DWORD lp)
{
    short i, n = 0;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && (ww[i].style & WS_CHILD) && ww[i].parent == par)
            en_list[n++] = HW(i);
    for (i = 0; i < n; i++)
        if (wptr(en_list[i]) && !en_call(f, en_list[i], lp))
            break;
    return 1;
}

HWND API GetLastActivePopup(HWND h)
{
    short i;
    for (i = 0; i < NWND; i++)                  /* sichtbares Popup des Besitzers */
        if (ww[i].used && ww[i].visible && ww[i].owner == h &&
            (ww[i].style & WS_POPUP) && HW(i) == active)
            return HW(i);
    return h;
}
void API SwitchToThisWindow(HWND h, BOOL restore)
{
    (void)restore;
    if (wptr(h)) {
        ShowWindow(h, 5);                       /* SW_SHOW */
        activate(h);
    }
}
/* Symbolgroesse gibt es noch nicht: Schliessen = Verkleinern bleibt aus */
void API CloseWindow(HWND h) { (void)h; }
BOOL API OpenIcon(HWND h) { return ShowWindow(h, 9); }      /* SW_RESTORE */
WORD API ArrangeIconicWindows(HWND h) { (void)h; return 0; }

void API GetCursorPos(POINT FAR *p)
{
    if (p) { p->x = mpos.x; p->y = mpos.y; }
}
/* stellt nur den Zeiger der GUI um - die naechste Mausbewegung kommt
   vom Kernel wieder als Abstand dazu */
void API SetCursorPos(short x, short y)
{
    if (x < 0) x = 0;
    if (x >= SCR_W) x = SCR_W - 1;
    if (y < 0) y = 0;
    if (y >= SCR_H) y = SCR_H - 1;
    mpos.x = x; mpos.y = y;
    gdi_cursor(x, y);
}
static short cur_cnt;
short API ShowCursor(BOOL show) { return show ? ++cur_cnt : --cur_cnt; }
BOOL API DestroyCursor(WORD c) { (void)c; return 1; }

void API AdjustWindowRect(RECT FAR *r, DWORD style, BOOL menu)
{
    static struct wnd t;
    short b;
    t.style = style;
    b = wnd_border(&t);
    r->left -= b; r->right += b;
    r->top -= b; r->bottom += b;
    if ((style & WS_CAPTION) == WS_CAPTION)
        r->top -= CAPH;
    if (menu)
        r->top -= MENUH;
}

/* SystemParametersInfo: nur das Lesen der ueblichen Werte */
BOOL API SystemParametersInfo(WORD act, WORD p, void FAR *pv, WORD ini)
{
    (void)ini; (void)p;
    switch (act) {
    case 0x0010: case 0x0012:                   /* SPI_GETSCREENSAVE... */
    case 0x0001:                                /* SPI_GETBEEP          */
        if (pv) *(short FAR *)pv = act == 1;
        return 1;
    case 0x000D: case 0x0018:                   /* ICONHORZ/VERTSPACING */
        if (pv) *(short FAR *)pv = act == 0x0D ? 75 : 75;
        return 1;
    case 0x0016:                                /* SPI_GETKEYBOARDDELAY */
    case 0x000A:                                /* SPI_GETKEYBOARDSPEED */
    case 0x0019:                                /* SPI_GETICONTITLEWRAP */
    case 0x001B:                                /* SPI_GETMENUDROPALIGN */
        if (pv) *(short FAR *)pv = act == 0x19;
        return 1;
    }
    return 0;
}
