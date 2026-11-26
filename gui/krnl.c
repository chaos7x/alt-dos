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

/* Modul zu hInstance: Modulnummer + 1, DS einer Task oder einer DLL
   (LoadLibrary liefert wie Windows das Autodata der DLL) -> 0.., -1 */
int mod_of_inst(WORD hinst)
{
    int t = task_of_inst(hinst), m;
    if (hinst > 0 && hinst <= NMOD)
        return hinst - 1;
    if (t >= 0)
        return wt[t].hmod - 1;
    for (m = 0; m < NMOD; m++)
        if (wm[m].used && wm[m].autodata && wm[m].seg_sel &&
            wm[m].seg[wm[m].autodata - 1].sel == hinst)
            return m;
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
    mm_task_end(t);
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
    if (cur_task < 0) {
        task_run(t);                            /* bis zum ersten Warten */
    } else {
        /* aus einer Task (WinExec): direkt in die neue springen, der
           Aufrufer bleibt in seinem Platz bereit - die Hauptschleife setzt
           ihn fort. task_run hier ueberschriebe mainsave. */
        int me = cur_task;
        cur_task = t;
        tsw((WORD FAR *)&wt[me].ss, wt[t].ss, wt[t].sp);
        cur_task = me;
    }
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
    /* wie Windows: Bloecke auf 4 Byte (Kopf 2 Byte vor den Daten, Daten
       durch 4 teilbar), Griffe verschiebbarer Bloecke haben Bit 1 gesetzt -
       Write benutzt Griffe direkt ohne LocalLock */
    start = (start + 3) & ~3u;
    if (end < start + 22)
        return 0;
    end = start + 6 + ((end - start - 6) & ~3u);
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
    add = h->end < lim - 1 ? (lim + 1 - h->end) & ~3u : 0;
    if (add < 8) {                              /* Segment vergroessern    */
        nsz = (DWORD)lim + 1 + need + 1024;
        if (nsz > 0xFFF0UL)
            nsz = 0xFFF0UL;
        if (nsz <= (DWORD)lim + 1)
            return 0;
        if (FAPI_FAILED(fapi_call(FAPI_SEGCTL, SEG_REALLOC, ds, (WORD)nsz)))
            return 0;
        lim = lsl_(ds);
        add = (lim + 1 - h->end) & ~3u;
        if (lim == 0xFFFF)
            add = (0xFFF0u - h->end) & ~3u;
    }
    if (add < 8)
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
    need = (n + 2 + 3) & ~3u;
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
            if (sz - need >= 8) {
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
    int m = mod_of_inst(hinst);
    if (m >= 0)
        ;
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
    WORD isc, ord;
    int m;
    if (hmod >= 0x100 && hmod < 0x110) {
        ord = SELOF(name) ? exp_byname(hmod - 0x100, name) : OFFOF(name);
        fp = ord ? exp_ord(hmod - 0x100, ord, &isc) : 0;
        if (!fp)
            glogn("GetProcAddress fehlt: Modul/Nummer ", ((hmod - 0x100) << 12) | ord);
        return fp && !isc ? (DWORD)fp : 0;
    }
    m = mod_of_inst(hmod);
    if (m >= 0) {
        ord = SELOF(name) ? w16_ord_byname(m, name) : OFFOF(name);
        fp = ord ? w16_entry(m, ord) : 0;
        if (fp)
            return (DWORD)fp;
    }
    if (SELOF(name))
        glogs("GetProcAddress: ", name);
    return 0;
}

/* FindResource/LoadResource/FreeResource: HRSRC ist ein Platz in einer
   kleinen Tabelle (w16_res_find liefert Modul + Stelle als DWORD) */
#define NRSRC 8
static DWORD rs_tab[NRSRC];
static BYTE  rs_next;
WORD API FindResource(WORD hinst, LPCSTR name, LPCSTR type)
{
    int m = mod_of_inst(hinst);
    DWORD r;
    if (m < 0 && cur_task >= 0)
        m = wt[cur_task].hmod - 1;
    r = w16_res_find(m, type, name);
    if (!r)
        return 0;
    rs_next = (BYTE)((rs_next + 1) % NRSRC);
    rs_tab[rs_next] = r;
    return rs_next + 1;
}
WORD API LoadResource(WORD hinst, WORD hr)
{
    (void)hinst;
    return hr && hr <= NRSRC && rs_tab[hr - 1] ? w16_res_load(0, rs_tab[hr - 1]) : 0;
}
DWORD API SizeofResource(WORD hinst, WORD hr)
{
    (void)hinst;
    return hr && hr <= NRSRC ? w16_res_size(rs_tab[hr - 1]) : 0;
}
BOOL API FreeResource(WORD h) { if (h) seg_del(h); return 0; }
WORD API LocalShrink(WORD seg, WORD n) { (void)seg; return n ? n : 0x1000; }

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

/* verworfene Bloecke wie Windows: GlobalAlloc/GlobalReAlloc mit Groesse 0
   und GMEM_MOVEABLE liefert einen Griff, dessen GlobalLock NULL ist, bis ihn
   GlobalReAlloc wieder fuellt (PROGMAN laedt seine Gruppen so nach) */
#define NDISC 16
static WORD gdisc[NDISC];
static int disc_at(WORD h)
{
    int i;
    for (i = 0; i < NDISC; i++)
        if (gdisc[i] && gdisc[i] == (h | 7))
            return i;
    return -1;
}
static void disc_set(WORD h, int on)
{
    int i = disc_at(h);
    if (!on) {
        if (i >= 0)
            gdisc[i] = 0;
    } else if (i < 0)
        for (i = 0; i < NDISC; i++)
            if (!gdisc[i]) {
                gdisc[i] = h | 7;
                break;
            }
}
WORD API GlobalAlloc(WORD flags, DWORD size)
{
    WORD s;
    if (size > 0x10000UL) {
        glogn("GlobalAlloc ueber 64 KB, KB: ", (WORD)(size >> 10));
        return 0;
    }
    s = seg_new(size ? (WORD)size : 1);          /* Kernel nullt immer */
    if (!s)
        glogn("GlobalAlloc gescheitert: ", (WORD)size);
    else if (!size && (flags & 0x02))           /* GMEM_MOVEABLE         */
        disc_set(s, 1);
    return s;
}
WORD API GlobalReAlloc(WORD h, DWORD size, WORD flags)
{
    if (flags & 0x80)                           /* GMEM_MODIFY: nur Merker */
        return h;
    if (size > 0x10000UL || !h) {
        glogn("GlobalReAlloc gescheitert, KB: ", (WORD)(size >> 10));
        return 0;
    }
    disc_set(h, !size && (flags & 0x02));       /* 0 + MOVEABLE: verwerfen */
    if (!size)
        size = 1;
    if (FAPI_FAILED(fapi_call(FAPI_SEGCTL, SEG_REALLOC, h, (WORD)size))) {
        glogn("GlobalReAlloc gescheitert: ", (WORD)size);
        return 0;
    }
    return h;
}
WORD API GlobalFree(WORD h) { disc_set(h, 0); seg_del(h & ~3u | 7); return 0; }
DWORD API GlobalLock(WORD h) { return h && disc_at(h) < 0 ? MAKELONG(0, h | 7) : 0; }
BOOL API GlobalUnlock(WORD h) { (void)h; return 0; }
DWORD API GlobalSize(WORD h) { return h && disc_at(h) < 0 ? (DWORD)lsl_(h | 7) + 1 : 0; }
DWORD API GlobalHandle(WORD sel) { return MAKELONG(sel, sel); }
WORD API GlobalFlags(WORD h) { return disc_at(h) >= 0 ? 0x4000 : 0; }   /* GMEM_DISCARDED */
DWORD API GlobalCompact(DWORD n) { (void)n; return 0x100000UL; }
WORD API LockSegment(WORD s) { return s; }
void API UnlockSegment(WORD s) { (void)s; }
DWORD API GlobalWire(WORD h) { return GlobalLock(h); }
BOOL API GlobalUnWire(WORD h) { (void)h; return 1; }
WORD API AllocSelector(WORD s) { (void)s; return 0; }
WORD API FreeSelector(WORD s) { (void)s; return 0; }

static WORD la_impl(WORD ds, WORD flags, WORD n)
{
    WORD r = (flags & 0x02) ? lh_hnew(ds, n, flags & LMEM_ZEROINIT)   /* Griff */
                            : lh_alloc(ds, n, flags & LMEM_ZEROINIT);
    if (!r)
        glogn("LocalAlloc gescheitert: ", n);
    return r;
}
/* nur DS holen (get_bp2: vor BP darf der Export nichts sichern) */
WORD API LocalAlloc(WORD flags, WORD n) { return la_impl(get_bp2(), flags, n); }
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
    WORD ds = get_bp2();
    if (flags & LMEM_MODIFY)
        return h;
    return lh_realloc(ds, h, n);
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
/* groesster freie Block (nach Zusammenlegen) samt dem, was die Halde
   bis 64 KB noch wachsen kann - Write prueft damit beim Start */
static WORD lh_largest(WORD ds)
{
    WORD hs = lh_of(ds), p, sz, best = 0, last = 0;
    struct lheap FAR *h;
    if (!hs)
        return 0;
    h = MKFP(ds, hs);
    for (p = h->start; p < h->end; p += sz) {
        sz = *(WORD FAR *)MKFP(ds, p) & ~1u;
        if (!sz)
            break;
        if (*(WORD FAR *)MKFP(ds, p) & 1) {
            last = 0;
            continue;
        }
        last += sz;
        if (last > best)
            best = last;
    }
    if (h->end < 0xFFF0u && 0xFFF0u - h->end + last > best)
        best = 0xFFF0u - h->end + last;
    return best > 2 ? best - 2 : 0;
}
WORD API LocalCompact(WORD n) { WORD ds = get_bp2(); (void)n; return lh_largest(ds); }
/* Griffe je Erweiterung der Grifftabelle: wir legen sie in festen
   Bloecken an (LH_NHE), der Wert wird nur gemerkt */
static WORD lh_delta = 32;
WORD API LocalHandleDelta(WORD d)
{
    if (d)
        lh_delta = d;
    return lh_delta;
}
BOOL API LocalInit(WORD seg, WORD start, WORD end)
{
    if (!seg)
        seg = get_bp2();
    if (!start) {
        /* wie Windows: start 0 = die letzten `end` Bytes des Segments (bei
           einer DLL also direkt hinter den statischen Daten - LibEntry ruft
           LocalInit(DS, 0, Haldengroesse)). Vorher lag die Halde ab 16 und
           ueberschrieb die Daten der DLL (Systemsteuerung, Laender) */
        WORD sz = (WORD)lsl_(seg);              /* Groesse - 1           */
        if (end && end <= sz - 16) {
            start = sz - end + 1;
            end = sz;
        } else {
            start = 16;
            end = sz;
        }
    }
    if (w16_trace) {
        glogn("LocalInit seg ", seg); glogn("  start ", start); glogn("  end ", end);
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


/* ---- Dateien (KERNEL _lopen & Co., auch fuer INT 21h) ----------------
   FAPI_SEEK kennt nur die Stelle vom Anfang: Stelle und Groesse fuehrt
   die GUI je Handle selbst. */
char fbuf[260];                         /* auch cpl.c (Arbeitspuffer)   */
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

/* Eintrag einer Datei (fbuf) ueber DosFindEntry suchen -> fd_e */
static int f_ent(void)
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
            return 1;
    }
}
static DWORD f_size(void) { return f_ent() ? fd_e.size : 0; }

/* Pfad je Handle (fuer 57h: Datum ueber Handle), eigenes Segment:
   je Platz 80 Byte Pfad, Datum, Zeit, Merker "beim Schliessen setzen" */
#define HP_SZ 86
static WORD hp_sel;
#define HP(h) ((BYTE FAR *)MKFP(hp_sel, (h) * HP_SZ))
static void hp_note(WORD h)
{
    if (h >= NFH)
        return;
    if (!hp_sel && (hp_sel = seg_new(NFH * HP_SZ)) == 0)
        return;
    fstrncpy((LPSTR)HP(h), fbuf, 80);
    HP(h)[85] = 0;
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
        hp_note(h);
    }
    return h;
}
static WORD f_create(LPCSTR name)
{
    WORD h;
    fstrncpy(fbuf, name, sizeof(fbuf));
    h = fapi_call(FAPI_CREATE, (WORD)fbuf, 0, 0);
    if (!FAPI_FAILED(h) && h < NFH) {
        fh[h].pos = fh[h].size = 0;
        hp_note(h);
    }
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
/* Schliessen; ein mit 5701h gesetztes Datum wird danach eingetragen
   (beim Schliessen schreibt das Dateisystem sonst die jetzige Zeit) */
static void h_close(WORD h)
{
    DosClose(h);
    if (hp_sel && h < NFH && HP(h)[85]) {
        static struct fapi_finfo fi;
        fstrncpy(fbuf, (LPCSTR)HP(h), 80);
        fi.mask = 2;
        fi.date = *(WORD FAR *)(HP(h) + 81);
        fi.time = *(WORD FAR *)(HP(h) + 83);
        DosSetFInfo(fbuf, &fi);
        HP(h)[85] = 0;
    }
}
WORD API _lclose(WORD h) { h_close(h); return 0; }
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
    else {
        h = f_open(p, st & 3);
        /* wie Windows: Name ohne Pfad auch im Windows- und SYSTEM-
           Verzeichnis, beim Programm und im PATH suchen (MAIN.CPL oeffnet
           die .FOT-Dateien aus [fonts] so) */
        if (FAPI_FAILED(h) && !(st & 0x8000) && path_find(name, p, 128))
            h = f_open(p, st & 3);
    }
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
static WORD last_err;                           /* fuer 59h             */
static BYTE nr_drv;                             /* Laufwerk + 1, das    */
                                                /* "nicht bereit" meldete */
static void err(struct iregs FAR *r, WORD c) { r->fl |= CF; r->ax = c; last_err = c; }
static WORD derr(WORD f)
{
    switch (f) {
    case FAPI_ERR_EXIST: return 80;
    case FAPI_ERR_DENIED: case FAPI_ERR_INUSE: case FAPI_ERR_WP: return 5;
    case FAPI_ERR_FULL: return 39;
    case FAPI_ERR_NOTREADY:
        if (!nr_drv) nr_drv = 0x80;             /* Laufwerk aus fbuf    */
        return 21;
    default: return 2;
    }
}

/* Suchzustand wie DOS im reservierten Teil des DTA: Byte 0 Kennung 'S',
   1 Platz in fs_t (Verzeichnis + Muster), 2/3 laufende Nummer,
   12 Attribute aus 4Eh (4Fh liest CX nicht) */
#define NFS 4
static struct { char dir[80]; char pat[14]; } fs_t[NFS];
static BYTE fs_next;

static void find(struct iregs FAR *r, int first)
{
    int t = cur_task;
    char FAR *dta = MKFP(wt[t].dta_s, wt[t].dta_o);
    char *b, *ls = 0, *dir, *pat;
    WORD x, i, n;
    BYTE sl, at;
    if (first) {
        sl = fs_next++ % NFS;
        dir = fs_t[sl].dir; pat = fs_t[sl].pat;
        fstrncpy(dir, MKFP(r->ds, r->dx), sizeof(fs_t[0].dir));
        for (b = dir; *b; b++)
            if (*b == '\\' || *b == ':')
                ls = b;
        if (ls) {
            for (i = 0; ls[1 + i] && i < 13; i++)
                pat[i] = ls[1 + i];
            pat[i] = 0;
            ls[1] = 0;
            if (*ls == '\\' && ls > dir && ls[-1] != ':')
                *ls = 0;
        } else {
            fstrncpy(pat, dir, sizeof(fs_t[0].pat));
            dir[0] = 0;
        }
        n = 0;
        at = (BYTE)r->cx;
        dta[0] = 'S';
        dta[1] = sl;
        dta[12] = at;
    } else {
        if (dta[0] != 'S' || (BYTE)dta[1] >= NFS) {
            err(r, 18);
            return;
        }
        sl = dta[1];
        dir = fs_t[sl].dir; pat = fs_t[sl].pat;
        n = *(WORD FAR *)(dta + 2);
        at = dta[12];
    }
    for (;;) {
        x = fapi_call(FAPI_FINDENTRY, (WORD)&fd_e, n++, dir[0] ? (WORD)dir : 0);
        if (FAPI_FAILED(x)) {
            *(WORD FAR *)(dta + 2) = n - 1;
            if (x == FAPI_ERR_NOTREADY)
                nr_drv = dir[0] && dir[1] == ':' ? up(dir[0]) - 'A' + 1 : 0x81;
            err(r, x == FAPI_ERR_NOTREADY ? 21 : first ? 2 : 18);
            return;
        }
        if ((fd_e.attr & 0x16) & ~(at & 0x16))
            continue;
        if (wmatch(pat, fd_e.name))
            break;
    }
    *(WORD FAR *)(dta + 2) = n;
    dta[21] = fd_e.attr;
    *(WORD FAR *)(dta + 22) = fd_e.time;
    *(WORD FAR *)(dta + 24) = fd_e.date;
    *(DWORD FAR *)(dta + 26) = fd_e.size;
    fstrncpy(dta + 30, fd_e.name, 13);
    if (w16_trace == 2)
        glogs("  -> ", fd_e.name);
    ok(r);
}

/* Aenderungsmeldungen wie KERNEL.FileCDR: ein Programm (der Datei-
   Manager) meldet eine Rueckruf-Funktion an, die nach jeder Aenderung im
   Dateisystem gerufen wird - Pascal (WORD ax, LPSTR pfad), ax = DOS-
   Funktion wie im AX des Aufrufs (3900h MD, 3A00h RD, 3C00h anlegen,
   4100h loeschen, 4301h Attribute; ermittelt durch Probieren: nur 3900h
   loeste im Datei-Manager die Aktualisierung aus) */
static DWORD cdr_fn;
static WORD cdr_ds;
DWORD API FileCDR(DWORD fn)
{
    DWORD o = cdr_fn;
    if (fn == 0xFFFFFFFFUL)                     /* nur abfragen          */
        return o;
    cdr_fn = fn;
    cdr_ds = wt[cur_task].hinst;
    return o ? o : 1;
}
static void fsc(WORD op, LPCSTR path)
{
    static char p[128];
    static WORD w[3];
    if (!cdr_fn)
        return;
    full_path(path, p, sizeof(p));
    if (w16_trace) glogs("FSC ", p);
    w[0] = op;
    w[1] = SELOF((LPSTR)p);
    w[2] = OFFOF((LPSTR)p);
    call16((void FAR *)cdr_fn, cdr_ds, 3, w);
}

static void dos_int1(void FAR *rv);
static WORD err_mode;                           /* SetErrorMode         */

/* Wie der Windows-Kern mit seinem INT-24h-Handler: meldet ein DOS-Aufruf
   "nicht bereit", fragt ein Systemfenster Wiederholen/Abbrechen (ausser
   bei SetErrorMode(SEM_FAILCRITICALERRORS)); Wiederholen ruft neu */
void dos_int(void FAR *rv)
{
    struct iregs FAR *r = rv;
    static struct iregs sv[NWT];
    static char nrt[40];
    int t = cur_task;
    for (;;) {
        fmemcpy((void FAR *)&sv[t], r, sizeof(sv[0]));
        nr_drv = 0;
        dos_int1(rv);
        if (w16_trace == 2 && r->n == 0x21)     /* Ergebnis: CF -> "E"  */
            glogn((r->fl & 1) ? "  E " : "  = ", r->ax);
        if (!nr_drv || (err_mode & 1) || r->n != 0x21)
            return;
        {
            BYTE d = nr_drv;
            if (d >= 0x80)
                d = (d == 0x80 && fbuf[0] && fbuf[1] == ':') ? up(fbuf[0]) - 'A' + 1
                    : (BYTE)DosQCurDisk() + 1;
            fstrncpy(nrt, "Laufwerk ?: ist nicht bereit.", sizeof(nrt));
            nrt[9] = 'A' + d - 1;
        }
        if (MessageBox(0, nrt, "Fehler", 0x1015) != 4)   /* RETRYCANCEL | */
            return;                             /* ICONHAND | SYSTEMMODAL */
        fmemcpy(r, (void FAR *)&sv[t], sizeof(sv[0]));
    }
}

static void dos_int1(void FAR *rv)
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
        if (r->n == 0x2F && (r->ax == 0x1500 || r->ax == 0x150B)) {
            /* MSCDEX: CD-Laufwerke sind die mit 2048-Byte-Bloecken */
            static struct fapi_fsinfo fi;
            WORD d, n = 0, first = 0;
            for (d = 0; d < 26; d++) {
                if (r->ax == 0x150B && d != r->cx)
                    continue;
                if (FAPI_FAILED(DosQFSInfo(d, &fi)))
                    continue;
                if (fi.bsize == 2048 || !fstricmp(fi.fsname, "ISO9660")) {
                    if (!n++) first = d;
                }
            }
            if (r->ax == 0x1500) {
                r->bx = n;
                r->cx = first;
            } else {
                r->ax = n ? 1 : 0;
                r->bx = 0xADAD;
            }
            return;
        }
        if (r->n == 0x25 || r->n == 0x26) {     /* absolut lesen/schreiben: */
            r->ax = 0x8002;                     /* "nicht bereit" (CF), die */
            r->fl |= 1;                         /* Flags bleiben (gcrt.asm) */
            return;
        }
        glogn("INT ", r->n);
        return;                                 /* unbekannt: nichts   */
    }
    if (w16_trace == 2) {                       /* GUITRACE=2: jeder Aufruf */
        static const BYTE __far pfn[] = { 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x41, 0x43, 0x4E, 0x56, 0x5B, 0x6C, 0 };
        short i;
        glogn("D ", r->ax);
        for (i = 0; pfn[i]; i++)
            if (pfn[i] == ah && r->ds) {
                fstrncpy(fbuf, MKFP(r->ds, ah == 0x6C ? r->si : r->dx), 80);
                glogs("  ", fbuf);
            }
    }
    if (w16_trace)                              /* Aufrufspur: F000h|AH  */
        if (ah != 0x2C) { if (tr_dseg) ((WORD FAR *)MKFP(tr_dseg, 512))[tr_pos & 127] = 0;
          tr_ring[tr_pos++ & 127] = 0xF000u | ah; }
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
            /* AL = Wochentag (0 = Sonntag), Verfahren nach Sakamoto -
               die Systemsteuerung zeigt das lange Datum damit an */
            static const BYTE __far wt_t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
            WORD y = (dt[0] >> 9) + 1980, mo = (dt[0] >> 5) & 15, d = dt[0] & 31;
            r->cx = y;
            r->dx = mo << 8 | d;
            if (mo < 3)
                y--;
            r->ax = (r->ax & 0xFF00) |
                    (y + y / 4 - y / 100 + y / 400 + wt_t[(mo - 1) % 12] + d) % 7;
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
        WORD dr = (r->dx & 0xFF) ? (r->dx & 0xFF) - 1 : DosQCurDisk();
        static struct fapi_fsinfo qi;           /* wie MS-DOS: leeres  */
        if (!FAPI_FAILED(DosQFSInfo(dr, &qi))   /* Laufwerk -> 15h     */
            && (qi.flags & FSI_NOMEDIA)) {      /* (WINFILE prueft so) */
            nr_drv = (BYTE)dr + 1;
            err(r, 21);
            return;
        }
        x = DosQCurDirD(dr, fbuf);
        if (FAPI_FAILED(x)) {
            err(r, 15);
            return;
        }
        fstrncpy(d, fbuf[1] == ':' ? fbuf + 3 : fbuf, 64);
        r->ax = 0x0100;                         /* wie MS-DOS (undokumentiert);
                                                   Write prueft es        */
        ok(r);
        return;
    }
    case 0x3C: case 0x5B:
        x = f_create(MKFP(r->ds, r->dx));
        if (FAPI_FAILED(x)) err(r, derr(x)); else { r->ax = x; ok(r); fsc(0x3C00, fbuf); }
        return;
    case 0x6C: {                                /* erweitert oeffnen/anlegen */
        BYTE ex;
        fstrncpy(fbuf, MKFP(r->ds, r->si), sizeof(fbuf));
        ex = (BYTE)f_ent();
        if (ex && (fd_e.attr & 0x10)) { err(r, 5); return; }
        if (ex) {
            switch (r->dx & 0x0F) {
            case 1: x = f_open(MKFP(r->ds, r->si), r->bx & 3); r->cx = 1; break;
            case 2: x = f_create(MKFP(r->ds, r->si)); r->cx = 3; break;
            default: err(r, 80); return;
            }
        } else {
            if (((r->dx >> 4) & 0x0F) != 1) { err(r, 2); return; }
            x = f_create(MKFP(r->ds, r->si));
            r->cx = 2;
        }
        if (FAPI_FAILED(x)) { err(r, derr(x)); return; }
        r->ax = x;
        ok(r);
        if (r->cx != 1) fsc(0x3C00, fbuf);
        return;
    }
    case 0x3D:
        x = f_open(MKFP(r->ds, r->dx), r->ax & 3);
        if (FAPI_FAILED(x)) err(r, derr(x)); else { r->ax = x; ok(r); }
        return;
    case 0x3E:
        h_close(r->bx);
        ok(r);
        return;
    case 0x39: case 0x3A:                       /* Verzeichnis an/weg    */
        fstrncpy(fbuf, MKFP(r->ds, r->dx), sizeof(fbuf));
        x = ah == 0x39 ? DosMkDir(fbuf) : DosRmDir(fbuf);
        if (FAPI_FAILED(x)) err(r, x == FAPI_ERR_EXIST ? 5 : derr(x));
        else { ok(r); fsc(ah << 8, fbuf); }
        return;
    case 0x56: {                                /* umbenennen/verschieben */
        static char to[128];
        fstrncpy(fbuf, MKFP(r->ds, r->dx), sizeof(fbuf));
        fstrncpy(to, MKFP(r->es, r->di), sizeof(to));
        x = DosMove(fbuf, to);
        if (FAPI_FAILED(x)) err(r, derr(x));
        else { ok(r); fsc(0x4100, fbuf); fsc(0x3C00, to); }
        return;
    }
    case 0x57:                                  /* Datum/Zeit ueber Handle */
        if (!hp_sel || r->bx >= NFH || !HP(r->bx)[0]) { err(r, 6); return; }
        if ((r->ax & 0xFF) == 0) {
            if (HP(r->bx)[85]) {
                r->dx = *(WORD FAR *)(HP(r->bx) + 81);
                r->cx = *(WORD FAR *)(HP(r->bx) + 83);
            } else {
                fstrncpy(fbuf, (LPCSTR)HP(r->bx), 80);
                if (!f_ent()) { err(r, 6); return; }
                r->dx = fd_e.date;
                r->cx = fd_e.time;
            }
        } else {
            *(WORD FAR *)(HP(r->bx) + 81) = r->dx;
            *(WORD FAR *)(HP(r->bx) + 83) = r->cx;
            HP(r->bx)[85] = 1;
        }
        ok(r);
        return;
    case 0x3F: case 0x40:
        x = f_rw(ah == 0x3F ? FAPI_READ : FAPI_WRITE, r->bx, MKFP(r->ds, r->dx), r->cx);
        if (x == 0xFFFF) err(r, 5); else { r->ax = x; ok(r); }
        return;
    case 0x41:
        fstrncpy(fbuf, MKFP(r->ds, r->dx), sizeof(fbuf));
        x = DosDelete(fbuf);
        if (FAPI_FAILED(x)) err(r, derr(x)); else { ok(r); fsc(0x4100, fbuf); }
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
        WORD drv = (r->dx & 0xFF) ? (r->dx & 0xFF) - 1 : DosQCurDisk();
        if (FAPI_FAILED(x = DosFreeSpace(drv, &fr))) {
            r->ax = 0xFFFF;
            last_err = x == FAPI_ERR_NOTREADY ? 21 : 15;
            if (last_err == 21) nr_drv = drv + 1;
            return;
        }
        r->ax = 64; r->cx = 512;                /* 32 KB je Cluster     */
        fr >>= 15;
        r->bx = fr > 0xFFFE ? 0xFFFE : (WORD)fr;
        r->dx = 0xFFFE;
        {                                       /* Gesamtgroesse         */
            static struct fapi_fsinfo fi;
            if (!FAPI_FAILED(DosQFSInfo(drv, &fi))
                && fi.bsize) {
                DWORD t = fi.sectors >> 6;      /* 512-Byte-Sektoren     */
                if (fi.bsize == 2048) {         /* CD: 2-KB-Cluster      */
                    r->ax = 4;
                    t = fi.sectors;
                }
                r->dx = t > 0xFFFE ? 0xFFFE : (WORD)t;
                if (r->bx > r->dx) r->bx = r->dx;
            }
        }
        return;
    }
    case 0x11: case 0x12: {                     /* FCB-Suche: nur die    */
        BYTE FAR *f = MKFP(r->ds, r->dx);       /* Bezeichnung (Attr. 8) */
        BYTE FAR *d = MKFP(wt[t].dta_s, wt[t].dta_o);
        static struct fapi_label lb;
        BYTE dr, i;
        r->ax |= 0xFF;
        if (ah == 0x12 || f[0] != 0xFF || !(f[6] & 8))
            return;
        dr = f[7] ? f[7] - 1 : (BYTE)DosQCurDisk();
        if (FAPI_FAILED(DosLabel(dr, &lb, 0)) || !lb.label[0])
            return;
        fmemset(d, 0, 44);
        d[0] = 0xFF; d[6] = 8; d[7] = dr + 1;
        for (i = 0; i < 11; i++) {
            BYTE c = lb.label[i] && lb.label[i] != '.' ? lb.label[i] : ' ';
            if (!lb.label[i]) { for (; i < 11; i++) d[8 + i] = ' '; break; }
            d[8 + i] = c;
        }
        d[8 + 11] = 8;                          /* Attribut im Eintrag   */
        r->ax &= 0xFF00;
        return;
    }
    case 0x59:                                  /* erweiterter Fehler   */
        r->ax = last_err;
        r->bx = last_err == 21 ? 0x0B01 : 0x0D01; /* Klasse/Aktion        */
        r->cx &= 0x00FF; r->cx |= 0x0200;        /* Ort: Blockgeraet     */
        return;
    case 0x43:                                  /* Attribute            */
        fstrncpy(fbuf, MKFP(r->ds, r->dx), sizeof(fbuf));
        if ((r->ax & 0xFF) == 0) {
            if (!f_ent()) { err(r, 2); return; }
            r->cx = fd_e.attr;
            ok(r);
        } else {
            static struct fapi_finfo fi;
            fi.mask = 1; fi.attr = r->cx;
            x = fapi_call(FAPI_SETFINFO, (WORD)fbuf, (WORD)&fi, 0);
            if (FAPI_FAILED(x)) err(r, derr(x)); else { ok(r); fsc(0x4301, fbuf); }
        }
        return;
    default:
        glogn("INT 21h AX=", r->ax);
        err(r, 1);
        return;
    }
}

