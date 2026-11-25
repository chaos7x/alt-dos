/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - NE-Lader (New Executable) mit DLLs (KTEXT4).
 *
 * Grundlage der GUI (docs/GUI-WIN16.md, Stufe 1): Programme und DLLs im
 * NE-Format (Windows 3.x, OS/2 1.x; Open Watcom `format os2`/`windows`).
 *
 * Aufteilung wie OS/2 1.x (Entwurfsfrage 1, Vorschlag A): jeder Task
 * behaelt seine LDT. Segment k einer EXE liegt im Eintrag k + 1 (0 und 1
 * bleiben frei - dort haengen bei FX-Programmen Code-Verwerfen und Daten-
 * segment, codeswap.c). Eine DLL wird einmal geladen; ihre Segmente
 * bekommen Eintraege von oben aus der LDT, in allen Tasks dieselben
 * (`ne_sh`, seg_alloc und VioGetBuf nehmen sie nie). Jeder Task, der die
 * DLL (auch mittelbar) benutzt, bekommt die Deskriptoren als Alias
 * (D_ALIAS: task_end gibt nur den Eintrag frei); die Seiten gibt erst
 * der letzte Benutzer frei (`refs`, Bitmaske `task.nemods`).
 *
 * Relokationen: intern (fest und ueber die Eintragstabelle), Import ueber
 * Ordinalzahl und Namen (residente, dann nicht residente Namen), additiv
 * und verkettet; LOBYTE, SEGMENT, FAR_ADDR, OFFSET. OSFIXUP (287-Ersatz)
 * wird uebergangen.
 *
 * Stufe 2: Start-Routinen der DLLs laufen im Ring 3 vor dem Programm
 * (Stummel `ne_stubc`, Liste auf dem Stapel der EXE, Abhaengigkeiten
 * zuerst): einmal beim ersten Laden, mit INITINSTANCE (Kopf-Bit 4) oder
 * Instanzdaten je Task. Register wie OS/2 1.x und Windows: AX = Modul-
 * kennung, CX = Heap, DS = ES = DI = Autodata; AX = 0 zurueck beendet das
 * Programm (255). MULTIPLEDATA-DLLs: Datensegmente (Autodata und nicht
 * geteilte) bekommt jeder Task als eigene Kopie unter demselben Selektor
 * (task_end gibt sie wie DosAllocSeg frei). Ressourcen: FAPI_GETRES/
 * FAPI_GETMOD wie DosGetResource/DosGetModHandle von OS/2 1.x (Typ und
 * Kennung als Zahl). Noch nicht: Namen als Ressourcenkennung,
 * DosLoadModule, Win16-Besonderheiten (Selektor-Kacheln, __AHINCR).
 *
 * DLLs werden gesucht: Verzeichnis des ladenden Moduls, dann \DOS\SYSTEM
 * und \DOS des Startlaufwerks, Name = Modulname + ".DLL".
 *
 * Waehrend des Ladens haelt der Lader die Kernelsperre (bkl_hold): die
 * Tabellen hier sind global, ein zweiter Task darf nicht dazwischen.
 */
#include "kern.h"
#include "mem.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */

pg_t __far kf_mem_get(unsigned n);
unsigned __far kf_seg_alloc(int t, unsigned size);
unsigned __far kf_seg_free(int t, unsigned sel);
extern unsigned alloc_units;
extern unsigned char k_alias;
extern unsigned char bkl_hold;
extern int sys_drive;

#define NEMOD   15                      /* geladene DLLs                  */
#define NEMSZ   4096U                   /* Platz je DLL im Kernelsegment  */
#define NESEG   32                      /* Segmente je DLL                */
#define NEDEP   8                       /* importierte Module je DLL      */
#define NEDEPTH 3                       /* DLL laedt DLL laedt DLL        */
#define NEMODS  16                      /* Modulverweise je Modul         */
#define NE_EXE  0x8000u                 /* task.nemods: NE-Programm       */

struct neseg {
    unsigned short idx;                 /* LDT-Eintrag (DLL: gemeinsam)   */
    unsigned short pages;
    pg_t           pg;                  /* erste Seite                    */
    unsigned short size;                /* Bytes - 1 (Grenze)            */
    unsigned char  acc;
    unsigned char  inst;                /* Instanzdaten: Kopie je Task    */
};
struct nemod {
    char           name[9];
    unsigned char  used, drive, nseg, ndep;
    unsigned       refs;
    unsigned long  nrpos;               /* nicht residente Namen (Datei)  */
    unsigned       nrlen;
    char           path[96];
    unsigned char  dep[NEDEP];
    struct neseg   seg[NESEG];
    unsigned       flags;               /* Kopf: NEF_*                    */
    unsigned       ics, iip, heap, ads; /* Start-Routine, Heap, Autodata  */
    unsigned char  inited, ninst;       /* Start lief, Instanzsegmente    */
    unsigned long  rpos;                /* Ressourcentabelle (Datei)      */
    unsigned       rlen;
    unsigned       entlen, reslen;      /* Eintragstabelle, residente Namen */
    unsigned char  tab[1];              /* ... bis NEMSZ                  */
};
#define NE_TABOFF ((unsigned)((char *)((struct nemod *)0)->tab - (char *)0))
#define NE_TABMAX (NEMSZ - NE_TABOFF)

unsigned char ne_sh[64];                /* LDT-Eintraege der DLLs (512)   */
static unsigned ne_sel;                 /* Kernelsegment der Modultabelle */
#define M(m) ((struct nemod __far *)MK_FP(ne_sel, (m) * NEMSZ))

