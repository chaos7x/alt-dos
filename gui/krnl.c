/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS GUI - KERNEL fuer Win16-Programme: Tasks, Speicher, DOS.
 *
 * Tasks: jeder Win16-Task hat seinen Stapel (im eigenen Autodata wie bei
 * Windows, SS = DS). Gewechselt wird nur ueber die Hauptschleife der GUI
 * (main.c): ein Task, der auf Nachrichten wartet, springt mit tsw()
 * dorthin zurueck; sie springt in den naechsten Task mit Arbeit.
 *
 * Speicher: GlobalAlloc = eigenes Segment (Handle = Selektor, GlobalLock
 * liefert Selektor:0), lokale Halde im Autodata hinter dem Stapel.
 *
 * DOS: Win16-Programme rufen INT 21h direkt (Watcom-Bibliothek, DOS3Call).
 * Der Kernel leitet INT n aus Ring 3 an int_entry (gcrt.asm) weiter
 * (FAPI_SEGCTL SEG_INTHND); dos_int uebersetzt in FAPI-Aufrufe.
 */
#include "gui.h"

struct wtask wt[NWT];
int  cur_task = -1;
WORD main_ss, main_sp;
static WORD mainsave[2];
static char up(char c) { return (char)(c >= 'a' && c <= 'z' ? c - 32 : c); }

#pragma aux get_bp2 = 0x8B 0x46 0x02 __value [__ax];    /* mov ax,[bp+2] */
unsigned get_bp2(void);         /* in __loadds-Exporten: DS des Aufrufers */
#pragma aux lsl_ = 0x0F 0x03 0xC3 0x74 0x02 0x31 0xC0 __parm [__bx] __value [__ax];
unsigned lsl_(unsigned sel);    /* LSL ax,bx; bei Fehler 0             */

void task_yield_to_main(void)
{
    int t = cur_task;
    if (t < 0)
        return;
    cur_task = -1;
    tsw((WORD FAR *)&wt[t].ss, mainsave[0], mainsave[1]);
    cur_task = t;
}

void task_run(int t)
{
    cur_task = t;
    tsw((WORD FAR *)mainsave, wt[t].ss, wt[t].sp);
    cur_task = -1;
}

int task_of_inst(WORD hinst)
{
    int t;
    for (t = 0; t < NWT; t++)
        if (wt[t].used && wt[t].hinst == hinst)
            return t;
    return -1;
}

/* Task t endet (INT 21h 4Ch, Absturz). Laeuft er gerade, steht er noch
   auf seinem eigenen Stapel (im Autodata, das hier frei wird): dann erst
   zur Hauptschleife, die raeumt ihn mit task_reap auf. */
void task_reap(int t)
{
    if (!wt[t].used)
        return;
    user_task_end(t);
    if (wt[t].psp)
        seg_del(wt[t].psp);
    if (wt[t].env)
        seg_del(wt[t].env);
    if (wt[t].hmod)
        w16_free(wt[t].hmod - 1);
    wt[t].used = 0;
}

void task_exit(int t, WORD code)
{
    glogn("Task-Ende ", code);
    wt[t].quitcode = code;
    wt[t].state = WT_DEAD;
    if (t == cur_task) {
        cur_task = -1;
        tsw((WORD FAR *)&wt[t].ss, mainsave[0], mainsave[1]);
        for (;;)
            ;                                   /* nie wieder geweckt   */
    }
    task_reap(t);
}

