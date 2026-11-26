/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Standarddialoge (COMMDLG): Datei oeffnen/speichern, Suchen,
 * Drucken (meldet "kein Drucker", solange es keine Druckertreiber gibt).
 * Schnittstelle wie COMMDLG.DLL von Windows 3.1 (OPENFILENAME, FINDREPLACE,
 * Nachricht "commdlg_FindReplace"); Aufbau und Aussehen eigener Entwurf.
 */
#include "gui.h"
#include "ttf.h"

HGDIOBJ API SelectObject(HDC h, HGDIOBJ o);
BOOL API DeleteObject(HGDIOBJ o);
DWORD API SetTextColor(HDC h, DWORD c);
HGDIOBJ API CreateFontIndirect(const void FAR *lf);
short API DrawText(HDC dc, LPCSTR s, short n, LPRECT r, WORD fmt);

#define WM_COMMAND  0x0111
#define IDOK        1
#define IDCANCEL    2
#define WS_BORDER   0x00800000UL
#define WS_VSCROLL  0x00200000UL
#define WS_GROUP    0x00020000UL
#define WS_TABSTOP  0x00010000UL
#define LBS_NOTIFY  0x0001
#define LBS_SORT    0x0002
#define ES_AUTOHSCROLL 0x0080
#define BS_DEFPUSH  0x0001
#define BS_AUTOCHECK 0x0003
#define BS_GROUPBOX 0x0007
#define BS_AUTORADIO 0x0009
#define LB_ADDSTRING 0x0401
#define LB_INSERTSTRING 0x0402
#define LB_RESETCONTENT 0x0405
#define LB_GETCURSEL 0x0409
#define LB_GETTEXT  0x040A
#define LB_GETCOUNT 0x040C
#define BM_GETCHECK 0x0400
#define BM_SETCHECK 0x0401

BOOL API ShowWindow(HWND h, short cmd);
WORD API RegisterWindowMessage(LPCSTR name);
void API PostQuitMessage(short code);
short API GetWindowTextLength(HWND h);
BOOL API IsWindowVisible(HWND h);
BOOL API EnableWindow(HWND h, BOOL on);

static DWORD cderr;
DWORD API CommDlgExtendedError(void) { return cderr; }

short API GetFileTitle(LPCSTR path, LPSTR buf, WORD n)
{
    LPCSTR p = path, s = path;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            s = p + 1;
    if (fstrlen(s) + 1 > n)
        return fstrlen(s) + 1;
    fstrncpy(buf, s, n);
    return 0;
}

/* ---- Vorlagen (Wine, gui/res/commdlg.rc), Hooks ---- */
#include "cdlgtpl.h"

#define WM_INITDIALOG 0x0110
#define WM_PAINT 0x000F
#define SW_HIDE  0

/* Vorlage waehlen: eigene des Programms (ENABLETEMPLATE[HANDLE]) oder unsere */
static const BYTE FAR *cd_tpl(DWORD fl, DWORD f_tpl, DWORD f_hnd, WORD hinst,
                              LPCSTR name, const BYTE FAR *ours, WORD FAR *sel)
{
    *sel = 0;
    if ((fl & f_hnd) && hinst)
        return (const BYTE FAR *)MKFP(hinst, 0);
    if ((fl & f_tpl) && hinst && name) {
        *sel = load_tpl(hinst, name);
        if (*sel)
            return (const BYTE FAR *)MKFP(*sel, 0);
    }
    return ours;
}

/* Hook des Programms (ENABLEHOOK): bekommt jede Nachricht zuerst */
static WORD hq[5];
static BOOL cd_hook(DWORD hook, WORD hinst, HWND d, WORD m, WORD wp, DWORD lp)
{
    if (!hook)
        return 0;
    hq[0] = d; hq[1] = m; hq[2] = wp; hq[3] = HIWORD(lp); hq[4] = LOWORD(lp);
    return (short)call16((void FAR *)hook, hinst, 5, hq) != 0;
}

static void hide(HWND d, short id)
{
    HWND c = GetDlgItem(d, id);
    if (c) ShowWindow(c, SW_HIDE);
}

/* ---- Datei oeffnen / speichern ---- */
#define ID_NAME  0x480          /* edt1 */
#define ID_FILES 0x460          /* lst1 */
#define ID_DIRS  0x461          /* lst2 */
#define ID_PATH  0x440          /* stc1 */
#define ID_TYPES 0x470          /* cmb1 */
#define ID_DRIVES 0x471         /* cmb2 */
#define ID_RO    0x410          /* chx1 */
#define ID_HELP  0x40E          /* pshHelp */
#define CB_ADDSTRING  0x0403
#define CB_GETCURSEL  0x0407
#define CB_GETLBTEXT  0x0408
#define CB_RESETCONTENT 0x040B
#define CB_SETCURSEL  0x040E
#define OFN_READONLY  0x0001UL
#define OFN_HIDEREADONLY 0x0004UL
#define OFN_NOCHANGEDIR 0x0008UL
#define OFN_SHOWHELP  0x0010UL
#define OFN_ENABLEHOOK 0x0020UL
#define OFN_ENABLETEMPLATE 0x0040UL
#define OFN_ENABLETEMPLATEHANDLE 0x0080UL

static struct {
    BYTE FAR *ofn;
    BYTE  save;
    DWORD hook;
    WORD  hinst;
    char  pat[80];
    char  cur[FAPI_PATHLEN];
    char  buf[FAPI_PATHLEN];
} fd;
static struct fapi_dirent fd_e;

static void fd_fill(HWND d)
{
    WORD i, x;
    HWND lf = GetDlgItem(d, ID_FILES), ld = GetDlgItem(d, ID_DIRS), cd = GetDlgItem(d, ID_DRIVES);
    static char line[24];
    send_msg(lf, LB_RESETCONTENT, 0, 0);
    send_msg(ld, LB_RESETCONTENT, 0, 0);
    for (i = 0; ; i++) {
        x = fapi_call(FAPI_FINDENTRY, (WORD)&fd_e, i, 0);
        if (FAPI_FAILED(x))
            break;
        if (fd_e.attr & 0x06)                   /* versteckt, System     */
            continue;
        if (fd_e.attr & 0x10) {
            if (fd_e.name[0] == '.' && !fd_e.name[1])
                continue;
            line[0] = '[';
            fstrncpy(line + 1, fd_e.name, 20);
            x = fstrlen(line);
            line[x] = ']';
            line[x + 1] = 0;
            for (x = 0; line[x]; x++)
                if (line[x] >= 'A' && line[x] <= 'Z') line[x] += 32;
            send_msg(ld, LB_ADDSTRING, 0, (DWORD)(void FAR *)line);
        } else if (fd.pat[0]) {
            char *p = fd.pat, *q;
            static char one[80];
            while (*p) {                        /* "*.TXT;*.INI"         */
                for (q = one; *p && *p != ';'; )
                    *q++ = *p++;
                *q = 0;
                if (*p) p++;
                if (wmatch(one, fd_e.name)) {
                    fstrncpy(line, fd_e.name, sizeof(line));
                    for (x = 0; line[x]; x++)
                        if (line[x] >= 'A' && line[x] <= 'Z') line[x] += 32;
                    send_msg(lf, LB_ADDSTRING, 0, (DWORD)(void FAR *)line);
                    break;
                }
            }
        }
    }
    send_msg(cd, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < 26; i++) {
        static char dummy[FAPI_PATHLEN];
        if (FAPI_FAILED(DosQCurDirD(i, dummy)))
            continue;
        line[0] = (char)('a' + i); line[1] = ':'; line[2] = 0;
        x = (WORD)send_msg(cd, CB_ADDSTRING, 0, (DWORD)(void FAR *)line);
        if (i == DosQCurDisk())
            send_msg(cd, CB_SETCURSEL, x, 0);
    }
    DosQCurDir(fd.cur);
    for (x = 0; fd.cur[x]; x++)
        if (fd.cur[x] >= 'A' && fd.cur[x] <= 'Z')
            fd.cur[x] += 32;
    SetDlgItemText(d, ID_PATH, fd.cur);
}