/* Datei und Ressourcen der EXE je Task (hinter den Modulen) */
struct neexe {
    unsigned char  drive;
    char           path[96];
    unsigned long  rpos;
    unsigned       rlen;
};
#define NEX(t) ((struct neexe __far *)MK_FP(ne_sel, NEMOD * NEMSZ + \
                                            (t) * sizeof(struct neexe)))

/* Ring-3-Stummel fuer die Start-Routinen der DLLs (gemeinsamer Eintrag).
   Stapel: je DLL Kennung, DS, Heap, IP, CS; dann 0, DS, IP, CS der EXE.
     next: pop ax / test ax,ax / jz done / pop ds / pop cx / mov di,ds
           push ds / pop es / mov bp,sp / call far [bp] / add sp,4
           test ax,ax / jnz next / mov ax,FAPI_EXIT / mov bx,255
           call 0023:0000 / jmp $
     done: pop ds / push ds / pop es / retf                              */
static const unsigned char ne_stubc[] = {
    0x58, 0x85, 0xC0, 0x74, 0x1F, 0x1F, 0x59, 0x8C, 0xDF, 0x1E, 0x07, 0x89,
    0xE5, 0xFF, 0x5E, 0x00, 0x83, 0xC4, 0x04, 0x85, 0xC0, 0x75, 0xE9, 0xB8,
    FAPI_EXIT, 0x00, 0xBB, 0xFF, 0x00, 0x9A, 0x00, 0x00, 0x23, 0x00, 0xEB, 0xFE,
    0x1F, 0x1E, 0x07, 0xCB
};
static unsigned ne_stub;                /* LDT-Eintrag des Stummels       */
static pg_t ne_stubpg;

/* Zustand je Ladestufe (EXE -> DLL -> DLL), nicht auf dem Kernelstapel */
struct nectx {
    struct ifs_file f;
    unsigned char   h[64];              /* NE-Kopf                        */
    unsigned long   neoff;
    pg_t            tu;                 /* Tabellen ab dem NE-Kopf        */
    unsigned        tn, tlen;
    unsigned char   mod[NEMODS];        /* Modulverweis -> Platz          */
    char            path[128];          /* Pfad dieser Stufe              */
};
static struct nectx nc[NEDEPTH];
static struct neseg xseg[NESEG];        /* Segmente der EXE (Stufe 0)     */
static unsigned ne_floor;               /* gemeinsame Eintraege erst ab hier */
static int nd = -1;                     /* aktuelle Stufe                 */
static unsigned char rb[256];           /* Relokationen (32 je Stueck)    */
static char np[IFS_PATHLEN];

#define H16(c, o) ((c)->h[o] | ((unsigned)(c)->h[(o) + 1] << 8))
#define NE_ENTTAB  0x04
#define NE_CBENT   0x06
#define NE_FLAGS   0x0C
#define NE_AUTO    0x0E
#define NE_HEAP    0x10
#define NE_STACK   0x12
#define NE_IP      0x14
#define NE_CS      0x16
#define NE_SP      0x18
#define NE_SS      0x1A
#define NE_CSEG    0x1C
#define NE_CMOD    0x1E
#define NE_CBNRES  0x20
#define NE_SEGTAB  0x22
#define NE_RESTAB  0x26
#define NE_MODTAB  0x28
#define NE_IMPTAB  0x2A
#define NE_NRESTAB 0x2C
#define NE_ALIGN   0x32
#define NE_OTHER   0x37
#define NE_RSRCTAB 0x24
#define NEF_LIB    0x8000
#define NEF_MULTI  0x0002               /* Daten je Instanz               */
#define NEF_INITI  0x0004               /* Start-Routine je Task          */
#define NESF_SHARE 0x0020               /* Segment: geteilt (Daten)       */

#define ACC_UCODE  0xFA
#define ACC_UDATA  0xF2
#define USEL(i)    (((i) << 3) | 4 | 3)
#define SH(i)      (ne_sh[(i) >> 3] & (1 << ((i) & 7)))

static void ne_log(const char *a, const char __far *b)
{
    static char m[64];
    unsigned n = 0;
    while (*a && n < 40)
        m[n++] = *a++;
    while (b && *b && n < 62)
        m[n++] = *b++;
    m[n++] = '\n';
    klog(KDATA, (unsigned)m, n);
}

static void desc_put(int t, unsigned idx, unsigned long base, unsigned lim,
                     unsigned char acc, int alias)
{
    struct desc __far *d = LDT(t, idx);
    if (k_alias)
        base += KALIAS;
    d->limit   = lim;
    d->base_lo = (unsigned short)base;
    d->base_hi = (unsigned char)(base >> 16);
    d->access  = acc;
    d->rsv     = (unsigned short)(((base >> 16) & 0xFF00) | (alias ? D_ALIAS : 0));
}

static unsigned long dbase(const struct desc __far *d)
{
    unsigned long b = d->base_lo | ((unsigned long)d->base_hi << 16) |
                      ((unsigned long)(d->rsv & 0xFF00) << 16);
    return k_alias ? b - KALIAS : b;
}

/* Tabellen der aktuellen Stufe (SCRATCH, nach jeder DLL neu legen) */
static void tmap(struct nectx *c)
{
    scratch_map(PG_LIN(c->tu), c->tn * MEM_PAGE - 1);
}
static unsigned t8(unsigned o)
{
    return *(unsigned char __far *)MK_FP(SCRATCH, o);
}
static unsigned t16(unsigned o)
{
    return t8(o) | (t8(o + 1) << 8);
}

