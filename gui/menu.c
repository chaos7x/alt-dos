/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Menues (USER): Menueleiste im Rahmen, Aufklappmenues,
 * Laden aus Ressourcen (RT_MENU), Tastenkuerzel (RT_ACCELERATOR).
 *
 * Menues liegen in einem eigenen Segment (mt), Handle = 7000h + 4 * i. Die
 * Bedienung laeuft wie unter Windows im Task des Fensters: ein Klick in die
 * Leiste (bzw. Alt+Buchstabe, F10) wird zu WM_SYSCOMMAND SC_MOUSEMENU /
 * SC_KEYMENU, DefWindowProc ruft menu_track(); waehrend des Verfolgens
 * leitet die Hauptschleife Maus und Tasten als MT_MOUSE/MT_KEY an das
 * Fenster (menu_input_*). Ausgefuehrt wird mit WM_COMMAND an das Fenster.
 * Eine Ebene Aufklappmenues (Untermenues in Aufklappmenues fehlen noch).
 * Aussehen eigener Entwurf im CUA-Stil.
 */
#include "gui.h"

#define NMENU   32
#define NMI     24
#define MTXT    40
struct mitem { WORD flags, id; char text[MTXT]; };
struct menu { BYTE used, n; short task; struct mitem it[NMI]; };
static struct menu FAR *mt;
#define HM(i)   ((HMENU)(0x7000u + ((i) << 2)))
#define MI(h)   ((((WORD)(h)) - 0x7000u) >> 2)

#define MF_GRAYED       0x0001
#define MF_DISABLED     0x0002
#define MF_BITMAP       0x0004
#define MF_CHECKED      0x0008
#define MF_POPUP        0x0010
#define MF_MENUBARBREAK 0x0020
#define MF_MENUBREAK    0x0040
#define MF_END          0x0080
#define MF_OWNERDRAW    0x0100
#define MF_BYPOSITION   0x0400
#define MF_SEPARATOR    0x0800
#define MF_HELP         0x4000

#define WM_COMMAND      0x0111
#define WM_INITMENU     0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MENUSELECT   0x011F
#define WM_PAINT        0x000F
#define WM_ERASEBKGND   0x0014

static struct menu FAR *mptr(HMENU h)
{
    WORD i = MI(h);
    if (!mt || h < 0x7000u || i >= NMENU || !mt[i].used)
        return 0;
    return &mt[i];
}

static HMENU mnew(void)
{
    short i;
    if (!mt) {
        WORD s = seg_new(sizeof(struct menu) * NMENU);
        if (!s)
            return 0;
        mt = MKFP(s, 0);
    }
    for (i = 0; i < NMENU; i++)
        if (!mt[i].used) {
            fmemset(&mt[i], 0, sizeof(struct menu));
            mt[i].used = 1;
            mt[i].task = cur_task;
            return HM(i);
        }
    return 0;
}

void menu_free(HMENU h)
{
    struct menu FAR *m = mptr(h);
    short i;
    if (!m)
        return;
    m->used = 0;
    for (i = 0; i < m->n; i++)
        if (m->it[i].flags & MF_POPUP)
            menu_free(m->it[i].id);
}

void menu_task_end(int t)
{
    short i;
    if (!mt)
        return;
    for (i = 0; i < NMENU; i++)
        if (mt[i].used && mt[i].task == t)
            mt[i].used = 0;
}

static void iset(struct mitem FAR *it, WORD fl, WORD id, LPCSTR s)
{
    it->flags = fl;
    it->id = id;
    it->text[0] = 0;
    if (!(fl & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) && s && SELOF(s))
        fstrncpy(it->text, s, MTXT);
    if (!(fl & MF_POPUP) && !it->text[0] && !(fl & (MF_BITMAP | MF_OWNERDRAW)))
        it->flags |= MF_SEPARATOR;
}

static int madd(HMENU h, WORD fl, WORD id, LPCSTR s, short pos)
{
    struct menu FAR *m = mptr(h);
    short i;
    if (!m || m->n >= NMI)
        return 0;
    if (pos < 0 || pos > m->n)
        pos = m->n;
    for (i = m->n; i > pos; i--)
        m->it[i] = m->it[i - 1];
    m->n++;
    iset(&m->it[pos], fl & ~(MF_BYPOSITION | MF_END), id, s);
    return 1;
}

/* Vorlage RT_MENU: Kopf (Version, Laenge), dann je Eintrag Wort Flags,
   ohne MF_POPUP ein Wort Kennung, Text; MF_END schliesst die Ebene */