WORD q_sel;
void krnl_init(void)
{
    q_sel = seg_new(NWT * NQ * sizeof(MSG));
    fapi_call(FAPI_SEGCTL, SEG_INTHND, SELOF((void FAR *)int_entry),
              OFFOF((void FAR *)int_entry));
}

/* ---- Stufe 1 fuer die osFree-Programme (25.11.2026) ---- */
WORD API SetErrorMode(WORD m) { WORD o = err_mode; err_mode = m; return o; }

LPSTR API lstrcpyn(LPSTR d, LPCSTR s, short n)
{
    LPSTR r = d;
    if (n <= 0)
        return d;
    while (--n > 0 && *s)
        *d++ = *s++;
    *d = 0;
    return r;
}

/* Name ohne Pfad und Endung, gross -> buf */
static void mod_base(LPCSTR p, char *buf, short n)
{
    LPCSTR s = p, e;
    short i = 0;
    for (e = p; *e; e++)
        if (*e == '\\' || *e == '/' || *e == ':')
            s = e + 1;
    while (*s && *s != '.' && i < n - 1)
        buf[i++] = up(*s++);
    buf[i] = 0;
}

/* hInstance eines geladenen Moduls: Autodata, sonst die Segmenttabelle
   (wie Windows ein Selektor > 32 - LoadLibrary < 32 hiesse Fehler) */