/* Filter nFilterIndex: Liste der Typen fuellen, Muster setzen */
static void fd_types(HWND d)
{
    LPCSTR f = *(LPCSTR FAR *)(fd.ofn + 8);
    WORD idx = *(WORD FAR *)(fd.ofn + 20), k = 0;
    HWND c = GetDlgItem(d, ID_TYPES);
    send_msg(c, CB_RESETCONTENT, 0, 0);
    fstrncpy(fd.pat, "*.*", sizeof(fd.pat));
    if (!idx) idx = 1;
    while (f && *f) {
        LPCSTR desc = f, pat;
        f += fstrlen(f) + 1;
        pat = f;
        if (!*pat)
            break;
        f += fstrlen(f) + 1;
        send_msg(c, CB_ADDSTRING, 0, (DWORD)desc);
        if (++k == idx)
            fstrncpy(fd.pat, pat, sizeof(fd.pat));
    }
    if (!k) {
        send_msg(c, CB_ADDSTRING, 0, (DWORD)(void FAR *)"Alle Dateien (*.*)");
        k = idx = 1;
    }
    send_msg(c, CB_SETCURSEL, (idx <= k ? idx : 1) - 1, 0);
}

static void fd_type_sel(HWND d)                 /* anderer Typ gewaehlt  */
{
    LPCSTR f = *(LPCSTR FAR *)(fd.ofn + 8);
    short s = (short)SendDlgItemMessage(d, ID_TYPES, CB_GETCURSEL, 0, 0), k = 0;
    while (f && *f && s >= 0) {
        LPCSTR pat;
        f += fstrlen(f) + 1;
        pat = f;
        if (!*pat)
            break;
        f += fstrlen(f) + 1;
        if (k++ == s) {
            fstrncpy(fd.pat, pat, sizeof(fd.pat));
            *(DWORD FAR *)(fd.ofn + 20) = (DWORD)(s + 1);
            break;
        }
    }
    fd_fill(d);
}
static int has_wild(LPCSTR s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return 1;
    return 0;
}

static int fd_ok(HWND d)
{
    BYTE FAR *o = fd.ofn;
    LPSTR file = *(LPSTR FAR *)(o + 24);
    WORD nmax = *(WORD FAR *)(o + 28), i, off = 0, ext = 0;
    LPSTR title = *(LPSTR FAR *)(o + 32);
    WORD tmax = *(WORD FAR *)(o + 36);
    DWORD fl = *(DWORD FAR *)(o + 48);
    LPCSTR defext = *(LPCSTR FAR *)(o + 56);
    WORD h;
    GetDlgItemText(d, ID_NAME, fd.buf, sizeof(fd.buf));
    if (!fd.buf[0])
        return 0;
    if (has_wild(fd.buf)) {
        fstrncpy(fd.pat, fd.buf, sizeof(fd.pat));
        fd_fill(d);
        return 0;
    }
    if (!FAPI_FAILED(DosChDir(fd.buf))) {       /* Verzeichnis           */
        if (fd.buf[1] == ':')
            DosSelectDisk((fd.buf[0] | 32) - 'a');
        SetDlgItemText(d, ID_NAME, "");
        fd_fill(d);
        return 0;
    }
    {                                           /* full_path: Quelle != Ziel */
        static char nm[FAPI_PATHLEN];
        fstrncpy(nm, fd.buf, sizeof(nm));
        full_path(nm, fd.buf, sizeof(fd.buf));
    }
    for (i = 0; fd.buf[i]; i++) {
        if (fd.buf[i] == '\\') { off = i + 1; ext = 0; }
        if (fd.buf[i] == '.') ext = i + 1;
    }
    if (!ext && defext && *defext && i + 1 + fstrlen(defext) < sizeof(fd.buf)) {
        fd.buf[i] = '.';
        fstrncpy(fd.buf + i + 1, defext, sizeof(fd.buf) - i - 1);
        ext = i + 1;
        for (i = ext; fd.buf[i]; i++)
            if (fd.buf[i] >= 'a' && fd.buf[i] <= 'z') fd.buf[i] -= 32;
    }
    h = DosOpen(fd.buf);
    if (!FAPI_FAILED(h))
        DosClose(h);
    if (!fd.save && FAPI_FAILED(h) && (fl & 0x1000)) {      /* MUSTEXIST */
        static char m[FAPI_PATHLEN + 40];
        fstrncpy(m, fd.buf + off, sizeof(m));
        fstrncpy(m + fstrlen(m), "\nDatei nicht gefunden.", 30);
        MessageBox(d, m, "\xD6" "ffnen", 0x30);
        return 0;
    }
    if (fd.save && !FAPI_FAILED(h) && (fl & 2)) {           /* OVERWRITEPROMPT */
        static char m[FAPI_PATHLEN + 60];
        fstrncpy(m, fd.buf + off, sizeof(m));
        fstrncpy(m + fstrlen(m), " ist schon vorhanden.\nErsetzen?", 40);
        if (MessageBox(d, m, "Speichern unter", 0x34) != 6)
            return 0;
    }
    if (fstrlen(fd.buf) + 1 > nmax) {
        cderr = 0x3003;                         /* FNERR_BUFFERTOOSMALL  */
        return 0;
    }
    fstrncpy(file, fd.buf, nmax);
    *(WORD FAR *)(o + 52) = off;
    *(WORD FAR *)(o + 54) = ext;
    if (title && tmax)
        fstrncpy(title, fd.buf + off, tmax);
    return 1;
}

