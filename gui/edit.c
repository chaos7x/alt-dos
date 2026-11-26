/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - eingebaute Fensterklasse EDIT (ein- und mehrzeilig) und die
 * Zwischenablage (USER).
 *
 * Wie unter Windows 3.x liegt der Text in der lokalen Halde des Programms,
 * das das Feld anlegt (hInstance = Autodata): EM_GETHANDLE liefert das
 * Handle, das Programm liest es mit LocalLock, EM_SETHANDLE gibt ihm einen
 * neuen Text. Felder ohne Programm-Halde (Dialoge der GUI) bekommen ein
 * eigenes Segment mit Halde. Zeilenanfaenge in einem eigenen Segment, nach
 * jeder Aenderung neu berechnet (bei Umbruch an der Breite des Fensters).
 * Bedienung wie CUA: Pfeile, Pos1/Ende, Bild auf/ab, Strg+Pfeil wortweise,
 * Umschalt markiert, Umschalt+Entf/Strg+Einfg/Umschalt+Einfg und Strg+X/C/V,
 * Alt+Ruecktaste bzw. Strg+Z macht rueckgaengig (eine Stufe).
 * Zwischenablage: CF_TEXT ueber die Ablage des Kernels (DosClipPut/Get),
 * damit Text zwischen GUI, Konsole und EDIT wandert (ANSI <-> OEM).
 */
#include "gui.h"

#define NED     16
#define NLINE   16000
struct ed {
    HWND  h;
    WORD  ds, ht, own;          /* Halde: Segment, Handle, eigenes Segment */
    WORD  len, lim;
    WORD  sa, sc;               /* Anker, Schreibmarke                    */
    WORD  top, left;            /* erste Zeile, erste Spalte (Zeichen)    */
    WORD  nl, lseg, maxw;       /* Zeilen, Segment mit Anfaengen          */
    WORD  useg, ulen, uc;       /* Rueckgaengig: Kopie, Laenge, Marke     */
    BYTE  ml, wrap, ro, mod, drag, ucan, utyp, pwc;
    WORD  xgoal;
};
static struct ed ed[NED];

#define ES_CENTER       0x0001
#define ES_MULTILINE    0x0004
#define ES_AUTOHSCROLL  0x0080
#define ES_PASSWORD     0x0020
#define ES_READONLY     0x0800
#define WS_HSCROLL      0x00100000UL
#define WS_VSCROLL      0x00200000UL

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
#define WM_SETFONT      0x0030
#define WM_GETFONT      0x0031
#define WM_GETDLGCODE   0x0087
#define WM_KEYDOWN      0x0100
#define WM_CHAR         0x0102
#define WM_SYSCHAR      0x0106
#define WM_COMMAND      0x0111
#define WM_HSCROLL      0x0114
#define WM_VSCROLL      0x0115
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_CUT          0x0300
#define WM_COPY         0x0301
#define WM_PASTE        0x0302
#define WM_CLEAR        0x0303
#define WM_UNDO         0x0304

#define EN_SETFOCUS     0x0100
#define EN_KILLFOCUS    0x0200
#define EN_CHANGE       0x0300
#define EN_UPDATE       0x0400
#define EN_ERRSPACE     0x0500
#define EN_MAXTEXT      0x0501
#define EN_HSCROLL      0x0601
#define EN_VSCROLL      0x0602

HWND API SetFocus(HWND h);
HWND API GetFocus(void);
HWND API SetCapture(HWND h);
void API ReleaseCapture(void);
short API GetKeyState(short vk);
short API SetScrollPos(HWND h, short bar, short pos, BOOL redraw);
void sb_setrange(HWND h, short bar, short lo, short hi, BOOL redraw);
void API GetClientRect(HWND h, LPRECT r);

static struct ed *eptr(HWND h)
{
    short i;
    for (i = 0; i < NED; i++)
        if (ed[i].h == h)
            return &ed[i];
    return 0;
}

#define T(e)    ((char FAR *)MKFP((e)->ds, lh_ptr((e)->ds, (e)->ht)))
#define LS(e)   ((WORD FAR *)MKFP((e)->lseg, 0))

static void fmove(char FAR *d, const char FAR *s, WORD n)
{
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
}

static void notify(struct ed *e, WORD code)
{
    struct wnd *w = wptr(e->h);
    if (w && w->parent)
        send_msg(w->parent, WM_COMMAND, w->id, MAKELONG(e->h, code));
}