/* ---- Start eines Programms ------------------------------------------ */
int task_start(LPCSTR path, LPCSTR args, WORD show)
{
    int t, m;
    WORD ds, sp, n;
    WORD FAR *st;
    char FAR *psp;
    struct w16mod *w;
    for (t = 0; t < NWT && wt[t].used; t++)
        ;
    if (t == NWT)
        return 1;
    m = w16_load(path);
    if (m < 0)
        return 2;
    w = &wm[m];
    if (w->flags & 0x8000u) {                   /* DLL                  */
        w16_free(m);
        return 3;
    }
    fmemset((void FAR *)&wt[t], 0, sizeof(wt[t]));
    wt[t].used = 1;
    wt[t].hmod = m + 1;
    fstrncpy(wt[t].name, w->name, 9);
    ds = w->seg[w->autodata - 1].sel;
    wt[t].hinst = ds;
    /* PSP: 256 Byte, Befehlszeile bei 80h (Laenge, Text, CR, 0) */
    wt[t].psp = seg_new(256);
    psp = MKFP(wt[t].psp, 0);
    psp[0] = (char)0xCD; psp[1] = 0x20;
    n = fstrlen(args);
    if (n > 120)
        n = 120;
    /* wie InitTask von Windows: das CR am Ende wird zur 0 (lpCmdLine
       ist eine C-Zeichenkette, ohne Argumente leer) */
    if (n) {
        psp[0x80] = (char)(n + 1);
        psp[0x81] = ' ';
        fmemcpy(psp + 0x82, args, n);
        psp[0x82 + n] = 0;
    } else {
        psp[0x80] = 0;
        psp[0x81] = 0;
    }
    /* Umgebung wie DOS: leer (0 0), Zaehler 1, Programmpfad. Die
       Startroutinen der C-Laufzeit durchsuchen PSP:2Ch - ein Selektor 0
       gibt dort #GP (QEMU laesst es durch, 86Box und echte CPUs nicht). */
    n = fstrlen(w->path);
    wt[t].env = seg_new(n + 16);
    if (wt[t].env) {
        char FAR *e = MKFP(wt[t].env, 0);
        e[0] = 0; e[1] = 0; e[2] = 1; e[3] = 0;
        fmemcpy(e + 4, w->path, n);
        e[4 + n] = 0;
        *(WORD FAR *)(psp + 0x2C) = wt[t].env;
    }
    wt[t].dta_s = wt[t].psp;
    wt[t].dta_o = 0x80;
    /* Stapel: Rahmen fuer tsw (ds, di, si, bp, IP, CS) -> tstart */
    sp = w->sp;
    st = MKFP(ds, sp - 12);
    st[0] = gui_ds;     /* ds */
    st[1] = 0;          /* di */
    st[2] = 0;          /* si */
    st[3] = 0;          /* bp */
    st[4] = OFFOF((void FAR *)tstart);
    st[5] = SELOF((void FAR *)tstart);
    wt[t].ss = ds;
    wt[t].sp = sp - 12;
    wt[t].state = WT_RUN;
    wt[t].s_wp = show;                          /* nCmdShow fuer InitTask */
    /* Register fuer den ersten Einsprung */
    ts_ax = 0; ts_bx = w->stack; ts_cx = w->heap;
    ts_di = ds; ts_si = 0; ts_es = wt[t].psp; ts_ds = ds;
    ts_cs = w->seg[w->entcs - 1].sel; ts_ip = w->entip;
    glogs("Start ", path);
    task_run(t);                                /* bis zum ersten Warten */
    return 0;
}

/* ---- lokale Halde ----------------------------------------------------
   Im Segment ds: Kopf bei hs (Wort Anfang, Wort Ende), Bloecke mit einem
   Wort Laenge (gerade, Bit 0 = belegt), Daten dahinter. Zeiger = Handle. */
/* Kopf der Halde: Bloecke ab start, Ende, erste Grifftabelle (0 = keine) */
struct lheap { WORD start, end, ht; };

/* Verschiebbare Griffe wie LMEM_MOVEABLE von Windows: ein Griff ist die
   Stelle eines Eintrags {Stelle des Blocks, Sperrzaehler} in einer
   Grifftabelle (Block der Halde: naechste Tabelle, LH_NHE Eintraege).
   LocalReAlloc verschiebt den Block, der Griff bleibt - Programme wie
   Notepad halten sich den Griff aus EM_GETHANDLE. */
#define LH_NHE   32
#define LH_FREE  0xFFFFu

static WORD lh_of(WORD ds)
{
    return *(WORD FAR *)MKFP(ds, 6);
}

BOOL lh_init(WORD ds, WORD start, WORD end)
{
    struct lheap FAR *h;
    WORD FAR *b;
    start = (start + 1) & ~1u;
    end &= ~1u;
    if (end < start + 16)
        return 0;
    h = MKFP(ds, start);
    h->start = start + 6;
    h->end = end;
    h->ht = 0;
    b = MKFP(ds, start + 6);
    *b = end - (start + 6);
    *(WORD FAR *)MKFP(ds, 6) = start;
    return 1;
}

/* Halde am Ende des Segments vergroessern wie Windows (LocalAlloc waechst
   das Autodata bis 64 KB); Handles sind Stellen, sie bleiben gueltig */
static BOOL lh_grow(WORD ds, WORD need)
{
    WORD hs = lh_of(ds), lim = lsl_(ds), add;
    DWORD nsz;
    struct lheap FAR *h = MKFP(ds, hs);
    if (h->end < lim - 1) {                     /* Segment hat noch Platz  */
        add = (lim + 1 - h->end) & ~1u;
    } else {
        nsz = (DWORD)lim + 1 + need + 1024;
        if (nsz > 0xFFF0UL)
            nsz = 0xFFF0UL;
        if (nsz <= (DWORD)lim + 1)
            return 0;
        if (FAPI_FAILED(fapi_call(FAPI_SEGCTL, SEG_REALLOC, ds, (WORD)nsz)))
            return 0;
        lim = lsl_(ds);
        add = (lim + 1 - h->end) & ~1u;
        if (lim == 0xFFFF)
            add = (0xFFF0u - h->end) & ~1u;
    }
    if (add < 6)
        return 0;
    *(WORD FAR *)MKFP(ds, h->end) = add;        /* neuer freier Block     */
    h->end += add;
    return 1;
}

