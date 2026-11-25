/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Standarddialoge (COMMDLG): Datei oeffnen/speichern, Suchen,
 * Drucken (meldet "kein Drucker", solange es keine Druckertreiber gibt).
 * Schnittstelle wie COMMDLG.DLL von Windows 3.1 (OPENFILENAME, FINDREPLACE,
 * Nachricht "commdlg_FindReplace"); Aufbau und Aussehen eigener Entwurf.
 */
#include "gui.h"

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

/* ---- Drucken, Farben, Schriften: noch nicht ---- */
BOOL API PrintDlg(BYTE FAR *p)
{
    (void)p;
    cderr = 0x1008;                             /* PDERR_NODEFAULTPRN    */
    return 0;
}
BOOL API ChooseColor(BYTE FAR *p) { (void)p; cderr = 0; return 0; }
BOOL API ChooseFont(BYTE FAR *p) { (void)p; cderr = 0; return 0; }
