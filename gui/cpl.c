/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - was die Systemsteuerung (CONTROL.EXE und die .CPL-Module
 * aus Windows 3.1) ausser den Grundmodulen braucht (docs/GUI-WIN16.md
 * „Stand: Systemsteuerung“): LZEXPAND (mit Entpacken des SZDD-Formats von
 * COMPRESS/EXPAND), VER (Dateien installieren), Comm- und WNet-Stummel,
 * installierbare Treiber (OpenDriver & Co. - es gibt keine), DlgDirList.
 * Eigener Code nach dem Verhalten, das die Windows-3.1-SDK beschreibt.
 */
#include "gui.h"

WORD  API OpenFile(LPCSTR name, BYTE FAR *of, WORD st);
WORD  API _lopen(LPCSTR name, WORD mode);
WORD  API _lcreat(LPCSTR name, WORD attr);
WORD  API _lclose(WORD h);
WORD  API _lread(WORD h, LPVOID p, WORD n);
WORD  API _lwrite(WORD h, LPCSTR p, WORD n);
DWORD API _llseek(WORD h, DWORD pos, short how);
WORD  API GetSystemDirectory(LPSTR buf, WORD n);
void  API GetWindowRect(HWND h, LPRECT r);
void  API GetClientRect(HWND h, LPRECT r);
void  API ScreenToClient(HWND h, POINT FAR *p);
void  API ClientToScreen(HWND h, POINT FAR *p);
BOOL  API IsWindowVisible(HWND h);
HWND  API GetWindow(HWND h, WORD cmd);
HWND  API GetDlgItem(HWND d, short id);
BOOL  API SetDlgItemText(HWND d, short id, LPCSTR s);
LONG  API SendMessage(HWND h, WORD m, WORD wp, DWORD lp);

/* ---- LZEXPAND --------------------------------------------------------
   Dateien sind gewoehnliche Handles; gepackt (SZDD: "SZDD" 88 F0 27 33,
   'A', letztes Zeichen des Namens, Groesse) entpackt nur LZCopy/
   CopyLZFile - so installieren die Systemsteuerung und SETUP von Disketten.
   LZRead liest roh (gepackte Dateien liest kein Programm der Systemsteuerung
   stueckweise). */
static const BYTE __far szdd[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };

static int is_szdd(const BYTE *h)
{
    WORD i;
    for (i = 0; i < 8; i++)
        if (h[i] != szdd[i])
            return 0;
    return 1;
}

WORD API LZOpenFile(LPSTR name, BYTE FAR *of, WORD st) { return OpenFile(name, of, st); }
WORD API LZInit(WORD h) { return h; }
DWORD API LZSeek(WORD h, DWORD pos, short how) { return _llseek(h, pos, how); }
short API LZRead(WORD h, LPSTR p, short n) { return _lread(h, p, n); }
void API LZClose(WORD h) { _lclose(h); }
short API LZStart(void) { return 1; }
void API LZDone(void) { }

/* SZDD entpacken: Ring 4 KB mit Leerzeichen, Start 4096 - 16; je
   Merkerbyte 8 Schritte: 1 = Zeichen, 0 = Stelle (12 Bit) + Laenge (4 + 3) */
