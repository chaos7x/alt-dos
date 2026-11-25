/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - MDI wie Windows 3.x (Klasse MDICLIENT, DefFrameProc,
 * DefMDIChildProc). Eigener Code; Verhalten nach der Windows-Doku.
 *
 * Ein MDI-Client verwaltet seine Kindfenster: Anlegen ueber WM_MDICREATE
 * (Kind mit Titel, Rahmen, Systemfeld), Aktivieren (Titelfarbe, oben in
 * der Reihenfolge), Liste im "Fenster"-Menue (Kennungen ab idFirstChild),
 * Ueberlappend/Nebeneinander, Vollbild im Client.
 */
#include "gui.h"

struct paintst { HDC hdc; BOOL erase; RECT rc; BOOL res, inc; BYTE rgb[16]; };
BOOL API RemoveMenu(HMENU h, WORD pos, WORD fl);
BOOL API AppendMenu(HMENU h, WORD fl, WORD id, LPCSTR s);
BOOL API BringWindowToTop(HWND h);
BOOL API MoveWindow(HWND h, short x, short y, short cx, short cy, BOOL rep);
void API OffsetRect(LPRECT r, short x, short y);
HMENU API GetMenu(HWND h);
BOOL API SetMenu(HWND h, HMENU m);
void API DrawMenuBar(HWND h);

#define WM_CREATE        0x0001
#define WM_SIZE          0x0005
#define WM_SETFOCUS      0x0007
#define WM_PAINT         0x000F
#define WM_CLOSE         0x0010
#define WM_ERASEBKGND    0x0014
#define WM_CHILDACTIVATE 0x0022
#define WM_COMMAND       0x0111
#define WM_SYSCOMMAND    0x0112
#define WM_MDICREATE     0x0220
#define WM_MDIDESTROY    0x0221
#define WM_MDIACTIVATE   0x0222
#define WM_MDIRESTORE    0x0223
#define WM_MDINEXT       0x0224
#define WM_MDIMAXIMIZE   0x0225
#define WM_MDITILE       0x0226
#define WM_MDICASCADE    0x0227
#define WM_MDIICONARRANGE 0x0228
#define WM_MDIGETACTIVE  0x0229
#define WM_MDISETMENU    0x0230
#define SC_CLOSE         0xF060
#define SC_NEXTWINDOW    0xF040

#define NMDI   4
#define NMDIC  24                       /* Kinder je Client              */
static struct mdi {
    HWND client, act;
    HMENU wmenu;
    WORD first;
    BYTE maxed, nlist;
    RECT rest;                          /* vor dem Vollbild (Client-Koord.) */
    HWND kid[NMDIC];                    /* Reihenfolge des Anlegens       */
} md[NMDI];

static struct mdi *mdp(HWND client)
{
    short i;
    for (i = 0; i < NMDI; i++)
        if (md[i].client == client && client)
            return &md[i];
    return 0;
}
static struct mdi *mdp_kid(HWND kid)
{
    struct wnd *w = wptr(kid);
    return w ? mdp(w->parent) : 0;
}

/* aktives MDI-Kind? (USER: Titelfarbe) */
int mdi_kid_active(HWND h)
{
    struct mdi *m = mdp_kid(h);
    return m && m->act == h;
}
int mdi_is_kid(HWND h)
{
    return mdp_kid(h) != 0;
}

