/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - weitere Teile von USER, KERNEL, KEYBOARD und SHELL, die der
 * Programm-Manager aus Windows 3.1 braucht (docs/GUI-WIN16.md „Stand:
 * PROGMAN“): Fensterreihenfolge, Lage speichern, Schreibmarke (Caret),
 * Bildlauf, Haken, Windows beenden, Tastennamen, Laufwerksart. Eigener
 * Code nach dem Verhalten, das die Windows-3.1-SDK beschreibt.
 */
#include "gui.h"

BOOL API MoveWindow(HWND h, short x, short y, short cx, short cy, BOOL rep);
BOOL API SetWindowPos(HWND h, HWND after, short x, short y, short cx, short cy, WORD f);
void API GetWindowRect(HWND h, LPRECT r);
void API ScreenToClient(HWND h, POINT FAR *p);
HDC  API GetDC(HWND h);
short API ReleaseDC(HWND h, HDC dc);
BOOL API PatBlt(HDC h, short x, short y, short w, short hh, DWORD rop);
BOOL API TextOut(HDC h, short x, short y, LPCSTR s, short n);
DWORD API SetTextColor(HDC h, DWORD c);
BOOL API IsIconic(HWND h);
BOOL API IsZoomed(HWND h);
int gdi_rgnbox(WORD h, RECT FAR *r);

#define WS_CHILD        0x40000000UL
#define WS_MINIMIZE     0x20000000UL
#define WS_MAXIMIZE     0x01000000UL

/* ---- GetWindow: Geschwister nach Rang (oben zuerst) ---------------- */
static HWND sib(HWND par, WORD z, int how)
{
    WORD i, best = 0, bz = 0;
    for (i = 0; i < NWND; i++) {
        struct wnd *o = &ww[i];
        if (!o->used || o->parent != par || HW(i) == desktop)
            continue;
        if (how == 1 ? (!best || o->z > bz) :           /* oberstes      */
            how == 2 ? (!best || o->z < bz) :           /* unterstes     */
            how == 3 ? (o->z > z && (!best || o->z < bz)) :  /* darueber */
                       (o->z < z && (!best || o->z > bz))) { /* darunter */
            best = i + 1;
            bz = o->z;
        }
    }
    return best ? HW(best - 1) : 0;
}
HWND API GetWindow(HWND h, WORD cmd)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    switch (cmd) {
    case 0: return sib(w->parent, 0, 1);        /* GW_HWNDFIRST          */
    case 1: return sib(w->parent, 0, 2);        /* GW_HWNDLAST           */
    case 2: return sib(w->parent, w->z, 0);     /* GW_HWNDNEXT (darunter) */
    case 3: return sib(w->parent, w->z, 3);     /* GW_HWNDPREV           */
    case 4: return w->owner;                    /* GW_OWNER              */
    case 5: return sib(h, 0, 1);                /* GW_CHILD              */
    }
    return 0;
}

/* ---- Lage speichern/herstellen (WINDOWPLACEMENT, 22 Byte) ---------- */
struct wpl { WORD len, flags, show; POINT mn, mx; RECT nr; };
BOOL API GetWindowPlacement(HWND h, struct wpl FAR *p)
{
    struct wnd *w = wptr(h);
    struct wnd *pw;
    if (!w)
        return 0;
    pw = (w->style & WS_CHILD) ? wptr(w->parent) : 0;
    p->len = 22; p->flags = 0;
    p->show = !w->visible ? 0 : (w->style & WS_MINIMIZE) ? 2 : (w->style & WS_MAXIMIZE) ? 3 : 1;
    p->mn.x = p->mn.y = p->mx.x = p->mx.y = -1;
    if (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) {
        fmemcpy(&p->nr, &w->norm, sizeof(RECT));
        if (w->style & WS_MINIMIZE) {
            p->mn.x = w->wr.left - (pw ? pw->cr.left : 0);
            p->mn.y = w->wr.top - (pw ? pw->cr.top : 0);
        }
    } else {
        p->nr.left = w->wr.left - (pw ? pw->cr.left : 0);
        p->nr.top = w->wr.top - (pw ? pw->cr.top : 0);
        p->nr.right = p->nr.left + (w->wr.right - w->wr.left);
        p->nr.bottom = p->nr.top + (w->wr.bottom - w->wr.top);
    }
    return 1;
}
BOOL API SetWindowPlacement(HWND h, const struct wpl FAR *p)
{
    struct wnd *w = wptr(h);
    if (!w)
        return 0;
    if (w->style & (WS_MINIMIZE | WS_MAXIMIZE))
        fmemcpy(&w->norm, &p->nr, sizeof(RECT));
    else
        MoveWindow(h, p->nr.left, p->nr.top, p->nr.right - p->nr.left,
                   p->nr.bottom - p->nr.top, 1);
    ShowWindow(h, p->show);
    return 1;
}

