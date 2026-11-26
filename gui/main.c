/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - Hauptschleife und Hilfen (docs/GUI-WIN16.md).
 *
 * GUI [Programm.EXE [Argumente]] - startet die Oberflaeche in einer
 * Grafiksitzung (VGA 640x480x16) und darin das Win16-Programm. Die
 * Hauptschleife holt Maus und Tastatur ab, stoesst das Zeichnen an und
 * wechselt der Reihe nach in jeden Task, der etwas zu tun hat. Ohne Task
 * endet die GUI (spaeter: Shell).
 */
#define GUI_MAIN
#include "gui.h"

WORD gui_ds;
static WORD logh = 0xFFFF;
static char lbuf[160];

void fmemcpy(void FAR *d, const void FAR *s, WORD n)
{
    char FAR *a = d;
    const char FAR *b = s;
    while (n--)
        *a++ = *b++;
}
/* Bereiche duerfen sich ueberlappen */
void fmemmove(void FAR *d, const void FAR *s, WORD n)
{
    char FAR *a = d;
    const char FAR *b = s;
    if (OFFOF(a) <= OFFOF(b) || SELOF(a) != SELOF(b)) {
        while (n--)
            *a++ = *b++;
    } else {
        a += n; b += n;
        while (n--)
            *--a = *--b;
    }
}
void fmemset(void FAR *d, BYTE v, WORD n)
{
    char FAR *a = d;
    while (n--)
        *a++ = v;
}
WORD fstrlen(LPCSTR s)
{
    WORD n = 0;
    while (s[n])
        n++;
    return n;
}
void fstrncpy(char FAR *d, LPCSTR s, WORD n)
{
    while (n > 1 && *s) {
        *d++ = *s++;
        n--;
    }
    *d = 0;
}
static char up(char c) { return (char)(c >= 'a' && c <= 'z' ? c - 32 : c); }
int fstricmp(LPCSTR a, LPCSTR b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return (BYTE)up(*a) - (BYTE)up(*b);
}
int fstrcmp(LPCSTR a, LPCSTR b)
{
    while (*a && *a == *b)
        a++, b++;
    return (BYTE)*a - (BYTE)*b;
}

/* Protokoll GUI.LOG im Verzeichnis beim Start (Fehlersuche); voller Pfad,
   denn Programme wechseln das Verzeichnis (PROGMAN ins Windows-Verz.) */
static char logpath[FAPI_CURDIRLEN + 10];
/* je Zeile oeffnen und schliessen: nach einem Absturz steht alles drin */
static void lput(const char *s)
{
    WORD n = 0, h;
    if (logh == 0xFFFF)
        return;
    while (s[n])
        n++;
    h = DosAppend(logpath);
    if (FAPI_FAILED(h))
        return;
    DosWrite(h, s, n);
    DosClose(h);
}
void glog(const char *s)
{
    lput(s);
    lput("\r\n");
}
void glogn(const char *s, WORD v)
{
    static char h[6];
    int i;
    for (i = 0; i < 4; i++)
        h[i] = "0123456789ABCDEF"[(v >> (12 - 4 * i)) & 15];
    h[4] = 0;
    lput(s);
    lput(h);
    lput("\r\n");
}
void glogs(const char *s, LPCSTR t)
{
    fstrncpy(lbuf, t, sizeof(lbuf));
    lput(s);
    glog(lbuf);
}

WORD seg_new(WORD bytes)
{
    WORD s = DosAllocSeg(bytes);
    return FAPI_FAILED(s) ? 0 : s;
}
void seg_del(WORD sel)
{
    if (sel)
        DosFreeSeg(sel);
}

static char args[FAPI_ARGLEN];
static char prog[128];

static int fstrchr_(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return 1;
    return 0;
}

static char sp_full[FAPI_PATHLEN], sp_path[512];
static int sp_try(LPCSTR dir, WORD n, LPCSTR name)
{
    WORD k = 0, i, h;
    for (i = 0; i < n && k < FAPI_PATHLEN - 2; i++)
        sp_full[k++] = dir[i];
    if (k && sp_full[k - 1] != '\\')
        sp_full[k++] = '\\';
    for (i = 0; name[i] && k < FAPI_PATHLEN - 1; i++)
        sp_full[k++] = name[i];
    sp_full[k] = 0;
    h = DosOpen(sp_full);
    if (FAPI_FAILED(h))
        return 0;
    DosClose(h);
    return 1;
}
/* Datei ohne Pfad finden -> out (voller oder relativer Pfad), 1 gefunden */
int path_find(LPCSTR name, LPSTR out, WORD n)
{
    char *s, *e;
    int ok = 0;
    WORD i;
    for (i = 0; name[i]; i++)
        if (name[i] == '\\' || name[i] == ':')
            break;
    ok = sp_try("", 0, name);                   /* wie angegeben         */
    if (!ok && !name[i])
        ok = sp_try(sp_path, GetWindowsDirectory(sp_path, sizeof(sp_path)), name);
    if (!ok && !name[i]) {                      /* SYSTEM-Verzeichnis    */
        WORD k = GetWindowsDirectory(sp_path, sizeof(sp_path) - 8);
        if (k && sp_path[k - 1] != '\\')
            sp_path[k++] = '\\';
        fstrncpy(sp_path + k, "SYSTEM", 7);
        ok = sp_try(sp_path, k + 6, name);
    }
    if (!ok && !name[i] && cur_task >= 0 && wt[cur_task].hmod) {   /* Aufrufer */
        const char FAR *p = wm[wt[cur_task].hmod - 1].path;
        WORD k = 0, j;
        for (j = 0; p[j]; j++)
            if (p[j] == '\\')
                k = j;
        ok = k && sp_try(p, k, name);
    }
    if (!ok && !name[i] && !FAPI_FAILED(DosGetEnv("PATH", sp_path, sizeof(sp_path))))
        for (s = sp_path; *s && !ok; s = *e ? e + 1 : e) {
            for (e = s; *e && *e != ';'; e++)
                ;
            ok = e > s && sp_try(s, (WORD)(e - s), name);
        }
    if (ok && out)                              /* sonst steht er in sp_full */
        fstrncpy(out, sp_full, n);
    return ok;
}
/* Programm ohne Pfad starten wie Windows (LoadModule): aktuelles
   Verzeichnis, Windows-Verzeichnis, Verzeichnis des aufrufenden Programms,
   PATH (GUI aus C:\ mit PROGMAN in C:\DOS\GUI; PROGMAN wechselt vor
   WinExec ins Arbeitsverzeichnis) */