static BYTE FAR *mparse(HMENU h, BYTE FAR *p)
{
    for (;;) {
        WORD fl = *(WORD FAR *)p, id = 0;
        LPCSTR s;
        p += 2;
        if (!(fl & MF_POPUP)) {
            id = *(WORD FAR *)p;
            p += 2;
        }
        s = (LPCSTR)p;
        while (*p)
            p++;
        p++;
        if (fl & MF_POPUP) {
            HMENU sub = mnew();
            if (!sub)
                return p;
            p = mparse(sub, p);
            madd(h, fl, sub, s, -1);
        } else
            madd(h, fl, id, s, -1);
        if (fl & MF_END)
            return p;
    }
}

HMENU API LoadMenuIndirect(LPVOID tpl)
{
    HMENU h = mnew();
    BYTE FAR *p = tpl;
    if (!h)
        return 0;
    p += 4 + *(WORD FAR *)(p + 2);
    mparse(h, p);
    return h;
}

HMENU API LoadMenu(WORD hinst, LPCSTR name)
{
    int t = task_of_inst(hinst), m = t >= 0 ? wt[t].hmod - 1 : -1;
    DWORD r;
    WORD sel;
    HMENU h;
    if (hinst > 0 && hinst <= NMOD)
        m = hinst - 1;
    r = w16_res_find(m, MKFP(0, 4), name);
    if (!r || !(sel = w16_res_load(m, r)))
        return 0;
    h = LoadMenuIndirect(MKFP(sel, 0));
    seg_del(sel);
    return h;
}

HMENU API CreateMenu(void) { return mnew(); }
HMENU API CreatePopupMenu(void) { return mnew(); }
BOOL API DestroyMenu(HMENU h) { menu_free(h); return 1; }
BOOL API AppendMenu(HMENU h, WORD fl, WORD id, LPCSTR s) { return madd(h, fl, id, s, -1); }

/* Eintrag nach Kennung (auch in Untermenues) oder Stelle */
static struct mitem FAR *mfind(HMENU h, WORD id, WORD fl, HMENU FAR *owner)
{
    struct menu FAR *m = mptr(h);
    short i;
    if (!m)
        return 0;
    if (fl & MF_BYPOSITION) {
        if (owner) *owner = h;
        return id < m->n ? &m->it[id] : 0;
    }
    for (i = 0; i < m->n; i++) {
        if (!(m->it[i].flags & MF_POPUP) && m->it[i].id == id) {
            if (owner) *owner = h;
            return &m->it[i];
        }
        if (m->it[i].flags & MF_POPUP) {
            struct mitem FAR *r = mfind(m->it[i].id, id, fl, owner);
            if (r)
                return r;
        }
    }
    return 0;
}

BOOL API InsertMenu(HMENU h, WORD pos, WORD fl, WORD id, LPCSTR s)
{
    struct menu FAR *m = mptr(h);
    short k = -1, i;
    if (!m)
        return 0;
    if (fl & MF_BYPOSITION)
        k = pos == 0xFFFF ? -1 : pos;
    else
        for (i = 0; i < m->n; i++)
            if (m->it[i].id == pos)
                k = i;
    return madd(h, fl, id, s, k);
}
BOOL API ModifyMenu(HMENU h, WORD pos, WORD fl, WORD id, LPCSTR s)
{
    struct mitem FAR *it = mfind(h, pos, fl, 0);
    if (!it)
        return 0;
    iset(it, fl & ~(MF_BYPOSITION | MF_END), id, s);
    return 1;
}
static BOOL mremove(HMENU h, WORD pos, WORD fl, int destroy)
{
    HMENU o = 0;
    struct mitem FAR *it = mfind(h, pos, fl, &o);
    struct menu FAR *m = mptr(o);
    short i;
    if (!it || !m)
        return 0;
    if (destroy && (it->flags & MF_POPUP))
        menu_free(it->id);
    for (i = (short)(it - m->it); i + 1 < m->n; i++)
        m->it[i] = m->it[i + 1];
    m->n--;
    return 1;
}
BOOL API DeleteMenu(HMENU h, WORD pos, WORD fl) { return mremove(h, pos, fl, 1); }
BOOL API RemoveMenu(HMENU h, WORD pos, WORD fl) { return mremove(h, pos, fl, 0); }