static int nread(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                 unsigned off, unsigned len)
{
    return ifs_read(f, pos, sel, off, len) == (int)len ? 0 : -1;
}

/* Ordinalzahl -> Segment (1..) und Offset aus einer Eintragstabelle
   (Selektor/Offset). 0 = nicht gefunden */
static unsigned ent_find(unsigned sel, unsigned off, unsigned len, unsigned ord,
                         unsigned *eoff)
{
    unsigned char __far *p = MK_FP(sel, off);
    unsigned i = 0, n, type, o = 1, sz;
    while (i < len) {
        n = p[i];
        if (!n)
            return 0;
        type = p[i + 1];
        i += 2;
        if (type == 0) {                        /* leere Ordinalzahlen   */
            o += n;
            continue;
        }
        sz = type == 0xFF ? 6 : 3;
        if (ord >= o && ord < o + n) {
            i += (ord - o) * sz;
            if (type == 0xFF) {                 /* beweglich: Flags, INT 3Fh, Seg, Ofs */
                *eoff = p[i + 4] | (p[i + 5] << 8);
                return p[i + 3];
            }
            *eoff = p[i + 1] | (p[i + 2] << 8);
            return type == 0xFE ? 0 : type;    /* FEh: Konstante - nicht */
        }
        o += n;
        i += n * sz;
    }
    return 0;
}

/* Name (Pascal-Zeichenkette, SCRATCH) in einer Namenstabelle (sel:off,
   len) -> Ordinalzahl, 0 = nicht da */
static unsigned name_find(unsigned sel, unsigned off, unsigned len, unsigned nm)
{
    unsigned char __far *p = MK_FP(sel, off);
    unsigned i = 0, k, n, ln = t8(nm);
    while (i < len && (n = p[i]) != 0) {
        if (n == ln) {
            for (k = 0; k < n; k++) {
                unsigned a = p[i + 1 + k], b = t8(nm + 1 + k);
                if (a >= 'a' && a <= 'z') a -= 32;
                if (b >= 'a' && b <= 'z') b -= 32;
                if (a != b)
                    break;
            }
            if (k == n)
                return p[i + 1 + n] | (p[i + 2 + n] << 8);
        }
        i += n + 3;
    }
    return 0;
}

/* gemeinsamen LDT-Eintrag holen: in keiner LDT belegt, von oben */
static unsigned sh_alloc(void)
{
    unsigned i;
    int t;
    for (i = nldt - 1; i >= ne_floor && i >= 2; i--) {
        if (SH(i))
            continue;
        for (t = 0; t < NTASK; t++)
            if (task[t].state != T_FREE && !task[t].v86 && LDT(t, i)->access)
                break;
        if (t == NTASK) {
            ne_sh[i >> 3] |= (unsigned char)(1 << (i & 7));
            return i;
        }
    }
    return 0;
}

static void mod_free(int m)
{
    struct nemod __far *p = M(m);
    unsigned k;
    for (k = 0; k < p->nseg; k++) {
        if (p->seg[k].pg) {
            mem_free(p->seg[k].pg, p->seg[k].pages);
            image_units -= p->seg[k].pages;
        }
        if (p->seg[k].idx)
            ne_sh[p->seg[k].idx >> 3] &= (unsigned char)~(1 << (p->seg[k].idx & 7));
    }
    p->used = 0;
}

/* Instanzkopie eines Segments fuer Task t (Vorlage = Segment der DLL) */
static int inst_copy(int t, struct neseg __far *g)
{
    pg_t u = kf_mem_get(g->pages);
    unsigned long n = (unsigned long)g->size + 1;
    if (!u)
        return -1;
    scratch_map(PG_LIN(g->pg), g->pages * MEM_PAGE - 1);
    scratch2_map(PG_LIN(u), g->pages * MEM_PAGE - 1);
    if (n > 0x8000UL) {
        far_copy(SCRATCH2, 0, SCRATCH, 0, 0x8000U);
        far_copy(SCRATCH2, 0x8000U, SCRATCH, 0x8000U, (unsigned)(n - 0x8000UL));
    } else
        far_copy(SCRATCH2, 0, SCRATCH, 0, (unsigned)n);
    desc_put(t, g->idx, PG_LIN(u), g->size, g->acc, 0);
    alloc_units += g->pages;
    return 0;
}

/* Task t bekommt DLL m (und was sie importiert); -1 = kein Speicher */
static int attach(int t, int m)
{
    struct nemod __far *p = M(m);
    unsigned k;
    if (task[t].nemods & (1u << m))
        return 0;
    task[t].nemods |= 1u << m;
    p->refs++;
    for (k = 0; k < p->nseg; k++)
        if (p->seg[k].inst) {
            if (inst_copy(t, &p->seg[k]))
                return -1;
        } else
            desc_put(t, p->seg[k].idx, PG_LIN(p->seg[k].pg), p->seg[k].size,
                     p->seg[k].acc, 1);
    for (k = 0; k < p->ndep; k++)
        if (attach(t, p->dep[k]))
            return -1;
    return 0;
}

/* Start-Routinen fuer Task t sammeln (Abhaengigkeiten zuerst) */
static unsigned char ilist[NEMOD];
static unsigned nil, iseen;
static void init_collect(int m)
{
    struct nemod __far *p = M(m);
    unsigned k;
    if (iseen & (1u << m))
        return;
    iseen |= 1u << m;
    for (k = 0; k < p->ndep; k++)
        init_collect(p->dep[k]);
    if (p->ics && (!p->inited || (p->flags & NEF_INITI) || p->ninst)) {
        p->inited = 1;
        ilist[nil++] = (unsigned char)m;
    }
}