/* ---- Schreibmarke: ein Rechteck, invertiert gezeichnet, ohne Blinken */
static HWND ct_h;
static short ct_x, ct_y, ct_w, ct_h2, ct_show;
static BYTE ct_on;
static void ct_xor(void)
{
    HDC dc = GetDC(ct_h);
    if (dc) {
        PatBlt(dc, ct_x, ct_y, ct_w, ct_h2, 0x00550009UL);   /* DSTINVERT */
        ReleaseDC(ct_h, dc);
    }
    ct_on ^= 1;
}
static void ct_off(void) { if (ct_on && wptr(ct_h)) ct_xor(); ct_on = 0; }
static void ct_upd(void) { if (!ct_on && ct_show > 0 && wptr(ct_h)) ct_xor(); }
void API CreateCaret(HWND h, WORD bm, short cx, short cy)
{
    (void)bm;
    ct_off();
    ct_h = h; ct_w = cx ? cx : 2; ct_h2 = cy ? cy : FONT_H;
    ct_x = ct_y = 0; ct_show = 0;
}
void API DestroyCaret(void) { ct_off(); ct_h = 0; ct_show = 0; }
void API SetCaretPos(short x, short y) { ct_off(); ct_x = x; ct_y = y; ct_upd(); }
void API HideCaret(HWND h)
{
    if (ct_h && (!h || h == ct_h)) {
        ct_show--;
        ct_off();
    }
}
void API ShowCaret(HWND h)
{
    if (ct_h && (!h || h == ct_h) && ct_show < 1) {
        ct_show++;
        ct_upd();
    }
}

/* ---- Bildlauf: Kinder verschieben, alles neu zeichnen lassen -------- */
void API ScrollWindow(HWND h, short dx, short dy, const RECT FAR *r, const RECT FAR *clip)
{
    WORD i;
    (void)clip;
    ct_off();
    if (!r)                                     /* nur ohne r: Kinder mit */
        for (i = 0; i < NWND; i++) {
            RECT wr;
            POINT p;
            if (!ww[i].used || ww[i].parent != h || !(ww[i].style & WS_CHILD))
                continue;
            GetWindowRect(HW(i), &wr);
            p.x = wr.left; p.y = wr.top;
            ScreenToClient(h, &p);
            SetWindowPos(HW(i), 0, p.x + dx, p.y + dy, 0, 0, 0x0015);  /* NOSIZE|NOZORDER|NOACTIVATE */
        }
    InvalidateRect(h, r, 1);
}
void API InvalidateRgn(HWND h, WORD rgn, BOOL erase)
{
    RECT r;
    if (!rgn)
        InvalidateRect(h, 0, erase);
    else if (gdi_rgnbox(rgn, &r))
        InvalidateRect(h, &r, erase);
}

/* ---- Zeichnen ------------------------------------------------------ */
void API DrawFocusRect(HDC dc, const RECT FAR *r)
{
    short w = r->right - r->left, hh = r->bottom - r->top;
    if (w <= 0 || hh <= 0)
        return;
    PatBlt(dc, r->left, r->top, w, 1, 0x00550009UL);
    PatBlt(dc, r->left, r->bottom - 1, w, 1, 0x00550009UL);
    if (hh > 2) {
        PatBlt(dc, r->left, r->top + 1, 1, hh - 2, 0x00550009UL);
        PatBlt(dc, r->right - 1, r->top + 1, 1, hh - 2, 0x00550009UL);
    }
}
/* grauer Text; mit eigener Ausgabe (proc) ruft Windows sie auf - hier
   nur der Fall ohne (Text) */
BOOL API GrayString(HDC dc, HBRUSH br, void FAR *proc, LPCSTR s, short n,
                    short x, short y, short cx, short cy)
{
    DWORD tc;
    (void)br; (void)cx; (void)cy;
    if (proc || !s)
        return 0;
    if (!n)
        n = fstrlen(s);
    tc = SetTextColor(dc, 0x808080UL);
    TextOut(dc, x, y, s, n);
    SetTextColor(dc, tc);
    return 1;
}