BOOL API CheckMenuItem(HMENU h, WORD id, WORD fl)
{
    struct mitem FAR *it = mfind(h, id, fl, 0);
    WORD o;
    if (!it)
        return (BOOL)-1;
    o = it->flags & MF_CHECKED;
    if (fl & MF_CHECKED) it->flags |= MF_CHECKED; else it->flags &= ~MF_CHECKED;
    return o;
}
BOOL API EnableMenuItem(HMENU h, WORD id, WORD fl)
{
    struct mitem FAR *it = mfind(h, id, fl, 0);
    WORD o;
    if (!it)
        return (BOOL)-1;
    o = it->flags & (MF_GRAYED | MF_DISABLED);
    it->flags = (it->flags & ~(MF_GRAYED | MF_DISABLED)) | (fl & (MF_GRAYED | MF_DISABLED));
    return o;
}
WORD API GetMenuState(HMENU h, WORD id, WORD fl)
{
    struct mitem FAR *it = mfind(h, id, fl, 0);
    if (!it)
        return 0xFFFF;
    if (it->flags & MF_POPUP) {
        struct menu FAR *s = mptr(it->id);
        return (it->flags & 0xFF) | ((s ? s->n : 0) << 8);
    }
    return it->flags;
}
short API GetMenuItemCount(HMENU h) { struct menu FAR *m = mptr(h); return m ? m->n : -1; }
WORD API GetMenuItemID(HMENU h, short pos)
{
    struct menu FAR *m = mptr(h);
    if (!m || pos < 0 || pos >= m->n)
        return 0xFFFF;
    return (m->it[pos].flags & MF_POPUP) ? 0xFFFF : m->it[pos].id;
}
HMENU API GetSubMenu(HMENU h, short pos)
{
    struct menu FAR *m = mptr(h);
    if (!m || pos < 0 || pos >= m->n || !(m->it[pos].flags & MF_POPUP))
        return 0;
    return m->it[pos].id;
}
short API GetMenuString(HMENU h, WORD id, LPSTR s, short n, WORD fl)
{
    struct mitem FAR *it = mfind(h, id, fl, 0);
    if (!it || n <= 0)
        return 0;
    fstrncpy(s, it->text, n);
    return fstrlen(s);
}
BOOL API HiliteMenuItem(HWND w, HMENU h, WORD id, WORD fl)
{
    (void)w; (void)h; (void)id; (void)fl;
    return 1;
}

/* ---- Zeichnen ------------------------------------------------------ */
static WORD tlen(LPCSTR s, int left)             /* Zeichen ohne '&'      */
{
    WORD n = 0;
    for (; *s && !(left && *s == '\t'); s++)
        if (*s != '&' || s[1] == '&')
            n++;
        else if (s[1] == '&')
            s++;
    return n;
}

/* Text mit '&' zeichnen: Buchstabe dahinter unterstrichen */
static void mtext(HDC dc, short x, short y, LPCSTR s, BYTE fg, int left)
{
    static char b[MTXT];
    WORD n = 0;
    short ul = -1;
    for (; *s && !(left && *s == '\t') && n < MTXT; s++) {
        if (*s == '&' && s[1] && s[1] != '&') {
            ul = n;
            continue;
        }
        if (*s == '&')
            s++;
        b[n++] = *s;
    }
    dc_rawtext(dc, x, y, b, n, fg);
    if (ul >= 0)
        dc_rawfill(dc, x + ul * FONT_W, y + FONT_H - 2, x + ul * FONT_W + FONT_W, y + FONT_H - 1, fg);
}

static BYTE mnem(LPCSTR s)                        /* Buchstabe nach '&'    */
{
    for (; *s; s++)
        if (*s == '&' && s[1] && s[1] != '&') {
            BYTE c = s[1];
            return (BYTE)(c >= 'a' && c <= 'z' ? c - 32 : c);
        } else if (*s == '&')
            s++;
    return 0;
}

