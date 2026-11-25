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

/* ---- Datei oeffnen / speichern ---- */
#define ID_NAME  1152           /* edt1 */
#define ID_FILES 1120           /* lst1 */
#define ID_DIRS  1121           /* lst2 */
#define ID_PATH  1088           /* stc1 */
#define ID_TYPE  1089           /* stc2 */

static struct {
    BYTE FAR *ofn;
    BYTE  save;
    char  pat[80];
    char  cur[FAPI_PATHLEN];
    char  buf[FAPI_PATHLEN];
} fd;
static struct fapi_dirent fd_e;

static void fd_fill(HWND d)
{
    WORD i, x;
    HWND lf = GetDlgItem(d, ID_FILES), ld = GetDlgItem(d, ID_DIRS);
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
                    send_msg(lf, LB_ADDSTRING, 0, (DWORD)(void FAR *)fd_e.name);
                    break;
                }
            }
        }
    }
    for (i = 0; i < 26; i++) {
        static char dummy[FAPI_PATHLEN];
        if (FAPI_FAILED(DosQCurDirD(i, dummy)))
            continue;
        line[0] = '['; line[1] = '-'; line[2] = (char)('a' + i); line[3] = '-';
        line[4] = ']'; line[5] = 0;
        send_msg(ld, LB_INSERTSTRING, (WORD)send_msg(ld, LB_GETCOUNT, 0, 0),
                 (DWORD)(void FAR *)line);
    }
    DosQCurDir(fd.cur);
    for (x = 0; fd.cur[x]; x++)
        if (fd.cur[x] >= 'A' && fd.cur[x] <= 'Z')
            fd.cur[x] += 32;
    SetDlgItemText(d, ID_PATH, fd.cur);
}

static void fd_filter(HWND d)                   /* Filter nFilterIndex   */
{
    LPCSTR f = *(LPCSTR FAR *)(fd.ofn + 8);
    WORD idx = *(WORD FAR *)(fd.ofn + 20), k = 1;
    static char t[80];
    fstrncpy(fd.pat, "*.*", sizeof(fd.pat));
    fstrncpy(t, "Dateityp: alle Dateien", sizeof(t));
    if (!idx) idx = 1;
    while (f && *f) {
        LPCSTR desc = f, pat;
        f += fstrlen(f) + 1;
        pat = f;
        if (!*pat)
            break;
        f += fstrlen(f) + 1;
        if (k++ == idx) {
            fstrncpy(fd.pat, pat, sizeof(fd.pat));
            fstrncpy(t, "Dateityp: ", sizeof(t));
            fstrncpy(t + 10, desc, sizeof(t) - 10);
            break;
        }
    }
    SetDlgItemText(d, ID_TYPE, t);
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
                SendDlgItemMessage(d, ID_DIRS, LB_GETTEXT, s, (DWORD)(void FAR *)fd.buf);
                if (fd.buf[1] == '-') {
                    DosSelectDisk(fd.buf[2] - 'a');
                } else {
                    WORD n = fstrlen(fd.buf);
                    fd.buf[n - 1] = 0;
                    DosChDir(fd.buf + 1);
                }
                fd_fill(d);
            }
        }
        return 1;
    case IDOK:
        if (fd_ok(d))
            EndDialog(d, 1);
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
    LPCSTR tt = *(LPCSTR FAR *)(o + 44), idir = *(LPCSTR FAR *)(o + 40);
    LPSTR file = *(LPSTR FAR *)(o + 24);
    DWORD fl = *(DWORD FAR *)(o + 48);
    static char home[FAPI_PATHLEN];
    WORD hd = DosQCurDisk();
    short r;
    cderr = 0;
    fd.ofn = o;
    fd.save = (BYTE)save;
    DosQCurDir(home);
    if (idir && *idir) {
        if (idir[1] == ':')
            DosSelectDisk((idir[0] | 32) - 'a');
        fstrncpy(fd.buf, idir, sizeof(fd.buf));
        DosChDir(fd.buf);
    }
    d = dlg_new(owner, tt ? tt : (save ? "Speichern unter" : "Datei \xF6" "ffnen"),
                -1, -1, 440, 270, (IDLGPROC)fd_proc, 0);
    if (!d) {
        cderr = 2;                              /* CDERR_INITIALIZATION  */
        return 0;
    }
    dlg_item(d, "STATIC", "Datei&name:", 0, 10, 6, 180, 16, 0xFFFF);
    dlg_item(d, "EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 10, 24, 180, 22, ID_NAME);
    dlg_item(d, "LISTBOX", "", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_SORT,
             10, 52, 180, 180, ID_FILES);
    dlg_item(d, "STATIC", "&Verzeichnisse:", 0, 200, 6, 140, 16, 0xFFFF);
    dlg_item(d, "STATIC", "", 0x80, 200, 26, 230, 16, ID_PATH);
    dlg_item(d, "LISTBOX", "", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_SORT,
             200, 52, 140, 180, ID_DIRS);
    dlg_item(d, "STATIC", "", 0x80, 10, 240, 330, 16, ID_TYPE);
    dlg_item(d, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSH, 352, 52, 80, 26, IDOK);
    dlg_item(d, "BUTTON", "Abbrechen", WS_TABSTOP, 352, 86, 80, 26, IDCANCEL);
    fd_filter(d);
    if (file && *file) {
        if (has_wild(file))
            fstrncpy(fd.pat, file, sizeof(fd.pat));
        else
            SetDlgItemText(d, ID_NAME, file);
    }
    fd_fill(d);
    SetFocus(GetDlgItem(d, ID_NAME));
    r = dlg_run(d, owner);
    if (fl & 8) {                               /* OFN_NOCHANGEDIR       */
        DosSelectDisk(hd);
        DosChDir(home);
    }
    return r == 1;
}