/* ---- Haken (SetWindowsHook): angenommen, aber nie gerufen ---------- */
DWORD API SetWindowsHook(short id, DWORD proc) { (void)id; (void)proc; return 0; }
BOOL API UnhookWindowsHook(short id, DWORD proc) { (void)id; (void)proc; return 1; }
DWORD API DefHookProc(short code, WORD wp, DWORD lp, DWORD FAR *next)
{
    (void)code; (void)wp; (void)lp; (void)next;
    return 0;
}
HWND API SetSysModalWindow(HWND h) { (void)h; return 0; }
/* Schlangen haben feste Groesse - Wunsch angenommen */
BOOL API SetMessageQueue(short n) { (void)n; return 1; }
/* ohne Wirkung - im Windows 3.1-SDK nicht beschrieben */
DWORD API GetInternalIconHeader(void) { return 0; }

/* ---- Windows beenden: alle oberen Fenster fragen, dann Ende -------- */
#define WM_QUERYENDSESSION 0x0011
#define WM_ENDSESSION      0x0016
BOOL API ExitWindows(DWORD rc, WORD res)
{
    WORD i;
    int t;
    (void)rc; (void)res;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && !(ww[i].style & WS_CHILD) && HW(i) != desktop &&
            !ww[i].internal && !send_msg(HW(i), WM_QUERYENDSESSION, 0, 0))
            return 0;
    for (i = 0; i < NWND; i++)
        if (ww[i].used && !(ww[i].style & WS_CHILD) && HW(i) != desktop && !ww[i].internal)
            send_msg(HW(i), WM_ENDSESSION, 1, 0);
    for (t = 0; t < NWT; t++)
        if (wt[t].used && t != cur_task && wt[t].state != WT_DEAD)
            task_exit(t, 0);
    if (cur_task >= 0)
        task_exit(cur_task, 0);
    return 1;
}

/* ---- Zeichenklassen (ANSI = Windows-Latin-1) ------------------------ */
static int alpha(BYTE c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}
BOOL API IsCharAlpha(WORD c) { return alpha((BYTE)c); }
BOOL API IsCharAlphaNumeric(WORD c) { return alpha((BYTE)c) || ((BYTE)c >= '0' && (BYTE)c <= '9'); }
WORD API AnsiUpperBuff(LPSTR s, WORD n)
{
    WORD i, k = n ? n : 65535u;
    for (i = 0; i < k && (n || s[i]); i++) {
        BYTE c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c != 0xF7 && c != 0xFF))
            s[i] = c - 32;
    }
    return i;
}

/* ---- KERNEL --------------------------------------------------------- */
WORD API GetDriveType(WORD d)
{
    static struct fapi_fsinfo fi;
    WORD r;
    fmemset(&fi, 0, sizeof fi);
    r = DosQFSInfo(d, &fi);
    if (FAPI_FAILED(r))
        return 0;
    if (fi.flags & FSI_REMOTE)
        return 4;                               /* DRIVE_REMOTE          */
    if (d < 2 || (fi.flags & FSI_REMDISK))
        return 2;                               /* DRIVE_REMOVABLE       */
    return 3;                                   /* DRIVE_FIXED (auch CD) */
}
WORD API GetNumTasks(void)
{
    WORD n = 0;
    int t;
    for (t = 0; t < NWT; t++)
        if (wt[t].used && wt[t].state != WT_DEAD)
            n++;
    return n;
}
DWORD API GetFreeSpace(WORD fl)
{
    static struct fapi_meminfo mi;
    (void)fl;
    fmemset(&mi, 0, sizeof mi);
    if (FAPI_FAILED(DosMemInfo(&mi)))
        return 0x100000UL;
    return (mi.free_kb32 > 0x3FFFFFUL ? 0x3FFFFFUL : mi.free_kb32) << 10;
}
BOOL API IsRomModule(WORD h) { (void)h; return 0; }
DWORD API GlobalLock(WORD h);
DWORD API LockResource(WORD h) { return GlobalLock(h); }

