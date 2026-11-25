/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - INI-Dateien wie Windows 3.x (Get/WritePrivateProfileString,
 * WIN.INI ueber Get/WriteProfileString). Eigener Code.
 *
 * Eine Datei zur Zeit liegt im Speicher (Segment ini_sel, bis INI_MAX Byte);
 * Lesen sucht Abschnitt und Schluessel ohne Gross/klein, Schreiben aendert
 * den Puffer und schreibt die Datei sofort ganz zurueck. Dateinamen ohne
 * Pfad liegen im Windows-Verzeichnis (%WINDIR%, sonst X:\DOS\GUI auf dem
 * Laufwerk von COMSPEC - es wird beim ersten Schreiben angelegt).
 */
#include "gui.h"

#define INI_MAX 32000u

static WORD ini_sel, ini_len;
static char ini_name[FAPI_PATHLEN];     /* geladene Datei ("" = keine)   */
static char ini_path[FAPI_PATHLEN];
static char win_dir[FAPI_PATHLEN];

#define IB ((char FAR *)MKFP(ini_sel, 0))

static char up(char c) { return (char)(c >= 'a' && c <= 'z' ? c - 32 : c); }

static void windir(void)
{
    static char cs[FAPI_PATHLEN];
    if (win_dir[0])
        return;
    if (!FAPI_FAILED(DosGetEnv("WINDIR", win_dir, sizeof(win_dir))) && win_dir[0])
        return;
    fstrncpy(win_dir, "C:\\DOS\\GUI", sizeof(win_dir));
    if (!FAPI_FAILED(DosGetEnv("COMSPEC", cs, sizeof(cs))) && cs[1] == ':')
        win_dir[0] = cs[0];
}

WORD API GetWindowsDirectory(LPSTR buf, WORD n)
{
    windir();
    fstrncpy(buf, win_dir, n);
    return fstrlen(buf);
}
WORD API GetSystemDirectory(LPSTR buf, WORD n)
{
    WORD l = GetWindowsDirectory(buf, n);
    if (l + 8 < n) {
        fstrncpy(buf + l, "\\SYSTEM", n - l);
        l += 7;
    }
    return l;
}

/* voller Pfad der INI-Datei -> ini_path */
static void ini_full(LPCSTR f)
{
    LPCSTR p;
    WORD i = 0;
    for (p = f; *p; p++)
        if (*p == '\\' || *p == ':' || *p == '/')
            break;
    if (*p) {
        fstrncpy(ini_path, f, sizeof(ini_path));
    } else {
        windir();
        fstrncpy(ini_path, win_dir, sizeof(ini_path));
        i = fstrlen(ini_path);
        if (i && ini_path[i - 1] != '\\' && i < sizeof(ini_path) - 1)
            ini_path[i++] = '\\';
        fstrncpy(ini_path + i, f, sizeof(ini_path) - i);
    }
    for (i = 0; ini_path[i]; i++)
        ini_path[i] = up(ini_path[i]);
}

static void ini_load(LPCSTR f)
{
    WORD h, r;
    ini_full(f);
    if (!fstricmp(ini_path, ini_name))
        return;
    if (!ini_sel && !(ini_sel = seg_new(INI_MAX + 2)))
        return;
    fstrncpy(ini_name, ini_path, sizeof(ini_name));
    ini_len = 0;
    h = DosOpen(ini_path);
    if (FAPI_FAILED(h))
        return;
    r = fapi_sel(FAPI_READ, ini_sel, h, 0, INI_MAX);
    DosClose(h);
    if (!FAPI_FAILED(r))
        ini_len = r;
}

static void ini_save(void)
{
    WORD h, i;
    static char dir[FAPI_PATHLEN];
    h = DosCreate(ini_path);
    if (FAPI_FAILED(h)) {                       /* Verzeichnis anlegen   */
        fstrncpy(dir, ini_path, sizeof(dir));
        for (i = fstrlen(dir); i && dir[i - 1] != '\\'; i--)
            ;
        if (i > 3) {
            dir[i - 1] = 0;
            DosMkDir(dir);
        }
        h = DosCreate(ini_path);
        if (FAPI_FAILED(h)) {
            glogs("INI nicht schreibbar: ", ini_path);
            return;
        }
    }
    if (ini_len)
        fapi_sel(FAPI_WRITE, ini_sel, h, 0, ini_len);
    DosClose(h);
}