/* Liste der Kinder im Fenster-Menue: Trennstrich + "&1 Titel" ... */
static void mdi_menu(struct mdi *m)
{
    short i, k = 0;
    static char t[72];
    if (!m->wmenu)
        return;
    for (i = 0; i < m->nlist + 1; i++)          /* alte Eintraege weg     */
        RemoveMenu(m->wmenu, m->first + i, 0);
    if (m->nlist)
        RemoveMenu(m->wmenu, m->first - 1, 0);  /* Trennstrich            */
    m->nlist = 0;
    for (i = 0; i < NMDIC; i++)
        if (m->kid[i] && wptr(m->kid[i]))
            k++;
    if (!k)
        return;
    AppendMenu(m->wmenu, 0x0800, m->first - 1, 0);       /* MF_SEPARATOR */
    k = 0;
    for (i = 0; i < NMDIC && k < 9; i++) {
        struct wnd *w = wptr(m->kid[i]);
        if (!w)
            continue;
        t[0] = '&'; t[1] = (char)('1' + k); t[2] = ' ';
        fstrncpy(t + 3, w->title, sizeof(t) - 3);
        AppendMenu(m->wmenu, m->act == m->kid[i] ? 0x0008 : 0, m->first + k, t);
        k++;
    }
    m->nlist = (BYTE)k;
}

static void mdi_act(struct mdi *m, HWND h)
{
    HWND o = m->act;
    struct wnd *w = wptr(h), *ow = wptr(o);
    if (!w)
        return;
    if (o != h) {
        m->act = h;
        if (ow) {
            ow->need_ncpaint = 1;
            send_msg(o, WM_MDIACTIVATE, 0, MAKELONG(h, o));
        }
        w->need_ncpaint = 1;
        send_msg(h, WM_MDIACTIVATE, 1, MAKELONG(h, o));
        mdi_menu(m);
    }
    BringWindowToTop(h);                        /* USER: Geschwister-Rang */
    SetFocus(h);
}

/* Fenster-Menue-Eintrag Nr. n -> Kind */
static HWND mdi_nth(struct mdi *m, WORD n)
{
    short i, k = 0;
    for (i = 0; i < NMDIC; i++)
        if (m->kid[i] && wptr(m->kid[i])) {
            if (k == n)
                return m->kid[i];
            k++;
        }
    return 0;
}

static void mdi_arrange(struct mdi *m, int tile)
{
    struct wnd *c = wptr(m->client);
    short i, n = 0, k = 0, cw, ch, rows, cols, x = 0, y = 0;
    if (!c)
        return;
    cw = c->cr.right - c->cr.left;
    ch = c->cr.bottom - c->cr.top;
    for (i = 0; i < NMDIC; i++)
        if (m->kid[i] && wptr(m->kid[i]))
            n++;
    if (!n)
        return;
    m->maxed = 0;
    if (tile) {
        for (cols = 1; cols * cols < n; cols++)
            ;
        rows = (n + cols - 1) / cols;
        for (i = 0; i < NMDIC; i++) {
            if (!m->kid[i] || !wptr(m->kid[i]))
                continue;
            x = (k % cols) * (cw / cols);
            y = (k / cols) * (ch / rows);
            MoveWindow(m->kid[i], x, y, cw / cols, ch / rows, 1);
            k++;
        }
    } else {
        for (i = 0; i < NMDIC; i++) {
            if (!m->kid[i] || !wptr(m->kid[i]))
                continue;
            MoveWindow(m->kid[i], x, y, cw * 2 / 3, ch * 2 / 3, 1);
            x += CAPH + 4; y += CAPH + 4;
            if (x > cw / 3 || y > ch / 3)
                x = y = 0;
        }
        if (m->act)
            mdi_act(m, m->act);
    }
}