/* sichtbare Zeilen/Spalten des Rechtecks */
static short rows(struct ed *e)
{
    struct wnd *w = wptr(e->h);
    short r = w ? (w->cr.bottom - w->cr.top - 2) / FONT_H : 1;
    return r < 1 ? 1 : r;
}
static short cols(struct ed *e)
{
    struct wnd *w = wptr(e->h);
    short c = w ? (w->cr.right - w->cr.left - 4) / FONT_W : 1;
    return c < 1 ? 1 : c;
}

/* Zeilenanfaenge (harte Umbrueche CR LF bzw. LF, weiche bei Umbruch) */
static void recalc(struct ed *e)
{
    char FAR *t = T(e);
    WORD FAR *ls = LS(e);
    WORD i = 0, n = 1, col = 0, st = 0, sp = 0xFFFF, vc = cols(e), mw = 0;
    ls[0] = 0;
    if (!e->ml) {
        e->nl = 1;
        e->maxw = e->len;
        return;
    }
    while (i < e->len && n < NLINE) {
        char c = t[i];
        if (c == '\r' && i + 1 < e->len && t[i + 1] == '\n') {
            if (col > mw) mw = col;
            i += 2; ls[n++] = i; st = i; col = 0; sp = 0xFFFF;
            continue;
        }
        if (c == '\n') {
            if (col > mw) mw = col;
            i++; ls[n++] = i; st = i; col = 0; sp = 0xFFFF;
            continue;
        }
        if (e->wrap && col >= vc) {
            WORD b = (sp != 0xFFFF && sp > st) ? sp : i;
            ls[n++] = b;
            st = b;
            col = 0;
            sp = 0xFFFF;
            i = b;
            continue;
        }
        if (c == ' ')
            sp = i + 1;
        col = c == '\t' ? (col + 8) & ~7u : col + 1;
        i++;
    }
    if (col > mw) mw = col;
    e->nl = n;
    e->maxw = mw;
}

static WORD line_of(struct ed *e, WORD pos)
{
    WORD FAR *ls = LS(e);
    WORD lo = 0, hi = e->nl - 1, m;
    while (lo < hi) {
        m = (lo + hi + 1) / 2;
        if (ls[m] <= pos) lo = m; else hi = m - 1;
    }
    return lo;
}
static WORD line_end(struct ed *e, WORD k)       /* ohne CR LF            */
{
    char FAR *t = T(e);
    WORD x = k + 1 < e->nl ? LS(e)[k + 1] : e->len;
    if (x > LS(e)[k] && t[x - 1] == '\n') x--;
    if (x > LS(e)[k] && t[x - 1] == '\r') x--;
    return x;
}
static WORD col_of(struct ed *e, WORD k, WORD pos)
{
    char FAR *t = T(e);
    WORD i, c = 0;
    for (i = LS(e)[k]; i < pos; i++)
        c = t[i] == '\t' ? (c + 8) & ~7u : c + 1;
    return c;
}
static WORD pos_at(struct ed *e, WORD k, WORD col)
{
    char FAR *t = T(e);
    WORD i = LS(e)[k], en = line_end(e, k), c = 0, nc;
    for (; i < en; i++) {
        nc = t[i] == '\t' ? (c + 8) & ~7u : c + 1;
        if (nc > col) {
            if (col - c > nc - col) i++;
            break;
        }
        c = nc;
    }
    return i;
}

static void sbars(struct ed *e)
{
    struct wnd *w = wptr(e->h);
    if (!w)
        return;
    if (w->style & WS_VSCROLL) {
        sb_setrange(e->h, 1, 0, e->nl > 1 ? e->nl - 1 : 0, 0);
        SetScrollPos(e->h, 1, e->top, 1);
    }
    if (w->style & WS_HSCROLL) {
        sb_setrange(e->h, 0, 0, e->maxw > 0 ? e->maxw : 0, 0);
        SetScrollPos(e->h, 0, e->left, 1);
    }
}

static void redraw(struct ed *e)
{
    struct wnd *w = wptr(e->h);
    if (w)
        InvalidateRect(e->h, 0, 0);
}

static void show_caret(struct ed *e)
{
    WORD k = line_of(e, e->sc), c = col_of(e, k, e->sc);
    short r = rows(e), cc = cols(e);
    WORD ot = e->top, ol = e->left;
    if (e->ml) {
        if (k < e->top) e->top = k;
        if (k >= e->top + r) e->top = k - r + 1;
    }
    if (!e->wrap) {
        if (c < e->left) e->left = c > 8 ? c - 8 : 0;
        if (c >= e->left + cc) e->left = c - cc + (e->ml ? 8 : 1);
    } else
        e->left = 0;
    if (ot != e->top) notify(e, EN_VSCROLL);
    if (ol != e->left) notify(e, EN_HSCROLL);
    sbars(e);
    redraw(e);
}