#define LZ_IN   4096
#define LZ_OUT  4608
static WORD lz_sel, lz_ip, lz_in, lz_op, lz_src, lz_dst;
static long lz_tot;
static int lz_get(void)
{
    BYTE FAR *b = MKFP(lz_sel, 0);
    if (lz_ip >= lz_in) {
        lz_in = _lread(lz_src, b + LZ_IN, 512);
        lz_ip = 0;
        if (!lz_in || lz_in == 0xFFFF) {
            lz_in = 0;
            return -1;
        }
    }
    return b[LZ_IN + lz_ip++];
}
static int lz_flush(void)
{
    BYTE FAR *b = MKFP(lz_sel, 0);
    if (lz_op && _lwrite(lz_dst, (LPCSTR)(b + LZ_OUT), lz_op) != lz_op)
        return -1;
    lz_tot += lz_op;
    lz_op = 0;
    return 0;
}
static int lz_put(BYTE c)
{
    BYTE FAR *b = MKFP(lz_sel, 0);
    b[LZ_OUT + lz_op++] = c;
    return lz_op < 512 ? 0 : lz_flush();
}
long API LZCopy(WORD src, WORD dst)
{
    BYTE FAR *b;
    WORD i, r, n;
    int c, f, k, lo, hi;
    static BYTE hd[14];
    lz_sel = seg_new(LZ_OUT + 512);
    if (!lz_sel)
        return -5;                              /* LZERROR_GLOBALLOC     */
    b = MKFP(lz_sel, 0);
    lz_src = src; lz_dst = dst; lz_ip = lz_in = lz_op = 0; lz_tot = 0;
    n = _lread(src, (LPVOID)hd, 14);
    if (n == 14 && is_szdd(hd) && hd[8] == 'A') {
        for (i = 0; i < 4096; i++)
            b[i] = ' ';
        r = 4096 - 16;
        for (;;) {
            if ((f = lz_get()) < 0)
                break;
            for (k = 0; k < 8; k++, f >>= 1) {
                if (f & 1) {
                    if ((c = lz_get()) < 0)
                        goto done;
                    b[r] = (BYTE)c;
                    r = (r + 1) & 4095;
                    if (lz_put((BYTE)c))
                        goto err;
                } else {
                    if ((lo = lz_get()) < 0 || (hi = lz_get()) < 0)
                        goto done;
                    i = lo | ((hi & 0xF0) << 4);
                    for (c = (hi & 15) + 3; c > 0; c--) {
                        BYTE x = b[i];
                        i = (i + 1) & 4095;
                        b[r] = x;
                        r = (r + 1) & 4095;
                        if (lz_put(x))
                            goto err;
                    }
                }
            }
        }
    } else {                                    /* ungepackt: kopieren  */
        _llseek(src, 0, 0);
        while ((n = _lread(src, b + LZ_IN, 512)) != 0 && n != 0xFFFF)
            if (_lwrite(dst, (LPCSTR)(b + LZ_IN), n) != n)
                goto err;
            else
                lz_tot += n;
    }
done:
    if (lz_flush())
        goto err;
    seg_del(lz_sel);
    return lz_tot;
err:
    seg_del(lz_sel);
    return -4;                                  /* LZERROR_WRITE         */
}
long API CopyLZFile(WORD src, WORD dst) { return LZCopy(src, dst); }

/* Name nach dem Entpacken: endet er auf '_', steht das Zeichen im Kopf */
short API GetExpandedName(LPCSTR src, LPSTR dst)
{
    static BYTE hd[10];
    WORD h, n = fstrlen(src);
    fstrncpy(dst, src, 128);
    if (!n || src[n - 1] != '_')
        return 1;
    h = _lopen(src, 0);
    if (h == 0xFFFF)
        return 1;
    if (_lread(h, (LPVOID)hd, 10) == 10 && is_szdd(hd) && hd[9])
        dst[n - 1] = hd[9];
    _lclose(h);
    return 1;
}

/* ---- VER -------------------------------------------------------------
   Versionsdaten liest niemand mit (VS_VERSION_INFO bleibt unbekannt);
   VerFindFile schlaegt das Systemverzeichnis vor, VerInstallFile kopiert
   (und entpackt) Quelle -> Ziel. */