static BOOL fd_proc(HWND d, WORD m, WORD wp, DWORD lp)
{
    if (cd_hook(fd.hook, fd.hinst, d, m, wp, m == WM_INITDIALOG ? (DWORD)fd.ofn : lp))
        return 1;
    if (m == WM_INITDIALOG) {
        DWORD fl = *(DWORD FAR *)(fd.ofn + 48);
        LPSTR file = *(LPSTR FAR *)(fd.ofn + 24);
        LPCSTR tt = *(LPCSTR FAR *)(fd.ofn + 44);
        if (tt && *tt)
            SetWindowText(d, tt);
        if (fl & OFN_HIDEREADONLY)
            hide(d, ID_RO);
        else
            SendDlgItemMessage(d, ID_RO, BM_SETCHECK, (fl & OFN_READONLY) ? 1 : 0, 0);
        if (!(fl & OFN_SHOWHELP))
            hide(d, ID_HELP);
        fd_types(d);
        if (file && *file) {
            if (has_wild(file))
                fstrncpy(fd.pat, file, sizeof(fd.pat));
            else
                SetDlgItemText(d, ID_NAME, file);
        }
        if (!GetWindowTextLength(GetDlgItem(d, ID_NAME)))
            SetDlgItemText(d, ID_NAME, fd.pat);
        fd_fill(d);
        SetFocus(GetDlgItem(d, ID_NAME));
        SendDlgItemMessage(d, ID_NAME, 0x0401, 0, MAKELONG(0, 0x7FFF));
        return 0;
    }
    if (m != WM_COMMAND)
        return 0;
    switch (wp) {
    case ID_FILES:
        if (HIWORD(lp) == 1 || HIWORD(lp) == 2) {
            short s = (short)SendDlgItemMessage(d, ID_FILES, LB_GETCURSEL, 0, 0);
            if (s >= 0) {
                SendDlgItemMessage(d, ID_FILES, LB_GETTEXT, s, (DWORD)(void FAR *)fd.buf);
                SetDlgItemText(d, ID_NAME, fd.buf);
            }
            if (HIWORD(lp) == 2 && fd_ok(d))
                EndDialog(d, 1);
        }
        return 1;
    case ID_DIRS:
        if (HIWORD(lp) == 2) {
            short s = (short)SendDlgItemMessage(d, ID_DIRS, LB_GETCURSEL, 0, 0);
            if (s >= 0) {
                WORD n;
                SendDlgItemMessage(d, ID_DIRS, LB_GETTEXT, s, (DWORD)(void FAR *)fd.buf);
                n = fstrlen(fd.buf);
                fd.buf[n - 1] = 0;
                DosChDir(fd.buf + 1);
                fd_fill(d);
            }
        }
        return 1;
    case ID_DRIVES:
        if (HIWORD(lp) == 1) {                  /* CBN_SELCHANGE         */
            short s = (short)SendDlgItemMessage(d, ID_DRIVES, CB_GETCURSEL, 0, 0);
            if (s >= 0) {
                SendDlgItemMessage(d, ID_DRIVES, CB_GETLBTEXT, s, (DWORD)(void FAR *)fd.buf);
                if (FAPI_FAILED(DosSelectDisk(fd.buf[0] - 'a')))
                    MessageBox(d, "Laufwerk nicht bereit.", "Laufwerke", 0x30);
            }
            fd_fill(d);
        }
        return 1;
    case ID_TYPES:
        if (HIWORD(lp) == 1)
            fd_type_sel(d);
        return 1;
    case IDOK:
        if (fd_ok(d)) {
            DWORD FAR *fl = (DWORD FAR *)(fd.ofn + 48);
            if (IsWindowVisible(GetDlgItem(d, ID_RO))) {
                if (SendDlgItemMessage(d, ID_RO, BM_GETCHECK, 0, 0)) *fl |= OFN_READONLY;
                else *fl &= ~OFN_READONLY;
            }
            EndDialog(d, 1);
        }
        return 1;
    case IDCANCEL:
        EndDialog(d, 0);
        return 1;
    }
    return 0;
}

static BOOL file_dlg(BYTE FAR *o, int save)
{
    HWND owner = *(WORD FAR *)(o + 4), d;
    LPCSTR idir = *(LPCSTR FAR *)(o + 40);
    DWORD fl = *(DWORD FAR *)(o + 48);
    static char home[FAPI_PATHLEN];
    const BYTE FAR *tpl;
    WORD hd = DosQCurDisk();
    static WORD sel;               /* static: SS != DS */
    short r;
    cderr = 0;
    fd.ofn = o;
    fd.save = (BYTE)save;
    fd.hinst = *(WORD FAR *)(o + 6);
    fd.hook = (fl & OFN_ENABLEHOOK) ? *(DWORD FAR *)(o + 64) : 0;
    DosQCurDir(home);
    if (idir && *idir) {
        if (idir[1] == ':')
            DosSelectDisk((idir[0] | 32) - 'a');
        fstrncpy(fd.buf, idir, sizeof(fd.buf));
        DosChDir(fd.buf);
    }
    tpl = cd_tpl(fl, OFN_ENABLETEMPLATE, OFN_ENABLETEMPLATEHANDLE, fd.hinst,
                 *(LPCSTR FAR *)(o + 68), save ? dlg_SAVE_FILE : dlg_OPEN_FILE, &sel);
    d = dlg_tpl(tpl, tpl == dlg_OPEN_FILE || tpl == dlg_SAVE_FILE ? 0 : fd.hinst,
                owner, (IDLGPROC)fd_proc, 0);
    if (sel)
        seg_del(sel);
    if (!d) {
        cderr = 2;                              /* CDERR_INITIALIZATION  */
        return 0;
    }
    r = dlg_run(d, owner);
    if (fl & OFN_NOCHANGEDIR) {
        DosSelectDisk(hd);
        DosChDir(home);
    }
    return r == 1;
}

BOOL API GetOpenFileName(BYTE FAR *o) { return file_dlg(o, 0); }
BOOL API GetSaveFileName(BYTE FAR *o) { return file_dlg(o, 1); }

/* ---- Suchen und Ersetzen (nicht modal) ---- */
#define ID_WHAT  0x480          /* edt1 */
#define ID_WITH  0x481          /* edt2 */
#define ID_WORD  0x410          /* chx1 */
#define ID_CASE  0x411          /* chx2 */
#define ID_UP    0x420          /* rad1 */
#define ID_DOWN  0x421          /* rad2 */
#define ID_REPL  0x400          /* psh1 */
#define ID_REPLALL 0x401        /* psh2 */
#define FR_DOWN       0x00000001UL
#define FR_WHOLEWORD  0x00000002UL
#define FR_MATCHCASE  0x00000004UL
#define FR_SHOWHELP   0x00000080UL
#define FR_ENABLEHOOK 0x00000100UL
#define FR_ENABLETEMPLATE 0x00000200UL
#define FR_NOUPDOWN   0x00000400UL
#define FR_NOMATCHCASE 0x00000800UL
#define FR_NOWHOLEWORD 0x00001000UL
#define FR_ENABLETEMPLATEHANDLE 0x00002000UL
#define FR_HIDEUPDOWN 0x00004000UL
#define FR_HIDEMATCHCASE 0x00008000UL
#define FR_HIDEWHOLEWORD 0x00010000UL

static BYTE FAR *fr;
static HWND fr_dlg;
static WORD fr_msg;
static BYTE fr_rep;
static DWORD fr_hook;

static void fr_send(HWND d, WORD flags)
{
    HWND owner = *(WORD FAR *)(fr + 4);
    DWORD fl = *(DWORD FAR *)(fr + 8) & ~0x007FUL;
    if (flags & 0x78) {                         /* Felder zurueckschreiben */
        GetDlgItemText(d, ID_WHAT, *(LPSTR FAR *)(fr + 12), *(WORD FAR *)(fr + 20));
        if (fr_rep)
            GetDlgItemText(d, ID_WITH, *(LPSTR FAR *)(fr + 16), *(WORD FAR *)(fr + 22));
        if (SendDlgItemMessage(d, ID_CASE, BM_GETCHECK, 0, 0)) fl |= FR_MATCHCASE;
        else fl &= ~FR_MATCHCASE;
        if (SendDlgItemMessage(d, ID_WORD, BM_GETCHECK, 0, 0)) fl |= FR_WHOLEWORD;
        else fl &= ~FR_WHOLEWORD;
        if (!fr_rep) {
            if (SendDlgItemMessage(d, ID_DOWN, BM_GETCHECK, 0, 0)) fl |= FR_DOWN;
            else fl &= ~FR_DOWN;
        }
    }
    fl |= flags;
    *(DWORD FAR *)(fr + 8) = fl;
    send_msg(owner, fr_msg, 0, (DWORD)(void FAR *)fr);
}