WORD lh_alloc(WORD ds, WORD n, WORD zero)
{
    WORD hs = lh_of(ds), p, sz, need, nx;
    struct lheap FAR *h;
    if (!hs)
        return 0;
    h = MKFP(ds, hs);
    need = (n + 2 + 1) & ~1u;
    if (need < 4)
        need = 4;
again:
    for (p = h->start; p < h->end; p += sz) {
        sz = *(WORD FAR *)MKFP(ds, p) & ~1u;
        if (!sz)
            break;
        if (*(WORD FAR *)MKFP(ds, p) & 1)
            continue;
        /* freie Nachbarn zusammenlegen */
        while (p + sz < h->end) {
            nx = *(WORD FAR *)MKFP(ds, p + sz);
            if (nx & 1 || !nx)
                break;
            sz += nx;
        }
        *(WORD FAR *)MKFP(ds, p) = sz;
        if (sz >= need) {
            if (sz - need >= 6) {
                *(WORD FAR *)MKFP(ds, p + need) = sz - need;
                sz = need;
            }
            *(WORD FAR *)MKFP(ds, p) = sz | 1;
            if (zero)
                fmemset(MKFP(ds, p + 2), 0, sz - 2);
            return p + 2;
        }
    }
    if (need < 0xFF00u && lh_grow(ds, need))
        goto again;
    return 0;
}

static WORD raw_size(WORD ds, WORD a)
{
    return a ? (*(WORD FAR *)MKFP(ds, a - 2) & ~1u) - 2 : 0;
}

static void raw_free(WORD ds, WORD a)
{
    if (a)
        *(WORD FAR *)MKFP(ds, a - 2) &= ~1u;
}

/* Eintrag zu einem Griff, sonst NULL (dann ist es eine feste Stelle) */
static WORD FAR *lh_hent(WORD ds, WORD x)
{
    WORD hs = lh_of(ds), t;
    if (!hs || !x)
        return 0;
    for (t = ((struct lheap FAR *)MKFP(ds, hs))->ht; t;
         t = *(WORD FAR *)MKFP(ds, t)) {
        if (x >= t + 2 && x < t + 2 + 4 * LH_NHE && !((x - t - 2) & 3)) {
            WORD FAR *e = MKFP(ds, x);
            return e[1] == LH_FREE ? 0 : e;
        }
    }
    return 0;
}

WORD lh_ptr(WORD ds, WORD x)
{
    WORD FAR *e = lh_hent(ds, x);
    return e ? e[0] : x;
}

/* neuer verschiebbarer Block -> Griff */
WORD lh_hnew(WORD ds, WORD n, WORD zero)
{
    WORD hs = lh_of(ds), t, i, p;
    struct lheap FAR *h;
    WORD FAR *tb;
    if (!hs)
        return 0;
    h = MKFP(ds, hs);
    for (t = h->ht; t; t = *(WORD FAR *)MKFP(ds, t)) {
        tb = MKFP(ds, t + 2);
        for (i = 0; i < LH_NHE; i++)
            if (tb[2 * i + 1] == LH_FREE)
                goto found;
    }
    t = lh_alloc(ds, 2 + 4 * LH_NHE, 0);
    if (!t)
        return 0;
    h = MKFP(ds, lh_of(ds));                    /* Segment kann gewachsen sein */
    *(WORD FAR *)MKFP(ds, t) = h->ht;
    h->ht = t;
    tb = MKFP(ds, t + 2);
    for (i = 0; i < LH_NHE; i++)
        tb[2 * i] = 0, tb[2 * i + 1] = LH_FREE;
    i = 0;
found:
    p = n ? lh_alloc(ds, n, zero) : 0;
    if (n && !p)
        return 0;
    tb = MKFP(ds, t + 2);
    tb[2 * i] = p;
    tb[2 * i + 1] = 0;
    return t + 2 + 4 * i;
}

WORD lh_size(WORD ds, WORD a)
{
    return raw_size(ds, lh_ptr(ds, a));
}

void lh_free(WORD ds, WORD a)
{
    WORD FAR *e = lh_hent(ds, a);
    if (e) {
        raw_free(ds, e[0]);
        e[0] = 0;
        e[1] = LH_FREE;
    } else
        raw_free(ds, a);
}

/* ---- Exporte KERNEL ---------------------------------------------- */
#define GMEM_ZEROINIT 0x40
#define LMEM_ZEROINIT 0x40

/* InitTask (KERNEL.91): Register; die Exportliste ruft init_task */
void init_task_c(void)
{
    int t = cur_task;
    struct w16mod *w = &wm[wt[t].hmod - 1];
    WORD ds = wt[t].hinst, top = w->sp, hs, he;
    WORD FAR *hd = MKFP(ds, 0);
    hd[5] = w->sp - w->stack;           /* pStackTop (oberste Grenze) */
    hd[6] = top;                        /* pStackMin                  */
    hd[7] = top;                        /* pStackBot                  */
    hs = w->sp;
    he = hs + w->heap;
    if (he < hs || w->heap == 0)
        he = hs + 512;
    lh_init(ds, hs, he);
    ts_ax = 1;
    /* wie InitTask von Windows: ES:BX auf die Befehlszeile ohne
       fuehrende Leerzeichen (Notepad nimmt lpCmdLine als Dateinamen) */
    {
        char FAR *c = MKFP(wt[t].psp, 0x81);
        WORD o = 0x81;
        while (o < 0xFF && (*c == ' ' || *c == '\t'))
            c++, o++;
        ts_bx = o;
    }
    ts_cx = w->sp - w->stack + 128;
    ts_dx = wt[t].s_wp;
    ts_si = 0;
    ts_di = ds;
    ts_es = wt[t].psp;
}