/* Zeile ab o: Ende (vor CR/LF) und naechste Zeile */
static WORD lend(WORD o)
{
    while (o < ini_len && IB[o] != '\r' && IB[o] != '\n')
        o++;
    return o;
}
static WORD lnext(WORD o)
{
    o = lend(o);
    if (o < ini_len && IB[o] == '\r') o++;
    if (o < ini_len && IB[o] == '\n') o++;
    return o;
}
static WORD skipsp(WORD o, WORD e)
{
    while (o < e && (IB[o] == ' ' || IB[o] == '\t'))
        o++;
    return o;
}
/* Text o..e gleich s (ohne Gross/klein, Leerzeichen hinten egal) */
static int same(WORD o, WORD e, LPCSTR s)
{
    while (e > o && (IB[e - 1] == ' ' || IB[e - 1] == '\t'))
        e--;
    while (o < e && *s) {
        if (up(IB[o]) != up(*s))
            return 0;
        o++, s++;
    }
    return o == e && !*s;
}

/* Abschnitt suchen: -> Stelle der ersten Zeile danach, 0xFFFF = fehlt */
static WORD sec_find(LPCSTR sec)
{
    WORD o = 0, e, a, b;
    while (o < ini_len) {
        e = lend(o);
        a = skipsp(o, e);
        if (a < e && IB[a] == '[') {
            for (b = a + 1; b < e && IB[b] != ']'; b++)
                ;
            if (b < e && same(a + 1, b, sec))
                return lnext(o);
        }
        o = lnext(o);
    }
    return 0xFFFF;
}
static int is_sec(WORD o)
{
    WORD e = lend(o), a = skipsp(o, e);
    return a < e && IB[a] == '[';
}
/* Schluessel im Abschnitt ab o: -> Zeilenanfang, *v = Wert, 0xFFFF = fehlt */
static WORD key_find(WORD o, LPCSTR key, WORD FAR *v)
{
    WORD e, a, q;
    for (; o < ini_len && !is_sec(o); o = lnext(o)) {
        e = lend(o);
        a = skipsp(o, e);
        if (a >= e || IB[a] == ';')
            continue;
        for (q = a; q < e && IB[q] != '='; q++)
            ;
        if (q < e && same(a, q, key)) {
            *v = q + 1;
            return o;
        }
    }
    return 0xFFFF;
}

/* Wert o..Zeilenende ohne Leerzeichen/Anfuehrungszeichen nach buf */
static short copy_val(WORD o, LPSTR buf, short n)
{
    WORD e = lend(o), k = 0;
    o = skipsp(o, e);
    while (e > o && (IB[e - 1] == ' ' || IB[e - 1] == '\t'))
        e--;
    if (e - o >= 2 && (IB[o] == '"' || IB[o] == '\'') && IB[e - 1] == IB[o])
        o++, e--;
    while (o < e && (short)k < n - 1)
        buf[k++] = IB[o++];
    buf[k] = 0;
    return k;
}

short API GetPrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR def,
                                  LPSTR buf, short n, LPCSTR f)
{
    WORD o, v, e, a, q, k = 0;
    if (!buf || n <= 0)
        return 0;
    buf[0] = 0;
    ini_load(f);
    if (!sec) {                                 /* alle Abschnitte       */
        for (o = 0; o < ini_len; o = lnext(o)) {
            e = lend(o); a = skipsp(o, e);
            if (a < e && IB[a] == '[')
                for (a++; a < e && IB[a] != ']' && (short)k < n - 2; a++)
                    buf[k++] = IB[a];
            if (a < e && IB[a] == ']' && (short)k < n - 1)
                buf[k++] = 0;
        }
        buf[k] = 0;
        return k ? k - 1 : 0;
    }
    o = sec_find(sec);
    if (o != 0xFFFF && !key) {                  /* alle Schluessel       */
        for (; o < ini_len && !is_sec(o); o = lnext(o)) {
            e = lend(o); a = skipsp(o, e);
            if (a >= e || IB[a] == ';')
                continue;
            for (q = a; q < e && IB[q] != '='; q++)
                ;
            if (q >= e)
                continue;
            while (q > a && (IB[q - 1] == ' ' || IB[q - 1] == '\t'))
                q--;
            for (; a < q && (short)k < n - 2; a++)
                buf[k++] = IB[a];
            buf[k++] = 0;
        }
        buf[k] = 0;
        return k ? k - 1 : 0;
    }
    if (o != 0xFFFF && key && key_find(o, key, &v) != 0xFFFF)
        return copy_val(v, buf, n);
    fstrncpy(buf, def ? def : "", n);
    return fstrlen(buf);
}