static BOOL fr_proc(HWND d, WORD m, WORD wp, DWORD lp)
{
    if (cd_hook(fr_hook, *(WORD FAR *)(fr + 6), d, m, wp, m == WM_INITDIALOG ? (DWORD)fr : lp))
        return 1;
    if (m == WM_INITDIALOG) {
        DWORD fl = *(DWORD FAR *)(fr + 8);
        SetDlgItemText(d, ID_WHAT, *(LPSTR FAR *)(fr + 12));
        if (fr_rep)
            SetDlgItemText(d, ID_WITH, *(LPSTR FAR *)(fr + 16));
        SendDlgItemMessage(d, ID_CASE, BM_SETCHECK, (fl & FR_MATCHCASE) ? 1 : 0, 0);
        SendDlgItemMessage(d, ID_WORD, BM_SETCHECK, (fl & FR_WHOLEWORD) ? 1 : 0, 0);
        if (!fr_rep)
            SendDlgItemMessage(d, (fl & FR_DOWN) ? ID_DOWN : ID_UP, BM_SETCHECK, 1, 0);
        if (fl & FR_HIDEMATCHCASE) hide(d, ID_CASE);
        else if (fl & FR_NOMATCHCASE) EnableWindow(GetDlgItem(d, ID_CASE), 0);
        if (fl & FR_HIDEWHOLEWORD) hide(d, ID_WORD);
        else if (fl & FR_NOWHOLEWORD) EnableWindow(GetDlgItem(d, ID_WORD), 0);
        if (!fr_rep) {
            if (fl & FR_HIDEUPDOWN) {
                hide(d, ID_UP); hide(d, ID_DOWN); hide(d, 0x430);
            } else if (fl & FR_NOUPDOWN) {
                EnableWindow(GetDlgItem(d, ID_UP), 0);
                EnableWindow(GetDlgItem(d, ID_DOWN), 0);
            }
        }
        if (!(fl & FR_SHOWHELP))
            hide(d, ID_HELP);
        SetFocus(GetDlgItem(d, ID_WHAT));
        SendDlgItemMessage(d, ID_WHAT, 0x0401, 0, MAKELONG(0, 0x7FFF));
        return 0;
    }
    if (m != WM_COMMAND)
        return 0;
    switch (wp) {
    case IDOK:       fr_send(d, 0x08); return 1;           /* FR_FINDNEXT   */
    case ID_REPL:    fr_send(d, 0x10); return 1;           /* FR_REPLACE    */
    case ID_REPLALL: fr_send(d, 0x20); return 1;           /* FR_REPLACEALL */
    case IDCANCEL:
        fr_send(d, 0x40);                       /* FR_DIALOGTERM         */
        fr_dlg = 0;
        DestroyWindow(d);
        return 1;
    }
    return 0;
}

static HWND find_dlg(BYTE FAR *f, int rep)
{
    HWND owner = *(WORD FAR *)(f + 4), d;
    DWORD fl = *(DWORD FAR *)(f + 8);
    WORD hinst = *(WORD FAR *)(f + 6);
    static WORD sel;
    const BYTE FAR *ours = rep ? dlg_REPLACEDLGORD : dlg_FINDDLGORD, FAR *tpl;
    cderr = 0;
    if (fr_dlg && wptr(fr_dlg))
        return fr_dlg;
    fr = f;
    fr_rep = (BYTE)rep;
    fr_hook = (fl & FR_ENABLEHOOK) ? *(DWORD FAR *)(f + 28) : 0;
    fr_msg = RegisterWindowMessage("commdlg_FindReplace");
    tpl = cd_tpl(fl, FR_ENABLETEMPLATE, FR_ENABLETEMPLATEHANDLE, hinst,
                 *(LPCSTR FAR *)(f + 32), ours, &sel);
    d = dlg_tpl(tpl, tpl == ours ? 0 : hinst, owner, (IDLGPROC)fr_proc, 0);
    if (sel)
        seg_del(sel);
    if (!d) {
        cderr = 2;
        return 0;
    }
    ShowWindow(d, 1);
    fr_dlg = d;
    return d;
}
HWND API FindText(BYTE FAR *f) { return find_dlg(f, 0); }
HWND API ReplaceText(BYTE FAR *f) { return find_dlg(f, 1); }

/* ---- Farbe (CHOOSE_COLOR) ---- */
/* 48 Grundfarben wie Windows 3.1 (Werte aus Wine colordlg.c) */
static const DWORD basic[48] = {
    0x8080FFL, 0x80FFFFL, 0x80FF80L, 0x80FF00L, 0xFFFF80L, 0xFF8000L, 0xC080FFL, 0xFF80FFL,
    0x0000FFL, 0x00FFFFL, 0x00FF80L, 0x40FF00L, 0xFFFF00L, 0xC08000L, 0xC08080L, 0xFF00FFL,
    0x404080L, 0x4080FFL, 0x00FF00L, 0x808000L, 0x804000L, 0xFF8080L, 0x400080L, 0x8000FFL,
    0x000080L, 0x0080FFL, 0x008000L, 0x408000L, 0xFF0000L, 0xA00000L, 0x800080L, 0xFF0080L,
    0x000040L, 0x004080L, 0x004000L, 0x404000L, 0x800000L, 0x400000L, 0x400040L, 0x800040L,
    0x000000L, 0x008080L, 0x408080L, 0x808080L, 0x808040L, 0xC0C0C0L, 0x400040L, 0xFFFFFFL };
extern DWORD vga_rgb[16];
BYTE rgb2c(DWORD rgb);
struct paintst { HDC hdc; BOOL erase; RECT rc; BOOL res, inc; BYTE rgb[16]; };
BOOL API MoveWindow(HWND h, short x, short y, short cx, short cy, BOOL rep);
void API SetDlgItemInt(HWND d, short id, WORD v, BOOL sgn);
WORD API GetDlgItemInt(HWND d, short id, BOOL FAR *ok, BOOL sgn);
void API InvalidateRect(HWND h, const RECT FAR *r, BOOL e);

#define CC_RGBINIT    0x01UL
#define CC_FULLOPEN   0x02UL
#define CC_PREVENTFULLOPEN 0x04UL
#define CC_SHOWHELP   0x08UL
#define CC_ENABLEHOOK 0x10UL
#define CC_ENABLETEMPLATE 0x20UL
#define CC_ENABLETEMPLATEHANDLE 0x40UL
#define ID_C_HUE   703
#define ID_C_SAT   704
#define ID_C_LUM   705
#define ID_C_RED   706
#define ID_C_GREEN 707
#define ID_C_BLUE  708
#define ID_C_ADD   712
#define ID_C_MIX   719

/* Bereiche im Client des Dialogs (Vorlage in Dialogeinheiten x 2) */
#define BX_L 8                  /* Grundfarben 8 x 6                    */
#define BX_T 28
#define CU_T 232                /* eigene 8 x 2                         */
#define CW   35
#define CH   28
#define RB_L 304                /* Farbfeld (Farbton waagerecht, Saettigung senkrecht) */
#define RB_T 8
#define RB_W 236
#define RB_H 232
#define LB_L 556                /* Helligkeit                           */
#define CUR_T 248               /* aktuelle Farbe                       */

static struct {
    BYTE FAR *cc;
    DWORD hook, rgb, cust[16];
    short h, s, l;
    short sel;                  /* 0..47 Grund, 48..63 eigene, -1       */
    BYTE full, busy;
} cl;