/* Task t gibt seine DLLs ab (task_end; Eintraege sind schon leer) */
void __far ne_task_end(int t)
{
    int m;
    if (!ne_sel)
        return;
    NEX(t)->drive = 0xFF;
    for (m = 0; m < NEMOD; m++)
        if (task[t].nemods & (1u << m)) {
            if (--M(m)->refs == 0)
                mod_free(m);
        }
    task[t].nemods = 0;
}

static int ne_do(int t, int drive, const char *path, int m, struct ne_start *st);

/* Modulname aus der Tabelle der Stufe c (Pascal bei imptab + o) -> Platz
   der geladenen DLL; laedt sie nach */
static int mod_get(struct nectx *c, int t, int drive, const char *path, unsigned o)
{
    char nm[9];
    unsigned n = t8(o), i, k;
    int m, d, free_m = -1;
    if (n == 0 || n > 8)
        return -1;
    for (i = 0; i < n; i++) {
        nm[i] = (char)t8(o + 1 + i);
        if (nm[i] >= 'a' && nm[i] <= 'z')
            nm[i] -= 32;
    }
    nm[n] = 0;
    for (m = 0; m < NEMOD; m++) {
        if (!M(m)->used) {
            if (free_m < 0)
                free_m = m;
            continue;
        }
        for (k = 0; k <= n && M(m)->name[k] == nm[k]; k++)
            ;
        if (k > n)
            return m;
    }
    if (free_m < 0 || nd + 1 >= NEDEPTH) {
        ne_log("NE: kein Platz fuer DLL ", (char __far *)nm);
        return -1;
    }
    /* suchen: Verzeichnis des Ladenden, \DOS\SYSTEM, \DOS */
    for (k = 0; k < 3; k++) {
        const char *pre = k == 1 ? "\\DOS\\SYSTEM\\" : "\\DOS\\";
        unsigned j = 0;
        d = drive;
        if (k == 0) {
            for (i = 0; path[i]; i++)
                ;
            while (i && path[i - 1] != '\\')
                i--;
            for (j = 0; j < i && j < IFS_PATHLEN - 14; j++)
                np[j] = path[j];
        } else {
            d = sys_drive;
            while (*pre)
                np[j++] = *pre++;
        }
        for (i = 0; i < n; i++)
            np[j++] = nm[i];
        np[j++] = '.'; np[j++] = 'D'; np[j++] = 'L'; np[j++] = 'L';
        np[j] = 0;
        M(free_m)->used = 1;
        for (i = 0; i <= n; i++)
            M(free_m)->name[i] = nm[i];
        if (ne_do(t, d, np, free_m, 0) == 0) {
            tmap(c);
            return free_m;
        }
        M(free_m)->used = 0;
        tmap(c);
    }
    ne_log("NE: DLL nicht geladen: ", (char __far *)nm);
    return -1;
}

/* Wert eines Ziels: Selektor:Offset (Segment 1.. des Moduls) */
static unsigned seg_sel(struct nectx *c, int m, unsigned s)
{
    if (s == 0 || s > H16(c, NE_CSEG))
        return 0;
    return USEL(m < 0 ? xseg[s - 1].idx : M(m)->seg[s - 1].idx);
}