static WORD mod_inst(int m)
{
    return wm[m].autodata ? wm[m].seg[wm[m].autodata - 1].sel : wm[m].seg_sel;
}

/* eingebaute Module (KERNEL, USER, ...) liefern ihre Kennung; DLLs wie
   die Systemsteuerungsmodule (.CPL) laedt der Lader, ruft LibEntry (DI =
   hInstance, CX = Halde, DS = Autodata, ES:SI = 0) und gibt hInstance
   zurueck - wie Windows: 2 = nicht gefunden, 11 = kein Win16-Modul */
static char we_path[FAPI_PATHLEN];
#define ll_path we_path                  /* nie gleichzeitig in Gebrauch  */
WORD API LoadLibrary(LPCSTR name)
{
    static char b[16];
    int m;
    mod_base(name, b, sizeof(b));
    m = exp_builtin(b);
    if (m)
        return 0x100 + m - 1;
    m = w16_byname(b);
    if (m < 0 && path_find(name, ll_path, sizeof(ll_path))) {
        full_path(ll_path, fbuf, 128);
        for (m = NMOD - 1; m >= 0; m--)         /* schon geladen (Pfad)? */
            if (wm[m].used && !fstricmp(wm[m].path, fbuf))
                break;
    }
    if (m >= 0) {
        wm[m].refs++;
        return mod_inst(m);
    }
    if (!path_find(name, ll_path, sizeof(ll_path))) {
        glogs("LoadLibrary: nicht vorhanden: ", name);
        return 2;
    }
    m = w16_load(ll_path);
    if (m < 0)
        return 11;
    if (!(wm[m].flags & 0x8000u)) {             /* keine DLL            */
        w16_free(m);
        return 11;
    }
    if (wm[m].entcs && wm[m].entcs <= wm[m].nseg) {
        WORD ds = wm[m].autodata ? wm[m].seg[wm[m].autodata - 1].sel : 0;
        if (!call_lib(MKFP(wm[m].seg[wm[m].entcs - 1].sel, wm[m].entip), ds,
                      wm[m].heap)) {
            glogs("LibEntry gescheitert: ", name);
            w16_free(m);
            return 14;
        }
    }
    return mod_inst(m);
}
WORD w16_lib_load(LPCSTR name) { return LoadLibrary(name); }