WORD API WaitEvent(WORD task) { (void)task; return 0; }
DWORD API GetVersion(void) { return 0x06000A03UL; }   /* Win 3.10, DOS 6.0 */
DWORD API GetWinFlags(void) { return 0x0413UL; }     /* PMODE|286|STANDARD|CPU386? */

short API GetModuleFileName(WORD hinst, LPSTR buf, short n)
{
    int t = task_of_inst(hinst), m;
    if (hinst > 0 && hinst <= NMOD)
        m = hinst - 1;
    else if (t >= 0)
        m = wt[t].hmod - 1;
    else if (cur_task >= 0)
        m = wt[cur_task].hmod - 1;
    else
        return 0;
    fstrncpy(buf, wm[m].path, n);
    return fstrlen(buf);
}

WORD API GetModuleHandle(LPCSTR name)
{
    int m;
    if (SELOF(name) == 0)
        return OFFOF(name);
    for (m = 0; m < NMOD; m++)
        if (wm[m].used && !fstricmp(wm[m].name, name))
            return m + 1;
    m = exp_builtin(name);
    return m ? 0x100 + m - 1 : 0;
}

void API OutputDebugString(LPCSTR s) { glogs("Debug: ", s); }
BOOL API IsDBCSLeadByte(WORD c) { (void)c; return 0; }

DWORD API GetProcAddress(WORD hmod, LPCSTR name)
{
    void FAR *fp;
    WORD isc;
    if (hmod >= 0x100 && hmod < 0x110 && !SELOF(name)) {
        fp = exp_ord(hmod - 0x100, OFFOF(name), &isc);
        return fp && !isc ? (DWORD)fp : 0;
    }
    if (SELOF(name))
        glogs("GetProcAddress: ", name);
    return 0;
}

short API GetModuleUsage(WORD h) { return h && h <= NMOD ? wm[h - 1].refs : 0; }

WORD API GetCurrentTask(void) { return cur_task + 1; }
BOOL API Yield(void)
{
    task_yield_to_main();
    return 0;
}

void API FatalExit(short code)
{
    glogn("FatalExit ", code);
    task_exit(cur_task, 0xFF);
}
void API FatalAppExit(WORD f, LPCSTR msg)
{
    (void)f;
    glogs("FatalAppExit: ", msg);
    task_exit(cur_task, 0xFF);
}

WORD API GlobalAlloc(WORD flags, DWORD size)
{
    (void)flags;
    if (size > 0x10000UL)
        return 0;
    if (!size)
        size = 1;
    return seg_new((WORD)size);                 /* Kernel nullt immer */
}
WORD API GlobalReAlloc(WORD h, DWORD size, WORD flags)
{
    (void)flags;
    if (size > 0x10000UL || !h)
        return 0;
    if (FAPI_FAILED(fapi_call(FAPI_SEGCTL, SEG_REALLOC, h, (WORD)size)))
        return 0;
    return h;
}
WORD API GlobalFree(WORD h) { seg_del(h & ~3u | 7); return 0; }
DWORD API GlobalLock(WORD h) { return h ? MAKELONG(0, h | 7) : 0; }
BOOL API GlobalUnlock(WORD h) { (void)h; return 0; }
DWORD API GlobalSize(WORD h) { return h ? (DWORD)lsl_(h | 7) + 1 : 0; }
DWORD API GlobalHandle(WORD sel) { return MAKELONG(sel, sel); }
WORD API GlobalFlags(WORD h) { (void)h; return 0; }
DWORD API GlobalCompact(DWORD n) { (void)n; return 0x100000UL; }
WORD API LockSegment(WORD s) { return s; }
void API UnlockSegment(WORD s) { (void)s; }
DWORD API GlobalWire(WORD h) { return GlobalLock(h); }
BOOL API GlobalUnWire(WORD h) { (void)h; return 1; }
WORD API AllocSelector(WORD s) { (void)s; return 0; }
WORD API FreeSelector(WORD s) { (void)s; return 0; }