static void rgb2hsl(DWORD c, short *h, short *s, short *l)
{
    short r = (BYTE)c, g = (BYTE)(c >> 8), b = (BYTE)(c >> 16);
    short mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    short mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
    long sum = (long)mx + mn, hh;
    *l = (short)((sum * 240 + 255) / 510);
    if (!d) {
        *s = 0;
        *h = 160;
        return;
    }
    *s = (short)(*l <= 120 ? ((long)d * 240 + sum / 2) / sum
                           : ((long)d * 240 + (510 - sum) / 2) / (510 - sum));
    if (r == mx)
        hh = (long)(g - b) * 40 / d;
    else if (g == mx)
        hh = 80 + (long)(b - r) * 40 / d;
    else
        hh = 160 + (long)(r - g) * 40 / d;
    if (hh < 0) hh += 240;
    *h = (short)hh;
}
static short hue2c(long m1, long m2, long h)    /* Teilwert 0..255        */
{
    long v;
    if (h < 0) h += 240;
    if (h > 240) h -= 240;
    if (h < 40) v = m1 + ((m2 - m1) * h + 20) / 40;
    else if (h < 120) v = m2;
    else if (h < 160) v = m1 + ((m2 - m1) * (160 - h) + 20) / 40;
    else v = m1;
    return (short)((v * 255 + 120) / 240);
}
static DWORD hsl2rgb(short h, short s, short l)
{
    long m1, m2;
    short r, g, b;
    if (!s) {
        r = g = b = (short)(((long)l * 255 + 120) / 240);
    } else {
        m2 = l <= 120 ? ((long)l * (240 + s) + 120) / 240 : l + s - ((long)l * s + 120) / 240;
        m1 = 2L * l - m2;
        r = hue2c(m1, m2, (long)h + 80);
        g = hue2c(m1, m2, h);
        b = hue2c(m1, m2, (long)h - 80);
    }
    return (DWORD)(BYTE)r | ((DWORD)(BYTE)g << 8) | ((DWORD)(BYTE)b << 16);
}

/* Farbe mit 16 VGA-Farben: Zeilen abwechselnd naechste und Ausgleich */
static void swatch(HDC dc, short l, short t, short r, short b, DWORD rgb)
{
    BYTE c1 = rgb2c(rgb), c2;
    DWORD v = vga_rgb[c1];
    short k, cr = 2 * (short)(BYTE)rgb - (short)(BYTE)v;
    short cg = 2 * (short)(BYTE)(rgb >> 8) - (short)(BYTE)(v >> 8);
    short cb = 2 * (short)(BYTE)(rgb >> 16) - (short)(BYTE)(v >> 16);
    if (cr < 0) cr = 0; if (cr > 255) cr = 255;
    if (cg < 0) cg = 0; if (cg > 255) cg = 255;
    if (cb < 0) cb = 0; if (cb > 255) cb = 255;
    c2 = rgb2c((DWORD)cr | ((DWORD)cg << 8) | ((DWORD)cb << 16));
    for (k = t; k < b; k++)
        dc_rawfill(dc, l, k, r, k + 1, ((k ^ (k >> 1)) & 1) ? c2 : c1);
}
static void box(HDC dc, short l, short t, short r, short b, BYTE c)
{
    dc_rawfill(dc, l, t, r, t + 1, c);
    dc_rawfill(dc, l, b - 1, r, b, c);
    dc_rawfill(dc, l, t, l + 1, b, c);
    dc_rawfill(dc, r - 1, t, r, b, c);
}

static void cc_paint(HWND d)
{
    static struct paintst ps;
    struct wnd *w = wptr(d);
    HDC dc = BeginPaint(d, (void FAR *)&ps);
    short x0 = w->cr.left, y0 = w->cr.top, i;
    for (i = 0; i < 64; i++) {
        short cx = x0 + BX_L + (i & 7) * CW, cy = y0 + (i < 48 ? BX_T + (i >> 3) * CH : CU_T + ((i - 48) >> 3) * CH);
        swatch(dc, cx + 4, cy + 4, cx + CW - 4, cy + CH - 4, i < 48 ? basic[i] : cl.cust[i - 48]);
        box(dc, cx + 3, cy + 3, cx + CW - 3, cy + CH - 3, 0);
        box(dc, cx + 1, cy + 1, cx + CW - 1, cy + CH - 1, i == cl.sel ? 0 : 15);
    }
    if (cl.full) {
        short hx, sy, k;
        for (sy = 0; sy < RB_H; sy += 8)        /* grob: 8 x 8 Bloecke   */
            for (hx = 0; hx < RB_W; hx += 8) {
                short e = hx + 8 > RB_W ? RB_W : hx + 8, f = sy + 8 > RB_H ? RB_H : sy + 8;
                swatch(dc, x0 + RB_L + hx, y0 + RB_T + sy, x0 + RB_L + e, y0 + RB_T + f,
                       hsl2rgb((short)((long)hx * 239 / RB_W), (short)(240 - (long)sy * 240 / RB_H), 120));
            }
        box(dc, x0 + RB_L - 1, y0 + RB_T - 1, x0 + RB_L + RB_W + 1, y0 + RB_T + RB_H + 1, 0);
        hx = x0 + RB_L + (short)((long)cl.h * RB_W / 240);
        sy = y0 + RB_T + (short)((long)(240 - cl.s) * (RB_H - 1) / 240);
        dc_rawfill(dc, hx - 6, sy, hx - 1, sy + 1, 0);
        dc_rawfill(dc, hx + 2, sy, hx + 7, sy + 1, 0);
        dc_rawfill(dc, hx, sy - 6, hx + 1, sy - 1, 0);
        dc_rawfill(dc, hx, sy + 2, hx + 1, sy + 7, 0);
        for (k = 0; k < RB_H; k += 8)
            swatch(dc, x0 + LB_L, y0 + RB_T + k, x0 + LB_L + 16, y0 + RB_T + (k + 8 > RB_H ? RB_H : k + 8),
                   hsl2rgb(cl.h, cl.s, (short)(240 - (long)k * 240 / RB_H)));
        box(dc, x0 + LB_L - 1, y0 + RB_T - 1, x0 + LB_L + 17, y0 + RB_T + RB_H + 1, 0);
        dc_rawfill(dc, x0 + LB_L + 18, y0 + RB_T - 6, x0 + LB_L + 30, y0 + RB_T + RB_H + 6, 15);
        k = y0 + RB_T + (short)((long)(240 - cl.l) * (RB_H - 1) / 240);
        for (i = 0; i < 5; i++)
            dc_rawfill(dc, x0 + LB_L + 19 + i, k - i, x0 + LB_L + 20 + i, k + i + 1, 0);
        swatch(dc, x0 + RB_L, y0 + CUR_T, x0 + RB_L + 40, y0 + CUR_T + 52, cl.rgb);
        dc_rawfill(dc, x0 + RB_L + 40, y0 + CUR_T, x0 + RB_L + 80, y0 + CUR_T + 52, rgb2c(cl.rgb));
        box(dc, x0 + RB_L - 1, y0 + CUR_T - 1, x0 + RB_L + 81, y0 + CUR_T + 53, 0);
    }
    EndPaint(d, (void FAR *)&ps);
}

static void cc_fields(HWND d, int from_rgb)
{
    cl.busy = 1;
    if (from_rgb)
        rgb2hsl(cl.rgb, &cl.h, &cl.s, &cl.l);
    SetDlgItemInt(d, ID_C_RED, (BYTE)cl.rgb, 0);
    SetDlgItemInt(d, ID_C_GREEN, (BYTE)(cl.rgb >> 8), 0);
    SetDlgItemInt(d, ID_C_BLUE, (BYTE)(cl.rgb >> 16), 0);
    SetDlgItemInt(d, ID_C_HUE, cl.h, 0);
    SetDlgItemInt(d, ID_C_SAT, cl.s, 0);
    SetDlgItemInt(d, ID_C_LUM, cl.l, 0);
    cl.busy = 0;
    InvalidateRect(d, 0, 0);
}