int start_search(LPCSTR name, LPCSTR a, WORD show)
{
    return path_find(name, 0, 0) ? task_start(sp_full, a, show) : 2;
}

void gui_main(void)
{
    char *p, *q;
    int any, t;
    gui_ds = SELOF((void FAR *)&gui_ds);
    {
        static char tv[4];
        if (!FAPI_FAILED(DosGetEnv("GUITRACE", tv, sizeof(tv))) && (tv[0] == '1' || tv[0] == '2'))
            w16_trace = tv[0] - '0';
    }
    {
        WORD n;
        DosQCurDir(logpath);
        for (n = 0; logpath[n]; n++)
            ;
        if (n && logpath[n - 1] != '\\')
            logpath[n++] = '\\';
        fstrncpy(logpath + n, "GUI.LOG", 8);
    }
    logh = DosCreate(logpath);
    if (FAPI_FAILED(logh))
        logh = 0xFFFF;
    else
        DosClose(logh);
    glog("AT-DOS GUI");
    if (w16_trace) {                            /* Absturzstellen zuordnen */
        void FAR *seg2 = (void FAR *)path_find;   /* Code 2 (krnl, user, gdi) */
        void FAR *seg3 = (void FAR *)glog;
        glogn("GUI Code-Selektoren: main ", SELOF(seg2) >> 3);
        glogn("                     glog ", SELOF(seg3) >> 3);
    }
    DosGetArgs(args);
    p = args;
    while (*p == ' ')
        p++;
    if (*p == '/' && p[1] == '?') {
        puts("GUI [Programm.EXE [Argumente]]\r\n"
             "Startet ein Windows-3.x-Programm in der Oberflaeche. Ohne Angabe\r\n"
             "die Shell aus SYSTEM.INI im Windows-Verzeichnis ([boot] shell=,\r\n"
             "Vorgabe PROGMAN.EXE) - wie Windows.\r\n");
        DosExit(1);
    }
    if (!*p) {                                  /* Shell wie Windows   */
        GetPrivateProfileString("boot", "shell", "PROGMAN.EXE", args,
                                sizeof(args), "SYSTEM.INI");
        p = args;
        while (*p == ' ')
            p++;
        if (!*p)
            p = "PROGMAN.EXE";
        glogs("Shell aus SYSTEM.INI: ", p);
    }
    q = prog;
    while (*p && *p != ' ' && q < prog + sizeof(prog) - 5)
        *q++ = *p++;
    *q = 0;
    for (q = prog; *q && *q != '.'; q++)
        ;
    if (!*q) {                                  /* ohne Endung: .EXE */
        q[0] = '.'; q[1] = 'E'; q[2] = 'X'; q[3] = 'E'; q[4] = 0;
    }
    while (*p == ' ')
        p++;
    krnl_init();
    gdi_init();
    user_init();
    t = task_start(prog, p, 1);
    if (t == 2 && !fstrchr_(prog, '\\') && !fstrchr_(prog, ':'))
        t = start_search(prog, p, 1);           /* Windows-Verz., PATH   */
    if (t != 0) {
        gdi_done();
        puts("GUI: Programm nicht geladen (siehe GUI.LOG)\r\n");
        glogn("task_start ", t);
        DosExit(2);
    }
    for (;;) {
        user_input();
        user_paint_pending();
        any = 0;
        for (t = 0; t < NWT; t++) {
            if (!wt[t].used)
                continue;
            if (wt[t].state == WT_DEAD) {
                task_reap(t);
                continue;
            }
            any = 1;
            if (wt[t].state == WT_RUN && user_task_has_work(t))
                task_run(t);
        }
        if (!any)
            break;
        mm_pump();
        gdi_flush();
        DosSleep(10);
    }
    gdi_done();
    glog("Ende");
    DosExit(0);
}
