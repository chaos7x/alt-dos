/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Dialoge (USER): Vorlagen aus Ressourcen (RT_DIALOG),
 * DialogBox/CreateDialog, IsDialogMessage (Tab, Enter, Esc, Alt+Buchstabe),
 * die eingebauten Klassen BUTTON (Druckknopf, Kontrollkaestchen, Options-
 * feld, Gruppenrahmen), STATIC und LISTBOX. Aussehen eigener Entwurf.
 *
 * Dialogeinheiten: Grundschrift 8 x 16 -> waagerecht und senkrecht je
 * Einheit 2 Bildpunkte. Ein Dialog ist ein Fenster der Klasse #32770 mit
 * einem Eintrag in dt[]: Prozedur des Programms (oder eine der GUI selbst,
 * z. B. COMMDLG), Ergebnis, beendet.
 */
#include "gui.h"

#define WM_INITDIALOG   0x0110
#define WM_COMMAND      0x0111
#define WM_CLOSE        0x0010
#define WM_SETFOCUS     0x0007
#define WM_KILLFOCUS    0x0008
#define WM_PAINT        0x000F
#define WM_ERASEBKGND   0x0014
#define WM_SETTEXT      0x000C
#define WM_ENABLE       0x000A
#define WM_GETDLGCODE   0x0087
#define WM_KEYDOWN      0x0100
#define WM_CHAR         0x0102
#define WM_SYSCHAR      0x0106
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_MOUSEMOVE    0x0200
#define WM_VSCROLL      0x0115
#define WM_DESTROY      0x0002

#define WS_CHILD        0x40000000UL
#define WS_POPUP        0x80000000UL
#define WS_VISIBLE      0x10000000UL
#define WS_DISABLED     0x08000000UL
#define WS_CAPTION      0x00C00000UL
#define WS_BORDER       0x00800000UL
#define WS_DLGFRAME     0x00400000UL
#define WS_SYSMENU      0x00080000UL
#define WS_VSCROLL      0x00200000UL
#define WS_GROUP        0x00020000UL
#define WS_TABSTOP      0x00010000UL
#define DS_SETFONT      0x0040
#define DS_MODALFRAME   0x0080

#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTTAB    0x0002
#define DLGC_WANTALLKEYS 0x0004
#define DLGC_WANTMESSAGE 0x0004
#define DLGC_HASSETSEL  0x0008
#define DLGC_DEFPUSH    0x0010
#define DLGC_UNDEFPUSH  0x0020
#define DLGC_RADIO      0x0040
#define DLGC_WANTCHARS  0x0080
#define DLGC_STATIC     0x0100
#define DLGC_BUTTON     0x2000

#define IDOK     1
#define IDCANCEL 2

struct paintst { HDC hdc; BOOL erase; RECT rc; BOOL res, inc; BYTE rgb[16]; };
HWND API SetCapture(HWND h);
void API ReleaseCapture(void);
DWORD API GetTickCount(void);
short API GetKeyState(short vk);
void API GetClientRect(HWND h, LPRECT r);
short API SetScrollPos(HWND h, short bar, short pos, BOOL redraw);
void sb_setrange(HWND h, short bar, short lo, short hi, BOOL redraw);
BOOL API ShowWindow(HWND h, short cmd);
BOOL API IsWindowEnabled(HWND h);
BOOL API IsWindowVisible(HWND h);
BOOL API TranslateMessage(const MSG FAR *m);
HWND API SetActiveWindow(HWND h);

/* ---- Dialogtabelle ---- */
#define NDLG 12
static struct dlg {
    HWND   h;
    DWORD  proc;                /* Programm: Fernzeiger; GUI: IDLGPROC */
    WORD   hinst;
    BYTE   internal, ended;
    short  result;
} dt[NDLG];

static struct dlg *dptr_(HWND h)
{
    short i;
    for (i = 0; i < NDLG; i++)
        if (dt[i].h == h && h)
            return &dt[i];
    return 0;
}

static WORD dq[5];
static LONG call_dlgproc(struct dlg *d, WORD m, WORD wp, DWORD lp)
{
    if (!d->proc)
        return 0;
    if (d->internal)
        return ((IDLGPROC)d->proc)(d->h, m, wp, lp);
    dq[0] = d->h; dq[1] = m; dq[2] = wp; dq[3] = HIWORD(lp); dq[4] = LOWORD(lp);
    return (short)call16((void FAR *)d->proc, d->hinst, 5, dq);
}

/* Steuerelemente eines Dialogs in Reihenfolge */
static HWND child_n(HWND d, short n)
{
    short i, k = 0;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && (ww[i].style & WS_CHILD) && ww[i].parent == d && k++ == n)
            return HW(i);
    return 0;
}
static short child_idx(HWND d, HWND c)
{
    short i, k = 0;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && (ww[i].style & WS_CHILD) && ww[i].parent == d) {
            if (HW(i) == c)
                return k;
            k++;
        }
    return -1;
}
static short child_cnt(HWND d)
{
    short i, k = 0;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && (ww[i].style & WS_CHILD) && ww[i].parent == d)
            k++;
    return k;
}

HWND API GetNextDlgTabItem(HWND d, HWND c, BOOL prev)
{
    short n = child_cnt(d), i = child_idx(d, c), k;
    if (n <= 0)
        return 0;
    if (i < 0)
        i = prev ? 0 : n - 1;
    for (k = 0; k < n; k++) {
        HWND x;
        struct wnd *w;
        i = prev ? (i + n - 1) % n : (i + 1) % n;
        x = child_n(d, i);
        w = wptr(x);
        if (w && w->visible && w->enabled && (w->style & WS_TABSTOP))
            return x;
    }
    return c;
}

static HWND def_button(HWND d)
{
    short i;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && ww[i].parent == d && ww[i].cls == 1 &&
            (ww[i].style & 0x0F) == 1)
            return HW(i);
    return 0;
}