/* ---- KEYBOARD ------------------------------------------------------- */
/* Scancode <-> virtuelle Taste fuer die Tasten, die Programme nennen */
static const BYTE vk_sc[][2] = {
    { 0x08, 0x0E }, { 0x09, 0x0F }, { 0x0D, 0x1C }, { 0x10, 0x2A }, { 0x11, 0x1D },
    { 0x12, 0x38 }, { 0x1B, 0x01 }, { 0x20, 0x39 }, { 0x21, 0x49 }, { 0x22, 0x51 },
    { 0x23, 0x4F }, { 0x24, 0x47 }, { 0x25, 0x4B }, { 0x26, 0x48 }, { 0x27, 0x4D },
    { 0x28, 0x50 }, { 0x2D, 0x52 }, { 0x2E, 0x53 },
    { 0x70, 0x3B }, { 0x71, 0x3C }, { 0x72, 0x3D }, { 0x73, 0x3E }, { 0x74, 0x3F },
    { 0x75, 0x40 }, { 0x76, 0x41 }, { 0x77, 0x42 }, { 0x78, 0x43 }, { 0x79, 0x44 },
    { 0x7A, 0x57 }, { 0x7B, 0x58 }, { 0, 0 }
};
static const char sc_let[] = "QWERTZUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0YXCVBNM";
static const char sc_dig[] = "1234567890";
static WORD vk2sc(WORD vk)
{
    WORD i;
    for (i = 0; vk_sc[i][0]; i++)
        if (vk_sc[i][0] == vk)
            return vk_sc[i][1];
    for (i = 0; i < sizeof(sc_let) - 1; i++)
        if (sc_let[i] == (char)vk)
            return 0x10 + i;
    if (vk >= '1' && vk <= '9')
        return 2 + vk - '1';
    if (vk == '0')
        return 0x0B;
    return 0;
}
static WORD sc2vk(WORD sc)
{
    WORD i;
    for (i = 0; vk_sc[i][0]; i++)
        if (vk_sc[i][1] == sc)
            return vk_sc[i][0];
    if (sc >= 0x10 && sc < 0x10 + sizeof(sc_let) - 1 && sc_let[sc - 0x10])
        return (BYTE)sc_let[sc - 0x10];
    if (sc >= 2 && sc <= 0x0B)
        return (BYTE)sc_dig[sc - 2];
    return 0;
}
WORD API MapVirtualKey(WORD code, WORD type)
{
    if (type == 0)
        return vk2sc(code);
    if (type == 1)
        return sc2vk(code);
    if (type == 2) {                            /* VK -> Zeichen         */
        if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ')
            return code;
        return 0;
    }
    return 0;
}
static const char *const kn[] = {
    "\x0E" "R\xFC" "cktaste", "\x0F" "Tab", "\x1C" "Eingabe", "\x2A" "Umschalt",
    "\x1D" "Strg", "\x38" "Alt", "\x01" "Esc", "\x39" "Leertaste", "\x49" "Bild auf",
    "\x51" "Bild ab", "\x4F" "Ende", "\x47" "Pos1", "\x4B" "Nach links",
    "\x48" "Nach oben", "\x4D" "Nach rechts", "\x50" "Nach unten", "\x52" "Einfg",
    "\x53" "Entf", 0
};
short API GetKeyNameText(DWORD lp, LPSTR buf, short n)
{
    WORD sc = (WORD)(lp >> 16) & 0xFF, i, vk;
    static char t[4];
    const char *s = 0;
    if (n <= 0)
        return 0;
    for (i = 0; kn[i]; i++)
        if ((BYTE)kn[i][0] == sc)
            s = kn[i] + 1;
    if (!s && sc >= 0x3B && sc <= 0x44) {       /* F1-F10                */
        t[0] = 'F'; t[1] = (char)('1' + sc - 0x3B); t[2] = 0;
        if (sc == 0x44) { t[1] = '1'; t[2] = '0'; t[3] = 0; }
        s = t;
    } else if (!s && (sc == 0x57 || sc == 0x58)) {
        t[0] = 'F'; t[1] = '1'; t[2] = (char)(sc == 0x57 ? '1' : '2'); t[3] = 0;
        s = t;
    } else if (!s && (vk = sc2vk(sc)) != 0) {
        t[0] = (char)vk; t[1] = 0;
        s = t;
    }
    if (!s) {
        buf[0] = 0;
        return 0;
    }
    fstrncpy(buf, s, n);
    return fstrlen(buf);
}