WORD API GetFileResourceSize(LPCSTR f, LPCSTR t, LPCSTR id, DWORD FAR *off)
{ (void)f; (void)t; (void)id; if (off) *off = 0; return 0; }
BOOL API GetFileResource(LPCSTR f, LPCSTR t, LPCSTR id, DWORD off, DWORD n, LPVOID p)
{ (void)f; (void)t; (void)id; (void)off; (void)n; (void)p; return 0; }
DWORD API GetFileVersionInfoSize(LPCSTR f, DWORD FAR *h) { (void)f; if (h) *h = 0; return 0; }
BOOL API GetFileVersionInfo(LPCSTR f, DWORD h, DWORD n, LPVOID p)
{ (void)f; (void)h; (void)n; (void)p; return 0; }
WORD API VerLanguageName(WORD lang, LPSTR p, WORD n) { (void)lang; if (n) p[0] = 0; return 0; }
BOOL API VerQueryValue(const void FAR *b, LPCSTR s, LPVOID FAR *p, WORD FAR *n)
{ (void)b; (void)s; if (p) *p = 0; if (n) *n = 0; return 0; }

static void dir_put(LPSTR d, WORD FAR *n, LPCSTR s)
{
    WORD l = fstrlen(s);
    if (l + 1 > *n) {
        *n = l + 1;
        return;
    }
    fstrncpy(d, s, *n);
    *n = l + 1;
}
WORD API VerFindFile(WORD fl, LPCSTR file, LPCSTR win, LPCSTR app, LPSTR cur,
                     WORD FAR *curn, LPSTR dst, WORD FAR *dstn)
{
    static char sys[80];
    (void)fl; (void)file; (void)win; (void)app;
    GetSystemDirectory(sys, sizeof(sys));
    dir_put(cur, curn, "");                     /* nicht vorhanden       */
    dir_put(dst, dstn, sys);
    return 0;
}

static char vi_a[FAPI_PATHLEN], vi_b[FAPI_PATHLEN];
static void cat_path(char *o, LPCSTR dir, LPCSTR name)
{
    WORD n;
    fstrncpy(o, dir, FAPI_PATHLEN - 14);
    n = fstrlen(o);
    if (n && o[n - 1] != '\\' && o[n - 1] != ':')
        o[n++] = '\\';
    fstrncpy(o + n, name, FAPI_PATHLEN - n);
}
DWORD API VerInstallFile(WORD fl, LPCSTR sf, LPCSTR df, LPCSTR sd, LPCSTR dd,
                         LPCSTR cd, LPSTR tmp, WORD FAR *tmpn)
{
    WORD s, d;
    long r;
    (void)fl; (void)cd;
    if (tmpn && *tmpn)
        tmp[0] = 0;
    cat_path(vi_a, sd, sf);
    cat_path(vi_b, dd, df);
    s = _lopen(vi_a, 0);
    if (s == 0xFFFF)
        return 0x0010;                          /* VIF_CANNOTREADSRC     */
    d = _lcreat(vi_b, 0);
    if (d == 0xFFFF) {
        _lclose(s);
        return 0x0800;                          /* VIF_CANNOTCREATE      */
    }
    r = LZCopy(s, d);
    _lclose(s);
    _lclose(d);
    return r < 0 ? 0x0200 : 0;                  /* VIF_OUTOFSPACE        */
}

/* ---- Comm, WNet, installierbare Treiber ----------------------------- */
short API OpenComm(LPCSTR name, WORD inq, WORD outq) { (void)name; (void)inq; (void)outq; return -10; }
short API CloseComm(short id) { (void)id; return -1; }
LONG API EscapeCommFunction(short id, short fn) { (void)id; (void)fn; return -1; }

WORD API WNetGetCaps(WORD idx) { (void)idx; return 0; }       /* kein Netz */
WORD API WNetGetConnection(LPSTR l, LPSTR r, WORD FAR *n) { (void)l; (void)r; (void)n; return 1; }
WORD API WNetDeviceMode(HWND h) { (void)h; return 1; }
WORD API WNetBrowseDialog(HWND h, WORD t, LPSTR p) { (void)h; (void)t; (void)p; return 1; }
WORD API WNetAddConnection(LPSTR r, LPSTR pw, LPSTR l) { (void)r; (void)pw; (void)l; return 1; }
WORD API WNetCancelConnection(LPSTR n, BOOL f) { (void)n; (void)f; return 1; }
WORD API WNetGetError(WORD FAR *e) { if (e) *e = 0; return 1; }
WORD API WNetGetErrorText(WORD e, LPSTR p, WORD FAR *n) { (void)e; if (n && *n) p[0] = 0; return 1; }
WORD API WNetConnectionDialog(HWND h, WORD t) { (void)h; (void)t; return 1; }