/* Rueckgaengig: ganze Kopie vor einer Aenderungsfolge */
static void snap(struct ed *e)
{
    if (e->useg) {
        seg_del(e->useg);
        e->useg = 0;
    }
    e->useg = seg_new(e->len ? e->len : 1);
    if (!e->useg) {
        e->ucan = 0;
        return;
    }
    fmemcpy(MKFP(e->useg, 0), T(e), e->len);
    e->ulen = e->len;
    e->uc = e->sc;
    e->ucan = 1;
}

/* a..b durch s[0..n) ersetzen; -> 0 ok */
static int repl(struct ed *e, WORD a, WORD b, const char FAR *s, WORD n, int typing)
{
    WORD need, nh, lim = e->lim ? e->lim : 0xFE00u;
    char FAR *t;
    if (a > b) { WORD x = a; a = b; b = x; }
    if (b > e->len) b = e->len;
    if ((long)e->len - (b - a) + n > (long)lim) {
        notify(e, EN_MAXTEXT);
        n = lim - (e->len - (b - a));
        if ((short)n <= 0)
            return 1;
    }
    if (!typing || !e->utyp)
        snap(e);
    e->utyp = typing ? 1 : 0;
    need = e->len - (b - a) + n + 1;
    if (need > lh_size(e->ds, e->ht)) {
        nh = lh_realloc(e->ds, e->ht, need + 256);
        if (!nh) {
            notify(e, EN_ERRSPACE);
            return 1;
        }
        e->ht = nh;
    }
    t = T(e);
    fmove(t + a + n, t + b, e->len - b);
    if (n)
        fmemcpy(t + a, s, n);
    e->len = e->len - (b - a) + n;
    t[e->len] = 0;
    e->sa = e->sc = a + n;
    e->mod = 1;
    recalc(e);
    notify(e, EN_UPDATE);
    show_caret(e);
    notify(e, EN_CHANGE);
    return 0;
}

static void set_text(struct ed *e, LPCSTR s)
{
    WORD n = s ? fstrlen(s) : 0, nh;
    nh = lh_realloc(e->ds, e->ht, n + 64);
    if (nh)
        e->ht = nh;
    if (n + 1 > lh_size(e->ds, e->ht))
        n = lh_size(e->ds, e->ht) ? lh_size(e->ds, e->ht) - 1 : 0;
    if (n)
        fmemcpy(T(e), s, n);
    T(e)[n] = 0;
    e->len = n;
    e->sa = e->sc = 0;
    e->top = e->left = 0;
    e->mod = 0;
    e->ucan = 0;
    e->utyp = 0;
    recalc(e);
    sbars(e);
    redraw(e);
}

/* ---- Zwischenablage ---- */
static WORD clip_sel;                           /* Daten fuer GetClipboardData */
BOOL API OpenClipboard(HWND h) { (void)h; return 1; }
BOOL API CloseClipboard(void) { return 1; }
BOOL API EmptyClipboard(void)
{
    static char z;
    DosClipPut(0, &z, 0);
    return 1;
}
WORD API SetClipboardData(WORD fmt, WORD hm)
{
    char FAR *p;
    WORD n, i, s;
    if (fmt != 1 && fmt != 7)                   /* CF_TEXT, CF_OEMTEXT   */
        return hm;
    if (!hm)
        return 0;
    p = MKFP(hm | 7, 0);
    for (n = 0; p[n] && n < 16383; n++)
        ;
    s = seg_new(n + 1);
    if (!s)
        return 0;
    for (i = 0; i < n; i++)
        ((char FAR *)MKFP(s, 0))[i] = fmt == 1 ? ansi2oem(p[i]) : p[i];
    fapi_call(FAPI_CLIPPUT, s, 0, n);
    seg_del(s);
    seg_del(hm | 7);                            /* gehoert jetzt der Ablage */
    return hm;
}
static WORD clip_get(char FAR * FAR *out)
{
    WORD n;
    if (clip_sel) {
        seg_del(clip_sel);
        clip_sel = 0;
    }
    clip_sel = seg_new(16385);
    if (!clip_sel)
        return 0;
    n = fapi_call(FAPI_CLIPGET, clip_sel, 0, 16384);
    if (FAPI_FAILED(n))
        n = 0;
    *out = MKFP(clip_sel, 0);
    (*out)[n] = 0;
    return n;
}
WORD API GetClipboardData(WORD fmt)
{
    char FAR *p;
    WORD n, i;
    extern const BYTE a2o[128];
    if (fmt != 1 && fmt != 7)
        return 0;
    n = clip_get(&p);
    if (!n)
        return 0;
    if (fmt == 1)
        for (i = 0; i < n; i++) {
            BYTE c = p[i], k;
            if (c >= 0x80) {
                for (k = 0; k < 128 && a2o[k] != c; k++)
                    ;
                p[i] = k < 128 ? (char)(0x80 + k) : '_';
            }
        }
    return clip_sel;
}
BOOL API IsClipboardFormatAvailable(WORD fmt)
{
    static char b[2];
    WORD n;
    if (fmt != 1 && fmt != 7)
        return 0;
    n = fapi_call(FAPI_CLIPGET, 0, (WORD)b, 1);
    return !FAPI_FAILED(n) && n > 0;
}
WORD API EnumClipboardFormats(WORD fmt)
{
    if (fmt == 0 && IsClipboardFormatAvailable(1))
        return 1;
    return 0;
}
short API CountClipboardFormats(void) { return IsClipboardFormatAvailable(1) ? 1 : 0; }

