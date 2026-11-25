/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Kombinationsfeld COMBOBOX (Windows 3.1): CBS_SIMPLE (Feld mit
 * Liste darunter), CBS_DROPDOWN (Feld + Klappliste), CBS_DROPDOWNLIST (nur
 * Auswahl + Klappliste). Die Eintraege haelt immer ein LISTBOX-Fenster:
 * bei SIMPLE ein Kind, sonst ein verborgenes Popup, das beim Aufklappen
 * unter dem Feld erscheint; verfolgt wird es wie ein Menue (menu.c
 * track_on/track_off, Maus und Tasten kommen als MT_MOUSE/MT_KEY).
 * CB_-Nachrichten wie in Win16 (WM_USER + n), Meldungen CBN_* an den
 * Elternteil. Eigener Entwurf nach der Beschreibung im Windows-3.1-SDK.
 */
#include "gui.h"

#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_SIZE         0x0005
#define WM_SETFOCUS     0x0007
#define WM_KILLFOCUS    0x0008
#define WM_ENABLE       0x000A
#define WM_SETTEXT      0x000C
#define WM_GETTEXT      0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT        0x000F
#define WM_ERASEBKGND   0x0014
#define WM_GETDLGCODE   0x0087
#define WM_KEYDOWN      0x0100
#define WM_CHAR         0x0102
#define WM_SYSKEYDOWN   0x0104
#define WM_COMMAND      0x0111
#define WM_VSCROLL      0x0115
#define WM_LBUTTONDOWN  0x0201
#define WS_CHILD        0x40000000UL
#define WS_POPUP        0x80000000UL
#define WS_VISIBLE      0x10000000UL
#define WS_BORDER       0x00800000UL
#define WS_VSCROLL      0x00200000UL
#define ES_AUTOHSCROLL  0x0080
#define LBS_NOTIFY      0x0001
#define LBS_SORT        0x0002
#define CBS_SORT        0x0100
#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTCHARS  0x0080

#define CBN_SELCHANGE   1
#define CBN_DBLCLK      2
#define CBN_SETFOCUS    3
#define CBN_KILLFOCUS   4
#define CBN_EDITCHANGE  5
#define CBN_EDITUPDATE  6
#define CBN_DROPDOWN    7
#define CBN_CLOSEUP     8
#define CBN_SELENDOK    9
#define CBN_SELENDCANCEL 10

#define ID_ED   1001
#define ID_LB   1000
#define FLD_H   (FONT_H + 6)                    /* Hoehe des Feldes       */
#define BTN_W   SBW

void API InvalidateRect(HWND h, const RECT FAR *r, BOOL e);
BOOL API ShowWindow(HWND h, short cmd);
BOOL API MoveWindow(HWND h, short x, short y, short cx, short cy, BOOL rep);
BOOL API SetWindowText(HWND h, LPCSTR s);
short API GetWindowText(HWND h, LPSTR s, short n);
DWORD API GetTickCount(void);
void API PostQuitMessage(short code);
struct paintst { HDC hdc; BOOL erase; RECT rc; BOOL res, inc; BYTE rgb[16]; };

#define NCB 12
static struct cb {
    HWND h, ed, lb;
    BYTE kind, drop;                            /* 1 SIMPLE 2 DROPDOWN 3 LIST */
    short droph;                                /* Hoehe der Klappliste   */
} cbt[NCB];

static struct cb *cbp(HWND h)
{
    short i;
    for (i = 0; i < NCB; i++)
        if (cbt[i].h == h && h)
            return &cbt[i];
    return 0;
}

static void notify(struct wnd *w, WORD code)
{
    post_msg(w->parent, WM_COMMAND, w->id, MAKELONG(HW(w - ww), code));
}

static short cursel(struct cb *c) { return (short)send_msg(c->lb, 0x0409, 0, 0); }