/* Lage der Leiste und eines Eintrags (Bildschirm) */
void menu_bar_rect(struct wnd *w, RECT FAR *r)
{
    short b = wnd_border(w);
    r->left = w->wr.left + b;
    r->right = w->wr.right - b;
    r->top = w->wr.top + b + ((w->style & 0x00C00000UL) == 0x00C00000UL ? CAPH : 0);
    r->bottom = r->top + MENUH;
}
static short bar_item(struct wnd *w, short i, short FAR *x0)
{
    struct menu FAR *m = mptr(w->menu);
    RECT r;
    short k, x;
    menu_bar_rect(w, &r);
    x = r.left + 4;
    for (k = 0; m && k < m->n; k++) {
        short wd = tlen(m->it[k].text, 0) * FONT_W + 16;
        if (k == i) {
            *x0 = x;
            return x + wd;
        }
        x += wd;
    }
    *x0 = x;
    return x;
}
short menu_bar_hit(struct wnd *w, short x, short y)
{
    struct menu FAR *m = mptr(w->menu);
    RECT r;
    short k, x0, x1;
    menu_bar_rect(w, &r);
    if (!m || y < r.top || y >= r.bottom || x < r.left || x >= r.right)
        return -1;
    for (k = 0; k < m->n; k++) {
        x1 = bar_item(w, k, &x0);
        if (x >= x0 && x < x1)
            return k;
    }
    return -2;                                  /* Leiste, kein Eintrag  */
}

static HWND mt_h;                               /* Fenster beim Verfolgen */
static short mt_cur = -1;                       /* hervorgehoben          */
static BYTE  mt_on;

void menu_bar_paint(HDC dc, struct wnd *w)
{
    struct menu FAR *m = mptr(w->menu);
    RECT r;
    short k, x0, x1;
    menu_bar_rect(w, &r);
    dc_rawfill(dc, r.left, r.top, r.right, r.bottom - 1, 15);
    dc_rawfill(dc, r.left, r.bottom - 1, r.right, r.bottom, 0);
    if (!m)
        return;
    for (k = 0; k < m->n; k++) {
        int hi = mt_on && HW(w - ww) == mt_h && k == mt_cur;
        BYTE fg = (m->it[k].flags & MF_GRAYED) ? 8 : 0;
        x1 = bar_item(w, k, &x0);
        if (x0 >= r.right)
            break;
        if (hi) {
            dc_rawfill(dc, x0, r.top + 1, x1, r.bottom - 2, 4);
            fg = 15;
        }
        mtext(dc, x0 + 8, r.top + 2, m->it[k].text, fg, 0);
    }
}

/* ---- Aufklappmenue (Fensterklasse #MENU, intern) -------------------- */
#define ITEMH   (FONT_H + 2)
#define SEPH    8
static HWND pop;
static HMENU pop_m;
static short pop_sel = -1;

static short item_h(struct mitem FAR *it) { return (it->flags & MF_SEPARATOR) ? SEPH : ITEMH; }

static void pop_size(HMENU h, short FAR *cx, short FAR *cy)
{
    struct menu FAR *m = mptr(h);
    short k, lw = 0, rw = 0, y = 4;
    for (k = 0; m && k < m->n; k++) {
        LPCSTR s = m->it[k].text, t = s;
        short a = tlen(s, 1), b = 0;
        while (*t && *t != '\t')
            t++;
        if (*t == '\t')
            b = fstrlen(t + 1);
        if (a > lw) lw = a;
        if (b > rw) rw = b;
        y += item_h(&m->it[k]);
    }
    *cx = (lw + rw) * FONT_W + (rw ? 32 : 8) + 24;
    *cy = y + 4;
}

static short pop_at(short y)                    /* Eintrag an Hoehe      */
{
    struct menu FAR *m = mptr(pop_m);
    struct wnd *w = wptr(pop);
    short k, t;
    if (!m || !w)
        return -1;
    t = w->cr.top + 2;
    for (k = 0; k < m->n; k++) {
        short ih = item_h(&m->it[k]);
        if (y >= t && y < t + ih)
            return (m->it[k].flags & MF_SEPARATOR) ? -1 : k;
        t += ih;
    }
    return -1;
}