/* Relokationen eines Segments anwenden */
static int reloc(struct nectx *c, int m, unsigned k, unsigned long rpos)
{
    unsigned n, i, j, cnt, src, fl, off, a, b, sel, val, nxt;
    struct neseg *sg = m < 0 ? &xseg[k] : 0;
    pg_t pg = m < 0 ? sg->pg : M(m)->seg[k].pg;
    unsigned pages = m < 0 ? sg->pages : M(m)->seg[k].pages;
    unsigned char __far *s;
    if (nread(&c->f, rpos, KDATA, (unsigned)rb, 2))
        return -1;
    cnt = rb[0] | (rb[1] << 8);
    rpos += 2;
    while (cnt) {
        n = cnt > 32 ? 32 : cnt;
        if (nread(&c->f, rpos, KDATA, (unsigned)rb, n * 8))
            return -1;
        rpos += n * 8;
        cnt -= n;
        for (i = 0; i < n; i++) {
            unsigned char *r = rb + i * 8;
            src = r[0];
            fl = r[1];
            off = r[2] | (r[3] << 8);
            a = r[4] | (r[5] << 8);
            b = r[6] | (r[7] << 8);
            tmap(c);
            switch (fl & 3) {
            case 0:                             /* intern                */
                if ((a & 0xFF) == 0xFF) {       /* beweglich: Ordinalzahl */
                    unsigned eo, es = ent_find(SCRATCH, H16(c, NE_ENTTAB),
                                               H16(c, NE_CBENT), b, &eo);
                    sel = seg_sel(c, m, es);
                    val = eo;
                } else {
                    sel = seg_sel(c, m, a & 0xFF);
                    val = b;
                }
                break;
            case 1:                             /* Import: Ordinalzahl   */
            case 2: {                           /* Import: Name          */
                int dm;
                unsigned es, eo, ord = b;
                if (a == 0 || a > NEMODS || a > H16(c, NE_CMOD))
                    return -1;
                dm = c->mod[a - 1];
                if ((fl & 3) == 2) {
                    unsigned nm = H16(c, NE_IMPTAB) + b;
                    ord = name_find(ne_sel, dm * NEMSZ + NE_TABOFF +
                                    M(dm)->entlen, M(dm)->reslen, nm);
                    if (!ord && M(dm)->nrlen) {         /* nicht resident:  */
                        struct ifs_file nf;             /* Datei der DLL   */
                        unsigned ln;
                        unsigned char tmp[8];
                        unsigned long p = M(dm)->nrpos, e = p + M(dm)->nrlen;
                        for (j = 0; M(dm)->path[j] && j < IFS_PATHLEN - 1; j++)
                            np[j] = M(dm)->path[j];
                        np[j] = 0;
                        if (ifs_open(M(dm)->drive, np, &nf))
                            return -1;
                        while (p < e && !ord) {
                            if (nread(&nf, p, KDATA, (unsigned)tmp, 1) || !tmp[0])
                                break;
                            ln = tmp[0];
                            if (ln == t8(nm) &&
                                nread(&nf, p + 1, KDATA, (unsigned)np, ln + 2) == 0) {
                                for (j = 0; j < ln; j++) {
                                    unsigned x = (unsigned char)np[j], y = t8(nm + 1 + j);
                                    if (x >= 'a' && x <= 'z') x -= 32;
                                    if (y >= 'a' && y <= 'z') y -= 32;
                                    if (x != y)
                                        break;
                                }
                                if (j == ln)
                                    ord = (unsigned char)np[ln] | ((unsigned char)np[ln + 1] << 8);
                            }
                            p += ln + 3;
                        }
                    }
                    if (!ord) {
                        ne_log("NE: Name fehlt in ", M(dm)->name);
                        return -1;
                    }
                }
                es = ent_find(ne_sel, dm * NEMSZ + NE_TABOFF,
                              M(dm)->entlen, ord, &eo);
                if (!es || es > M(dm)->nseg) {
                    ne_log("NE: Ordinalzahl fehlt in ", M(dm)->name);
                    return -1;
                }
                sel = USEL(M(dm)->seg[es - 1].idx);
                val = eo;
                break;
            }
            default:                            /* OSFIXUP: uebergehen   */
                continue;
            }
            if (!sel)
                return -1;
            scratch2_map(PG_LIN(pg), pages * MEM_PAGE - 1);
            s = MK_FP(SCRATCH2, 0);
            for (j = 0; j < 4096; j++) {        /* Kette (hoechstens 4096) */
                if ((unsigned long)off + (src == 3 ? 4 : src == 0 ? 1 : 2) >
                    (unsigned long)pages * MEM_PAGE)
                    return -1;
                nxt = s[off] | (s[off + 1] << 8);
                switch (src) {
                case 0:                         /* LOBYTE                */
                    s[off] = (unsigned char)(fl & 4 ? s[off] + val : val);
                    break;
                case 2:                         /* SEGMENT               */
                    s[off] = (unsigned char)sel;
                    s[off + 1] = (unsigned char)(sel >> 8);
                    break;
                case 3:                         /* FAR_ADDR              */
                    if (fl & 4)
                        val += nxt;
                    s[off] = (unsigned char)val;
                    s[off + 1] = (unsigned char)(val >> 8);
                    s[off + 2] = (unsigned char)sel;
                    s[off + 3] = (unsigned char)(sel >> 8);
                    break;
                case 5:                         /* OFFSET                */
                    if (fl & 4)
                        val += nxt;
                    s[off] = (unsigned char)val;
                    s[off + 1] = (unsigned char)(val >> 8);
                    break;
                default:
                    return -1;
                }
                if ((fl & 4) || src == 0 || nxt == 0xFFFF)
                    break;
                off = nxt;
            }
        }
    }
    return 0;
}