/* Text der Auswahl ins Feld (DROPDOWN/SIMPLE) bzw. neu zeichnen */
static void sel_to_field(struct cb *c)
{
    short s = cursel(c);
    static char t[260];
    if (c->ed) {
        t[0] = 0;
        if (s >= 0)
            send_msg(c->lb, 0x040A, s, (DWORD)(void FAR *)t);
        SetWindowText(c->ed, t);
        send_msg(c->ed, 0x0401, 0, MAKELONG(0, 0x7FFF));  /* EM_SETSEL */
    }
    InvalidateRect(c->h, 0, 0);
}

static void set_sel(struct cb *c, struct wnd *w, short s, int tell)
{
    if (s == cursel(c))
        return;
    send_msg(c->lb, 0x0407, s, 0);              /* LB_SETCURSEL           */
    sel_to_field(c);
    if (tell)
        notify(w, CBN_SELCHANGE);
}

/* Klappliste verfolgen; mouse: Taste ist beim Aufklappen gedrueckt */
static void drop(struct cb *c, struct wnd *w, int mouse)
{
    static MSG msg;
    struct wnd *lw = wptr(c->lb);
    short x = w->wr.left, y = w->wr.bottom, cx = w->wr.right - w->wr.left;
    short cy = c->droph, old = cursel(c), n = (short)send_msg(c->lb, 0x040C, 0, 0);
    BYTE lbtn = mouse ? 1 : 0, moved = 0, ok = 0;
    if (!lw || c->drop)
        return;
    if (n * FONT_H + 4 < cy)                    /* nicht laenger als noetig */
        cy = (n ? n : 1) * FONT_H + 4;
    if (y + cy > SCR_H)
        y = w->wr.top - cy;
    if (!track_on(c->h))
        return;
    notify(w, CBN_DROPDOWN);
    MoveWindow(c->lb, x, y, cx, cy, 0);
    send_msg(c->lb, 0x0407, old, 0);
    wnd_raise(c->lb);
    ShowWindow(c->lb, 4);
    c->drop = 1;
    InvalidateRect(c->h, 0, 0);
    while (track_busy() && wptr(c->h)) {
        if (!user_get_msg(&msg, 0, 0, 0, 1, 1)) {
            PostQuitMessage(msg.wParam);
            break;
        }
        if (msg.hwnd == c->h && msg.message == MT_MOUSE) {
            short mx = LOWORD(msg.lParam), my = HIWORD(msg.lParam);
            BYTE btn = (BYTE)msg.wParam, dn = btn & ~lbtn, up = lbtn & ~btn;
            lbtn = btn;
            lw = wptr(c->lb);
            if (mx >= lw->wr.left && mx < lw->wr.right && my >= lw->wr.top && my < lw->wr.bottom) {
                if (mx >= lw->cr.right) {       /* Bildlaufleiste         */
                    if (dn) {
                        WORD sb = my < lw->cr.top + SBW ? 0 : my >= lw->cr.bottom - SBW ? 1 :
                                  my < (lw->cr.top + lw->cr.bottom) / 2 ? 2 : 3;
                        send_msg(c->lb, WM_VSCROLL, sb, 0);
                    }
                    continue;
                }
                if (my >= lw->cr.top && my < lw->cr.bottom && (btn || moved || mouse)) {
                    short k = (short)send_msg(c->lb, 0x040F, 0, 0) + (my - lw->cr.top) / FONT_H;
                    if (k < n && k != cursel(c))
                        send_msg(c->lb, 0x0407, k, 0);
                    moved = 1;
                }
                if (up && cursel(c) >= 0) {
                    ok = 1;
                    break;
                }
                continue;
            }
            if (mx >= w->wr.left && mx < w->wr.right && my >= w->wr.top && my < w->wr.bottom) {
                if (dn)                         /* Klick aufs Feld: zu    */
                    break;
                continue;
            }
            if (dn)
                break;
            continue;
        }
        if (msg.hwnd == c->h && msg.message == MT_KEY) {
            BYTE ch = (BYTE)msg.wParam, sc = msg.wParam >> 8;
            static const BYTE scv[][2] = { {0x48, 0x26}, {0x50, 0x28}, {0x49, 0x21},
                                           {0x51, 0x22}, {0x47, 0x24}, {0x4F, 0x23} };
            short i;
            if (sc == 0x01)
                break;
            if (ch == 0x0D || sc == 0x3E) {     /* Enter, F4              */
                ok = 1;
                break;
            }
            for (i = 0; i < 6; i++)
                if (scv[i][0] == sc) {
                    send_msg(c->lb, WM_KEYDOWN, scv[i][1], 0);
                    break;
                }
            if (i == 6 && ch >= ' ')
                send_msg(c->lb, WM_CHAR, ch, 0);
            continue;
        }
        DispatchMessage(&msg);
    }
    track_off();
    c->drop = 0;
    if (wptr(c->lb))
        ShowWindow(c->lb, 0);
    if (!wptr(c->h))
        return;
    if (!ok)
        send_msg(c->lb, 0x0407, old, 0);
    notify(w, CBN_CLOSEUP);
    if (ok && cursel(c) != old) {
        sel_to_field(c);
        notify(w, CBN_SELCHANGE);
    }
    notify(w, ok ? CBN_SELENDOK : CBN_SELENDCANCEL);
    InvalidateRect(c->h, 0, 0);
}