static void cc_full(HWND d, int on)
{
    struct wnd *w = wptr(d);
    static const short ids[] = { ID_C_HUE, ID_C_SAT, ID_C_LUM, ID_C_RED, ID_C_GREEN, ID_C_BLUE,
                                 ID_C_ADD, 723, 724, 725, 726, 727, 728, 0 };
    short i;
    cl.full = (BYTE)on;
    for (i = 0; ids[i]; i++)
        ShowWindow(GetDlgItem(d, ids[i]), on ? 5 : 0);
    if (!on) {                                  /* schmal wie Windows    */
        struct wnd *o = wptr(w->owner);
        MoveWindow(d, w->wr.left, w->wr.top, 2 * 152 + 6, w->wr.bottom - w->wr.top, 1);
        (void)o;
    } else {
        MoveWindow(d, w->wr.left, w->wr.top, 2 * 300 + 6, w->wr.bottom - w->wr.top, 1);
        EnableWindow(GetDlgItem(d, ID_C_MIX), 0);
    }
}

static BOOL cc_proc(HWND d, WORD m, WORD wp, DWORD lp)
{
    if (cd_hook(cl.hook, *(WORD FAR *)(cl.cc + 6), d, m, wp, m == WM_INITDIALOG ? (DWORD)cl.cc : lp))
        return 1;
    switch (m) {
    case WM_INITDIALOG: {
        DWORD fl = *(DWORD FAR *)(cl.cc + 16);
        short i;
        static const short hidden[] = { 720, 721, 710, 702, 709, 713, 730, 731, 0 };
        for (i = 0; hidden[i]; i++)
            ShowWindow(GetDlgItem(d, hidden[i]), 0);
        if (!(fl & CC_SHOWHELP))
            hide(d, ID_HELP);
        if (fl & CC_PREVENTFULLOPEN)
            EnableWindow(GetDlgItem(d, ID_C_MIX), 0);
        cc_full(d, (fl & CC_FULLOPEN) && !(fl & CC_PREVENTFULLOPEN));
        cc_fields(d, 1);
        return 1;
    }
    case WM_PAINT:
        cc_paint(d);
        return 1;
    case 0x0201: case 0x0203: {                 /* WM_LBUTTONDOWN/DBLCLK */
        short x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x >= BX_L && x < BX_L + 8 * CW) {
            short k = -1;
            if (y >= BX_T && y < BX_T + 6 * CH)
                k = (x - BX_L) / CW + ((y - BX_T) / CH) * 8;
            else if (y >= CU_T && y < CU_T + 2 * CH)
                k = 48 + (x - BX_L) / CW + ((y - CU_T) / CH) * 8;
            if (k >= 0) {
                cl.sel = k;
                cl.rgb = k < 48 ? basic[k] : cl.cust[k - 48];
                cc_fields(d, 1);
                if (m == 0x0203)
                    post_msg(d, WM_COMMAND, IDOK, 0);
            }
        } else if (cl.full && y >= RB_T && y < RB_T + RB_H) {
            if (x >= RB_L && x < RB_L + RB_W) {
                cl.h = (short)((long)(x - RB_L) * 239 / RB_W);
                cl.s = (short)(240 - (long)(y - RB_T) * 240 / RB_H);
            } else if (x >= LB_L && x < LB_L + 30)
                cl.l = (short)(240 - (long)(y - RB_T) * 240 / RB_H);
            else
                return 1;
            cl.rgb = hsl2rgb(cl.h, cl.s, cl.l);
            cc_fields(d, 0);
        }
        return 1;
    }
    case WM_COMMAND:
        switch (wp) {
        case ID_C_RED: case ID_C_GREEN: case ID_C_BLUE:
            if (HIWORD(lp) == 0x0300 && !cl.busy) {     /* EN_CHANGE     */
                WORD r = GetDlgItemInt(d, ID_C_RED, 0, 0), g = GetDlgItemInt(d, ID_C_GREEN, 0, 0);
                WORD b = GetDlgItemInt(d, ID_C_BLUE, 0, 0);
                cl.rgb = (DWORD)(r > 255 ? 255 : r) | ((DWORD)(g > 255 ? 255 : g) << 8) |
                         ((DWORD)(b > 255 ? 255 : b) << 16);
                rgb2hsl(cl.rgb, &cl.h, &cl.s, &cl.l);
                cl.busy = 1;
                SetDlgItemInt(d, ID_C_HUE, cl.h, 0);
                SetDlgItemInt(d, ID_C_SAT, cl.s, 0);
                SetDlgItemInt(d, ID_C_LUM, cl.l, 0);
                cl.busy = 0;
                InvalidateRect(d, 0, 0);
            }
            return 1;
        case ID_C_HUE: case ID_C_SAT: case ID_C_LUM:
            if (HIWORD(lp) == 0x0300 && !cl.busy) {
                WORD h = GetDlgItemInt(d, ID_C_HUE, 0, 0), sa = GetDlgItemInt(d, ID_C_SAT, 0, 0);
                WORD l = GetDlgItemInt(d, ID_C_LUM, 0, 0);
                cl.h = h > 239 ? 239 : h; cl.s = sa > 240 ? 240 : sa; cl.l = l > 240 ? 240 : l;
                cl.rgb = hsl2rgb(cl.h, cl.s, cl.l);
                cl.busy = 1;
                SetDlgItemInt(d, ID_C_RED, (BYTE)cl.rgb, 0);
                SetDlgItemInt(d, ID_C_GREEN, (BYTE)(cl.rgb >> 8), 0);
                SetDlgItemInt(d, ID_C_BLUE, (BYTE)(cl.rgb >> 16), 0);
                cl.busy = 0;
                InvalidateRect(d, 0, 0);
            }
            return 1;
        case ID_C_MIX:
            cc_full(d, 1);
            return 1;
        case ID_C_ADD: {                        /* in das gewaehlte bzw. naechste Feld */
            short k = cl.sel >= 48 ? cl.sel - 48 : 0;
            cl.cust[k] = cl.rgb;
            cl.sel = 48 + (k + 1) % 16;
            InvalidateRect(d, 0, 0);
            return 1;
        }
        case IDOK: {
            DWORD FAR *cu = *(DWORD FAR * FAR *)(cl.cc + 12);
            short i;
            *(DWORD FAR *)(cl.cc + 8) = cl.rgb;
            if (cu)
                for (i = 0; i < 16; i++) cu[i] = cl.cust[i];
            EndDialog(d, 1);
            return 1;
        }
        case IDCANCEL:
            EndDialog(d, 0);
            return 1;
        }
    }
    return 0;
}

BOOL API ChooseColor(BYTE FAR *p)
{
    HWND owner = *(WORD FAR *)(p + 4), d;
    DWORD fl = *(DWORD FAR *)(p + 16);
    DWORD FAR *cu = *(DWORD FAR * FAR *)(p + 12);
    WORD hinst = *(WORD FAR *)(p + 6);
    static WORD sel;
    const BYTE FAR *tpl;
    short i;
    cderr = 0;
    cl.cc = p;
    cl.hook = (fl & CC_ENABLEHOOK) ? *(DWORD FAR *)(p + 24) : 0;
    cl.rgb = (fl & CC_RGBINIT) ? *(DWORD FAR *)(p + 8) & 0xFFFFFFL : 0;
    cl.sel = -1;
    for (i = 0; i < 16; i++)
        cl.cust[i] = cu ? cu[i] : 0xFFFFFFL;
    for (i = 0; i < 48; i++)
        if (basic[i] == cl.rgb) { cl.sel = i; break; }
    tpl = cd_tpl(fl, CC_ENABLETEMPLATE, CC_ENABLETEMPLATEHANDLE, hinst,
                 *(LPCSTR FAR *)(p + 28), dlg_CHOOSE_COLOR, &sel);
    d = dlg_tpl(tpl, tpl == dlg_CHOOSE_COLOR ? 0 : hinst, owner, (IDLGPROC)cc_proc, 0);
    if (sel)
        seg_del(sel);
    if (!d) {
        cderr = 2;
        return 0;
    }
    return dlg_run(d, owner) == 1;
}