static void copy_sel(struct ed *e)
{
    WORD a = e->sa < e->sc ? e->sa : e->sc, b = e->sa < e->sc ? e->sc : e->sa, s, i;
    char FAR *t = T(e);
    if (a == b || e->pwc)
        return;
    s = seg_new(b - a + 1);
    if (!s)
        return;
    for (i = a; i < b; i++)
        ((char FAR *)MKFP(s, 0))[i - a] = ansi2oem(t[i]);
    fapi_call(FAPI_CLIPPUT, s, 0, b - a);
    seg_del(s);
}
static void paste(struct ed *e)
{
    char FAR *p;
    WORD n, i;
    if (!GetClipboardData(1))
        return;
    p = MKFP(clip_sel, 0);
    n = fstrlen(p);
    if (!e->ml)                                 /* einzeilig: bis Zeilenende */
        for (i = 0; i < n; i++)
            if (p[i] == '\r' || p[i] == '\n') { n = i; break; }
    if (e->ml) {                                /* LF allein -> CR LF        */
        static char tmp[512];
        WORD k = 0;
        repl(e, e->sa, e->sc, "", 0, 0);
        for (i = 0; i < n; i++) {
            if (p[i] == '\n' && (i == 0 || p[i - 1] != '\r'))
                tmp[k++] = '\r';
            tmp[k++] = p[i];
            if (k >= sizeof(tmp) - 2 || i + 1 == n) {
                e->utyp = 1;
                repl(e, e->sc, e->sc, tmp, k, 1);
                k = 0;
            }
        }
        e->utyp = 0;
        return;
    }
    repl(e, e->sa, e->sc, p, n, 0);
}

static void undo(struct ed *e)
{
    WORD s, l, c;
    if (!e->ucan || !e->useg)
        return;
    s = e->useg; l = e->ulen; c = e->uc;
    e->useg = 0;
    e->utyp = 0;
    snap(e);                                    /* Rueckgaengig des Rueckgaengig */
    if (lh_size(e->ds, e->ht) < l + 1) {
        WORD nh = lh_realloc(e->ds, e->ht, l + 64);
        if (!nh) { seg_del(s); return; }
        e->ht = nh;
    }
    fmemcpy(T(e), MKFP(s, 0), l);
    T(e)[l] = 0;
    e->len = l;
    seg_del(s);
    e->sa = e->sc = c > l ? l : c;
    e->mod = 1;
    recalc(e);
    show_caret(e);
    notify(e, EN_CHANGE);
}