LONG FAR PASCAL menu_pop_proc(HWND h, WORD msg, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    struct menu FAR *m = mptr(pop_m);
    if (!w)
        return 0;
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        static struct { HDC hdc; BOOL e; RECT rc; BOOL r, i; BYTE x[16]; } ps;
        HDC dc = BeginPaint(h, (void FAR *)&ps);
        short k, y = w->cr.top + 2, x = w->cr.left;
        dc_rawfill(dc, w->cr.left, w->cr.top, w->cr.right, w->cr.bottom, 15);
        for (k = 0; m && k < m->n; k++) {
            struct mitem FAR *it = &m->it[k];
            short ih = item_h(it);
            if (it->flags & MF_SEPARATOR) {
                dc_rawfill(dc, x + 2, y + SEPH / 2 - 1, w->cr.right - 2, y + SEPH / 2, 0);
            } else {
                BYTE fg = (it->flags & MF_GRAYED) ? 8 : 0;
                LPCSTR t = it->text;
                if (k == pop_sel) {
                    dc_rawfill(dc, x + 1, y, w->cr.right - 1, y + ih, 4);
                    fg = (it->flags & MF_GRAYED) ? 7 : 15;
                }
                if (it->flags & MF_CHECKED) {           /* Haken         */
                    short cx = x + 5, cy = y + 9, i;
                    for (i = 0; i < 3; i++)
                        dc_rawfill(dc, cx + i, cy + i, cx + i + 2, cy + i + 2, fg);
                    for (i = 0; i < 6; i++)
                        dc_rawfill(dc, cx + 3 + i, cy + 1 - i, cx + 5 + i, cy + 3 - i, fg);
                }
                mtext(dc, x + 20, y + 1, t, fg, 1);
                while (*t && *t != '\t')
                    t++;
                if (*t == '\t') {
                    short n = fstrlen(t + 1);
                    dc_rawtext(dc, w->cr.right - 10 - n * FONT_W, y + 1, t + 1, n, fg);
                }
            }
            y += ih;
        }
        EndPaint(h, (void FAR *)&ps);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void pop_close(void)
{
    if (pop) {
        DestroyWindow(pop);
        pop = 0;
    }
    pop_m = 0;
    pop_sel = -1;
}

static void pop_open(struct wnd *w, short k)
{
    struct menu FAR *m = mptr(w->menu);
    short x0, cx, cy;
    RECT r;
    pop_close();
    mt_cur = k;
    w->need_ncpaint = 1;
    if (!m || k < 0 || k >= m->n || !(m->it[k].flags & MF_POPUP) ||
        (m->it[k].flags & MF_GRAYED))
        return;
    send_msg(HW(w - ww), WM_INITMENUPOPUP, m->it[k].id, MAKELONG(k, 0));
    pop_m = m->it[k].id;
    pop_size(pop_m, &cx, &cy);
    bar_item(w, k, &x0);
    menu_bar_rect(w, &r);
    if (x0 + cx > SCR_W) x0 = SCR_W - cx;
    pop = CreateWindowEx(0, MKFP(0, 0xC000 + 3), "", 0x80000000UL | 0x00800000UL,
                         x0, r.bottom, cx, cy, HW(w - ww), 0, 0, 0);
    if (pop) {
        wptr(pop)->task = cur_task;
        ShowWindow(pop, 4);                     /* ohne Aktivieren       */
    }
}

static void pop_move(short d)
{
    struct menu FAR *m = mptr(pop_m);
    short k, i;
    if (!m || !m->n)
        return;
    k = pop_sel;
    for (i = 0; i < m->n; i++) {
        k = k < 0 ? (d > 0 ? 0 : m->n - 1) : (k + d + m->n) % m->n;
        if (!(m->it[k].flags & MF_SEPARATOR))
            break;
    }
    pop_sel = k;
    if (pop)
        InvalidateRect(pop, 0, 0);
}

/* Maus und Tasten waehrend des Verfolgens (aus der Hauptschleife) */
int menu_input_mouse(short x, short y, BYTE btn)
{
    if (!mt_on)
        return 0;
    post_msg(mt_h, MT_MOUSE, btn, MAKELONG(x, y));
    return 1;
}
int menu_input_key(WORD k)
{
    if (!mt_on)
        return 0;
    post_msg(mt_h, MT_KEY, k, 0);
    return 1;
}

/* Verfolgen im Task des Fensters; bar = Eintrag zum Aufklappen, -1 nur
   die Leiste (F10/Alt); -> 0 */
void menu_track(HWND h, short bar, int bykey)
{
    struct wnd *w = wptr(h);
    struct menu FAR *m;
    static MSG msg;
    WORD cmd = 0;
    BYTE lbtn = bykey ? 0 : 1, moved = 0;
    if (!w || !(m = mptr(w->menu)) || mt_on)
        return;
    send_msg(h, WM_INITMENU, w->menu, 0);
    mt_on = 1;
    mt_h = h;
    mt_cur = bar < 0 ? 0 : bar;
    if (bar >= 0)
        pop_open(w, bar);
    else
        w->need_ncpaint = 1;
    if (bykey && pop)
        pop_move(1);
    while (mt_on && wptr(h)) {
        if (!user_get_msg(&msg, 0, 0, 0, 1, 1)) {
            PostQuitMessage(msg.wParam);
            break;
        }
        if (msg.hwnd == h && msg.message == MT_MOUSE) {
            short x = LOWORD(msg.lParam), y = HIWORD(msg.lParam), k;
            BYTE btn = (BYTE)msg.wParam, dn = btn & ~lbtn, up = lbtn & ~btn;
            struct wnd *pw = wptr(pop);
            lbtn = btn;
            k = menu_bar_hit(w, x, y);
            if (k >= 0) {
                if (k != mt_cur || (!pop && (btn & 1))) {
                    pop_open(w, k);
                    moved = 1;
                } else if (dn & 1) {            /* Klick auf offenen     */
                    if (!moved)
                        break;
                    moved = 0;
                }
                continue;
            }
            if (pw && x >= pw->wr.left && x < pw->wr.right && y >= pw->wr.top && y < pw->wr.bottom) {
                short s = pop_at(y);
                moved = 1;
                if (s != pop_sel) {
                    pop_sel = s;
                    InvalidateRect(pop, 0, 0);
                }
                if ((up & 1) && s >= 0) {
                    struct mitem FAR *it = &mptr(pop_m)->it[s];
                    if (!(it->flags & (MF_GRAYED | MF_DISABLED | MF_POPUP))) {
                        cmd = it->id;
                        break;
                    }
                }
                continue;
            }
            if (dn)
                break;                          /* Klick daneben         */
            continue;
        }
        if (msg.hwnd == h && msg.message == MT_KEY) {
            BYTE ch = (BYTE)msg.wParam, sc = msg.wParam >> 8;
            if (sc == 0x01)                     /* Esc                   */
                break;
            if (sc == 0x4B || sc == 0x4D) {     /* links/rechts          */
                short n = m->n, k = (mt_cur + (sc == 0x4D ? 1 : n - 1)) % n;
                int was = pop != 0;
                pop_open(w, k);
                if (!was && !pop) w->need_ncpaint = 1;
                if (pop) pop_move(1);
                continue;
            }
            if (sc == 0x48 || sc == 0x50) {     /* hoch/runter           */
                if (!pop) {
                    pop_open(w, mt_cur);
                    if (pop) pop_move(1);
                } else
                    pop_move(sc == 0x50 ? 1 : -1);
                continue;
            }
            if (ch == 0x0D) {
                if (!pop) {
                    pop_open(w, mt_cur);
                    if (pop) pop_move(1);
                    else if (mt_cur >= 0 && !(m->it[mt_cur].flags & (MF_GRAYED | MF_POPUP))) {
                        cmd = m->it[mt_cur].id;
                        break;
                    }
                } else if (pop_sel >= 0) {
                    struct mitem FAR *it = &mptr(pop_m)->it[pop_sel];
                    if (!(it->flags & (MF_GRAYED | MF_DISABLED | MF_POPUP))) {
                        cmd = it->id;
                        break;
                    }
                }
                continue;
            }
            if (ch > ' ') {                     /* Buchstabe             */
                struct menu FAR *pm = pop ? mptr(pop_m) : m;
                short k;
                BYTE u = (BYTE)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
                if (u == 0x84) u = 0x8E; else if (u == 0x94) u = 0x99; else if (u == 0x81) u = 0x9A;
                for (k = 0; pm && k < pm->n; k++)
                    if (ansi2oem(mnem(pm->it[k].text)) == u || mnem(pm->it[k].text) == u)
                        break;
                if (!pm || k == pm->n) {
                    MessageBeep_c();
                    continue;
                }
                if (!pop) {
                    pop_open(w, k);
                    if (pop) pop_move(1);
                    else if (!(m->it[k].flags & MF_GRAYED)) {
                        cmd = m->it[k].id;
                        break;
                    }
                } else if (!(pm->it[k].flags & (MF_GRAYED | MF_DISABLED | MF_POPUP))) {
                    cmd = pm->it[k].id;
                    break;
                }
            }
            continue;
        }
        DispatchMessage(&msg);
    }
    pop_close();
    mt_on = 0;
    mt_cur = -1;
    if (wptr(h))
        wptr(h)->need_ncpaint = 1;
    if (cmd && wptr(h))
        post_msg(h, WM_COMMAND, cmd, 0);
}

/* Klappmenue frei auf dem Bildschirm (Systemmenue, TrackPopupMenu):
   -> Kennung, 0 = abgebrochen, FFFFh = Klick in box (Systemfeld).
   mouse: Maustaste ist beim Oeffnen gedrueckt (Loslassen ueber einem
   Eintrag waehlt ihn) */
/* fuer andere Verfolger (Klappliste der COMBOBOX): gleicher Weg */
int track_on(HWND h)
{
    if (mt_on || !wptr(h))
        return 0;
    mt_on = 1;
    mt_h = h;
    mt_cur = -1;
    return 1;
}
int track_busy(void) { return mt_on; }
void track_off(void) { mt_on = 0; mt_cur = -1; }

WORD menu_popup(HWND h, HMENU hm, short x, short y, const RECT FAR *box, int mouse)
{
    static MSG msg;
    struct menu FAR *m = mptr(hm);
    short cx, cy;
    WORD cmd = 0;
    BYTE lbtn = mouse ? 1 : 0;
    if (!m || !m->n || mt_on || !wptr(h))
        return 0;
    pop_close();
    pop_m = hm;
    pop_size(hm, &cx, &cy);
    if (x + cx > SCR_W) x = SCR_W - cx;
    if (y + cy > SCR_H) y = SCR_H - cy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    pop = CreateWindowEx(0, MKFP(0, 0xC000 + 3), "", 0x80000000UL | 0x00800000UL,
                         x, y, cx, cy, h, 0, 0, 0);
    if (!pop) {
        pop_m = 0;
        return 0;
    }
    wptr(pop)->task = cur_task;
    ShowWindow(pop, 4);
    mt_on = 1;
    mt_h = h;
    mt_cur = -1;
    if (!mouse)
        pop_move(1);
    while (mt_on && wptr(h)) {
        if (!user_get_msg(&msg, 0, 0, 0, 1, 1)) {
            PostQuitMessage(msg.wParam);
            break;
        }
        if (msg.hwnd == h && msg.message == MT_MOUSE) {
            short mx = LOWORD(msg.lParam), my = HIWORD(msg.lParam);
            BYTE btn = (BYTE)msg.wParam, dn = btn & ~lbtn, up = lbtn & ~btn;
            struct wnd *pw = wptr(pop);
            lbtn = btn;
            if (pw && mx >= pw->wr.left && mx < pw->wr.right && my >= pw->wr.top && my < pw->wr.bottom) {
                short k = pop_at(my);
                if (k != pop_sel) {
                    pop_sel = k;
                    InvalidateRect(pop, 0, 0);
                }
                if ((up & 1) && k >= 0) {
                    struct mitem FAR *it = &mptr(pop_m)->it[k];
                    if (!(it->flags & (MF_GRAYED | MF_DISABLED | MF_POPUP))) {
                        cmd = it->id;
                        break;
                    }
                }
                continue;
            }
            if (dn && box && mx >= box->left && mx < box->right && my >= box->top && my < box->bottom) {
                cmd = 0xFFFF;
                break;
            }
            if (dn)
                break;
            continue;
        }
        if (msg.hwnd == h && msg.message == MT_KEY) {
            BYTE ch = (BYTE)msg.wParam, sc = msg.wParam >> 8;
            struct menu FAR *pm = mptr(pop_m);
            if (sc == 0x01)
                break;
            if (sc == 0x48 || sc == 0x50) {
                pop_move(sc == 0x50 ? 1 : -1);
                continue;
            }
            if (ch == 0x0D) {
                if (pop_sel >= 0 && pm) {
                    struct mitem FAR *it = &pm->it[pop_sel];
                    if (!(it->flags & (MF_GRAYED | MF_DISABLED | MF_POPUP))) {
                        cmd = it->id;
                        break;
                    }
                }
                continue;
            }
            if (ch > ' ' && pm) {
                short k;
                BYTE u = (BYTE)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
                if (u == 0x84) u = 0x8E; else if (u == 0x94) u = 0x99; else if (u == 0x81) u = 0x9A;
                for (k = 0; k < pm->n; k++)
                    if (ansi2oem(mnem(pm->it[k].text)) == u || mnem(pm->it[k].text) == u)
                        break;
                if (k < pm->n && !(pm->it[k].flags & (MF_GRAYED | MF_DISABLED | MF_POPUP))) {
                    cmd = pm->it[k].id;
                    break;
                }
                MessageBeep_c();
            }
            continue;
        }
        DispatchMessage(&msg);
    }
    pop_close();
    mt_on = 0;
    mt_cur = -1;
    return cmd;
}

BOOL API TrackPopupMenu(HMENU hm, WORD fl, short x, short y, short r, HWND h, const RECT FAR *rc)
{
    WORD c;
    (void)fl; (void)r; (void)rc;
    c = menu_popup(h, hm, x, y, 0, 0);
    if (c && c != 0xFFFF)
        post_msg(h, WM_COMMAND, c, 0);
    return 1;
}

/* Buchstabe nach Alt: Eintrag der Leiste */
short menu_mnemonic(HMENU h, BYTE ch)
{
    struct menu FAR *m = mptr(h);
    short k;
    BYTE u = (BYTE)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    for (k = 0; m && k < m->n; k++)
        if (mnem(m->it[k].text) == u)
            return k;
    return -1;
}

/* ---- Tastenkuerzel ------------------------------------------------- */
#define FVIRTKEY  0x01
#define FSHIFT    0x04
#define FCONTROL  0x08
#define FALT      0x10

WORD API LoadAccelerators(WORD hinst, LPCSTR name)
{
    int t = task_of_inst(hinst), m = t >= 0 ? wt[t].hmod - 1 : -1;
    DWORD r = w16_res_find(m, MKFP(0, 9), name);
    return r ? w16_res_load(m, r) : 0;
}

/* wie Windows: vor dem WM_COMMAND eines Kuerzels WM_INITMENU und
   WM_INITMENUPOPUP schicken; steht der Menuepunkt danach gesperrt da,
   kommt kein Befehl (PROGMAN: Enter in leerer Gruppe = "Oeffnen") */
HMENU API GetMenu(HWND h);
static int acc_ok(HWND h, WORD id)
{
    HMENU hm = GetMenu(h), own = 0;
    struct menu FAR *m;
    struct mitem FAR *it;
    short k;
    if (!hm || !mfind(hm, id, 0, &own))
        return 1;
    send_msg(h, WM_INITMENU, hm, 0);
    m = mptr(hm);
    if (m && own != hm)
        for (k = 0; k < m->n; k++)
            if ((m->it[k].flags & MF_POPUP) && m->it[k].id == own) {
                send_msg(h, WM_INITMENUPOPUP, own, MAKELONG(k, 0));
                break;
            }
    it = mfind(hm, id, 0, 0);
    return !it || !(it->flags & (MF_GRAYED | MF_DISABLED));
}

short API TranslateAccelerator(HWND h, WORD acc, MSG FAR *msg)
{
    BYTE FAR *p;
    WORD i, sh, vk = msg->wParam, ch;
    int virt;
    if (!acc || !h)
        return 0;
    if (msg->message == 0x0100 || msg->message == 0x0104)
        virt = 1;
    else if (msg->message == 0x0102 || msg->message == 0x0106)
        virt = 0;
    else
        return 0;
    sh = (WORD)(msg->lParam >> 24);             /* Bits aus key_event    */
    for (i = 0; i < 256; i++) {
        BYTE fl;
        p = MKFP(acc, i * 5);
        fl = p[0];
        ch = *(WORD FAR *)(p + 1);
        if (virt && (fl & FVIRTKEY)) {
            if (ch == vk &&
                !!(fl & FSHIFT) == !!(sh & 2) &&
                !!(fl & FCONTROL) == !!(sh & 4) &&
                !!(fl & FALT) == !!(sh & 0x20)) {
                if (acc_ok(h, *(WORD FAR *)(p + 3)))
                    send_msg(h, WM_COMMAND, *(WORD FAR *)(p + 3), MAKELONG(0, 1));
                return 1;
            }
        } else if (!virt && !(fl & FVIRTKEY) && ch == vk &&
                   (!(fl & FALT) || (sh & 0x20))) {
            if (acc_ok(h, *(WORD FAR *)(p + 3)))
                send_msg(h, WM_COMMAND, *(WORD FAR *)(p + 3), MAKELONG(0, 1));
            return 1;
        }
        if (fl & 0x80)
            break;
    }
    return 0;
}

/* Windows 2.x: ein Aufruf fuer Einfuegen/Aendern/Anhaengen/Loeschen */
BOOL API ChangeMenu(HMENU h, WORD pos, LPCSTR s, WORD id, WORD fl)
{
    WORD f = fl & ~(0x0080 | 0x0100 | 0x0200 | 0x1000);
    if (fl & 0x0100)                            /* MF_APPEND            */
        return AppendMenu(h, f, id, s);
    if (fl & 0x0200)                            /* MF_DELETE            */
        return mremove(h, pos, f, 1);
    if (fl & 0x1000)                            /* MF_REMOVE            */
        return mremove(h, pos, f, 0);
    if (fl & 0x0080)                            /* MF_CHANGE            */
        return ModifyMenu(h, pos, f, id, s);
    return InsertMenu(h, pos, f, id, s);        /* MF_INSERT            */
}