/* Modul laden: m < 0 EXE in Task t (Start nach st), sonst DLL in Platz m */
static int ne_do(int t, int drive, const char *path, int m, struct ne_start *st)
{
    struct nectx *c;
    unsigned long fo, sz;
    unsigned k, ns, al, fl, len, mn, i, tlen, auto_s, nmod;
    int err = EXEC_ERR_FORMAT, r;
    if (nd + 1 >= NEDEPTH)
        return EXEC_ERR_FORMAT;
    c = &nc[++nd];
    c->tu = 0;
    for (i = 0; path[i] && i < sizeof(c->path) - 1; i++)
        c->path[i] = path[i];
    c->path[i] = 0;
    path = c->path;
    if (m < 0)
        for (k = 0; k < NESEG; k++)
            xseg[k].pg = 0, xseg[k].idx = 0;
    if (m >= 0) {
        M(m)->nseg = M(m)->ndep = 0;
        M(m)->refs = 0;
    }
    if (ifs_open(drive, path, &c->f)) {
        err = EXEC_ERR_NOTFOUND;
        goto out;
    }
    if (nread(&c->f, 0, KDATA, (unsigned)c->h, 64) || c->h[0] != 'M' || c->h[1] != 'Z')
        goto out;
    c->neoff = c->h[0x3C] | ((unsigned long)c->h[0x3D] << 8) |
               ((unsigned long)c->h[0x3E] << 16) | ((unsigned long)c->h[0x3F] << 24);
    if (nread(&c->f, c->neoff, KDATA, (unsigned)c->h, 64) || c->h[0] != 'N' || c->h[1] != 'E')
        goto out;
    fl = H16(c, NE_FLAGS);
    if ((m >= 0) != ((fl & NEF_LIB) != 0))
        goto out;                               /* DLL als EXE o. ae.     */
    ns = H16(c, NE_CSEG);
    nmod = H16(c, NE_CMOD);
    al = H16(c, NE_ALIGN);
    if (!al)
        al = 9;
    if (ns == 0 || ns > NESEG || nmod > NEMODS || al > 15 ||
        (m < 0 && ns + 2 > nldt)) {
        ne_log("NE: zu viele Segmente/Module ", 0);
        goto out;
    }
    if (m < 0)
        ne_floor = ns + 2;
    tlen = H16(c, NE_ENTTAB) + H16(c, NE_CBENT);
    if (tlen < H16(c, NE_IMPTAB) || tlen > 0xF000U)
        goto out;
    c->tlen = tlen;
    c->tn = PAGES(tlen);
    c->tu = kf_mem_get(c->tn);
    if (!c->tu) {
        err = EXEC_ERR_NOMEM;
        goto out;
    }
    tmap(c);
    if (nread(&c->f, c->neoff, SCRATCH, 0, tlen))
        goto out;

    /* 1. importierte Module (laden ueberschreibt SCRATCH: danach tmap) */
    for (i = 0; i < nmod; i++) {
        r = mod_get(c, t, drive, path, H16(c, NE_IMPTAB) +
                    t16(H16(c, NE_MODTAB) + 2 * i));
        if (r < 0) {
            err = EXEC_ERR_DLL;
            goto out;
        }
        c->mod[i] = (unsigned char)r;
        if (m >= 0 && M(m)->ndep < NEDEP)
            M(m)->dep[M(m)->ndep++] = (unsigned char)r;
    }

    /* 2. Segmente anlegen und fuellen */
    auto_s = H16(c, NE_AUTO);
    for (k = 0; k < ns; k++) {
        struct neseg g;
        unsigned o = H16(c, NE_SEGTAB) + k * 8;
        tmap(c);
        fo = (unsigned long)t16(o) << al;
        len = t16(o + 2);
        mn = t16(o + 6);
        g.acc = (t16(o + 4) & 1) ? ACC_UDATA : ACC_UCODE;
        g.inst = (unsigned char)(m >= 0 && (fl & NEF_MULTI) && (t16(o + 4) & 1) &&
                                 (k + 1 == auto_s || !(t16(o + 4) & NESF_SHARE)));
        sz = mn ? mn : 0x10000UL;
        if (fo && len && len > sz)
            sz = len;
        if (k + 1 == auto_s)
            sz += (unsigned long)H16(c, NE_HEAP) + (m < 0 ? H16(c, NE_STACK) : 0);
        if (sz > 0x10000UL)
            sz = 0x10000UL;
        g.size = (unsigned short)(sz - 1);
        g.pages = PAGES(sz);
        g.pg = kf_mem_get(g.pages);
        if (!g.pg) {
            err = EXEC_ERR_NOMEM;
            goto out;
        }
        g.idx = m < 0 ? k + 2 : sh_alloc();
        if (m < 0) {
            xseg[k] = g;
            alloc_units += g.pages;
        } else {
            M(m)->seg[k] = g;
            M(m)->nseg = k + 1;
            image_units += g.pages;
            if (!g.idx) {
                ne_log("NE: LDT voll ", 0);
                err = EXEC_ERR_NOMEM;
                goto out;
            }
        }
        scratch2_map(PG_LIN(g.pg), g.pages * MEM_PAGE - 1);
        zero_seg(SCRATCH2, g.pages * (MEM_PAGE / 2));
        if (fo) {
            unsigned long dl = len ? len : 0x10000UL;
            if (dl > 0x8000UL) {
                if (nread(&c->f, fo, SCRATCH2, 0, 0x8000U) ||
                    nread(&c->f, fo + 0x8000UL, SCRATCH2, 0x8000U, (unsigned)(dl - 0x8000UL)))
                    goto out;
            } else if (nread(&c->f, fo, SCRATCH2, 0, (unsigned)dl))
                goto out;
        }
    }

    /* 3. Relokationen */
    for (k = 0; k < ns; k++) {
        unsigned o = H16(c, NE_SEGTAB) + k * 8;
        tmap(c);
        fo = (unsigned long)t16(o) << al;
        len = t16(o + 2);
        if (fo && (t16(o + 4) & 0x100) &&
            reloc(c, m, k, fo + (len ? len : 0x10000UL))) {
            ne_log("NE: Relokation gescheitert ", m >= 0 ? M(m)->name : 0);
            goto out;
        }
    }

    tmap(c);
    if (m >= 0) {                               /* DLL: Tabellen merken  */
        unsigned el = H16(c, NE_CBENT), ro = H16(c, NE_RESTAB);
        unsigned rl = H16(c, NE_MODTAB) - ro;
        unsigned char __far *d = M(m)->tab;
        if ((unsigned long)el + rl > NE_TABMAX) {
            ne_log("NE: Tabellen zu gross ", M(m)->name);
            goto out;
        }
        for (i = 0; i < el; i++)
            d[i] = (unsigned char)t8(H16(c, NE_ENTTAB) + i);
        for (i = 0; i < rl; i++)
            d[el + i] = (unsigned char)t8(ro + i);
        M(m)->entlen = el;
        M(m)->reslen = rl;
        M(m)->nrpos = c->h[NE_NRESTAB] | ((unsigned long)c->h[NE_NRESTAB + 1] << 8) |
                      ((unsigned long)c->h[NE_NRESTAB + 2] << 16) |
                      ((unsigned long)c->h[NE_NRESTAB + 3] << 24);
        M(m)->nrlen = H16(c, NE_CBNRES);
        M(m)->flags = fl;
        M(m)->ics = H16(c, NE_CS) <= ns ? H16(c, NE_CS) : 0;
        M(m)->iip = H16(c, NE_IP);
        M(m)->heap = H16(c, NE_HEAP);
        M(m)->ads = auto_s <= ns ? auto_s : 0;
        M(m)->inited = 0;
        M(m)->ninst = 0;
        for (k = 0; k < ns; k++)
            M(m)->ninst += M(m)->seg[k].inst;
        M(m)->rpos = c->neoff + H16(c, NE_RSRCTAB);
        M(m)->rlen = ro - H16(c, NE_RSRCTAB);
        M(m)->drive = (unsigned char)drive;
        for (i = 0; path[i] && i < sizeof(M(m)->path) - 1; i++)
            M(m)->path[i] = path[i];
        M(m)->path[i] = 0;
    } else {                                    /* EXE: Task einrichten  */
        unsigned cs = H16(c, NE_CS), ss = H16(c, NE_SS), sp = H16(c, NE_SP);
        if (cs == 0 || cs > ns || ss > ns || auto_s > ns)
            goto out;
        if (ss == 0)
            ss = auto_s;
        if (ss == 0)
            goto out;
        for (k = 0; k < ns; k++)
            desc_put(t, xseg[k].idx, PG_LIN(xseg[k].pg), xseg[k].size,
                     xseg[k].acc, 0);
        nil = 0;
        iseen = 0;
        for (i = 0; i < nmod; i++)
            init_collect(c->mod[i]);
        for (i = 0; i < nmod; i++)
            if (attach(t, c->mod[i])) {
                err = EXEC_ERR_NOMEM;
                goto out;
            }
        st->cs = USEL(cs + 1);
        st->ip = H16(c, NE_IP);
        st->ss = USEL(ss + 1);
        st->sp = sp ? sp : (unsigned)(xseg[ss - 1].size + 1);    /* 64 KB -> 0 */
        st->ds = auto_s ? USEL(auto_s + 1) : st->ss;
        st->lfn = (unsigned char)(c->h[NE_OTHER] & 1);
        NEX(t)->drive = (unsigned char)drive;
        for (i = 0; path[i] && i < sizeof(NEX(t)->path) - 1; i++)
            NEX(t)->path[i] = path[i];
        NEX(t)->path[i] = 0;
        NEX(t)->rpos = c->neoff + H16(c, NE_RSRCTAB);
        NEX(t)->rlen = H16(c, NE_RESTAB) - H16(c, NE_RSRCTAB);
        task[t].nemods |= NE_EXE;
        if (nil) {                              /* Start-Routinen zuerst  */
            unsigned w = nil * 5 + 4, sp2 = st->sp - w * 2, q;
            unsigned short __far *s;
            if (!ne_stub || (unsigned long)xseg[ss - 1].size + 1 < w * 2 + 64U) {
                ne_log("NE: kein Platz fuer DLL-Start ", 0);
                goto out;
            }
            desc_put(t, ne_stub, PG_LIN(ne_stubpg), sizeof(ne_stubc) - 1, ACC_UCODE, 1);
            scratch2_map(PG_LIN(xseg[ss - 1].pg), xseg[ss - 1].pages * MEM_PAGE - 1);
            s = MK_FP(SCRATCH2, sp2);
            for (i = 0, q = 0; i < nil; i++) {
                struct nemod __far *p = M(ilist[i]);
                s[q++] = ilist[i] + 1;          /* Modulkennung           */
                s[q++] = p->ads ? USEL(p->seg[p->ads - 1].idx) : 0;
                s[q++] = p->heap;
                s[q++] = p->iip;
                s[q++] = USEL(p->seg[p->ics - 1].idx);
            }
            s[q++] = 0;
            s[q++] = st->ds;
            s[q++] = st->ip;
            s[q++] = st->cs;
            st->cs = USEL(ne_stub);
            st->ip = 0;
            st->sp = sp2;
        }
    }
    err = 0;
out:
    if (c->tu)
        mem_free(c->tu, c->tn);
    if (err) {
        if (m < 0) {
            for (k = 0; k < NESEG; k++)
                if (xseg[k].pg) {
                    mem_free(xseg[k].pg, xseg[k].pages);
                    alloc_units -= xseg[k].pages;
                    xseg[k].pg = 0;
                }
        } else
            mod_free(m);
    }
    nd--;
    return err;
}