/* Freigeben: WEP rufen und das Modul loswerden, wenn keiner es mehr hat */
void API FreeLibrary(WORD h)
{
    int m = mod_of_inst(h);
    WORD o, ds;
    void FAR *wep;
    if (h >= 0x100 && h < 0x110)
        return;
    if (m < 0 || !(wm[m].flags & 0x8000u))
        return;
    if (wm[m].refs > 1) {
        wm[m].refs--;
        return;
    }
    o = w16_ord_byname(m, "WEP");
    wep = o ? w16_entry(m, o) : 0;
    if (wep) {
        static WORD wa[1];
        ds = wm[m].autodata ? wm[m].seg[wm[m].autodata - 1].sel : 0;
        wa[0] = 0;                              /* WEP_FREE_DLL         */
        call16(wep, ds, 1, wa);
    }
    w16_free(m);
}

/* WinExec: Win16-Programm als neue Task, sonst ueber das FAPI (DOS-
   oder FX-Programm) in eigener Sitzung. > 31 = gestartet */
static int fstrchr_s(LPCSTR s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return 1;
    return 0;
}
WORD API WinExec(LPCSTR cmd, WORD show)
{
    LPCSTR a;
    short i = 0, dot = 0;
    while (*cmd == ' ')
        cmd++;
    for (a = cmd; *a && *a != ' ' && i < FAPI_PATHLEN - 5; a++) {
        if (*a == '.') dot = 1;
        if (*a == '\\') dot = 0;
        we_path[i++] = *a;
    }
    if (!dot) {
        we_path[i++] = '.'; we_path[i++] = 'E'; we_path[i++] = 'X'; we_path[i++] = 'E';
    }
    we_path[i] = 0;
    while (*a == ' ')
        a++;
    if (!task_start(we_path, a, show))
        return 33;
    if (!fstrchr_s(we_path, '\\') && !fstrchr_s(we_path, ':') &&
        !start_search(we_path, a, show))
        return 33;
    glogs("WinExec gescheitert: ", we_path);
    return 2;
}