WORD API OpenDriver(LPCSTR n, LPCSTR s, DWORD lp) { (void)n; (void)s; (void)lp; return 0; }
LONG API CloseDriver(WORD h, DWORD a, DWORD b) { (void)h; (void)a; (void)b; return 0; }
LONG API SendDriverMessage(WORD h, WORD m, DWORD a, DWORD b) { (void)h; (void)m; (void)a; (void)b; return 0; }
WORD API GetDriverModuleHandle(WORD h) { (void)h; return 0; }
WORD API GetNextDriver(WORD h, DWORD fl) { (void)h; (void)fl; return 0; }

/* ---- kleine USER-Reste ---------------------------------------------- */
void API ClipCursor(const RECT FAR *r) { (void)r; }
void API SetCaretBlinkTime(WORD ms) { (void)ms; }
BOOL API EqualRect(const RECT FAR *a, const RECT FAR *b)
{
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

/* Kind an einer Stelle (Client-Koordinaten des Elternfensters), auch
   unsichtbare/gesperrte wie Windows; sonst das Fenster selbst oder 0 */
HWND API ChildWindowFromPoint(HWND h, POINT p)
{
    HWND c;
    RECT r;
    POINT a, b;
    GetClientRect(h, &r);
    if (p.x < r.left || p.y < r.top || p.x >= r.right || p.y >= r.bottom)
        return 0;
    for (c = GetWindow(h, 5); c; c = GetWindow(c, 2)) {     /* GW_CHILD, NEXT */
        GetWindowRect(c, &r);
        a.x = r.left; a.y = r.top; b.x = r.right; b.y = r.bottom;
        ScreenToClient(h, &a);
        ScreenToClient(h, &b);
        if (p.x >= a.x && p.y >= a.y && p.x < b.x && p.y < b.y)
            return c;
    }
    return h;
}

/* DlgDirList: Liste mit Dateien des Musters (Verzeichnisteil wechselt das
   aktuelle Verzeichnis), [Verzeichnisse] und [-x-] Laufwerke nach attr */
#define LB_RESETCONTENT 0x0405
#define LB_ADDSTRING    0x0401
#define LB_GETCURSEL    0x0409
#define LB_GETTEXT      0x040A
static struct fapi_dirent dd_e;
static char dd_pat[80], dd_ln[24];
short API DlgDirList(HWND d, LPSTR spec, short idl, short ids, WORD attr)
{
    HWND lb = idl ? GetDlgItem(d, idl) : 0;
    WORD i, x, k = 0;
    for (i = 0; spec && spec[i]; i++)
        if (spec[i] == '\\' || spec[i] == ':')
            k = i + 1;
    if (k) {
        fstrncpy(vi_a, spec, FAPI_PATHLEN);
        vi_a[k > 1 && vi_a[k - 1] == '\\' && vi_a[k - 2] != ':' ? k - 1 : k] = 0;
        if (FAPI_FAILED(DosChDir(vi_a)))
            return 0;
    }
    fstrncpy(dd_pat, spec && spec[k] ? spec + k : "*.*", sizeof(dd_pat));
    if (spec && k)
        fstrncpy(spec, dd_pat, 80);
    if (lb) {
        SendMessage(lb, LB_RESETCONTENT, 0, 0);
        for (i = 0; ; i++) {
            x = fapi_call(FAPI_FINDENTRY, (WORD)&dd_e, i, 0);
            if (FAPI_FAILED(x))
                break;
            if (dd_e.attr & 0x10) {
                if (!(attr & 0x10) || (dd_e.name[0] == '.' && !dd_e.name[1]))
                    continue;
                dd_ln[0] = '[';
                fstrncpy(dd_ln + 1, dd_e.name, 20);
                x = fstrlen(dd_ln);
                dd_ln[x] = ']'; dd_ln[x + 1] = 0;
            } else {
                if ((attr & 0x8000u) || (dd_e.attr & 0x06 & ~attr) ||
                    !wmatch(dd_pat, dd_e.name))
                    continue;
                fstrncpy(dd_ln, dd_e.name, sizeof(dd_ln));
            }
            for (x = 0; dd_ln[x]; x++)
                if (dd_ln[x] >= 'A' && dd_ln[x] <= 'Z') dd_ln[x] += 32;
            SendMessage(lb, LB_ADDSTRING, 0, (DWORD)(void FAR *)dd_ln);
        }
        if (attr & 0x4000)                      /* DDL_DRIVES           */
            for (i = 0; i < 26; i++) {
                if (FAPI_FAILED(DosQCurDirD(i, vi_b)))
                    continue;
                dd_ln[0] = '['; dd_ln[1] = '-'; dd_ln[2] = (char)('a' + i);
                dd_ln[3] = '-'; dd_ln[4] = ']'; dd_ln[5] = 0;
                SendMessage(lb, LB_ADDSTRING, 0, (DWORD)(void FAR *)dd_ln);
            }
    }
    if (ids) {
        vi_b[0] = (char)('A' + DosQCurDisk()); vi_b[1] = ':'; vi_b[2] = '\\';
        DosQCurDir(vi_b + 3);
        for (x = 0; vi_b[x]; x++)
            if (vi_b[x] >= 'A' && vi_b[x] <= 'Z') vi_b[x] += 32;
        SetDlgItemText(d, ids, vi_b);
    }
    return 1;
}

/* Auswahl der Liste -> Name (Verzeichnis mit '\', Laufwerk mit ':');
   TRUE, wenn es ein Verzeichnis oder Laufwerk war */
BOOL API DlgDirSelect(HWND d, LPSTR out, short idl)
{
    HWND lb = GetDlgItem(d, idl);
    short i = (short)SendMessage(lb, LB_GETCURSEL, 0, 0);
    WORD n;
    out[0] = 0;
    if (i < 0)
        return 0;
    SendMessage(lb, LB_GETTEXT, i, (DWORD)(void FAR *)dd_ln);
    if (dd_ln[0] == '[' && dd_ln[1] == '-') {
        out[0] = dd_ln[2]; out[1] = ':'; out[2] = 0;
        return 1;
    }
    if (dd_ln[0] == '[') {
        n = fstrlen(dd_ln);
        fstrncpy(out, dd_ln + 1, 20);
        out[n - 2] = '\\'; out[n - 1] = 0;
        return 1;
    }
    fstrncpy(out, dd_ln, 20);
    return 0;
}

/* fuer den Datei-Manager: Netzfunktionen ohne Netz (WN_NOT_SUPPORTED = 1) */
WORD API WNetErrorText(WORD e, LPSTR p, WORD n) { (void)e; if (p && n) p[0] = 0; return 0; }
WORD API WNetRestoreConnection(HWND h, LPSTR d) { (void)h; (void)d; return 1; }
WORD API WNetConnectDialog(HWND h, WORD t) { (void)h; (void)t; return 1; }
WORD API WNetDisconnectDialog(HWND h, WORD t) { (void)h; (void)t; return 1; }
WORD API WNetPropertyDialog(HWND h, WORD b, WORD s, LPSTR n, WORD t)
{ (void)h; (void)b; (void)s; (void)n; (void)t; return 1; }
WORD API WNetGetDirectoryType(LPSTR n, short FAR *t) { (void)n; if (t) *t = 0; return 0; }
WORD API WNetDirectoryNotify(HWND h, LPSTR d, WORD o) { (void)h; (void)d; (void)o; return 0; }
WORD API WNetGetPropertyText(WORD b, WORD s, LPSTR n, LPSTR bn, WORD cb, WORD t)
{ (void)b; (void)s; (void)n; (void)cb; (void)t; if (bn && cb) bn[0] = 0; return 1; }