static void tri(HDC dc, short x, short y)       /* Pfeil nach unten       */
{
    short k;
    for (k = 0; k < 4; k++)
        dc_rawfill(dc, x + 4 + k, y + 6 + k, x + 12 - k, y + 7 + k, 0);
}

LONG FAR PASCAL combo_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    struct cb *c = cbp(h);
    if (!w)
        return 0;
    if (m == WM_CREATE) {
        short i, cx = w->wr.right - w->wr.left, cy = w->wr.bottom - w->wr.top;
        DWORD ls = LBS_NOTIFY | WS_VSCROLL | WS_BORDER | ((w->style & CBS_SORT) ? LBS_SORT : 0);
        for (i = 0; i < NCB && cbt[i].h; i++)
            ;
        if (i == NCB)
            return -1;
        c = &cbt[i];
        c->h = h;
        c->kind = (BYTE)(w->style & 3);
        if (!c->kind)
            c->kind = 1;
        c->drop = 0;
        c->droph = cy - FLD_H > 2 * FONT_H ? cy - FLD_H : 6 * FONT_H + 4;
        w->style &= ~(WS_BORDER | WS_VSCROLL);
        if (c->kind != 3) {
            c->ed = CreateWindowEx(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                   0, 0, cx - (c->kind == 1 ? 0 : BTN_W), FLD_H, h, ID_ED, w->hinst, 0);
            if (c->ed) wptr(c->ed)->task = w->task;
        } else
            c->ed = 0;
        if (c->kind == 1)
            c->lb = CreateWindowEx(0, "LISTBOX", "", WS_CHILD | WS_VISIBLE | ls,
                                   0, FLD_H + 2, cx, cy - FLD_H - 2, h, ID_LB, w->hinst, 0);
        else
            c->lb = CreateWindowEx(0, "LISTBOX", "", WS_POPUP | ls,
                                   0, 0, cx, c->droph, h, 0, w->hinst, 0);  /* 0: kein Menue */
        if (!c->lb) {
            c->h = 0;
            return -1;
        }
        wptr(c->lb)->task = w->task;
        if (c->kind != 1) {                     /* Feld so hoch wie Text  */
            w->wr.bottom = w->wr.top + FLD_H;
            w->cr.bottom = w->cr.top + FLD_H;
        }
        return 0;
    }
    if (!c)
        return DefWindowProc(h, m, wp, lp);
    switch (m) {
    case WM_DESTROY:
        if (c->kind != 1 && wptr(c->lb))
            DestroyWindow(c->lb);               /* Popup gehoert uns      */
        c->h = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        static struct paintst ps;
        static char t[260];
        HDC dc = BeginPaint(h, (void FAR *)&ps);
        short l = w->cr.left, tp = w->cr.top, r = w->cr.right, b = tp + FLD_H;
        if (c->kind == 1) {
            EndPaint(h, (void FAR *)&ps);
            return 0;
        }
        if (c->kind == 3) {
            short s = cursel(c), n = 0, mx = (r - BTN_W - l - 6) / FONT_W;
            int foc = GetFocus() == h && !c->drop;
            dc_rawfill(dc, l, tp, r - BTN_W, b, 15);
            dc_rawfill(dc, l, tp, r - BTN_W, tp + 1, 0);
            dc_rawfill(dc, l, b - 1, r - BTN_W, b, 0);
            dc_rawfill(dc, l, tp, l + 1, b, 0);
            t[0] = 0;
            if (s >= 0)
                send_msg(c->lb, 0x040A, s, (DWORD)(void FAR *)t);
            n = (short)fstrlen(t);
            if (n > mx) n = mx;
            if (foc)
                dc_rawfill(dc, l + 2, tp + 2, r - BTN_W - 1, b - 2, 4);
            dc_rawtext(dc, l + 3, tp + 3, t, n, foc ? 15 : (w->enabled ? 0 : 8));
        }
        ui_bevel(dc, r - BTN_W, tp, r, b, c->drop);
        tri(dc, r - BTN_W, tp + 3);
        EndPaint(h, (void FAR *)&ps);
        return 0;
    }
    case WM_SETFOCUS:
        if (c->ed)
            SetFocus(c->ed);
        else
            InvalidateRect(h, 0, 0);
        notify(w, CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(h, 0, 0);
        notify(w, CBN_KILLFOCUS);
        return 0;
    case WM_ENABLE:
        if (c->ed) EnableWindow(c->ed, wp);
        EnableWindow(c->lb, wp);
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN:
        if (GetFocus() != h && (!c->ed || GetFocus() != c->ed))
            SetFocus(h);
        if (c->kind == 3 || (c->kind == 2 && (short)LOWORD(lp) >= w->cr.right - w->cr.left - BTN_W))
            drop(c, w, 1);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == 0x28 || wp == 0x26) {         /* Alt+Pfeil              */
            if (c->kind != 1) drop(c, w, 0);
            return 0;
        }
        return DefWindowProc(h, m, wp, lp);
    case WM_KEYDOWN: {
        short s = cursel(c), n = (short)send_msg(c->lb, 0x040C, 0, 0);
        if (wp == 0x73 && c->kind != 1) {       /* F4                     */
            drop(c, w, 0);
            return 0;
        }
        switch (wp) {
        case 0x26: case 0x25: s = s > 0 ? s - 1 : 0; break;
        case 0x28: case 0x27: s = s + 1 < n ? s + 1 : n - 1; break;
        case 0x24: s = 0; break;
        case 0x23: s = n - 1; break;
        default: return 0;
        }
        if (n)
            set_sel(c, w, s, 1);
        return 0;
    }
    case WM_CHAR:
        if (c->kind == 3 && (BYTE)wp >= ' ') {
            short o = cursel(c);
            send_msg(c->lb, WM_CHAR, wp, 0);    /* Anfangsbuchstabe       */
            if (cursel(c) != o) {
                sel_to_field(c);
                notify(w, CBN_SELCHANGE);
            }
        }
        return 0;
    case WM_COMMAND:                            /* vom Feld oder der Liste */
        if (wp == ID_ED) {
            if (HIWORD(lp) == 0x0300) notify(w, CBN_EDITCHANGE);
            if (HIWORD(lp) == 0x0400) notify(w, CBN_EDITUPDATE);
            if (HIWORD(lp) == 0x0200) notify(w, CBN_KILLFOCUS);
        } else if (wp == ID_LB && c->kind == 1) {
            if (HIWORD(lp) == 1) { sel_to_field(c); notify(w, CBN_SELCHANGE); }
            if (HIWORD(lp) == 2) notify(w, CBN_DBLCLK);
        }
        return 0;
    case WM_SETTEXT:
        if (c->ed)
            return SetWindowText(c->ed, (LPCSTR)lp);
        {                                       /* LIST: passenden waehlen */
            short k = (short)send_msg(c->lb, 0x040D, (WORD)-1, lp);
            InvalidateRect(h, 0, 0);
            return k >= 0;
        }
    case WM_GETTEXT:
        if (c->ed)
            return GetWindowText(c->ed, (LPSTR)lp, wp);
        if (cursel(c) < 0 || !wp) {
            if (wp) *(LPSTR)lp = 0;
            return 0;
        }
        {
            static char t[260];
            send_msg(c->lb, 0x040A, cursel(c), (DWORD)(void FAR *)t);
            fstrncpy((LPSTR)lp, t, wp);
            ((LPSTR)lp)[wp - 1] = 0;
            return fstrlen((LPSTR)lp);
        }
    case WM_GETTEXTLENGTH:
        if (c->ed)
            return send_msg(c->ed, m, 0, 0);
        return cursel(c) < 0 ? 0 : send_msg(c->lb, 0x040B, cursel(c), 0);

    /* ---- CB_* (Win16: WM_USER + n) ---- */
    case 0x0400: return c->ed ? send_msg(c->ed, 0x0400, 0, 0) : -1;     /* GETEDITSEL   */
    case 0x0401: return c->ed ? send_msg(c->ed, 0x0415, wp, 0), 1 : -1; /* LIMITTEXT    */
    case 0x0402: return c->ed ? send_msg(c->ed, 0x0401, 0, lp), 1 : -1; /* SETEDITSEL   */
    case 0x0403: return send_msg(c->lb, 0x0401, 0, lp);                  /* ADDSTRING    */
    case 0x0404: return send_msg(c->lb, 0x0403, wp, 0);                  /* DELETESTRING */
    case 0x0405: return send_msg(c->lb, 0x040E, wp, lp);                 /* DIR          */
    case 0x0406: return send_msg(c->lb, 0x040C, 0, 0);                   /* GETCOUNT     */
    case 0x0407: return cursel(c);                                       /* GETCURSEL    */
    case 0x0408: return send_msg(c->lb, 0x040A, wp, lp);                 /* GETLBTEXT    */
    case 0x0409: return send_msg(c->lb, 0x040B, wp, 0);                  /* GETLBTEXTLEN */
    case 0x040A: return send_msg(c->lb, 0x0402, wp, lp);                 /* INSERTSTRING */
    case 0x040B:                                                         /* RESETCONTENT */
        send_msg(c->lb, 0x0405, 0, 0);
        if (c->ed) SetWindowText(c->ed, "");
        InvalidateRect(h, 0, 0);
        return 0;
    case 0x040C: return send_msg(c->lb, 0x0410, wp, lp);                 /* FINDSTRING   */
    case 0x0418: return send_msg(c->lb, 0x0423, wp, lp);                 /* FINDSTRINGEXACT */
    case 0x040D: {                                                       /* SELECTSTRING */
        short k = (short)send_msg(c->lb, 0x0410, wp, lp);
        if (k >= 0) {
            send_msg(c->lb, 0x0407, k, 0);
            sel_to_field(c);
        }
        return k;
    }
    case 0x040E:                                                         /* SETCURSEL    */
        send_msg(c->lb, 0x0407, wp, 0);
        sel_to_field(c);
        return cursel(c) == (short)wp ? (LONG)(short)wp : -1L;
    case 0x040F:                                                         /* SHOWDROPDOWN */
        if (wp && c->kind != 1) drop(c, w, 0);
        return 1;
    case 0x0410: return send_msg(c->lb, 0x041A, wp, 0);                  /* GETITEMDATA  */
    case 0x0411: return send_msg(c->lb, 0x041B, wp, lp);                 /* SETITEMDATA  */
    case 0x0412: {                                                       /* GETDROPPEDCONTROLRECT */
        RECT FAR *r = (RECT FAR *)lp;
        r->left = w->wr.left; r->right = w->wr.right;
        r->top = w->wr.top; r->bottom = w->wr.bottom + (c->kind == 1 ? 0 : c->droph);
        return 1;
    }
    case 0x0413: return 0;                                               /* SETITEMHEIGHT */
    case 0x0414: return FONT_H;                                          /* GETITEMHEIGHT */
    case 0x0415: case 0x0416: return 0;                                  /* EXTENDEDUI   */
    case 0x0417: return c->drop;                                         /* GETDROPPEDSTATE */
    }
    return DefWindowProc(h, m, wp, lp);
}