/* ---- Atome: eine Tabelle fuer lokale und globale (wie USER.EXE es fuer
   globale tut); Eintrag [Zaehler][Laenge][Name], Atom = C000h + Platz.
   Zahlen (MAKEINTATOM, "#123") sind ihr eigenes Atom. ---- */
#define NATOM   128
#define ATOMLEN 40
static WORD atom_sel;
#define ATOMP(i) ((BYTE FAR *)MKFP(atom_sel, (i) * (ATOMLEN + 2)))
static int atom_num(LPCSTR s, WORD FAR *v)
{
    WORD i, n = 0;
    if (SELOF(s) == 0) {
        *v = OFFOF(s);
        return 1;
    }
    if (s[0] != '#')
        return 0;
    for (i = 1; s[i] >= '0' && s[i] <= '9'; i++)
        n = n * 10 + (s[i] - '0');
    *v = n;
    return 1;
}
static short atom_look(LPCSTR s)
{
    WORD i, k, l = fstrlen(s);
    if (!atom_sel || l > ATOMLEN)
        return -1;
    for (i = 0; i < NATOM; i++) {
        BYTE FAR *p = ATOMP(i);
        if (!p[0] || p[1] != l)
            continue;
        for (k = 0; k < l; k++) {
            BYTE a = p[2 + k], b = s[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b)
                break;
        }
        if (k == l)
            return (short)i;
    }
    return -1;
}
WORD atom_add(LPCSTR s)
{
    WORD v, i, l;
    short f;
    if (atom_num(s, &v))
        return v;
    if (!atom_sel) {
        atom_sel = seg_new(NATOM * (ATOMLEN + 2));
        if (!atom_sel)
            return 0;
        fmemset(MKFP(atom_sel, 0), 0, NATOM * (ATOMLEN + 2));
    }
    f = atom_look(s);
    if (f >= 0) {
        if (ATOMP(f)[0] < 255)
            ATOMP(f)[0]++;
        return 0xC000u + f;
    }
    l = fstrlen(s);
    if (l > ATOMLEN)
        l = ATOMLEN;
    for (i = 0; i < NATOM; i++)
        if (!ATOMP(i)[0]) {
            BYTE FAR *p = ATOMP(i);
            p[0] = 1;
            p[1] = (BYTE)l;
            fmemcpy(p + 2, s, l);
            return 0xC000u + i;
        }
    glog("Atomtabelle voll");
    return 0;
}
static WORD atom_find(LPCSTR s)
{
    WORD v;
    short f;
    if (atom_num(s, &v))
        return v;
    f = atom_look(s);
    return f < 0 ? 0 : 0xC000u + f;
}
static WORD atom_del(WORD a)
{
    if (a >= 0xC000u && a - 0xC000u < NATOM && atom_sel) {
        BYTE FAR *p = ATOMP(a - 0xC000u);
        if (p[0] && p[0] < 255)
            p[0]--;
    }
    return 0;
}
static WORD atom_name(WORD a, LPSTR buf, short n)
{
    WORD l;
    if (!buf || n <= 0)
        return 0;
    if (a < 0xC000u) {                          /* Zahl -> "#123"        */
        static char t[8];
        WORD i = sizeof(t) - 1;
        t[i] = 0;
        do t[--i] = (char)('0' + a % 10); while ((a /= 10) && i > 1);
        t[--i] = '#';
        fstrncpy(buf, t + i, n);
        return fstrlen(buf);
    }
    if (!atom_sel || a - 0xC000u >= NATOM || !ATOMP(a - 0xC000u)[0]) {
        buf[0] = 0;
        return 0;
    }
    l = ATOMP(a - 0xC000u)[1];
    if (l >= (WORD)n)
        l = n - 1;
    fmemcpy(buf, ATOMP(a - 0xC000u) + 2, l);
    buf[l] = 0;
    return l;
}
BOOL API InitAtomTable(short n) { (void)n; return 1; }
WORD API AddAtom(LPCSTR s) { return atom_add(s); }
WORD API FindAtom(LPCSTR s) { return atom_find(s); }
WORD API DeleteAtom(WORD a) { return atom_del(a); }
WORD API GetAtomName(WORD a, LPSTR buf, short n) { return atom_name(a, buf, n); }
WORD API GlobalAddAtom(LPCSTR s) { return atom_add(s); }
WORD API GlobalFindAtom(LPCSTR s) { return atom_find(s); }
WORD API GlobalDeleteAtom(WORD a) { return atom_del(a); }
WORD API GlobalGetAtomName(WORD a, LPSTR buf, short n) { return atom_name(a, buf, n); }