static void dlg_cmd(HWND d, WORD id, HWND c)
{
    post_msg(d, WM_COMMAND, id, MAKELONG(c, 0));
}

static BYTE mnem_of(LPCSTR s)
{
    for (; *s; s++)
        if (*s == '&' && s[1] && s[1] != '&') {
            BYTE c = s[1];
            return (BYTE)(c >= 'a' && c <= 'z' ? c - 32 : c);
        } else if (*s == '&')
            s++;
    return 0;
}

/* Kind von d, in dem h steckt (Feld einer COMBOBOX -> die COMBOBOX) */
static HWND dlg_kid(HWND d, HWND h)
{
    struct wnd *w = wptr(h);
    while (w && (w->style & WS_CHILD) && w->parent != d)
        w = wptr(w->parent);
    return w && (w->style & WS_CHILD) && w->parent == d ? HW(w - ww) : 0;
}

/* naechstes/voriges Element derselben Gruppe (WS_GROUP beginnt eine),
   im Kreis, nur sichtbare und bediente (wie Windows) */
HWND API GetNextDlgGroupItem(HWND d, HWND c, BOOL prev)
{
    short n = child_cnt(d), me = child_idx(d, c), a, b, i, k;
    if (me < 0 || n <= 0)
        return c;
    for (a = me; a > 0; a--) {
        struct wnd *w = wptr(child_n(d, a));
        if (w && (w->style & WS_GROUP)) break;
    }
    for (b = me + 1; b < n; b++) {
        struct wnd *w = wptr(child_n(d, b));
        if (w && (w->style & WS_GROUP)) break;
    }
    for (k = 1, i = me; k < b - a; k++) {
        struct wnd *w;
        i = prev ? (i > a ? i - 1 : b - 1) : (i + 1 < b ? i + 1 : a);
        w = wptr(child_n(d, i));
        if (w && w->visible && w->enabled)
            return child_n(d, i);
    }
    return c;
}