LONG FAR PASCAL mdi_client_proc(HWND h, WORD msg, WORD wp, DWORD lp)
{
    struct mdi *m = mdp(h);
    struct wnd *w = wptr(h);
    short i;
    switch (msg) {
    case WM_CREATE: {
        /* CREATESTRUCT: lpCreateParams -> CLIENTCREATESTRUCT {hWindowMenu, idFirstChild} */
        WORD FAR *ccs = *(WORD FAR * FAR *)lp;
        for (i = 0; i < NMDI && md[i].client; i++)
            ;
        if (i == NMDI)
            return -1;
        m = &md[i];
        fmemset((void FAR *)m, 0, sizeof(*m));
        m->client = h;
        if (ccs) {
            m->wmenu = ccs[0];
            m->first = ccs[1];
        }
        if (!m->first)
            m->first = 0x7F00;
        return 0;
    }
    case 0x0002:                                /* WM_DESTROY            */
        if (m)
            m->client = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        static struct paintst ps;
        HDC dc = BeginPaint(h, (void FAR *)&ps);
        if (w)
            dc_rawfill(dc, w->cr.left, w->cr.top, w->cr.right, w->cr.bottom,
                       rgb2c(GetSysColor_c(12)));      /* APPWORKSPACE */
        EndPaint(h, (void FAR *)&ps);
        return 0;
    }
    case WM_SIZE:
        if (m && m->maxed && m->act)
            MoveWindow(m->act, 0, 0, LOWORD(lp), HIWORD(lp), 1);
        return 0;
    case WM_MDICREATE: {
        /* MDICREATESTRUCT: szClass, szTitle, hOwner, x, y, cx, cy, style, lParam */
        const BYTE FAR *c = (const BYTE FAR *)lp;
        LPCSTR cls = *(LPCSTR FAR *)c, tit = *(LPCSTR FAR *)(c + 4);
        WORD own = *(WORD FAR *)(c + 8);
        short x = *(short FAR *)(c + 10), y = *(short FAR *)(c + 12);
        short cx = *(short FAR *)(c + 14), cy = *(short FAR *)(c + 16);
        DWORD st = *(DWORD FAR *)(c + 18);
        HWND k;
        short n = 0;
        if (!m)
            return 0;
        for (i = 0; i < NMDIC; i++)
            if (m->kid[i] && wptr(m->kid[i]))
                n++;
        if (x == (short)0x8000 || cx == (short)0x8000) {
            struct wnd *cw = w;
            short W = cw ? cw->cr.right - cw->cr.left : 400;
            short H = cw ? cw->cr.bottom - cw->cr.top : 300;
            if (x == (short)0x8000) { x = (n % 6) * (CAPH + 4); y = x; }
            if (cx == (short)0x8000) { cx = W * 2 / 3; cy = H * 2 / 3; }
        }
        for (i = 0; i < NMDIC && m->kid[i] && wptr(m->kid[i]); i++)
            ;
        if (i == NMDIC)
            return 0;
        k = CreateWindowEx(0, cls, tit,
                           st | 0x40000000UL | 0x00C00000UL | 0x00080000UL |
                           0x00040000UL | 0x00020000UL | 0x00010000UL,
                           x, y, cx, cy, h, m->first + i, own, (LPVOID)lp);
        if (!k)
            return 0;
        m->kid[i] = k;
        ShowWindow(k, 1);
        mdi_act(m, k);
        mdi_menu(m);
        return k;
    }
    case WM_MDIDESTROY: {
        HWND k = wp;
        if (!m)
            return 0;
        for (i = 0; i < NMDIC; i++)
            if (m->kid[i] == k)
                m->kid[i] = 0;
        if (m->act == k) {
            m->act = 0;
            m->maxed = 0;
        }
        DestroyWindow(k);
        for (i = NMDIC - 1; i >= 0; i--)        /* oberstes noch da       */
            if (m->kid[i] && wptr(m->kid[i])) {
                mdi_act(m, m->kid[i]);
                break;
            }
        mdi_menu(m);
        return 0;
    }
    case WM_MDIACTIVATE:
        if (m && wptr(wp))
            mdi_act(m, wp);
        return 0;
    case WM_MDIGETACTIVE:
        return m ? MAKELONG(m->act, m->maxed) : 0;
    case WM_MDINEXT: {
        short cur = -1, j;
        if (!m)
            return 0;
        for (i = 0; i < NMDIC; i++)
            if (m->kid[i] == (wp ? wp : m->act))
                cur = i;
        for (j = 1; j <= NMDIC; j++) {
            short k = (short)((cur + (lp ? NMDIC - j : j) + NMDIC) % NMDIC);
            if (m->kid[k] && wptr(m->kid[k]) && k != cur) {
                mdi_act(m, m->kid[k]);
                break;
            }
        }
        return 0;
    }
    case WM_MDIMAXIMIZE: {
        struct wnd *k = wptr(wp);
        if (!m || !k || !w)
            return 0;
        mdi_act(m, wp);
        m->maxed = 1;
        ShowWindow(wp, 3);                      /* WS_MAXIMIZE, Lage in norm */
        return 0;
    }
    case WM_MDIRESTORE:
        if (m && wptr(wp)) {
            m->maxed = 0;
            ShowWindow(wp, 9);
        }
        return 0;
    case WM_MDITILE:
        if (m) mdi_arrange(m, 1);
        return 0;
    case WM_MDICASCADE:
        if (m) mdi_arrange(m, 0);
        return 0;
    case WM_MDIICONARRANGE:
        return 0;
    case WM_MDISETMENU: {
        HWND fr = w ? w->parent : 0;
        HMENU old = fr ? GetMenu(fr) : 0;
        if (LOWORD(lp) && fr)
            SetMenu(fr, LOWORD(lp));
        if (m && HIWORD(lp)) {
            m->wmenu = HIWORD(lp);
            m->nlist = 0;
            mdi_menu(m);
        }
        if (fr)
            DrawMenuBar(fr);
        return old;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* Rahmenfenster: Fenster-Menue, Groesse des Clients, Fokus */
LONG API DefFrameProc(HWND h, HWND client, WORD msg, WORD wp, DWORD lp)
{
    struct mdi *m = mdp(client);
    switch (msg) {
    case WM_COMMAND:
        if (m && wp >= m->first && wp < m->first + 9) {
            HWND k = mdi_nth(m, wp - m->first);
            if (k)
                mdi_act(m, k);
            return 0;
        }
        break;
    case WM_SIZE:
        if (client && wptr(client))
            MoveWindow(client, 0, 0, LOWORD(lp), HIWORD(lp), 1);
        return 0;
    case WM_SETFOCUS:
        if (m && m->act)
            SetFocus(m->act);
        else if (client)
            SetFocus(client);
        return 0;
    case WM_SYSCOMMAND:
        if (m && wp == SC_NEXTWINDOW) {
            send_msg(client, WM_MDINEXT, 0, 0);
            return 0;
        }
        break;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* Kindfenster: Schliessen ueber den Client, Aktivieren beim Klick */
LONG API DefMDIChildProc(HWND h, WORD msg, WORD wp, DWORD lp)
{
    struct mdi *m = mdp_kid(h);
    switch (msg) {
    case WM_CLOSE:
        if (m) {
            send_msg(m->client, WM_MDIDESTROY, h, 0);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            send_msg(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (m && (wp & 0xFFF0) == 0xF030) {     /* SC_MAXIMIZE           */
            send_msg(m->client, WM_MDIMAXIMIZE, h, 0);
            return 0;
        }
        if (m && (wp & 0xFFF0) == 0xF120) {     /* SC_RESTORE            */
            send_msg(m->client, WM_MDIRESTORE, h, 0);
            return 0;
        }
        if (m && (wp & 0xFFF0) == 0xF020 && m->maxed && m->act == h)
            m->maxed = 0;                       /* SC_MINIMIZE           */
        if (m && (wp & 0xFFF0) == SC_NEXTWINDOW) {
            send_msg(m->client, WM_MDINEXT, h, 0);
            return 0;
        }
        break;
    case WM_CHILDACTIVATE:
    case WM_SETFOCUS:
        if (m && m->act != h)
            mdi_act(m, h);
        break;
    case 0x000C:                                /* WM_SETTEXT            */
        {
            LONG r = DefWindowProc(h, msg, wp, lp);
            if (m)
                mdi_menu(m);
            return r;
        }
    }
    return DefWindowProc(h, msg, wp, lp);
}

LONG API TranslateMDISysAccel(HWND client, const MSG FAR *msg)
{
    (void)client; (void)msg;
    return 0;
}