WORD API LocalAlloc(WORD flags, WORD n)
{
    WORD ds = get_bp2();
    if (flags & 0x02)                           /* LMEM_MOVEABLE: Griff */
        return lh_hnew(ds, n, flags & LMEM_ZEROINIT);
    return lh_alloc(ds, n, flags & LMEM_ZEROINIT);
}
WORD API LocalFree(WORD h)
{
    WORD ds = get_bp2();
    lh_free(ds, h);
    return 0;
}
static WORD raw_realloc(WORD ds, WORD h, WORD n)
{
    WORD nh, old;
    if (!h)
        return lh_alloc(ds, n, 1);
    old = raw_size(ds, h);
    if (n <= old)
        return h;
    nh = lh_alloc(ds, n, 1);
    if (!nh)
        return 0;
    fmemcpy(MKFP(ds, nh), MKFP(ds, h), old);
    raw_free(ds, h);
    return nh;
}
/* Griff: Block verschieben, Griff bleibt; feste Stelle: neue Stelle */
WORD lh_realloc(WORD ds, WORD h, WORD n)
{
    WORD FAR *e = lh_hent(ds, h);
    WORD p;
    if (!e)
        return raw_realloc(ds, h, n);
    p = raw_realloc(ds, e[0], n);
    if (!p)
        return 0;
    e = lh_hent(ds, h);                         /* Segment kann gewachsen sein */
    e[0] = p;
    return h;
}
#define LMEM_MOVEABLE 0x02
#define LMEM_MODIFY   0x80
WORD API LocalReAlloc(WORD h, WORD n, WORD flags)
{
    if (flags & LMEM_MODIFY)
        return h;
    return lh_realloc(get_bp2(), h, n);
}
/* Griff-Aufrufe: op 0 Lock, 1 Unlock, 2 Handle, 3 Flags. Die Exporte
   holen nur DS (get_bp2 gilt nur ohne gesicherte SI/DI vor BP) und
   rufen dann diese Hilfe. */
static WORD lh_api(WORD ds, WORD h, int op)
{
    WORD FAR *e;
    WORD hs, t, i;
    if (op == 2) {
        hs = lh_of(ds);
        if (hs)
            for (t = ((struct lheap FAR *)MKFP(ds, hs))->ht; t;
                 t = *(WORD FAR *)MKFP(ds, t))
                for (i = 0; i < LH_NHE; i++) {
                    e = MKFP(ds, t + 2 + 4 * i);
                    if (e[1] != LH_FREE && e[0] == h && h)
                        return t + 2 + 4 * i;
                }
        return h;
    }
    e = lh_hent(ds, h);
    if (!e)
        return op == 0 ? h : 0;
    if (op == 0) {
        if ((e[1] & 0xFF) < 0xFF)
            e[1]++;
        return e[0];
    }
    if (op == 1) {
        if (e[1] & 0xFF)
            e[1]--;
        return (e[1] & 0xFF) != 0;
    }
    return e[1] & 0xFF;
}
WORD API LocalLock(WORD h)    { WORD ds = get_bp2(); return lh_api(ds, h, 0); }
BOOL API LocalUnlock(WORD h)  { WORD ds = get_bp2(); return lh_api(ds, h, 1); }
WORD API LocalSize(WORD h)    { WORD ds = get_bp2(); return lh_size(ds, h); }
WORD API LocalHandle(WORD p)  { WORD ds = get_bp2(); return lh_api(ds, p, 2); }
WORD API LocalFlags(WORD h)   { WORD ds = get_bp2(); return lh_api(ds, h, 3); }
WORD API LocalCompact(WORD n) { (void)n; return 0x1000; }
BOOL API LocalInit(WORD seg, WORD start, WORD end)
{
    if (!seg)
        seg = get_bp2();
    if (!start) {               /* Windows: ganzes Segment ab ...      */
        start = 16;
        end = (WORD)lsl_(seg) - 1;
    }
    return lh_init(seg, start, end);
}

DWORD API MakeProcInstance(DWORD fn, WORD hinst) { (void)hinst; return fn; }
void API FreeProcInstance(DWORD fn) { (void)fn; }
DWORD API GetDOSEnvironment(void)
{
    static WORD envsel;
    if (!envsel && (envsel = seg_new(16)) != 0)
        fmemset(MKFP(envsel, 0), 0, 16);
    return MAKELONG(0, envsel);
}

short API lstrlen(LPCSTR s) { return s ? fstrlen(s) : 0; }
LPSTR API lstrcpy(LPSTR d, LPCSTR s)
{
    LPSTR r = d;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}