BOOL API IsDialogMessage(HWND d, MSG FAR *m)
{
    struct wnd *dw = wptr(d), *t = wptr(m->hwnd);
    if (!dw || !t || (m->hwnd != d && !dlg_kid(d, m->hwnd)))
        return 0;
    if (m->message == WM_KEYDOWN || m->message == WM_CHAR) {
        HWND f = GetFocus(), fk = dlg_kid(d, f);
        WORD code = f ? (WORD)send_msg(f, WM_GETDLGCODE, 0, 0) : 0;
        BYTE k = (BYTE)m->wParam;
        if (k == 0x09 && !(code & (DLGC_WANTTAB | DLGC_WANTALLKEYS))) {
            if (m->message == WM_KEYDOWN) {
                HWND n = GetNextDlgTabItem(d, fk ? fk : f, GetKeyState(0x10) < 0);
                if (n) {
                    SetFocus(n);
                    if (wptr(n) && (WORD)send_msg(n, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
                        send_msg(n, 0x0401, 0, MAKELONG(0, 0x7FFF));
                }
            }
            return 1;
        }
        if (k == 0x0D && !(code & DLGC_WANTALLKEYS)) {
            if (m->message == WM_KEYDOWN) {
                struct wnd *fw = wptr(f);
                if (fw && fw->cls == 1 && (fw->style & 0x0F) <= 1)
                    dlg_cmd(d, fw->id, f);
                else {
                    HWND b = def_button(d);
                    /* wie Windows: gesperrter Standardknopf -> nichts */
                    if (!b || IsWindowEnabled(b))
                        dlg_cmd(d, b ? wptr(b)->id : IDOK, b);
                }
            }
            return 1;
        }
        if (k == 0x1B) {
            if (m->message == WM_KEYDOWN)
                dlg_cmd(d, IDCANCEL, GetDlgItem(d, IDCANCEL));
            return 1;
        }
        /* Pfeile: innerhalb der Gruppe weiter, Optionsfelder werden dabei
           gewaehlt (wie Windows) */
        if (m->message == WM_KEYDOWN && k >= 0x25 && k <= 0x28 && fk &&
            !(code & (DLGC_WANTARROWS | DLGC_WANTALLKEYS))) {
            HWND n = GetNextDlgGroupItem(d, fk, k == 0x25 || k == 0x26);
            if (n && n != fk) {
                SetFocus(n);
                if ((WORD)send_msg(n, WM_GETDLGCODE, 0, 0) & DLGC_RADIO)
                    send_msg(n, WM_KEYDOWN, 0x20, 0);
            }
            return 1;
        }
    }
    if (m->message == WM_SYSCHAR) {             /* Alt+Buchstabe         */
        BYTE u = (BYTE)m->wParam;
        short i, n = child_cnt(d);
        if (u >= 'a' && u <= 'z') u -= 32;
        for (i = 0; i < n; i++) {
            HWND c = child_n(d, i);
            struct wnd *cw = wptr(c);
            if (cw && cw->visible && cw->enabled && mnem_of(cw->title) == u) {
                if (cw->cls == 2) {             /* STATIC: naechstes     */
                    HWND nx = GetNextDlgTabItem(d, c, 0);
                    if (nx) SetFocus(nx);
                } else if (cw->cls == 1 && (cw->style & 0x0F) != 7) {
                    SetFocus(c);
                    send_msg(c, WM_KEYDOWN, 0x20, 0);
                } else
                    SetFocus(c);
                return 1;
            }
        }
        return 1;
    }
    TranslateMessage(m);
    DispatchMessage(m);
    return 1;
}

/* Fensterprozedur der Dialoge */
LONG FAR PASCAL dlg_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    struct dlg *d = dptr_(h);
    LONG r;
    if (!w)
        return 0;
    if (d && m != WM_DESTROY) {
        r = call_dlgproc(d, m, wp, lp);
        if (r && m != WM_PAINT && m != WM_ERASEBKGND)
            return m == WM_INITDIALOG || m == WM_GETDLGCODE ? r : *(LONG *)w->extra;
        if (r && m == WM_ERASEBKGND)
            return 1;
    }
    switch (m) {
    case WM_ERASEBKGND:
        dc_rawfill((HDC)wp, w->cr.left, w->cr.top, w->cr.right, w->cr.bottom, 15);
        return 1;
    case WM_CLOSE:
        post_msg(h, WM_COMMAND, IDCANCEL, MAKELONG(GetDlgItem(h, IDCANCEL), 0));
        return 0;
    case WM_SETFOCUS: {
        HWND c = GetNextDlgTabItem(h, 0, 0);
        if (c) SetFocus(c);
        return 0;
    }
    case WM_DESTROY:
        if (d) {
            call_dlgproc(d, m, wp, lp);
            d->h = 0;
        }
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

static struct dlg *dnew(HWND h, DWORD proc, WORD hinst, BYTE internal)
{
    short i;
    for (i = 0; i < NDLG && dt[i].h; i++)
        ;
    if (i == NDLG)
        return 0;
    dt[i].h = h; dt[i].proc = proc; dt[i].hinst = hinst;
    dt[i].internal = internal; dt[i].ended = 0; dt[i].result = 0;
    return &dt[i];
}

static void place(HWND owner, short FAR *x, short FAR *y, short cx, short cy, int center)
{
    struct wnd *o = wptr(owner);
    if (center || !o) {
        *x = (SCR_W - cx) / 2;
        *y = (SCR_H - cy) / 2;
    } else {
        *x += o->cr.left;
        *y += o->cr.top;
    }
    if (*x + cx > SCR_W) *x = SCR_W - cx;
    if (*y + cy > SCR_H) *y = SCR_H - cy;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

/* Vorlage auswerten und anlegen */
static LPCSTR tstr(BYTE FAR * FAR *pp)
{
    BYTE FAR *p = *pp;
    LPCSTR s = (LPCSTR)p;
    if (*p == 0xFF) {                           /* Ordinalzahl           */
        *pp = p + 3;
        return MKFP(0, *(WORD FAR *)(p + 1));
    }
    while (*p) p++;
    *pp = p + 1;
    return s;
}

static const char *pcls[] = { "BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX" };

static HWND dlg_from(WORD hinst, BYTE FAR *p, HWND owner, DWORD proc, DWORD param, BYTE intl)
{
    DWORD st = *(DWORD FAR *)p;
    BYTE n = p[4], i;
    short x = *(short FAR *)(p + 5) * 2, y = *(short FAR *)(p + 7) * 2;
    short cx = *(short FAR *)(p + 9) * 2, cy = *(short FAR *)(p + 11) * 2, wx, wy;
    LPCSTR title;
    HWND h, f;
    struct wnd *w;
    struct dlg *d;
    p += 13;
    tstr(&p);                                   /* Menue                 */
    tstr(&p);                                   /* Klasse                */
    title = tstr(&p);
    if (st & DS_SETFONT) {
        p += 2;
        tstr(&p);
    }
    {
        int cap = (st & WS_CAPTION) == WS_CAPTION;
        wx = cx + 6;
        wy = cy + 6 + (cap ? CAPH : 0);
        place(owner, &x, &y, wx, wy, 0);
        st &= ~(WS_VISIBLE | WS_CHILD | WS_BORDER | 0xFFFFUL);
        st |= WS_POPUP | WS_DLGFRAME | (cap ? WS_BORDER : 0);
    }
    h = CreateWindowEx(0, MKFP(0, 0xC000 + 5), SELOF(title) ? title : "",
                       st, x, y, wx, wy, owner, 0, hinst, 0);
    w = wptr(h);
    if (!w)
        return 0;
    w->hinst = hinst;
    d = dnew(h, proc, hinst, intl);
    if (!d) {
        DestroyWindow(h);
        return 0;
    }
    for (i = 0; i < n; i++) {
        short ix = *(short FAR *)p * 2, iy = *(short FAR *)(p + 2) * 2;
        short icx = *(short FAR *)(p + 4) * 2, icy = *(short FAR *)(p + 6) * 2;
        WORD id = *(WORD FAR *)(p + 8);
        DWORD ist = *(DWORD FAR *)(p + 10);
        LPCSTR cls, txt;
        HWND c;
        p += 14;
        if (*p >= 0x80) {
            cls = (*p - 0x80) < 6 ? pcls[*p - 0x80] : "STATIC";
            p++;
        } else
            cls = tstr(&p);
        txt = tstr(&p);
        p += 1 + *p;                            /* Zusatzdaten           */
        c = CreateWindowEx(0, cls, SELOF(txt) ? txt : "", ist | WS_CHILD,
                           ix, iy, icx, icy, h, id, hinst, 0);
        if (c && !(ist & WS_VISIBLE))
            ShowWindow(c, 0);
        if (c)
            wptr(c)->task = w->task;
    }
    f = GetNextDlgTabItem(h, 0, 0);
    if (call_dlgproc(d, WM_INITDIALOG, f, param) && f)
        SetFocus(f);
    return h;
}

WORD load_tpl(WORD hinst, LPCSTR name)
{
    int t = task_of_inst(hinst), m = t >= 0 ? wt[t].hmod - 1 : -1;
    DWORD r;
    if (hinst > 0 && hinst <= NMOD)
        m = hinst - 1;
    r = w16_res_find(m, MKFP(0, 5), name);
    return r ? w16_res_load(m, r) : 0;
}

/* interner Dialog aus einer Vorlage (COMMDLG): proc ist eine GUI-Funktion */
HWND dlg_tpl(const BYTE FAR *tpl, WORD hinst, HWND owner, IDLGPROC proc, DWORD param)
{
    return dlg_from(hinst, (BYTE FAR *)tpl, owner, (DWORD)proc, param, 1);
}

short dlg_run(HWND h, HWND owner)
{
    static MSG msg;
    struct dlg *d = dptr_(h);
    struct wnd *ow = wptr(owner);
    short r;
    if (!d)
        return -1;
    if (ow)
        EnableWindow(owner, 0);
    ShowWindow(h, 1);
    if (!GetFocus() || !wptr(GetFocus()) || wptr(GetFocus())->parent != h) {
        HWND f = GetNextDlgTabItem(h, 0, 0);
        if (f) SetFocus(f);
    }
    while (!d->ended && wptr(h)) {
        if (cur_task >= 0) {
            if (!user_get_msg(&msg, 0, 0, 0, 1, 1)) {
                PostQuitMessage(msg.wParam);
                break;
            }
            if (!IsDialogMessage(h, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        } else
            break;
    }
    r = d->result;
    if (ow)
        EnableWindow(owner, 1);
    DestroyWindow(h);
    if (ow)
        SetActiveWindow(owner);
    return r;
}

short API DialogBoxParam(WORD hinst, LPCSTR name, HWND owner, DWORD proc, DWORD param)
{
    WORD sel = load_tpl(hinst, name);
    HWND h;
    if (!sel) {
        glogs("Dialog fehlt: ", SELOF(name) ? name : "(Zahl)");
        return -1;
    }
    h = dlg_from(hinst, MKFP(sel, 0), owner, proc, param, 0);
    seg_del(sel);
    return h ? dlg_run(h, owner) : -1;
}
short API DialogBox(WORD hinst, LPCSTR name, HWND owner, DWORD proc)
{
    return DialogBoxParam(hinst, name, owner, proc, 0);
}
short API DialogBoxIndirectParam(WORD hinst, WORD tpl, HWND owner, DWORD proc, DWORD param)
{
    HWND h = dlg_from(hinst, MKFP(tpl, 0), owner, proc, param, 0);
    return h ? dlg_run(h, owner) : -1;
}
short API DialogBoxIndirect(WORD hinst, WORD tpl, HWND owner, DWORD proc)
{
    return DialogBoxIndirectParam(hinst, tpl, owner, proc, 0);
}
HWND API CreateDialogParam(WORD hinst, LPCSTR name, HWND owner, DWORD proc, DWORD param)
{
    WORD sel = load_tpl(hinst, name);
    HWND h;
    if (!sel)
        return 0;
    h = dlg_from(hinst, MKFP(sel, 0), owner, proc, param, 0);
    seg_del(sel);
    if (h && (wptr(h)->style & WS_VISIBLE))
        ShowWindow(h, 1);
    return h;
}
HWND API CreateDialog(WORD hinst, LPCSTR name, HWND owner, DWORD proc)
{
    return CreateDialogParam(hinst, name, owner, proc, 0);
}
HWND API CreateDialogIndirect(WORD hinst, LPCSTR tpl, HWND owner, DWORD proc)
{
    return dlg_from(hinst, (BYTE FAR *)tpl, owner, proc, 0, 0);
}

BOOL API EndDialog(HWND h, short r)
{
    struct dlg *d = dptr_(h);
    if (!d)
        return 0;
    d->ended = 1;
    d->result = r;
    return 1;
}
LONG API DefDlgProc(HWND h, WORD m, WORD wp, DWORD lp) { return DefWindowProc(h, m, wp, lp); }
void API MapDialogRect(HWND h, LPRECT r)
{
    (void)h;
    r->left *= 2; r->right *= 2; r->top *= 2; r->bottom *= 2;
}
DWORD API GetDialogBaseUnits(void) { return MAKELONG(8, 16); }

/* interne Dialoge der GUI (COMMDLG) */
HWND dlg_new(HWND owner, LPCSTR title, short x, short y, short cx, short cy,
             IDLGPROC proc, WORD hinst)
{
    HWND h;
    struct dlg *d;
    short wx = cx + 6, wy = cy + 6 + CAPH;
    place(owner, &x, &y, wx, wy, x < 0);
    h = CreateWindowEx(0, MKFP(0, 0xC000 + 5), title,
                       WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_DLGFRAME,
                       x, y, wx, wy, owner, 0, hinst, 0);
    if (!h)
        return 0;
    wptr(h)->hinst = hinst;
    d = dnew(h, (DWORD)proc, hinst, 1);
    if (!d) {
        DestroyWindow(h);
        return 0;
    }
    return h;
}
HWND dlg_item(HWND d, LPCSTR cls, LPCSTR text, DWORD style, short x, short y,
              short cx, short cy, WORD id)
{
    struct wnd *w = wptr(d);
    HWND c = CreateWindowEx(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, cx, cy,
                            d, id, w ? w->hinst : 0, 0);
    if (c && w)
        wptr(c)->task = w->task;
    return c;
}

/* ---- Hilfen fuer Steuerelemente im Dialog ---- */
BOOL API SetDlgItemText(HWND d, short id, LPCSTR s) { return SetWindowText(GetDlgItem(d, id), s); }
short API GetDlgItemText(HWND d, short id, LPSTR s, short n) { return GetWindowText(GetDlgItem(d, id), s, n); }
void API SetDlgItemInt(HWND d, short id, WORD v, BOOL sgn)
{
    static char b[8];
    short i = 7;
    BYTE neg = sgn && (short)v < 0;
    if (neg) v = (WORD)-(short)v;
    b[7] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 1);
    if (neg) b[--i] = '-';
    SetDlgItemText(d, id, b + i);
}
WORD API GetDlgItemInt(HWND d, short id, BOOL FAR *ok, BOOL sgn)
{
    static char b[16];
    short i = 0;
    long v = 0;
    BYTE neg = 0, any = 0;
    GetDlgItemText(d, id, b, sizeof(b));
    while (b[i] == ' ') i++;
    if (sgn && b[i] == '-') { neg = 1; i++; }
    for (; b[i] >= '0' && b[i] <= '9'; i++) { v = v * 10 + (b[i] - '0'); any = 1; }
    while (b[i] == ' ') i++;
    if (ok) *ok = any && !b[i] && v <= (sgn ? 32767L : 65535L);
    return (WORD)(neg ? -v : v);
}
BOOL API CheckDlgButton(HWND d, short id, WORD c) { send_msg(GetDlgItem(d, id), 0x0401, c, 0); return 1; }
WORD API IsDlgButtonChecked(HWND d, short id) { return (WORD)send_msg(GetDlgItem(d, id), 0x0400, 0, 0); }
void API CheckRadioButton(HWND d, short a, short b, short c)
{
    short i;
    for (i = a; i <= b; i++)
        send_msg(GetDlgItem(d, i), 0x0401, i == c, 0);
}
short API GetDlgCtrlID(HWND h) { struct wnd *w = wptr(h); return w ? w->id : 0; }

/* ---- BUTTON ---- */
#define BS_TYPE(w)  ((w)->style & 0x0F)
static void frame(HDC dc, short l, short t, short r, short b, BYTE c)
{
    dc_rawfill(dc, l, t, r, t + 1, c);
    dc_rawfill(dc, l, b - 1, r, b, c);
    dc_rawfill(dc, l, t, l + 1, b, c);
    dc_rawfill(dc, r - 1, t, r, b, c);
}
static void mtxt(HDC dc, short x, short y, LPCSTR s, BYTE fg)
{
    static char b[64];
    short n = 0, ul = -1;
    for (; *s && n < 63; s++) {
        if (*s == '&' && s[1] && s[1] != '&') { ul = n; continue; }
        if (*s == '&') s++;
        b[n++] = *s;
    }
    dc_ptext(dc, x, y, b, n, fg);
    if (ul >= 0) {
        short ux = x + gfx_ptextw(b, ul);
        dc_rawfill(dc, ux, y + FONT_H - 2, ux + gfx_ptextw(b + ul, 1) - 1, y + FONT_H - 1, fg);
    }
}
/* Breite in Punkten ohne die &-Zeichen (proportional wie mtxt) */
static short mwid(LPCSTR s)
{
    static char b[64];
    short n = 0;
    for (; *s && n < 63; s++) {
        if (*s == '&' && s[1] && s[1] != '&') continue;
        if (*s == '&') s++;
        b[n++] = *s;
    }
    return gfx_ptextw(b, n);
}

static void radio_group(HWND h)                 /* andere Optionsfelder   */
{
    struct wnd *w = wptr(h);
    short i, me, a, b;
    HWND d = w->parent;
    short n = child_cnt(d);
    me = child_idx(d, h);
    for (a = me; a > 0; a--) {
        struct wnd *c = wptr(child_n(d, a));
        if (c && (c->style & WS_GROUP)) break;
    }
    for (b = me + 1; b < n; b++) {
        struct wnd *c = wptr(child_n(d, b));
        if (c && (c->style & WS_GROUP)) break;
    }
    for (i = a; i < b; i++) {
        HWND c = child_n(d, i);
        struct wnd *cw = wptr(c);
        if (c != h && cw && cw->cls == 1 && (BS_TYPE(cw) == 4 || BS_TYPE(cw) == 9) && cw->extra[1]) {
            cw->extra[1] = 0;
            InvalidateRect(c, 0, 0);
        }
    }
}

static void btn_click(HWND h, struct wnd *w)
{
    switch (BS_TYPE(w)) {
    case 3: w->extra[1] = !w->extra[1]; break;              /* AUTOCHECKBOX */
    case 6: w->extra[1] = (w->extra[1] + 1) % 3; break;     /* AUTO3STATE   */
    case 9: w->extra[1] = 1; radio_group(h); break;         /* AUTORADIO    */
    }
    InvalidateRect(h, 0, 0);
    post_msg(w->parent, WM_COMMAND, w->id, MAKELONG(h, 0));
}

LONG FAR PASCAL button_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        static struct paintst ps;
        HDC dc = BeginPaint(h, (void FAR *)&ps);
        short l = w->cr.left, t = w->cr.top, r = w->cr.right, b = w->cr.bottom;
        short ty = (t + b - FONT_H) / 2, tw = mwid(w->title);
        BYTE fg = w->enabled ? 0 : 8;
        int foc = GetFocus() == h, dn = w->extra[0], ck = w->extra[1];
        switch (BS_TYPE(w)) {
        case 2: case 3: case 5: case 6:                     /* Kaestchen     */
            dc_rawfill(dc, l, t, r, b, 15);
            frame(dc, l, ty + 2, l + 12, ty + 14, 0);
            if (ck == 1) {
                short k;
                for (k = 0; k < 8; k++) {
                    dc_rawfill(dc, l + 2 + k, ty + 4 + k, l + 3 + k, ty + 5 + k, 0);
                    dc_rawfill(dc, l + 9 - k, ty + 4 + k, l + 10 - k, ty + 5 + k, 0);
                }
            } else if (ck == 2)
                dc_rawfill(dc, l + 2, ty + 4, l + 10, ty + 12, 7);
            mtxt(dc, l + 18, ty, w->title, fg);
            if (foc) frame(dc, l + 16, ty, l + 20 + tw, ty + FONT_H, 7);
            break;
        case 4: case 9: {                                   /* Optionsfeld   */
            static const BYTE rr[12][2] = { {4,8},{2,10},{1,11},{1,11},{0,12},{0,12},
                                            {0,12},{0,12},{1,11},{1,11},{2,10},{4,8} };
            short k;
            dc_rawfill(dc, l, t, r, b, 15);
            for (k = 0; k < 12; k++) {
                dc_rawfill(dc, l + rr[k][0], ty + 2 + k, l + rr[k][0] + 1, ty + 3 + k, 0);
                dc_rawfill(dc, l + rr[k][1] - 1, ty + 2 + k, l + rr[k][1], ty + 3 + k, 0);
            }
            dc_rawfill(dc, l + 4, ty + 2, l + 8, ty + 3, 0);
            dc_rawfill(dc, l + 4, ty + 13, l + 8, ty + 14, 0);
            if (ck)
                dc_rawfill(dc, l + 4, ty + 6, l + 8, ty + 10, 0);
            mtxt(dc, l + 18, ty, w->title, fg);
            if (foc) frame(dc, l + 16, ty, l + 20 + tw, ty + FONT_H, 7);
            break;
        }
        case 7:                                             /* Gruppenrahmen */
            frame(dc, l, t + FONT_H / 2, r, b, 0);
            if (w->title[0]) {
                dc_rawfill(dc, l + 6, t, l + 10 + tw, t + FONT_H, 15);
                mtxt(dc, l + 8, t, w->title, fg);
            }
            break;
        default:                                            /* Druckknopf    */
            ui_bevel(dc, l, t, r, b, dn);
            if (BS_TYPE(w) == 1 || foc)
                frame(dc, l - 1, t - 1, r + 1, b + 1, 0);
            mtxt(dc, (l + r - tw) / 2 + dn, ty + dn, w->title, fg);
            if (foc)
                frame(dc, (l + r - tw) / 2 - 3, ty, (l + r + tw) / 2 + 3, ty + FONT_H, 8);
            break;
        }
        EndPaint(h, (void FAR *)&ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_GETDLGCODE:
        if (BS_TYPE(w) == 7) return DLGC_STATIC;
        if (BS_TYPE(w) == 4 || BS_TYPE(w) == 9) return DLGC_BUTTON | DLGC_RADIO;
        return DLGC_BUTTON | (BS_TYPE(w) == 1 ? DLGC_DEFPUSH : BS_TYPE(w) == 0 ? DLGC_UNDEFPUSH : 0);
    case WM_LBUTTONDOWN:
        if (BS_TYPE(w) == 7) return 0;
        w->extra[0] = 1;
        SetCapture(h);
        if (GetFocus() != h) SetFocus(h);
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_LBUTTONUP: {
        short x = LOWORD(lp), y = HIWORD(lp);
        int in = x >= 0 && y >= 0 && x < w->cr.right - w->cr.left && y < w->cr.bottom - w->cr.top;
        ReleaseCapture();
        if (w->extra[0]) {
            w->extra[0] = 0;
            InvalidateRect(h, 0, 0);
            if (in)
                btn_click(h, w);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == 0x20 && BS_TYPE(w) != 7)
            btn_click(h, w);
        return 0;
    case 0x0400: return w->extra[1];                        /* BM_GETCHECK   */
    case 0x0401:                                            /* BM_SETCHECK   */
        if (w->extra[1] != (BYTE)wp) {
            w->extra[1] = (BYTE)wp;
            InvalidateRect(h, 0, 0);
        }
        return 0;
    case 0x0402: return w->extra[1] | (w->extra[0] ? 4 : 0) | (GetFocus() == h ? 8 : 0);
    case 0x0403:                                            /* BM_SETSTATE   */
        w->extra[0] = wp ? 1 : 0;
        InvalidateRect(h, 0, 0);
        return 0;
    case 0x0404:                                            /* BM_SETSTYLE   */
        w->style = (w->style & ~0x0FUL) | (wp & 0x0F);
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_SETTEXT:
        DefWindowProc(h, m, wp, lp);
        InvalidateRect(h, 0, 0);
        return 1;
    }
    return DefWindowProc(h, m, wp, lp);
}

/* ---- STATIC ---- */
LONG FAR PASCAL static_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_GETDLGCODE)
        return DLGC_STATIC;
    if (m == WM_SETTEXT) {
        DefWindowProc(h, m, wp, lp);
        InvalidateRect(h, 0, 1);
        return 1;
    }
    if (m == WM_PAINT) {
        static struct paintst ps;
        HDC dc = BeginPaint(h, (void FAR *)&ps);
        short l = w->cr.left, t = w->cr.top, r = w->cr.right, b = w->cr.bottom;
        WORD ss = (WORD)(w->style & 0x1F);
        BYTE fg = w->enabled ? 0 : 8;
        if (ss >= 4 && ss <= 9) {               /* Rechtecke/Rahmen      */
            BYTE c = (ss == 4 || ss == 7) ? 0 : (ss == 5 || ss == 8) ? 8 : 15;
            if (ss <= 6) dc_rawfill(dc, l, t, r, b, c);
            else frame(dc, l, t, r, b, c);
        } else if (ss != 3) {                   /* Text, mit Umbruch     */
            LPCSTR s = w->title;
            short y = t, maxw = r - l;
            dc_rawfill(dc, l, t, r, b, 15);
            {                                   /* erstes Wort passt nicht: eng */
                static char wb[64];
                short k;
                for (k = 0; s[k] && s[k] != ' ' && s[k] != '\n' && k < 63; k++)
                    wb[k] = s[k];
                wb[k] = 0;
                gfx_ptight(mwid(wb) > maxw);
            }
            while (*s && y < b) {
                short n = 0, br = 0, k, len, px = 0;
                LPCSTR q = s;
                while (q[n] && q[n] != '\n' && q[n] != '\r') {
                    if (q[n] == ' ') br = n;
                    if (q[n] != '&' || (w->style & 0x80))
                        px += gfx_ptextw(q + n, 1);
                    if (px > maxw && n && ss != 0xC && ss != 0xB) {
                        if (br) n = br;
                        break;
                    }
                    n++;
                }
                {
                    static char lb[96];
                    for (k = 0; k < n && k < 95; k++) lb[k] = q[k];
                    lb[k] = 0;
                    len = (w->style & 0x80) ? gfx_ptextw(lb, k) : mwid(lb);
                    k = ss == 1 ? (l + r - len) / 2 : ss == 2 ? r - len : l;
                    if (w->style & 0x80)
                        dc_ptext(dc, k, y, lb, fstrlen(lb), fg);
                    else
                        mtxt(dc, k, y, lb, fg);
                }
                s = q + n;
                if (*s == ' ') s++;
                if (*s == '\r') s++;
                if (*s == '\n') s++;
                y += FONT_H;
            }
        }
        gfx_ptight(0);
        EndPaint(h, (void FAR *)&ps);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

/* ---- LISTBOX: Eintraege in einem eigenen Segment ----
   Segment: 512 Worte Stellen der Texte, ab 1024 die Texte; Bit 15 des
   Wortes frei, Daten (LB_SETITEMDATA) in einem zweiten Feld */
#define NLB     16
#define LBMAX   500
static struct lb {
    HWND h;
    WORD seg, n, used, cur, top;
    DWORD last;
} lbt[NLB];
#define LBO(l)  ((WORD FAR *)MKFP((l)->seg, 0))
#define LBS(l, i) ((char FAR *)MKFP((l)->seg, LBO(l)[i]))
#define LBD(l)  ((DWORD FAR *)MKFP((l)->seg, 1024))      /* Elementdaten  */
#define LB_TXT  3072                            /* Texte dahinter        */

static struct lb *lbp(HWND h)
{
    short i;
    for (i = 0; i < NLB; i++)
        if (lbt[i].h == h && h)
            return &lbt[i];
    return 0;
}
static short lb_rows(struct wnd *w)
{
    short r = (w->cr.bottom - w->cr.top) / FONT_H;
    return r < 1 ? 1 : r;
}
static void lb_sb(struct lb *l, struct wnd *w)
{
    short r = lb_rows(w), mx = l->n > (WORD)r ? l->n - r : 0;
    if (w->style & WS_VSCROLL) {
        sb_setrange(l->h, 1, 0, mx, 0);
        SetScrollPos(l->h, 1, l->top, 1);
    }
}
static void lb_show(struct lb *l, struct wnd *w)
{
    short r = lb_rows(w);
    if (l->cur != 0xFFFF) {
        if (l->cur < l->top) l->top = l->cur;
        if (l->cur >= l->top + r) l->top = l->cur - r + 1;
    }
    if (l->top + r > l->n) l->top = l->n > (WORD)r ? l->n - r : 0;
    lb_sb(l, w);
    InvalidateRect(l->h, 0, 0);
}
static int lb_cmp(LPCSTR a, LPCSTR b) { return fstricmp(a, b); }
static short lb_insert(struct lb *l, struct wnd *w, short at, LPCSTR s)
{
    WORD n = fstrlen(s) + 1, i;
    if (l->n >= LBMAX || l->used + n > 0xFF00u)
        return -2;                              /* LB_ERRSPACE           */
    if (at < 0 || at > (short)l->n) {
        at = l->n;
        if (w->style & 2)                       /* LBS_SORT              */
            for (at = 0; at < (short)l->n && lb_cmp(LBS(l, at), s) <= 0; at++)
                ;
    }
    for (i = l->n; i > (WORD)at; i--) {
        LBO(l)[i] = LBO(l)[i - 1];
        LBD(l)[i] = LBD(l)[i - 1];
    }
    LBO(l)[at] = l->used;
    LBD(l)[at] = 0;
    fmemcpy(MKFP(l->seg, l->used), s, n);
    l->used += n;
    l->n++;
    lb_show(l, w);
    return at;
}
static void lb_delete(struct lb *l, WORD at)
{
    WORD i;
    if (at >= l->n)
        return;
    for (i = at; i + 1 < l->n; i++) {
        LBO(l)[i] = LBO(l)[i + 1];
        LBD(l)[i] = LBD(l)[i + 1];
    }
    l->n--;
    if (l->cur != 0xFFFF && l->cur >= l->n)
        l->cur = l->n ? l->n - 1 : 0xFFFF;
}

LONG FAR PASCAL listbox_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    struct lb *l = lbp(h);
    if (!w)
        return 0;
    if (m == 0x0001) {                          /* WM_CREATE             */
        short i;
        for (i = 0; i < NLB && lbt[i].h; i++)
            ;
        if (i == NLB)
            return -1;
        l = &lbt[i];
        l->seg = seg_new(0xFF00u);
        if (!l->seg)
            return -1;
        l->h = h;
        l->n = 0; l->used = LB_TXT; l->cur = 0xFFFF; l->top = 0; l->last = 0;
        if (!(w->style & WS_BORDER))
            w->style |= WS_BORDER;
        return 0;
    }
    if (!l)
        return DefWindowProc(h, m, wp, lp);
    switch (m) {
    case WM_DESTROY:
        seg_del(l->seg);
        l->h = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        static struct paintst ps;
        HDC dc = BeginPaint(h, (void FAR *)&ps);
        short r = lb_rows(w), k;
        int foc = GetFocus() == h;
        dc_rawfill(dc, w->cr.left, w->cr.top, w->cr.right, w->cr.bottom, 15);
        for (k = 0; k < r && l->top + k < l->n; k++) {
            WORD i = l->top + k;
            short y = w->cr.top + k * FONT_H, n = fstrlen(LBS(l, i)), mx = (w->cr.right - w->cr.left - 4) / FONT_W;
            BYTE sel = i == l->cur;
            if (n > mx) n = mx;
            if (sel)
                dc_rawfill(dc, w->cr.left, y, w->cr.right, y + FONT_H, foc ? 4 : 7);
            dc_rawtext(dc, w->cr.left + 2, y, LBS(l, i), n, sel && foc ? 15 : 0);
        }
        EndPaint(h, (void FAR *)&ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN: {
        WORD i = l->top + (short)HIWORD(lp) / FONT_H;
        DWORD now = GetTickCount();
        if (GetFocus() != h) SetFocus(h);
        if (i >= l->n)
            return 0;
        if (i == l->cur && now - l->last < 500) {
            if (w->style & 1)
                post_msg(w->parent, WM_COMMAND, w->id, MAKELONG(h, 2));   /* LBN_DBLCLK */
            l->last = 0;
            return 0;
        }
        l->last = now;
        if (i != l->cur) {
            l->cur = i;
            lb_show(l, w);
            if (w->style & 1)
                post_msg(w->parent, WM_COMMAND, w->id, MAKELONG(h, 1)); /* LBN_SELCHANGE */
        }
        return 0;
    }
    case WM_KEYDOWN: {
        long c = l->cur == 0xFFFF ? -1 : l->cur;
        short r = lb_rows(w);
        switch (wp) {
        case 0x26: c--; break;
        case 0x28: c++; break;
        case 0x21: c -= r - 1; break;
        case 0x22: c += r - 1; break;
        case 0x24: c = 0; break;
        case 0x23: c = (long)l->n - 1; break;
        default: return 0;
        }
        if (c >= (long)l->n) c = (long)l->n - 1;
        if (c < 0) c = 0;
        if (l->n && (WORD)c != l->cur) {
            l->cur = (WORD)c;
            lb_show(l, w);
            if (w->style & 1)
                post_msg(w->parent, WM_COMMAND, w->id, MAKELONG(h, 1));
        }
        return 0;
    }
    case WM_CHAR: {                             /* Anfangsbuchstabe      */
        WORD i, k;
        BYTE u = (BYTE)wp;
        if (u >= 'a' && u <= 'z') u -= 32;
        for (k = 1; k <= l->n; k++) {
            BYTE c;
            i = (l->cur == 0xFFFF ? k - 1 : l->cur + k) % l->n;
            c = LBS(l, i)[0];
            if (c == '[' ) c = LBS(l, i)[1];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == u) {
                l->cur = i;
                lb_show(l, w);
                if (w->style & 1)
                    post_msg(w->parent, WM_COMMAND, w->id, MAKELONG(h, 1));
                break;
            }
        }
        return 0;
    }
    case WM_VSCROLL: {
        long t = l->top;
        short r = lb_rows(w);
        switch (wp) {
        case 0: t--; break;
        case 1: t++; break;
        case 2: t -= r - 1; break;
        case 3: t += r - 1; break;
        case 4: case 5: t = (short)LOWORD(lp); break;
        default: return 0;
        }
        if (t > (long)l->n - r) t = (long)l->n - r;
        if (t < 0) t = 0;
        l->top = (WORD)t;
        lb_sb(l, w);
        InvalidateRect(h, 0, 0);
        return 0;
    }
    case 0x0401: case 0x0402:                   /* LB_ADDSTRING/INSERT   */
        return lb_insert(l, w, m == 0x0401 ? -1 : (short)wp, (LPCSTR)lp);
    case 0x0403:                                /* LB_DELETESTRING       */
        if (wp >= l->n)                         /* wie Windows: LB_ERR   */
            return -1;
        lb_delete(l, wp);
        lb_show(l, w);
        return l->n;
    case 0x0405:                                /* LB_RESETCONTENT       */
        l->n = 0; l->used = LB_TXT; l->cur = 0xFFFF; l->top = 0;
        lb_show(l, w);
        return 0;
    case 0x0407:                                /* LB_SETCURSEL          */
        l->cur = wp < l->n ? wp : 0xFFFF;
        lb_show(l, w);
        return wp < l->n ? (LONG)wp : -1L;
    case 0x0409:                                /* LB_GETCURSEL          */
        return l->cur == 0xFFFF ? -1L : (LONG)l->cur;
    case 0x040A:                                /* LB_GETTEXT            */
        if (wp >= l->n) return -1;
        fstrncpy((LPSTR)lp, LBS(l, wp), 260);
        return fstrlen((LPSTR)lp);
    case 0x040B:                                /* LB_GETTEXTLEN         */
        return wp < l->n ? (LONG)fstrlen(LBS(l, wp)) : -1L;
    case 0x040C:                                /* LB_GETCOUNT           */
        return l->n;
    case 0x0410: case 0x040D: {                 /* LB_FINDSTRING/SELECT  */
        WORD i, n = fstrlen((LPCSTR)lp), k;
        for (k = 0; k < l->n; k++) {
            i = (WORD)((short)wp + 1 + k) % l->n;
            {
                LPCSTR a = LBS(l, i), b = (LPCSTR)lp;
                WORD j;
                for (j = 0; j < n; j++) {
                    char x = a[j], y = b[j];
                    if (x >= 'a' && x <= 'z') x -= 32;
                    if (y >= 'a' && y <= 'z') y -= 32;
                    if (x != y) break;
                }
                if (j == n) {
                    if (m == 0x040D) { l->cur = i; lb_show(l, w); }
                    return i;
                }
            }
        }
        return -1;
    }
    case 0x041A:                                /* LB_GETITEMDATA        */
        return wp < l->n ? (LONG)LBD(l)[wp] : -1L;
    case 0x041B:                                /* LB_SETITEMDATA        */
        if (wp >= l->n) return -1;
        LBD(l)[wp] = lp;
        return 0;
    case 0x0423: {                              /* LB_FINDSTRINGEXACT    */
        WORD i, k;
        for (k = 0; k < l->n; k++) {
            i = (WORD)((short)wp + 1 + k) % l->n;
            if (!fstricmp(LBS(l, i), (LPCSTR)lp))
                return i;
        }
        return -1;
    }
    case 0x040F: return l->top;                 /* LB_GETTOPINDEX        */
    case 0x0418:                                /* LB_SETTOPINDEX        */
        l->top = wp < l->n ? wp : l->top;
        lb_show(l, w);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}