/* ---- Schriftart (CHOOSE_FONT) - es gibt nur die Systemschrift ---- */
#define CF_SHOWHELP   0x00000004UL
#define CF_ENABLEHOOK 0x00000008UL
#define CF_ENABLETEMPLATE 0x00000010UL
#define CF_ENABLETEMPLATEHANDLE 0x00000020UL
#define CF_INITTOLOGFONTSTRUCT 0x00000040UL
#define CF_EFFECTS    0x00000100UL
#define CF_APPLY      0x00000200UL
#define CF_LIMITSIZE  0x00002000UL
#define ID_F_FACE  0x470        /* cmb1 */
#define ID_F_STYLE 0x471        /* cmb2 */
#define ID_F_SIZE  0x472        /* cmb3 */
#define ID_F_COLOR 0x473        /* cmb4 */
#define ID_F_SCRIPT 0x474       /* cmb5 */
#define ID_F_STRIKE 0x410
#define ID_F_UNDER 0x411
#define ID_F_APPLY 0x402        /* psh3 */
#define CB_FINDSTRINGEXACT 0x0418
#define CB_SETITEMDATA 0x0411
#define CB_GETITEMDATA 0x0410
#define WM_CHOOSEFONT_GETLOGFONT 0x0401

static const char *fstyle[] = { "Standard", "Kursiv", "Fett", "Fett Kursiv" };
static const BYTE fsizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, 0 };
static const char *cname[16] = { "Schwarz", "Dunkelrot", "Dunkelgr\xFCn", "Oliv", "Dunkelblau",
    "Violett", "Blaugr\xFCn", "Grau", "Silber", "Rot", "Hellgr\xFCn", "Gelb", "Blau",
    "Magenta", "T\xFCrkis", "Wei\xDF" };
static const DWORD crgb[16] = { 0x000000L, 0x000080L, 0x008000L, 0x008080L, 0x800000L,
    0x800080L, 0x808000L, 0x808080L, 0xC0C0C0L, 0x0000FFL, 0x00FF00L, 0x00FFFFL, 0xFF0000L,
    0xFF00FFL, 0xFFFF00L, 0xFFFFFFL };

static struct {
    BYTE FAR *cf;
    DWORD hook;
} cf;

static short cf_get(HWND d, short id)
{
    return (short)SendDlgItemMessage(d, id, CB_GETCURSEL, 0, 0);
}

static void cf_face(HWND d, BYTE FAR *lf)      /* Familie ins LOGFONT */
{
    static char fn[32];
    short i, n = tt_nface();
    GetDlgItemText(d, ID_F_FACE, fn, sizeof(fn));
    if (!fn[0]) fstrncpy(fn, "System", 8);
    fstrncpy((LPSTR)(lf + 18), fn, 32);
    lf[17] = 0x22;                              /* VARIABLE_PITCH|FF_SWISS */
    lf[13] = 0;                                 /* ANSI_CHARSET          */
    for (i = 0; i < n; i++)
        if (!fstricmp(tt_facename(i), fn)) {
            BYTE s = tt_facestyle(i);
            if (s & 0x10) lf[17] = 0x31;        /* FIXED_PITCH|FF_MODERN */
            if (s & 0x20) lf[13] = 2;           /* SYMBOL_CHARSET        */
            lf[16] = 4;                         /* PROOF_QUALITY         */
            break;
        }
}

static void cf_store(HWND d)                    /* Auswahl -> LOGFONT usw. */
{
    BYTE FAR *p = cf.cf;
    BYTE FAR *lf = *(BYTE FAR * FAR *)(p + 8);
    short st = cf_get(d, ID_F_STYLE), pt;
    static char t[8];
    GetDlgItemText(d, ID_F_SIZE, t, sizeof(t));
    for (pt = 0; *t && pt < 1000 && t[0]; ) {
        short i;
        for (i = 0; t[i] >= '0' && t[i] <= '9'; i++)
            pt = pt * 10 + (t[i] - '0');
        break;
    }
    if (!pt) pt = 10;
    if (st < 0) st = 0;
    *(short FAR *)(p + 12) = pt * 10;
    if (lf) {
        fmemset(lf, 0, 50);
        *(short FAR *)lf = -(short)(((long)pt * 96 + 36) / 72);
        *(short FAR *)(lf + 8) = (st & 2) ? 700 : 400;
        lf[10] = (BYTE)(st & 1);
        lf[11] = (BYTE)SendDlgItemMessage(d, ID_F_UNDER, BM_GETCHECK, 0, 0);
        lf[12] = (BYTE)SendDlgItemMessage(d, ID_F_STRIKE, BM_GETCHECK, 0, 0);
        cf_face(d, lf);
    }
    if (cf_get(d, ID_F_COLOR) >= 0)
        *(DWORD FAR *)(p + 18) = crgb[cf_get(d, ID_F_COLOR)];
    *(WORD FAR *)(p + 40) = 0x2000 | ((st & 2) ? 0x100 : 0x400) | ((st & 1) ? 0x200 : 0);
}

static void cf_paint(HWND d)
{
    static struct paintst ps;
    struct wnd *w = wptr(d);
    HDC dc = BeginPaint(d, (void FAR *)&ps);
    short x = w->cr.left + 206, y = w->cr.top + 206, c = cf_get(d, ID_F_COLOR);
    BYTE fg = c >= 0 ? rgb2c(crgb[c]) : 0;
    static BYTE lf[50];
    static char t[8];
    short st = cf_get(d, ID_F_STYLE), pt = 0, i;
    HFONT f, of;
    RECT r;
    dc_rawfill(dc, x, y, x + 218, y + 44, 15);
    GetDlgItemText(d, ID_F_SIZE, t, sizeof(t));
    for (i = 0; t[i] >= '0' && t[i] <= '9'; i++)
        pt = pt * 10 + (t[i] - '0');
    if (pt <= 0 || pt > 36) pt = pt > 36 ? 36 : 10;  /* passt ins Musterfeld */
    fmemset(lf, 0, 50);
    *(short FAR *)lf = -(short)(((long)pt * 96 + 36) / 72);
    *(short FAR *)(lf + 8) = (st > 0 && (st & 2)) ? 700 : 400;
    lf[10] = (BYTE)(st > 0 && (st & 1));
    lf[11] = (BYTE)SendDlgItemMessage(d, ID_F_UNDER, BM_GETCHECK, 0, 0);
    lf[12] = (BYTE)SendDlgItemMessage(d, ID_F_STRIKE, BM_GETCHECK, 0, 0);
    cf_face(d, lf);
    f = CreateFontIndirect((void FAR *)lf);
    of = SelectObject(dc, f);
    SetTextColor(dc, c >= 0 ? crgb[c] : 0);
    SetBkMode(dc, 1);                           /* TRANSPARENT */
    r.left = 206; r.top = 206; r.right = 206 + 218; r.bottom = 206 + 44;
    DrawText(dc, "AaBbYyZz", 8, (void FAR *)&r, 0x25);  /* CENTER|VCENTER|SINGLELINE */
    SelectObject(dc, of);
    DeleteObject(f);
    (void)fg;
    EndPaint(d, (void FAR *)&ps);
}