BOOL API GetOpenFileName(BYTE FAR *o) { return file_dlg(o, 0); }
BOOL API GetSaveFileName(BYTE FAR *o) { return file_dlg(o, 1); }

/* ---- Suchen (nicht modal) ---- */
#define ID_WHAT  1152
#define ID_CASE  1041
#define ID_UP    1056
#define ID_DOWN  1057
static BYTE FAR *fr;
static HWND fr_dlg;
static WORD fr_msg;

static void fr_send(WORD flags)
{
    HWND owner = *(WORD FAR *)(fr + 4);
    DWORD fl = *(DWORD FAR *)(fr + 8) & ~0x007FUL;
    fl |= flags;
    *(DWORD FAR *)(fr + 8) = fl;
    send_msg(owner, fr_msg, 0, (DWORD)(void FAR *)fr);
}

static BOOL fr_proc(HWND d, WORD m, WORD wp, DWORD lp)
{
    (void)lp;
    if (m != WM_COMMAND)
        return 0;
    if (wp == IDOK) {
        LPSTR what = *(LPSTR FAR *)(fr + 12);
        WORD n = *(WORD FAR *)(fr + 20);
        WORD f = 8;                             /* FR_FINDNEXT           */
        GetDlgItemText(d, ID_WHAT, what, n);
        if (SendDlgItemMessage(d, ID_CASE, BM_GETCHECK, 0, 0)) f |= 4;
        if (SendDlgItemMessage(d, ID_DOWN, BM_GETCHECK, 0, 0)) f |= 1;
        fr_send(f);
        return 1;
    }
    if (wp == IDCANCEL) {
        fr_send(0x40);                          /* FR_DIALOGTERM         */
        fr_dlg = 0;
        DestroyWindow(d);
        return 1;
    }
    return 0;
}

HWND API FindText(BYTE FAR *f)
{
    HWND owner = *(WORD FAR *)(f + 4), d;
    DWORD fl = *(DWORD FAR *)(f + 8);
    cderr = 0;
    if (fr_dlg && wptr(fr_dlg))
        return fr_dlg;
    fr = f;
    fr_msg = RegisterWindowMessage("commdlg_FindReplace");
    d = dlg_new(owner, "Suchen", -1, 60, 440, 110, (IDLGPROC)fr_proc, 0);
    if (!d) {
        cderr = 2;
        return 0;
    }
    dlg_item(d, "STATIC", "Suchen &nach:", 0, 10, 12, 110, 16, 0xFFFF);
    dlg_item(d, "EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 120, 8, 200, 22, ID_WHAT);
    dlg_item(d, "BUTTON", "Gro\xDF-/&Kleinschreibung", WS_TABSTOP | BS_AUTOCHECK | WS_GROUP,
             10, 76, 200, 20, ID_CASE);
    dlg_item(d, "BUTTON", "Richtung", BS_GROUPBOX, 216, 40, 110, 62, 0xFFFF);
    dlg_item(d, "BUTTON", "Auf&w\xE4rts", WS_TABSTOP | BS_AUTORADIO | WS_GROUP, 222, 58, 100, 18, ID_UP);
    dlg_item(d, "BUTTON", "&Abw\xE4rts", BS_AUTORADIO, 222, 78, 100, 18, ID_DOWN);
    dlg_item(d, "BUTTON", "&Weitersuchen", WS_TABSTOP | BS_DEFPUSH | WS_GROUP, 330, 8, 104, 26, IDOK);
    dlg_item(d, "BUTTON", "Abbrechen", WS_TABSTOP, 330, 40, 104, 26, IDCANCEL);
    SetDlgItemText(d, ID_WHAT, *(LPSTR FAR *)(f + 12));
    SendDlgItemMessage(d, ID_CASE, BM_SETCHECK, (fl & 4) ? 1 : 0, 0);
    SendDlgItemMessage(d, (fl & 1) ? ID_DOWN : ID_UP, BM_SETCHECK, 1, 0);
    ShowWindow(d, 1);
    SetFocus(GetDlgItem(d, ID_WHAT));
    fr_dlg = d;
    return d;
}
HWND API ReplaceText(BYTE FAR *f) { (void)f; cderr = 2; return 0; }

/* ---- Drucken, Farben, Schriften: noch nicht ---- */
BOOL API PrintDlg(BYTE FAR *p)
{
    (void)p;
    cderr = 0x1008;                             /* PDERR_NODEFAULTPRN    */
    return 0;
}
BOOL API ChooseColor(BYTE FAR *p) { (void)p; cderr = 0; return 0; }
BOOL API ChooseFont(BYTE FAR *p) { (void)p; cderr = 0; return 0; }