/* NE-Programm in Task t laden (load_program, nachdem load_image FX
   abgelehnt hat). EXEC_ERR_FORMAT: keine NE-Datei */
int __far ne_load(int t, int drive, const char *path, struct ne_start *st)
{
    int r, m;
    unsigned i;
    if (!ne_sel) {
        ne_sel = kseg_alloc((unsigned long)NEMOD * NEMSZ +
                            NTASK * sizeof(struct neexe));
        if (!ne_sel)
            return EXEC_ERR_NOMEM;
        for (m = 0; m < NEMOD; m++)
            M(m)->used = 0;
        for (m = 0; m < NTASK; m++)
            NEX(m)->drive = 0xFF;
    }
    bkl_hold++;
    if (!ne_stub && (ne_stubpg = kf_mem_get(1)) != 0) {
        ne_floor = 2;
        ne_stub = sh_alloc();                   /* bleibt fuer immer       */
        if (!ne_stub) {
            mem_free(ne_stubpg, 1);
            ne_stubpg = 0;
        } else {
            scratch_map(PG_LIN(ne_stubpg), MEM_PAGE - 1);
            for (i = 0; i < sizeof(ne_stubc); i++)
                *(unsigned char __far *)MK_FP(SCRATCH, i) = ne_stubc[i];
            image_units++;
        }
    }
    task[t].nemods = 0;
    for (i = 0; i < nldt; i++) {
        LDT(t, i)->access = 0;
        LDT(t, i)->rsv = 0;
    }
    ne_floor = 2;
    r = ne_do(t, drive, path, -1, st);
    if (r) {
        for (i = ne_floor; i < nldt; i++)       /* Instanzkopien der DLLs */
            if (LDT(t, i)->access && !(LDT(t, i)->rsv & D_ALIAS)) {
                unsigned n = PAGES((unsigned long)LDT(t, i)->limit + 1);
                mem_free(dbase(LDT(t, i)) >> 12, n);
                alloc_units -= n;
                LDT(t, i)->access = 0;
            }
        ne_task_end(t);                         /* schon angehaengte DLLs */
        for (i = 2; i < nldt; i++)              /* Aliase wieder weg      */
            if (LDT(t, i)->rsv & D_ALIAS)
                LDT(t, i)->access = 0, LDT(t, i)->rsv = 0;
        for (m = 0; m < NEMOD; m++)             /* dabei geladen, ohne Benutzer */
            if (M(m)->used && !M(m)->refs)
                mod_free(m);
    }
    ne_floor = 2;
    bkl_hold--;
    return r;
}