LPSTR API lstrcat(LPSTR d, LPCSTR s)
{
    LPSTR r = d;
    while (*d)
        d++;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

/* Profile: noch keine INI-Dateien - Vorgabe zurueck */
short API GetProfileString(LPCSTR a, LPCSTR k, LPCSTR def, LPSTR buf, short n)
{
    (void)a; (void)k;
    fstrncpy(buf, def ? def : "", n);
    return fstrlen(buf);
}
WORD API GetProfileInt(LPCSTR a, LPCSTR k, short def) { (void)a; (void)k; return def; }
short API GetPrivateProfileString(LPCSTR a, LPCSTR k, LPCSTR def, LPSTR buf, short n, LPCSTR f)
{
    (void)f;
    return GetProfileString(a, k, def, buf, n);
}
WORD API GetPrivateProfileInt(LPCSTR a, LPCSTR k, short def, LPCSTR f) { (void)a; (void)k; (void)f; return def; }
BOOL API WriteProfileString(LPCSTR a, LPCSTR k, LPCSTR v) { (void)a; (void)k; (void)v; return 1; }
BOOL API WritePrivateProfileString(LPCSTR a, LPCSTR k, LPCSTR v, LPCSTR f) { (void)a; (void)k; (void)v; (void)f; return 1; }

/* ---- Dateien (KERNEL _lopen & Co., auch fuer INT 21h) ----------------
   FAPI_SEEK kennt nur die Stelle vom Anfang: Stelle und Groesse fuehrt
   die GUI je Handle selbst. */
static char fbuf[260];
#define NFH 48
static struct { DWORD pos, size; } fh[NFH];
static struct fapi_dirent fd_e;

int wmatch(const char *p, const char FAR *n)
{
    for (;;) {
        if (*p == '*') {
            p++;
            if (!*p)
                return 1;
            if (*p == '.' && !p[1] && !*n)
                return 1;
            while (*n) {
                if (wmatch(p, n))
                    return 1;
                n++;
            }
            return wmatch(p, n);
        }
        if (!*p)
            return !*n;
        if (*p == '.' && !*n && (p[1] == '*' || !p[1]))
            return p[1] != '*' || !p[2];
        if (*p != '?' && up(*p) != up(*n))
            return 0;
        if (!*n)
            return 0;
        p++, n++;
    }
}

/* Groesse einer Datei ueber DosFindEntry (Verzeichnis + Name) */
static DWORD f_size(void)
{
    static char dir[260];
    char *b = fbuf, *ls = 0;
    WORD i, x;
    for (; *b; b++)
        if (*b == '\\' || *b == ':')
            ls = b;
    if (ls) {
        i = (WORD)(ls - fbuf) + 1;
        fmemcpy(dir, fbuf, i);
        dir[i] = 0;
        if (*ls == '\\' && i > 1 && dir[i - 2] != ':')
            dir[i - 1] = 0;
        b = ls + 1;
    } else {
        dir[0] = 0;
        b = fbuf;
    }
    for (i = 0; ; i++) {
        x = fapi_call(FAPI_FINDENTRY, (WORD)&fd_e, i, dir[0] ? (WORD)dir : 0);
        if (FAPI_FAILED(x))
            return 0;
        if (wmatch(b, fd_e.name))
            return fd_e.size;
    }
}

static WORD f_open(LPCSTR name, WORD mode)
{
    WORD h;
    fstrncpy(fbuf, name, sizeof(fbuf));
    h = (mode & 3) ? fapi_call(FAPI_OPEN, (WORD)fbuf, OPEN_RW, 0)
                   : fapi_call(FAPI_OPEN, (WORD)fbuf, 0, 0);
    if (!FAPI_FAILED(h) && h < NFH) {
        fh[h].pos = 0;
        fh[h].size = f_size();
    }
    return h;
}
static WORD f_create(LPCSTR name)
{
    WORD h;
    fstrncpy(fbuf, name, sizeof(fbuf));
    h = fapi_call(FAPI_CREATE, (WORD)fbuf, 0, 0);
    if (!FAPI_FAILED(h) && h < NFH)
        fh[h].pos = fh[h].size = 0;
    return h;
}
static WORD f_rw(WORD fn, WORD h, void FAR *p, WORD n)
{
    WORD got = 0, k, r;
    while (n) {
        k = n > 0x7000 ? 0x7000 : n;
        r = fapi_sel(fn, SELOF(p), h, OFFOF(p) + got, k);
        if (FAPI_FAILED(r))
            return got ? got : 0xFFFF;
        got += r;
        n -= r;
        if (r < k)
            break;
    }
    if (h < NFH) {
        fh[h].pos += got;
        if (fh[h].pos > fh[h].size)
            fh[h].size = fh[h].pos;
    }
    return got;
}
static DWORD f_seek(WORD h, DWORD pos, WORD how)
{
    static unsigned long sp;
    if (h >= NFH)
        return 0xFFFFFFFFUL;
    if (how == 1)
        pos += fh[h].pos;
    else if (how == 2)
        pos += fh[h].size;
    sp = pos;
    if (FAPI_FAILED(fapi_call(FAPI_SEEK, h, (WORD)&sp, 0)))
        return 0xFFFFFFFFUL;
    fh[h].pos = pos;
    return pos;
}

WORD API _lopen(LPCSTR name, WORD mode)
{
    WORD h = f_open(name, mode);
    return FAPI_FAILED(h) ? 0xFFFF : h;
}
WORD API _lcreat(LPCSTR name, WORD attr)
{
    WORD h;
    (void)attr;
    h = f_create(name);
    return FAPI_FAILED(h) ? 0xFFFF : h;
}
WORD API _lclose(WORD h) { DosClose(h); return 0; }
WORD API _lread(WORD h, LPVOID p, WORD n) { return f_rw(FAPI_READ, h, p, n); }
WORD API _lwrite(WORD h, LPCSTR p, WORD n) { return f_rw(FAPI_WRITE, h, (void FAR *)p, n); }
DWORD API _llseek(WORD h, DWORD pos, short how) { return f_seek(h, pos, how); }

/* voller Pfad wie Windows (OFSTRUCT.szPathName) */
static WORD derr(WORD f);
static char of_cur[FAPI_PATHLEN];
void full_path(LPCSTR n, LPSTR out, WORD max)
{
    WORD k = 0;
    if (n[0] && n[1] == ':' && n[2] == '\\') {
        fstrncpy(out, n, max);
    } else {
        if (n[0] && n[1] == ':') {
            if (FAPI_FAILED(DosQCurDirD(up(n[0]) - 'A', of_cur))) {
                of_cur[0] = up(n[0]); of_cur[1] = ':'; of_cur[2] = '\\'; of_cur[3] = 0;
            }
            n += 2;
        } else
            DosQCurDir(of_cur);
        if (n[0] == '\\')
            of_cur[2] = 0;
        fstrncpy(out, of_cur, max);
        k = fstrlen(out);
        if (n[0] && n[0] != '\\' && k > 3 && k < max - 1) {
            out[k++] = '\\';
            out[k] = 0;
        }
        fstrncpy(out + k, n, max - k);
    }
    for (k = 0; out[k]; k++)
        out[k] = up(out[k]);
}

/* OpenFile (KERNEL.74): OFSTRUCT = Laenge, fest, Fehler, 4 frei, Pfad[128] */
WORD API OpenFile(LPCSTR name, BYTE FAR *of, WORD st)
{
    WORD h;
    LPSTR p = (LPSTR)(of + 8);
    if (!(st & 0x8000))                         /* OF_REOPEN: Pfad gilt     */
        full_path(name, p, 128);
    of[0] = 136;
    of[1] = 1;
    *(WORD FAR *)(of + 2) = 0;
    if (st & 0x100)                             /* OF_PARSE                 */
        return 0;
    if (st & 0x200) {                           /* OF_DELETE                */
        fstrncpy(fbuf, p, sizeof(fbuf));
        if (FAPI_FAILED(DosDelete(fbuf))) {
            *(WORD FAR *)(of + 2) = 2;
            return 0xFFFF;
        }
        return 1;
    }
    if (st & 0x1000)                            /* OF_CREATE                */
        h = f_create(p);
    else
        h = f_open(p, st & 3);
    if (FAPI_FAILED(h)) {
        *(WORD FAR *)(of + 2) = derr(h) == 5 ? 5 : 2;
        return 0xFFFF;
    }
    if (st & 0x4000) {                          /* OF_EXIST: wieder zu      */
        DosClose(h);
        return 1;
    }
    return h;
}

/* ---- INT 21h ---------------------------------------------------------- */
struct iregs { WORD di, si, dx, cx, bx, ax, es, ds, bp, n, ip, cs, fl; };
#define CF 1
static void ok(struct iregs FAR *r) { r->fl &= ~CF; }
static void err(struct iregs FAR *r, WORD c) { r->fl |= CF; r->ax = c; }
static WORD derr(WORD f)
{
    switch (f) {
    case FAPI_ERR_EXIST: return 80;
    case FAPI_ERR_DENIED: case FAPI_ERR_INUSE: case FAPI_ERR_WP: return 5;
    case FAPI_ERR_FULL: return 39;
    default: return 2;
    }
}

static WORD fd_n;
static char fd_pat[80], fd_dir[80];

static void find(struct iregs FAR *r, int first)
{
    int t = cur_task;
    char FAR *dta = MKFP(wt[t].dta_s, wt[t].dta_o);
    char *b, *ls = 0;
    WORD x, i;
    if (first) {
        fstrncpy(fd_dir, MKFP(r->ds, r->dx), sizeof(fd_dir));
        for (b = fd_dir; *b; b++)
            if (*b == '\\' || *b == ':')
                ls = b;
        if (ls) {
            for (i = 0; ls[1 + i] && i < 13; i++)
                fd_pat[i] = ls[1 + i];
            fd_pat[i] = 0;
            ls[1] = 0;
            if (*ls == '\\' && ls > fd_dir && ls[-1] != ':')
                *ls = 0;
        } else {
            fstrncpy(fd_pat, fd_dir, sizeof(fd_pat));
            fd_dir[0] = 0;
        }
        fd_n = 0;
    }
    for (;;) {
        x = fapi_call(FAPI_FINDENTRY, (WORD)&fd_e, fd_n++, fd_dir[0] ? (WORD)fd_dir : 0);
        if (FAPI_FAILED(x)) {
            err(r, first ? 2 : 18);
            return;
        }
        if ((fd_e.attr & 0x16) & ~(r->cx & 0x16))
            continue;
        if (wmatch(fd_pat, fd_e.name))
            break;
    }
    dta[21] = fd_e.attr;
    *(WORD FAR *)(dta + 22) = fd_e.time;
    *(WORD FAR *)(dta + 24) = fd_e.date;
    *(DWORD FAR *)(dta + 26) = fd_e.size;
    fstrncpy(dta + 30, fd_e.name, 13);
    ok(r);
}

void dos_int(void FAR *rv)
{
    struct iregs FAR *r = rv;
    int t = cur_task;
    BYTE ah = r->ax >> 8;
    WORD x;
    static WORD dt[2];
    if (r->n != 0x21) {
        if (r->n == 0x2F && r->ax == 0x1600) {  /* Enhanced Mode? nein */
            r->ax &= 0xFF00;
            return;
        }
        glogn("INT ", r->n);
        return;                                 /* unbekannt: nichts   */
    }
    switch (ah) {
    case 0x4C:
    case 0x00:
        task_exit(t, r->ax & 0xFF);
        return;
    case 0x30:
        r->ax = 0x0006;                         /* DOS 6.00             */
        r->bx = 0;
        r->cx = 0;
        return;
    case 0x2A: case 0x2C:
        DosGetDateTime(dt);
        if (ah == 0x2A) {
            r->cx = (dt[0] >> 9) + 1980;
            r->dx = ((dt[0] >> 5) & 15) << 8 | (dt[0] & 31);
            r->ax = r->ax & 0xFF00;
        } else {
            r->cx = (dt[1] >> 11) << 8 | ((dt[1] >> 5) & 63);
            r->dx = ((dt[1] & 31) * 2) << 8;
        }
        return;
    case 0x19:
        r->ax = (r->ax & 0xFF00) | DosQCurDisk();
        return;
    case 0x0E:
        DosSelectDisk(r->dx & 0xFF);
        r->ax = (r->ax & 0xFF00) | 26;
        return;
    case 0x1A:
        wt[t].dta_s = r->ds;
        wt[t].dta_o = r->dx;
        return;
    case 0x2F:
        r->es = wt[t].dta_s;
        r->bx = wt[t].dta_o;
        return;
    case 0x25:
        return;                                 /* Vektoren: verpufft  */
    case 0x35:
        r->es = 0;
        r->bx = 0;
        return;
    case 0x3B:
        fstrncpy(fbuf, MKFP(r->ds, r->dx), sizeof(fbuf));
        x = DosChDir(fbuf);
        if (FAPI_FAILED(x)) err(r, 3); else ok(r);
        return;
    case 0x47: {
        char FAR *d = MKFP(r->ds, r->si);
        x = DosQCurDirD((r->dx & 0xFF) ? (r->dx & 0xFF) - 1 : DosQCurDisk(), fbuf);
        if (FAPI_FAILED(x)) {
            err(r, 15);
            return;
        }
        fstrncpy(d, fbuf[1] == ':' ? fbuf + 3 : fbuf, 64);
        ok(r);
        return;
    }
    case 0x3C: case 0x5B:
        x = f_create(MKFP(r->ds, r->dx));
        if (FAPI_FAILED(x)) err(r, derr(x)); else { r->ax = x; ok(r); }
        return;
    case 0x3D:
        x = f_open(MKFP(r->ds, r->dx), r->ax & 3);
        if (FAPI_FAILED(x)) err(r, derr(x)); else { r->ax = x; ok(r); }
        return;
    case 0x3E:
        DosClose(r->bx);
        ok(r);
        return;
    case 0x3F: case 0x40:
        x = f_rw(ah == 0x3F ? FAPI_READ : FAPI_WRITE, r->bx, MKFP(r->ds, r->dx), r->cx);
        if (x == 0xFFFF) err(r, 5); else { r->ax = x; ok(r); }
        return;
    case 0x41:
        fstrncpy(fbuf, MKFP(r->ds, r->dx), sizeof(fbuf));
        x = DosDelete(fbuf);
        if (FAPI_FAILED(x)) err(r, derr(x)); else ok(r);
        return;
    case 0x42: {
        DWORD p = f_seek(r->bx, MAKELONG(r->dx, r->cx), r->ax & 0xFF);
        if (p == 0xFFFFFFFFUL) err(r, 6);
        else { r->ax = LOWORD(p); r->dx = HIWORD(p); ok(r); }
        return;
    }
    case 0x44:
        if ((r->ax & 0xFF) == 0) {
            x = DosQHandType(r->bx);
            r->dx = (x == HT_FILE) ? 0 : 0x80;
            ok(r);
        } else
            err(r, 1);
        return;
    case 0x4E: case 0x4F:
        find(r, ah == 0x4E);
        return;
    case 0x36: {
        static unsigned long fr;
        if (FAPI_FAILED(DosFreeSpace((r->dx & 0xFF) ? (r->dx & 0xFF) - 1 : DosQCurDisk(), &fr))) {
            r->ax = 0xFFFF;
            return;
        }
        r->ax = 64; r->cx = 512;                /* 32 KB je Cluster     */
        fr >>= 15;
        r->bx = fr > 0xFFFE ? 0xFFFE : (WORD)fr;
        r->dx = 0xFFFE;
        return;
    }
    case 0x59:
        r->ax = 0;
        return;
    default:
        glogn("INT 21h AX=", r->ax);
        err(r, 1);
        return;
    }
}

void krnl_init(void)
{
    fapi_call(FAPI_SEGCTL, SEG_INTHND, SELOF((void FAR *)int_entry),
              OFFOF((void FAR *)int_entry));
}