WORD API GetPrivateProfileInt(LPCSTR sec, LPCSTR key, short def, LPCSTR f)
{
    static char b[16];
    short i = 0, neg = 0;
    WORD v = 0;
    if (!GetPrivateProfileString(sec, key, "", b, sizeof(b), f))
        return def;
    if (b[0] == '-') neg = 1, i = 1;
    if (b[i] < '0' || b[i] > '9')
        return def;
    for (; b[i] >= '0' && b[i] <= '9'; i++)
        v = v * 10 + (b[i] - '0');
    return neg ? (WORD)-(short)v : v;
}

/* Bereich o..e (Bytes) durch s ersetzen */
static int splice(WORD o, WORD e, LPCSTR s, WORD n)
{
    WORD rest = ini_len - e;
    if ((unsigned long)ini_len - (e - o) + n > INI_MAX)
        return 0;
    fmemmove(IB + o + n, IB + e, rest);
    if (n)
        fmemcpy(IB + o, s, n);
    ini_len = ini_len - (e - o) + n;
    return 1;
}

static char wl[300];
BOOL API WritePrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR val, LPCSTR f)
{
    WORD o, v, ln, k;
    if (!sec)                                   /* Puffer leeren: sofort geschrieben */
        return 1;
    ini_load(f);
    if (!ini_sel)
        return 0;
    o = sec_find(sec);
    if (!key) {                                 /* Abschnitt loeschen    */
        WORD s0, e;
        if (o == 0xFFFF)
            return 1;
        for (s0 = 0; lnext(s0) < o && lnext(s0) != s0; s0 = lnext(s0))
            ;
        for (e = o; e < ini_len && !is_sec(e); e = lnext(e))
            ;
        splice(s0, e, 0, 0);
        ini_save();
        return 1;
    }
    if (o == 0xFFFF) {                          /* Abschnitt anhaengen   */
        if (!val)
            return 1;
        k = 0;
        if (ini_len && IB[ini_len - 1] != '\n') { wl[k++] = '\r'; wl[k++] = '\n'; }
        wl[k++] = '[';
        fstrncpy(wl + k, sec, sizeof(wl) - k - 4);
        k = fstrlen(wl);
        wl[k++] = ']'; wl[k++] = '\r'; wl[k++] = '\n';
        if (!splice(ini_len, ini_len, wl, k))
            return 0;
        o = ini_len;
    }
    ln = key_find(o, key, &v);
    k = 0;
    if (val) {
        fstrncpy(wl, key, 120);
        k = fstrlen(wl);
        wl[k++] = '=';
        fstrncpy(wl + k, val, sizeof(wl) - k - 3);
        k = fstrlen(wl);
        wl[k++] = '\r'; wl[k++] = '\n';
    }
    if (ln != 0xFFFF) {
        if (!splice(ln, lnext(ln), wl, k))
            return 0;
    } else if (val) {
        WORD e;                                 /* hinter die letzte Zeile mit Inhalt */
        for (e = o; e < ini_len && !is_sec(e); e = lnext(e))
            ;
        while (e > o && (IB[e - 1] == '\n' || IB[e - 1] == '\r')) {
            WORD p = e - 1;
            if (IB[p] == '\n' && p > o && IB[p - 1] == '\r') p--;
            if (p > o && (IB[p - 1] == '\n' || IB[p - 1] == '\r'))
                e = p;                          /* Leerzeile davor        */
            else
                break;
        }
        if (e > 0 && e == ini_len && IB[e - 1] != '\n') {
            if (!splice(e, e, "\r\n", 2))
                return 0;
            e += 2;
        }
        if (!splice(e, e, wl, k))
            return 0;
    }
    ini_save();
    return 1;
}

short API GetProfileString(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR buf, short n)
{
    return GetPrivateProfileString(sec, key, def, buf, n, "WIN.INI");
}
WORD API GetProfileInt(LPCSTR sec, LPCSTR key, short def)
{
    return GetPrivateProfileInt(sec, key, def, "WIN.INI");
}
BOOL API WriteProfileString(LPCSTR sec, LPCSTR key, LPCSTR val)
{
    return WritePrivateProfileString(sec, key, val, "WIN.INI");
}
void API WriteOutProfiles(void) { }