/* FAPI_GETMOD: Kennung der DLL `nm` (Grossbuchstaben, ohne .DLL), die
   Task t benutzt; 0 = nicht da */
unsigned __far ne_getmod(int t, const char *nm)
{
    int m;
    unsigned i;
    if (!ne_sel)
        return 0;
    for (m = 0; m < NEMOD; m++) {
        struct nemod __far *p = M(m);
        if (!p->used || !(task[t].nemods & (1u << m)))
            continue;
        for (i = 0; i < 9; i++) {
            char a = nm[i];
            if (a >= 'a' && a <= 'z')
                a -= 32;
            if (a == '.')
                a = 0;
            if (a != p->name[i])
                break;
            if (!a)
                return m + 1;
        }
    }
    return 0;
}

/* FAPI_GETRES (wie DosGetResource): Ressource Typ/Kennung (Zahlen) des
   Moduls h (0 = eigenes Programm) in ein neues Segment des Tasks lesen
   -> Selektor, FAPI_ERROR. */
static struct ifs_file rs_f;
static unsigned char rs_b[12];
unsigned __far ne_getres(int t, unsigned h, unsigned type, unsigned id)
{
    unsigned long p, e, pos = 0, len = 0;
    unsigned sh, tid, cnt, k, sel = FAPI_ERROR, drive;
    const char __far *path;
    unsigned i;
    if (!ne_sel || !(task[t].nemods & NE_EXE))
        return FAPI_ERROR;
    if (h == 0) {
        p = NEX(t)->rpos; e = p + NEX(t)->rlen;
        drive = NEX(t)->drive; path = NEX(t)->path;
    } else if (h <= NEMOD && (task[t].nemods & (1u << (h - 1)))) {
        p = M(h - 1)->rpos; e = p + M(h - 1)->rlen;
        drive = M(h - 1)->drive; path = M(h - 1)->path;
    } else
        return FAPI_ERROR;
    if (e < p + 4)
        return FAPI_ERROR;                      /* keine Ressourcen       */
    bkl_hold++;
    for (i = 0; path[i] && i < IFS_PATHLEN - 1; i++)
        np[i] = path[i];
    np[i] = 0;
    if (ifs_open(drive, np, &rs_f) || nread(&rs_f, p, KDATA, (unsigned)rs_b, 2))
        goto out;
    sh = rs_b[0] | (rs_b[1] << 8);
    if (sh > 15)
        goto out;
    for (p += 2; p + 8 <= e && !len; ) {
        if (nread(&rs_f, p, KDATA, (unsigned)rs_b, 8))
            goto out;
        tid = rs_b[0] | (rs_b[1] << 8);
        cnt = rs_b[2] | (rs_b[3] << 8);
        if (!tid)
            break;
        p += 8;
        if (tid != (type | 0x8000u)) {
            p += ((unsigned long)cnt << 3) + ((unsigned long)cnt << 2);
            continue;
        }
        for (k = 0; k < cnt && p + 12 <= e; k++, p += 12) {
            if (nread(&rs_f, p, KDATA, (unsigned)rs_b, 12))
                goto out;
            if ((rs_b[6] | (rs_b[7] << 8)) == (id | 0x8000u)) {
                pos = (unsigned long)(rs_b[0] | (rs_b[1] << 8)) << sh;
                len = (unsigned long)(rs_b[2] | (rs_b[3] << 8)) << sh;
                break;
            }
        }
        break;
    }
    if (!len || len > 0x10000UL)
        goto out;
    if (pos + len > rs_f.size)                  /* Rest hinter dem Ende   */
        len = rs_f.size > pos ? rs_f.size - pos : 0;
    if (!len)
        goto out;
    sel = kf_seg_alloc(t, (unsigned)len);       /* 64 KB -> 0             */
    if (sel == FAPI_ERROR)
        goto out;
    if (len > 0x8000UL) {
        if (nread(&rs_f, pos, sel, 0, 0x8000U) ||
            nread(&rs_f, pos + 0x8000UL, sel, 0x8000U, (unsigned)(len - 0x8000UL)))
            goto bad;
    } else if (nread(&rs_f, pos, sel, 0, (unsigned)len))
        goto bad;
    goto out;
bad:
    kf_seg_free(t, sel);
    sel = FAPI_ERROR;
out:
    bkl_hold--;
    return sel;
}