/* ---- Zeichnen ---- */
static void paint(struct ed *e, struct wnd *w)
{
    static struct { HDC hdc; BOOL er; RECT rc; BOOL r, i; BYTE x[16]; } ps;
    HDC dc = BeginPaint(e->h, (void FAR *)&ps);
    char FAR *t = T(e);
    short r = rows(e), x0 = w->cr.left + 2, y0 = w->cr.top + 1, k;
    WORD a = e->sa < e->sc ? e->sa : e->sc, b = e->sa < e->sc ? e->sc : e->sa;
    int foc = GetFocus() == e->h, nohide = (w->style & 0x0100) != 0;  /* ES_NOHIDESEL */
    BYTE bg = w->enabled ? 15 : 7;
    static char lb[96];
    if (!e->ml)
        y0 = w->cr.top + (w->cr.bottom - w->cr.top - FONT_H) / 2;
    dc_rawfill(dc, w->cr.left, w->cr.top, w->cr.right, w->cr.bottom, bg);
    for (k = 0; k < r && e->top + k < e->nl; k++) {
        WORD ln = e->top + k, i = LS(e)[ln], en = line_end(e, ln), c = 0, n = 0, cc = cols(e);
        short y = y0 + k * FONT_H;
        /* in Spalten aufloesen, ab e->left */
        while (i < en && c < e->left + cc) {
            char ch = e->pwc ? (char)e->pwc : t[i];
            WORD nc = ch == '\t' && !e->pwc ? (c + 8) & ~7u : c + 1;
            for (; c < nc; c++) {
                if (c >= e->left && n < sizeof(lb)) {
                    BYTE sel = (foc || nohide) && i >= a && i < b;
                    lb[n] = ch == '\t' ? ' ' : ch;
                    if (sel)
                        dc_rawfill(dc, x0 + n * FONT_W, y, x0 + (n + 1) * FONT_W, y + FONT_H, 4);
                    dc_rawtext(dc, x0 + n * FONT_W, y, lb + n, 1, sel ? 15 : (w->enabled ? 0 : 8));
                    n++;
                }
                if (ch != '\t') { c++; break; }
            }
            if (ch == '\t') c = nc;
            i++;
        }
        /* Markierung ueber das Zeilenende (CR LF) */
        if ((foc || nohide) && a < b && en < (ln + 1 < e->nl ? LS(e)[ln + 1] : e->len) &&
            en >= a && en < b && n < cc)
            dc_rawfill(dc, x0 + n * FONT_W, y, x0 + n * FONT_W + 4, y + FONT_H, 4);
    }
    if (foc) {                                  /* Schreibmarke          */
        WORD ln = line_of(e, e->sc);
        short cx = col_of(e, ln, e->sc) - e->left, cy = ln - e->top;
        if (cy >= 0 && cy < r && cx >= 0 && cx <= cols(e))
            dc_rawfill(dc, x0 + cx * FONT_W, y0 + cy * FONT_H, x0 + cx * FONT_W + 2,
                       y0 + cy * FONT_H + FONT_H, 0);
    }
    EndPaint(e->h, (void FAR *)&ps);
}

static WORD hit(struct ed *e, struct wnd *w, short x, short y)
{
    short k = e->ml ? y / FONT_H : 0, c = (x - 2 + FONT_W / 2) / FONT_W;
    WORD ln;
    if (k < 0) k = 0;
    if (c < 0) c = 0;
    ln = e->top + k;
    if (ln >= e->nl) ln = e->nl - 1;
    (void)w;
    return pos_at(e, ln, e->left + c);
}

static int is_wc(char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; }

static void move_to(struct ed *e, WORD p, int ext)
{
    e->sc = p;
    if (!ext)
        e->sa = p;
    e->utyp = 0;
    show_caret(e);
}

static void key(struct ed *e, WORD vk)
{
    int sh = GetKeyState(0x10) < 0, ct = GetKeyState(0x11) < 0;
    WORD k = line_of(e, e->sc), p = e->sc;
    char FAR *t = T(e);
    short r = rows(e);
    if (vk != 0x26 && vk != 0x28 && vk != 0x21 && vk != 0x22)
        e->xgoal = 0xFFFF;
    switch (vk) {
    case 0x25:                                  /* links                 */
        if (!sh && e->sa != e->sc) { move_to(e, e->sa < e->sc ? e->sa : e->sc, 0); return; }
        if (p) {
            p--;
            if (p && t[p] == '\n' && t[p - 1] == '\r') p--;
            if (ct) {
                while (p && !is_wc(t[p])) p--;
                while (p && is_wc(t[p - 1])) p--;
            }
        }
        move_to(e, p, sh);
        return;
    case 0x27:                                  /* rechts                */
        if (!sh && e->sa != e->sc) { move_to(e, e->sa > e->sc ? e->sa : e->sc, 0); return; }
        if (p < e->len) {
            if (t[p] == '\r' && t[p + 1] == '\n') p += 2; else p++;
            if (ct) {
                while (p < e->len && is_wc(t[p])) p++;
                while (p < e->len && !is_wc(t[p])) p++;
            }
        }
        move_to(e, p, sh);
        return;
    case 0x26: case 0x28: case 0x21: case 0x22: {   /* hoch/runter/Bild  */
        short d = vk == 0x26 ? -1 : vk == 0x28 ? 1 : vk == 0x21 ? -(r - 1) : r - 1;
        long nk = (long)k + d;
        if (!e->ml) return;
        if (e->xgoal == 0xFFFF) e->xgoal = col_of(e, k, p);
        if (nk < 0) nk = 0;
        if (nk >= (long)e->nl) nk = e->nl - 1;
        if (vk == 0x21 || vk == 0x22) {
            long nt = (long)e->top + d;
            if (nt < 0) nt = 0;
            if (nt > (long)e->nl - 1) nt = e->nl - 1;
            e->top = (WORD)nt;
        }
        move_to(e, pos_at(e, (WORD)nk, e->xgoal), sh);
        return;
    }
    case 0x24:                                  /* Pos1                  */
        move_to(e, ct ? 0 : LS(e)[k], sh);
        return;
    case 0x23:                                  /* Ende                  */
        move_to(e, ct ? e->len : line_end(e, k), sh);
        return;
    case 0x2E:                                  /* Entf                  */
        if (e->ro) return;
        if (sh) { copy_sel(e); repl(e, e->sa, e->sc, "", 0, 0); return; }
        if (e->sa != e->sc) { repl(e, e->sa, e->sc, "", 0, 0); return; }
        if (p < e->len)
            repl(e, p, p + ((t[p] == '\r' && t[p + 1] == '\n') ? 2 : 1), "", 0, 0);
        return;
    case 0x2D:                                  /* Einfg                 */
        if (ct) copy_sel(e);
        else if (sh && !e->ro) paste(e);
        return;
    }
}