/* ---- temporaere Dateien: %TEMP% (sonst Stamm des Startlaufwerks) ---- */
DWORD API GetTickCount(void);
static char tmpdir[FAPI_PATHLEN];
static void tmp_dir(void)
{
    WORD l;
    if (FAPI_FAILED(DosGetEnv("TEMP", tmpdir, sizeof(tmpdir))) || !tmpdir[0])
        if (FAPI_FAILED(DosGetEnv("TMP", tmpdir, sizeof(tmpdir))) || !tmpdir[0])
            fstrncpy(tmpdir, "C:\\", sizeof(tmpdir));
    l = fstrlen(tmpdir);
    if (l && tmpdir[l - 1] != '\\' && l + 1 < sizeof(tmpdir)) {
        tmpdir[l] = '\\';
        tmpdir[l + 1] = 0;
    }
}
BYTE API GetTempDrive(BYTE d)
{
    (void)d;
    tmp_dir();
    return (BYTE)(tmpdir[1] == ':' ? (tmpdir[0] & ~0x20) : 'C');
}
WORD API GetTempFileName(BYTE drv, LPCSTR pre, WORD uniq, LPSTR buf)
{
    WORD u = uniq, k, l, tries;
    static const char hx[] = "0123456789ABCDEF";
    tmp_dir();
    if (drv & 0x80) {                           /* TF_FORCEDRIVE        */
        buf[0] = (char)(drv & 0x7F);
        buf[1] = ':';
        buf[2] = '\\';
        buf[3] = 0;
    } else
        fstrncpy(buf, tmpdir, 128);
    l = fstrlen(buf);
    if (!u)
        u = (WORD)GetTickCount();
    for (tries = 0; tries < 100; tries++, u++) {
        WORD i = l, h;
        if (!u)
            u = 1;
        buf[i++] = '~';
        for (k = 0; k < 3 && pre && pre[k]; k++)
            buf[i++] = pre[k];
        for (k = 0; k < 4; k++)
            buf[i++] = hx[(u >> (12 - 4 * k)) & 15];
        fstrncpy(buf + i, ".TMP", 5);
        if (uniq)
            return u;                           /* nicht anlegen         */
        h = _lopen(buf, 0);
        if (h != 0xFFFF) {
            _lclose(h);
            continue;                           /* gibt es schon         */
        }
        h = _lcreat(buf, 0);
        if (h != 0xFFFF)
            _lclose(h);
        return u;
    }
    return 0;
}