static BOOL cf_proc(HWND d, WORD m, WORD wp, DWORD lp)
{
    BYTE FAR *p = cf.cf;
    if (cd_hook(cf.hook, *(WORD FAR *)(p + 34), d, m, wp, m == WM_INITDIALOG ? (DWORD)p : lp))
        return 1;
    switch (m) {
    case WM_INITDIALOG: {
        DWORD fl = *(DWORD FAR *)(p + 14);
        BYTE FAR *lf = *(BYTE FAR * FAR *)(p + 8);
        short i, st = 0, pt = *(short FAR *)(p + 12) / 10, lo = 0, hi = 999;
        static char t[8];
        if (fl & CF_LIMITSIZE) {
            lo = *(short FAR *)(p + 42);
            hi = *(short FAR *)(p + 44);
        }
        SendDlgItemMessage(d, ID_F_FACE, CB_ADDSTRING, 0, (DWORD)(void FAR *)"System");
        for (i = 0; i < tt_nface(); i++)        /* TrueType-Familien, jede einmal */
            if ((short)SendDlgItemMessage(d, ID_F_FACE, CB_FINDSTRINGEXACT, (WORD)-1,
                    (DWORD)tt_facename(i)) < 0)
                SendDlgItemMessage(d, ID_F_FACE, CB_ADDSTRING, 0, (DWORD)tt_facename(i));
        i = -1;
        if ((fl & CF_INITTOLOGFONTSTRUCT) && lf && lf[18])
            i = (short)SendDlgItemMessage(d, ID_F_FACE, CB_FINDSTRINGEXACT, (WORD)-1,
                    (DWORD)(void FAR *)(lf + 18));
        if (i < 0)
            i = (short)SendDlgItemMessage(d, ID_F_FACE, CB_FINDSTRINGEXACT, (WORD)-1,
                    (DWORD)(void FAR *)"System");
        SendDlgItemMessage(d, ID_F_FACE, CB_SETCURSEL, i, 0);
        for (i = 0; i < 4; i++)
            SendDlgItemMessage(d, ID_F_STYLE, CB_ADDSTRING, 0, (DWORD)(void FAR *)fstyle[i]);
        {                                       /* Groessen nach Zahl, nicht als Text */
            struct wnd *sw = wptr(GetDlgItem(d, ID_F_SIZE));
            if (sw) sw->style &= ~0x0100UL;    /* CBS_SORT              */
        }
        for (i = 0; fsizes[i]; i++) {
            short v = fsizes[i], k = 0;
            if (v < lo || v > hi)
                continue;
            if (v >= 10) t[k++] = (char)('0' + v / 10);
            t[k++] = (char)('0' + v % 10);
            t[k] = 0;
            SendDlgItemMessage(d, ID_F_SIZE, CB_ADDSTRING, 0, (DWORD)(void FAR *)t);
        }
        for (i = 0; i < 16; i++)
            SendDlgItemMessage(d, ID_F_COLOR, CB_ADDSTRING, 0, (DWORD)(void FAR *)cname[i]);
        SendDlgItemMessage(d, ID_F_SCRIPT, CB_ADDSTRING, 0, (DWORD)(void FAR *)"Westlich");
        SendDlgItemMessage(d, ID_F_SCRIPT, CB_SETCURSEL, 0, 0);
        if ((fl & CF_INITTOLOGFONTSTRUCT) && lf) {
            short hgt = *(short FAR *)lf;
            if (hgt < 0) pt = (short)(((long)-hgt * 72 + 48) / 96);
            st = (*(short FAR *)(lf + 8) >= 600 ? 2 : 0) | (lf[10] ? 1 : 0);
            SendDlgItemMessage(d, ID_F_UNDER, BM_SETCHECK, lf[11] != 0, 0);
            SendDlgItemMessage(d, ID_F_STRIKE, BM_SETCHECK, lf[12] != 0, 0);
        }
        if (pt <= 0) pt = 10;
        SendDlgItemMessage(d, ID_F_STYLE, CB_SETCURSEL, st, 0);
        SetDlgItemInt(d, ID_F_SIZE, pt, 0);
        GetDlgItemText(d, ID_F_SIZE, t, sizeof(t));
        if (SendDlgItemMessage(d, ID_F_SIZE, CB_FINDSTRINGEXACT, (WORD)-1, (DWORD)(void FAR *)t) >= 0)
            SendDlgItemMessage(d, ID_F_SIZE, CB_SETCURSEL,
                (WORD)SendDlgItemMessage(d, ID_F_SIZE, CB_FINDSTRINGEXACT, (WORD)-1, (DWORD)(void FAR *)t), 0);
        for (i = 0; i < 16; i++)
            if (crgb[i] == (*(DWORD FAR *)(p + 18) & 0xFFFFFFL)) break;
        SendDlgItemMessage(d, ID_F_COLOR, CB_SETCURSEL, i < 16 ? i : 0, 0);
        if (!(fl & CF_EFFECTS)) {
            static const short e[] = { 0x430, ID_F_STRIKE, ID_F_UNDER, 0x443, ID_F_COLOR, 0 };
            for (i = 0; e[i]; i++) hide(d, e[i]);
        }
        if (!(fl & CF_APPLY)) hide(d, ID_F_APPLY);
        if (!(fl & CF_SHOWHELP)) hide(d, ID_HELP);
        return 1;
    }
    case WM_PAINT:
        cf_paint(d);
        return 1;
    case WM_CHOOSEFONT_GETLOGFONT:
        cf_store(d);
        if (lp && *(DWORD FAR *)(p + 8))
            fmemcpy((void FAR *)lp, *(void FAR * FAR *)(p + 8), 50);
        return 1;
    case WM_COMMAND:
        switch (wp) {
        case ID_F_COLOR: case ID_F_UNDER: case ID_F_STRIKE: case ID_F_STYLE: case ID_F_SIZE:
        case ID_F_FACE:
            InvalidateRect(d, 0, 0);
            return 1;
        case ID_F_APPLY:
            cf_store(d);
            return 1;
        case IDOK:
            cf_store(d);
            EndDialog(d, 1);
            return 1;
        case IDCANCEL:
            EndDialog(d, 0);
            return 1;
        }
    }
    return 0;
}

BOOL API ChooseFont(BYTE FAR *p)
{
    HWND owner = *(WORD FAR *)(p + 4), d;
    DWORD fl = *(DWORD FAR *)(p + 14);
    WORD hinst = *(WORD FAR *)(p + 34);
    static WORD sel;
    const BYTE FAR *tpl;
    cderr = 0;
    cf.cf = p;
    cf.hook = (fl & CF_ENABLEHOOK) ? *(DWORD FAR *)(p + 26) : 0;
    tpl = cd_tpl(fl, CF_ENABLETEMPLATE, CF_ENABLETEMPLATEHANDLE, hinst,
                 *(LPCSTR FAR *)(p + 30), dlg_CHOOSE_FONT, &sel);
    d = dlg_tpl(tpl, tpl == dlg_CHOOSE_FONT ? 0 : hinst, owner, (IDLGPROC)cf_proc, 0);
    if (sel)
        seg_del(sel);
    if (!d) {
        cderr = 2;
        return 0;
    }
    return dlg_run(d, owner) == 1;
}

/* ---- Drucken: es gibt noch keine Druckertreiber der Oberflaeche ---- */
BOOL API PrintDlg(BYTE FAR *p)
{
    DWORD fl = *(DWORD FAR *)(p + 12);
    cderr = 0x1008;                             /* PDERR_NODEFAULTPRN    */
    if (!(fl & 0x00000480UL))                   /* PD_NOWARNING, RETURNDEFAULT */
        MessageBox(*(WORD FAR *)(p + 4),
                   "Es ist kein Drucker eingerichtet.\n"
                   "Drucken aus der grafischen Oberfl\xE4" "che geht noch nicht.",
                   (fl & 0x40) ? "Druckereinrichtung" : "Drucken", 0x30);
    return 0;
}