static void chr(struct ed *e, BYTE c)
{
    char FAR *t = T(e);
    WORD p = e->sc;
    if (c == 3) { copy_sel(e); return; }        /* Strg+C               */
    if (e->ro) { MessageBeep_c(); return; }
    if (c == 0x18) { copy_sel(e); repl(e, e->sa, e->sc, "", 0, 0); return; }
    if (c == 0x16) { paste(e); return; }
    if (c == 0x1A) { undo(e); return; }
    if (c == 8) {
        if (e->sa != e->sc) { repl(e, e->sa, e->sc, "", 0, 0); return; }
        if (!p) return;
        repl(e, p - ((p >= 2 && t[p - 2] == '\r' && t[p - 1] == '\n') ? 2 : 1), p, "", 0, 0);
        return;
    }
    if (c == '\r') {
        if (e->ml) repl(e, e->sa, e->sc, "\r\n", 2, 0);
        return;
    }
    if (c == '\t' && !e->ml)
        return;
    if (c < ' ' && c != '\t')
        return;
    {
        static char b[1];
        b[0] = c;
        repl(e, e->sa, e->sc, b, 1, e->sa == e->sc);
    }
}

LONG FAR PASCAL edit_proc(HWND h, WORD m, WORD wp, DWORD lp)
{
    struct wnd *w = wptr(h);
    struct ed *e = eptr(h);
    if (!w)
        return 0;
    if (m == WM_CREATE) {
        short i;
        WORD hinst = *(WORD FAR *)((BYTE FAR *)lp + 4);
        for (i = 0; i < NED && ed[i].h; i++)
            ;
        if (i == NED)
            return -1;
        e = &ed[i];
        fmemset((void FAR *)e, 0, sizeof(*e));
        e->h = h;
        e->ml = (w->style & ES_MULTILINE) ? 1 : 0;
        e->wrap = e->ml && !(w->style & (WS_HSCROLL | ES_AUTOHSCROLL));
        e->ro = (w->style & ES_READONLY) ? 1 : 0;
        e->pwc = (w->style & ES_PASSWORD) && !e->ml ? '*' : 0;
        e->lim = e->ml ? 0 : 0x7FFF;
        e->xgoal = 0xFFFF;
        e->lseg = seg_new(e->ml ? NLINE * 2 : 16);
        if (task_of_inst(hinst) >= 0)
            e->ds = hinst;
        else {
            e->own = seg_new(1024);
            e->ds = e->own;
            if (!e->own || !lh_init(e->own, 16, 1022)) {
                e->h = 0;
                return -1;
            }
        }
        e->ht = lh_hnew(e->ds, 64, 1);
        if (!e->ht || !e->lseg) {
            e->h = 0;
            return -1;
        }
        e->len = 0;
        recalc(e);
        if (w->title[0])
            set_text(e, w->title);
        return 0;
    }
    if (!e)
        return DefWindowProc(h, m, wp, lp);
    switch (m) {
    case WM_DESTROY:
        if (e->ht) lh_free(e->ds, e->ht);
        if (e->own) seg_del(e->own);
        if (e->lseg) seg_del(e->lseg);
        if (e->useg) seg_del(e->useg);
        e->h = 0;
        return 0;
    case WM_SIZE:
        recalc(e);
        if (e->sc > e->len) e->sa = e->sc = e->len;
        show_caret(e);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(e, w);
        return 0;
    case WM_SETFOCUS:
        redraw(e);
        notify(e, EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        redraw(e);
        notify(e, EN_KILLFOCUS);
        return 0;
    case WM_ENABLE:
        redraw(e);
        return 0;
    case WM_SETTEXT:
        set_text(e, (LPCSTR)lp);
        return 1;
    case WM_GETTEXT: {
        WORD n = e->len < wp ? e->len : (wp ? wp - 1 : 0);
        if (!wp) return 0;
        fmemcpy((LPSTR)lp, T(e), n);
        ((LPSTR)lp)[n] = 0;
        return n;
    }
    case WM_GETTEXTLENGTH:
        return e->len;
    case WM_SETFONT:
    case WM_GETFONT:
        return 0;
    case WM_GETDLGCODE:
        return e->ml ? 0x0001 | 0x0080 | 0x0004 | 0x0008 : 0x0001 | 0x0080 | 0x0008;
    case WM_KEYDOWN:
        {                                       /* Feld einer COMBOBOX:   */
            struct wnd *pw = wptr(w->parent);   /* Liste blaettern        */
            if (pw && (w->style & 0x40000000UL) && pw->cls == 9 &&
                (wp == 0x26 || wp == 0x28 || wp == 0x21 || wp == 0x22 || wp == 0x73)) {
                send_msg(w->parent, WM_KEYDOWN, wp, lp);
                return 0;
            }
        }
        key(e, wp);
        return 0;
    case WM_CHAR:
        chr(e, (BYTE)wp);
        return 0;
    case WM_SYSCHAR:
        if (wp == 8) { undo(e); return 0; }     /* Alt+Ruecktaste        */
        return DefWindowProc(h, m, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        WORD p = hit(e, w, LOWORD(lp), HIWORD(lp));
        if (GetFocus() != h)
            SetFocus(h);
        if (m == WM_LBUTTONDBLCLK) {
            char FAR *t = T(e);
            WORD a = p, b = p;
            while (a && is_wc(t[a - 1])) a--;
            while (b < e->len && is_wc(t[b])) b++;
            e->sa = a;
            e->sc = b;
            show_caret(e);
            return 0;
        }
        e->drag = 1;
        SetCapture(h);
        move_to(e, p, GetKeyState(0x10) < 0);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->drag && (wp & 1)) {
            short x = LOWORD(lp), y = HIWORD(lp);
            if (y < 0 && e->top) e->top--;
            if (y >= rows(e) * FONT_H && e->top + rows(e) < e->nl) e->top++;
            move_to(e, hit(e, w, x, y), 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->drag) {
            e->drag = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_VSCROLL: {
        long nt = e->top;
        short r = rows(e);
        switch (wp) {
        case 0: nt--; break;
        case 1: nt++; break;
        case 2: nt -= r - 1; break;
        case 3: nt += r - 1; break;
        case 4: case 5: nt = (short)LOWORD(lp); break;
        default: return 0;
        }
        if (nt > (long)e->nl - 1) nt = e->nl - 1;
        if (nt < 0) nt = 0;
        e->top = (WORD)nt;
        sbars(e);
        redraw(e);
        return 0;
    }
    case WM_HSCROLL: {
        long nl = e->left;
        short c = cols(e);
        switch (wp) {
        case 0: nl--; break;
        case 1: nl++; break;
        case 2: nl -= c / 2; break;
        case 3: nl += c / 2; break;
        case 4: case 5: nl = (short)LOWORD(lp); break;
        default: return 0;
        }
        if (nl > (long)e->maxw) nl = e->maxw;
        if (nl < 0) nl = 0;
        e->left = (WORD)nl;
        sbars(e);
        redraw(e);
        return 0;
    }
    case WM_CUT: copy_sel(e); if (!e->ro) repl(e, e->sa, e->sc, "", 0, 0); return 0;
    case WM_COPY: copy_sel(e); return 0;
    case WM_PASTE: if (!e->ro) paste(e); return 0;
    case WM_CLEAR: if (!e->ro) repl(e, e->sa, e->sc, "", 0, 0); return 0;
    case WM_UNDO: case 0x0417: undo(e); return 1;
    case 0x0400:                                /* EM_GETSEL             */
        return MAKELONG(e->sa < e->sc ? e->sa : e->sc, e->sa < e->sc ? e->sc : e->sa);
    case 0x0401: {                              /* EM_SETSEL             */
        WORD a = LOWORD(lp), b = HIWORD(lp);
        if (a > e->len) a = e->len;
        if (b > e->len) b = e->len;
        if (a == 0 && b == 0xFFFF) b = e->len;
        e->sa = a;
        e->sc = b;
        e->utyp = 0;
        if (wp) redraw(e); else show_caret(e);
        return 1;
    }
    case 0x0402: {                              /* EM_GETRECT            */
        RECT FAR *r = (RECT FAR *)lp;
        GetClientRect(h, r);
        return 0;
    }
    case 0x0403: case 0x0404:                   /* EM_SETRECT(NP)        */
        return 0;
    case 0x0405:                                /* EM_SCROLL             */
        send_msg(h, WM_VSCROLL, wp, 0);
        return 1;
    case 0x0406: {                              /* EM_LINESCROLL         */
        long nt = (long)e->top + (short)LOWORD(lp), nl = (long)e->left + (short)HIWORD(lp);
        if (nt > (long)e->nl - 1) nt = e->nl - 1;
        if (nt < 0) nt = 0;
        if (nl < 0) nl = 0;
        e->top = (WORD)nt;
        e->left = e->wrap ? 0 : (WORD)nl;
        sbars(e);
        redraw(e);
        return 1;
    }
    case 0x0408: return e->mod;                 /* EM_GETMODIFY          */
    case 0x0409: e->mod = wp ? 1 : 0; return 0; /* EM_SETMODIFY          */
    case 0x040A: return e->nl;                  /* EM_GETLINECOUNT       */
    case 0x040B: {                              /* EM_LINEINDEX          */
        WORD k = wp == 0xFFFF ? line_of(e, e->sc) : wp;
        return k < e->nl ? (LONG)LS(e)[k] : -1L;
    }
    case 0x040C:                                /* EM_SETHANDLE          */
        if (e->own) {
            glog("EM_SETHANDLE ohne Programm-Halde");
            return 0;
        }
        e->ht = wp;
        e->len = wp ? fstrlen(T(e)) : 0;
        e->sa = e->sc = 0;
        e->top = e->left = 0;
        e->mod = 0;
        e->ucan = 0;
        recalc(e);
        sbars(e);
        redraw(e);
        return 0;
    case 0x040D:                                /* EM_GETHANDLE          */
        return e->ht;
    case 0x040E:                                /* EM_GETTHUMB           */
        return e->top;
    case 0x0411: {                              /* EM_LINELENGTH         */
        WORD k;
        if (wp == 0xFFFF) {
            WORD a = e->sa < e->sc ? e->sa : e->sc;
            k = line_of(e, a);
            return line_end(e, k) - LS(e)[k] - (e->sa > e->sc ? e->sa - e->sc : e->sc - e->sa);
        }
        k = line_of(e, wp);
        return line_end(e, k) - LS(e)[k];
    }
    case 0x0412:                                /* EM_REPLACESEL         */
        repl(e, e->sa, e->sc, (LPCSTR)lp, fstrlen((LPCSTR)lp), 0);
        return 0;
    case 0x0414: {                              /* EM_GETLINE            */
        WORD k = wp, n, max = *(WORD FAR *)lp;
        if (k >= e->nl) return 0;
        n = line_end(e, k) - LS(e)[k];
        if (n > max) n = max;
        fmemcpy((LPSTR)lp, T(e) + LS(e)[k], n);
        return n;
    }
    case 0x0415:                                /* EM_LIMITTEXT          */
        e->lim = wp ? wp : (e->ml ? 0 : 0x7FFF);
        return 0;
    case 0x0416: return e->ucan;                /* EM_CANUNDO            */
    case 0x0418: return 0;                      /* EM_FMTLINES           */
    case 0x0419:                                /* EM_LINEFROMCHAR       */
        return line_of(e, wp == 0xFFFF ? e->sc : wp);
    case 0x041B: return 1;                      /* EM_SETTABSTOPS        */
    case 0x041C: e->pwc = (BYTE)wp; redraw(e); return 0;
    case 0x041D: e->ucan = 0; return 0;         /* EM_EMPTYUNDOBUFFER    */
    case 0x041E: return e->top;                 /* EM_GETFIRSTVISIBLELINE */
    case 0x041F: e->ro = wp ? 1 : 0; return 1;  /* EM_SETREADONLY        */
    case 0x041A: case 0x0420: return 0;         /* Wortumbruch-Prozedur  */
    }
    return DefWindowProc(h, m, wp, lp);
}