/* Daten aus der Instanz hinst an dieselbe Stelle im eigenen DS kopieren
   (eine zweite Instanz holt so Einstellungen der ersten) */
static short gid_impl(WORD ds, WORD hinst, WORD off, short n)
{
    if (!hinst || n <= 0)
        return 0;
    fmemcpy(MKFP(ds, off), MKFP(hinst, off), n);
    return n;
}
short API GetInstanceData(WORD hinst, WORD off, short n)
{
    return gid_impl(get_bp2(), hinst, off, n);  /* nur DS holen (s. LocalAlloc) */
}

/* ---- fuer den Datei-Manager ---- */
WORD API SetHandleCount(WORD n) { return n < 255 ? n : 255; }
/* DOS-Speicher: es gibt keinen Real Mode - ein Segment, HIWORD gilt nur
   fuer INT 21h, das ohnehin uebersetzt wird */
DWORD API GlobalDOSAlloc(DWORD n)
{
    WORD s = seg_new(n > 0xFFF0UL ? 0xFFF0U : (WORD)n);
    if (s)
        fmemset(MKFP(s, 0), 0, n > 0xFFF0UL ? 0xFFF0U : (WORD)n);
    return MAKELONG(s, s);
}
WORD API GlobalDOSFree(WORD s) { seg_del(s); return 0; }
/* grosse Zeiger: Selektoren im Abstand __AHINCR (8) */
DWORD API LongPtrAdd(DWORD p, DWORD n)
{
    DWORD o = (DWORD)LOWORD(p) + n;
    return MAKELONG(LOWORD(o), HIWORD(p) + HIWORD(o) * 8);
}