/* ---- SHELL ---------------------------------------------------------- */
WORD API WinExec(LPCSTR cmd, WORD show);
short API GetProfileString(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR buf, short n);
void API DragQueryPoint(WORD hd, POINT FAR *p) { (void)hd; p->x = p->y = 0; }
BOOL API RegisterShellHook(HWND h, WORD fn) { (void)h; (void)fn; return 0; }
/* Umgebungsvariablen in s ersetzen: hier unveraendert */
DWORD API DoEnvironmentSubst(LPSTR s, WORD n)
{
    (void)n;
    return MAKELONG(1, fstrlen(s));
}
static char sx_cmd[FAPI_PATHLEN + 128], sx_as[128];
static int exe_like(LPCSTR f)
{
    LPCSTR e = 0;
    for (; *f; f++)
        if (*f == '.') e = f;
        else if (*f == '\\') e = 0;
    return !e || !fstricmp(e, ".EXE") || !fstricmp(e, ".COM") || !fstricmp(e, ".BAT") ||
           !fstricmp(e, ".PIF");
}
/* Zuordnungen wie Windows 3.1: WIN.INI [Extensions] "txt=notepad.exe ^.txt"
   (^ = Dateiname ohne Endung, also steht dort der ganze Name); fehlt der
   Eintrag, die Vorgaben von Windows 3.1 */
static const char *const as_def[] = {
    "TXT", "notepad.exe ^.txt", "INI", "notepad.exe ^.ini", "LOG", "notepad.exe ^.log",
    "WRI", "write.exe ^.wri", "BMP", "pbrush.exe ^.bmp", "CRD", "cardfile.exe ^.crd",
    "TRM", "terminal.exe ^.trm", "REC", "recorder.exe ^.rec", "CAL", "calendar.exe ^.cal", 0
};
/* -> sx_cmd = Befehl fuer die Datei, 1 gefunden */
static int assoc(LPCSTR file)
{
    LPCSTR e = 0, f;
    const char *d = "";
    WORD i, k;
    for (f = file; *f; f++)
        if (*f == '.') e = f + 1;
        else if (*f == '\\' || *f == ':') e = 0;
    if (!e || !*e)
        return 0;
    for (i = 0; as_def[i]; i += 2)
        if (!fstricmp(e, as_def[i]))
            d = as_def[i + 1];
    if (!GetProfileString("Extensions", e, d, sx_as, sizeof(sx_as)) || !sx_as[0])
        return 0;
    for (i = k = 0; sx_as[i] && sx_as[i] != '^' && k < sizeof(sx_cmd) - 1; i++)
        sx_cmd[k++] = sx_as[i];
    if (!sx_as[i] && k < sizeof(sx_cmd) - 1)
        sx_cmd[k++] = ' ';
    fstrncpy(sx_cmd + k, file, sizeof(sx_cmd) - k);
    return 1;
}
/* fuer ExtractAssociatedIcon: Programm zur Datei (ohne Argumente) */
int shell_assoc(LPCSTR file, LPSTR prog, WORD n)
{
    WORD i;
    if (!assoc(file))
        return 0;
    for (i = 0; sx_cmd[i] && sx_cmd[i] != ' ' && i < n - 1; i++)
        prog[i] = sx_cmd[i];
    prog[i] = 0;
    return 1;
}
WORD API ShellExecute(HWND h, LPCSTR op, LPCSTR file, LPCSTR par, LPCSTR dir, short show)
{
    WORD n;
    (void)h; (void)op; (void)dir;
    if (!file)
        return 2;
    if (exe_like(file)) {
        fstrncpy(sx_cmd, file, FAPI_PATHLEN);
        if (par && *par) {
            n = fstrlen(sx_cmd);
            sx_cmd[n++] = ' ';
            fstrncpy(sx_cmd + n, par, sizeof(sx_cmd) - n);
        }
    } else if (!assoc(file))
        return 31;                              /* SE_ERR_NOASSOC        */
    return WinExec(sx_cmd, show);
}
WORD API FindExecutable(LPCSTR file, LPCSTR dir, LPSTR res)
{
    (void)dir;
    if (exe_like(file)) {
        fstrncpy(res, file, FAPI_PATHLEN);
        return 33;
    }
    if (!shell_assoc(file, res, FAPI_PATHLEN)) {
        res[0] = 0;
        return 31;
    }
    return 33;
}
