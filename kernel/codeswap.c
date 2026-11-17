/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.1 - Auslagerung Teil 2, Schritt 1: Codesegmente verwerfen
 * (KTEXT2), wie OS/2 1.x "discardable" Segmente.
 *
 * Der Code eines FX-Programms (Version 2: eigenes Codesegment, LDT-Eintrag
 * 0, unter Instanzen geteilt) steht unveraendert in seiner EXE. Wird der
 * Speicher knapp (mem_get, nach Cache und geparkten Boxen), gibt
 * code_drop den Code eines Programms frei, das gerade nicht laeuft und
 * dessen Codesegment seit der letzten Runde nicht benutzt wurde (Accessed-
 * Bit im Deskriptor, Uhr-Verfahren). Der Deskriptor wird "nicht
 * vorhanden"; laedt die CPU den Selektor wieder (meist beim IRET/RETF
 * zurueck ins Programm: Takt, FAPI-Ruecksprung, oder beim Taskwechsel),
 * kommt #NP (Vektor 11, Fehlercode = Selektor) - code_np laedt den Code
 * aus der EXE neu (keine Schreibzugriffe) und der Befehl laeuft noch
 * einmal. Beim Laden gemerkte Pruefsumme schuetzt vor einer inzwischen
 * ueberschriebenen EXE: passt sie nicht, endet das Programm mit Meldung.
 * Pfade fuer das Nachladen in einem Kernelsegment (cs_sel), angelegt beim
 * ersten Programm. Code von Wechselmedien (Diskette, CD, ZIP, Loop) wird
 * nie verworfen - das Medium kann inzwischen ein anderes sein (SETUP
 * wechselt die Disketten).
 */
#include "kern.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "../include/fx.h"
#include "k2.h"
#include "task.h"

void __far kf_klock_enter(void);            /* kernel.c                   */
void __far kf_klock_leave(void);
void __far kf_vio_kputs(int s, const char *str);
void __far kf_abort_current(void);          /* Programm beenden (kernel.c) */
void __far kf_seg_acct(int img, int pages); /* MEM-Zaehler                 */
int __far swap_run_alloc(unsigned k);       /* swap.c                      */
void __far swap_run_free(unsigned s, unsigned k);
int __far swap_page_io(unsigned s, pg_t pg, int wr);
static int seg_np(int t, unsigned idx);
extern int current;

#define CS_ENT  (IFS_PATHLEN + 3)       /* Laufwerk, Pruefsumme, Pfad      */
static unsigned cs_sel;
#define CSE(t)  ((unsigned char __far *)MK_FP(cs_sel, (t) * CS_ENT))

/* Summe der Worte (Code ist ein Vielfaches von 16 Byte) */
unsigned __far code_sum(unsigned sel, unsigned n)
{
    const unsigned __far *p = MK_FP(sel, 0);
    unsigned s = 0;
    for (n >>= 1; n; n--)
        s += *p++;
    return s;
}

/* beim Laden: Laufwerk, Pfad und Pruefsumme des Codes von Task t */
void __far code_note(int t, int drive, const char *path, unsigned sum)
{
    unsigned char __far *e;
    unsigned i;
    if (!cs_sel && !(cs_sel = kseg_alloc((unsigned long)NTASK * CS_ENT)))
        return;
    e = CSE(t);
    e[0] = (unsigned char)drive;
    e[1] = (unsigned char)sum;
    e[2] = (unsigned char)(sum >> 8);
    for (i = 0; i < IFS_PATHLEN - 1 && path[i]; i++)
        e[3 + i] = path[i];
    e[3 + i] = 0;
}

static int removable(unsigned d)        /* Laufwerk auf Wechselmedium? */
{
    int b;
    if (d >= NDRIVE)
        return 1;
    b = vol[d].blk;
    return b < 0 || b >= nblk || blk[b].drv < 0 || blk[b].rem || blk[b].bsize == 2048;
}

static int sharer(int k, int t)         /* teilt k den Code von t?       */
{
    return task[k].state != T_FREE && (task[k].code_unit ? task[k].code_unit == task[t].code_unit
                                                         : 0);
}

/* einen Code verwerfen -> 1 */
int __far code_drop(void)
{
    int t, k, pass, busy;
    pg_t u;

    if (!cs_sel)
        return 0;
    for (pass = 0; pass < 2; pass++)
        for (t = 0; t < NTASK; t++) {
            if (task[t].state == T_FREE || !task[t].code_unit || task[t].v86 ||
                CSE(t)[3] == 0 || removable(CSE(t)[0]))
                continue;
            busy = 0;
            for (k = 0; k < NTASK; k++)
                if (sharer(k, t) && (k == current || CSE(k)[3] == 0 ||
                                     (LDT(k, 0)->access & 1)))
                    busy = 1;
            if (busy) {                 /* zweite Chance: A-Bits weg     */
                for (k = 0; k < NTASK; k++)
                    if (sharer(k, t) && k != current)
                        LDT(k, 0)->access &= (unsigned char)~1;
                continue;
            }
            u = task[t].code_unit;
            for (k = 0; k < NTASK; k++)
                if (k != t && sharer(k, t)) {
                    task[k].code_unit = 0;
                    task[k].code_gone = 1;
                    LDT(k, 0)->access &= (unsigned char)~0x80;
                }
            task[t].code_unit = 0;
            task[t].code_gone = 1;
            LDT(t, 0)->access &= (unsigned char)~0x80;
            mem_free(u, task[t].code_cnt);
            image_units -= task[t].code_cnt;
            return 1;
        }
    return 0;
}

static void desc_base_set(struct desc __far *d, unsigned long b)
{
    if (k_alias)
        b += 0x80000000UL;
    d->base_lo = (unsigned short)b;
    d->base_hi = (unsigned char)(b >> 16);
    d->rsv = (unsigned short)((d->rsv & 0x00FF) | ((unsigned)(b >> 24) << 8));
    d->access |= 0x80;
}

/* Code von t aus der EXE neu laden; 0 = ok */
static int code_load(int t)
{
    struct ifs_file f;                  /* Stapel: DGROUP ist voll       */
    struct fx_header h;
    char p[IFS_PATHLEN];
    unsigned char __far *e = CSE(t);
    unsigned i, n = task[t].code_cnt, fx, sum, op = 0;
    unsigned short w[8];
    pg_t u;
    int k, r = -1;

    for (k = 0; k < NTASK; k++)         /* anderer Task hat ihn schon    */
        if (k != t && task[k].state != T_FREE && task[k].code_unit &&
            task[k].code_key == task[t].code_key && task[k].code_fsize == task[t].code_fsize) {
            u = task[k].code_unit;
            goto share;
        }
    for (i = 0; i < IFS_PATHLEN - 1 && e[3 + i]; i++)
        p[i] = (char)e[3 + i];
    p[i] = 0;
    while (!(u = mem_alloc(n)) && (seg_compact(n, 0) || code_drop() ||
                                    seg_out(n) || swap_out(n + 16)))
        ;
    if (!u)
        return -1;
    win_save(w);
    if (ifs_open(e[0], p, &f) || (op = 1, f.size != task[t].code_fsize) ||
        ifs_read(&f, 0x3C, KDATA, (unsigned)&fx, 2) != 2 ||
        ifs_read(&f, fx, KDATA, (unsigned)&h, sizeof(h)) != (int)sizeof(h))
        goto out;
    scratch_map(PG_LIN(u), n * MEM_PAGE - 1);
    if (h.code_size == 0 || PAGES(h.code_size) != n ||
        ifs_read(&f, h.image_off, SCRATCH, 0, h.code_size) != (int)h.code_size)
        goto out;
    sum = code_sum(SCRATCH, h.code_size);
    if (sum != (e[1] | ((unsigned)e[2] << 8)))
        goto out;
    r = 0;
out:
    if (op)
        ifs_close(&f);
    win_restore(w);
    if (r) {
        mem_free(u, n);
        return r;
    }
    image_units += n;
share:
    for (k = 0; k < NTASK; k++)
        if (task[k].state != T_FREE && task[k].code_gone &&
            task[k].code_key == task[t].code_key && task[k].code_fsize == task[t].code_fsize) {
            task[k].code_unit = u;
            task[k].code_gone = 0;
            desc_base_set(LDT(k, 0), PG_LIN(u));
        }
    return 0;
}

/* #NP (Vektor 11) mit Fehlercode err: verworfener Code des laufenden
   Tasks? -> 1 nachgeladen (Befehl wiederholen), 0 nicht zustaendig */
int __far code_np(unsigned err)
{
    int t = current, r;
    if (t >= 0 && (err & 4) && (err & 0xFFF8) && seg_np(t, err >> 3))
        return 1;                       /* ausgelagertes Datensegment    */
    if (t < 0 || !task[t].code_gone || (err & 0xFFFC) != 4 || !cs_sel)
        return 0;
    kf_klock_enter();
    r = task[t].code_gone ? code_load(t) : 0;   /* vielleicht schon da   */
    kf_klock_leave();
    if (r) {
        kf_vio_kputs(task[t].session, "\n\xFE Programmcode nicht nachladbar "
                                      "(EXE ge\x84ndert oder fehlt, kein Speicher)\n");
        kf_abort_current();             /* nur das Programm, kehrt nicht zurueck */
    }
    return 1;
}

/* ---- Schritt 2: Datensegmente verschieben (wie MEMMAN=MOVE) -------------
   mem_get findet keine n Seiten am Stueck, obwohl genug frei ist (Segmente
   brauchen ihren Speicher am Stueck). Gesucht wird ein Fenster aus n
   Seiten, in dem jede Seite frei ist oder zu einem verschiebbaren
   Datensegment gehoert; diese Segmente ziehen woandershin um, nur die
   Basis im Deskriptor aendert sich (der Kernel fasst Programmsegmente
   ueber Selektoren an, beim naechsten Laden des Selektors gilt die neue
   Basis). Verschiebbar: LDT-Eintrag 1 (Daten, Stapel) und DosAllocSeg-
   Segmente von FX-Programmen, nicht Aliase (VioGetBuf), nicht DOS-Boxen,
   nicht Tasks mitten in einer Operation (sie koennten eine lineare
   Adresse halten, waehrend sie schlafen), den laufenden Task nur, wenn
   der Aufrufer es erlaubt (DosAllocSeg - der Kernel haelt dabei keinen
   Zeiger in seine Segmente). Code wird verworfen statt verschoben. */
int __far kf_in_op(int t);                  /* kernel.c                   */
#define SC_TOP  32768UL                 /* gesucht wird in den ersten 128 MB */

static unsigned long seg_base(struct desc __far *d)
{
    unsigned long b = d->base_lo | ((unsigned long)d->base_hi << 16) |
                      ((unsigned long)(d->rsv >> 8) << 24);
    return k_alias ? b - 0x80000000UL : b;
}

static int seg_movable(int t, unsigned i, int cur)
{
    struct desc __far *d = LDT(t, i);
    if (!(d->access & 0x80) || (d->access & 0x18) != 0x10 || (d->rsv & D_ALIAS))
        return 0;                       /* nicht da, kein Datensegment   */
    if (t == current)
        return cur;
    return !kf_in_op(t);
}

/* Seiten eines Datensegments (Eintrag 1: das ganze Abbild, img_cnt) */
static unsigned seg_pages(int t, unsigned i)
{
    if (i == 1 && task[t].img_unit)
        return task[t].img_cnt;
    return PAGES((unsigned long)LDT(t, i)->limit + 1);
}

static int task_ok(int t)
{
    return task[t].state != T_FREE && !task[t].v86;
}

unsigned __far seg_compact(unsigned n, int cur)
{
    unsigned short w[8];
    unsigned char __far *bm;
    pg_t top, p, q, first, np, oldp[16], newp;
    unsigned oldn[16], nm = 0, i, k, t, best = 0, took = 0;
    unsigned long b;
    int r = 0;

    top = mem_top_pg();
    if (top > MEM_LOW_PG)
        top = MEM_LOW_PG;
    if (top > SC_TOP)
        top = SC_TOP;
    if (n > 16 || (first = mem_alloc(1)) == 0)
        return 0;
    win_save(w);
    scratch2_map(PG_LIN(first), MEM_PAGE - 1);
    bm = MK_FP(SCRATCH2, 0);
    for (i = 0; i < MEM_PAGE / 2; i++)
        ((unsigned __far *)bm)[i] = 0;
    /* Bitmap: Seite gehoert einem verschiebbaren Segment */
    for (t = 0; t < NTASK; t++) {
        if (!task_ok(t))
            continue;
        for (i = 1; i < nldt; i++) {
            if (!seg_movable(t, i, cur))
                continue;
            b = seg_base(LDT(t, i)) >> 12;
            k = seg_pages(t, i);
            if ((i == 1 && !task[t].code_unit && LDT(t, 0)->access & 0x80) || k >= n)
                continue;               /* Version 1: Code im selben Block;
                                           ein Segment so gross wie die
                                           Anforderung zu verschieben,
                                           bringt nichts               */
            for (; k && b < top; k--, b++)
                bm[(unsigned)(b >> 3)] |= (unsigned char)(1 << ((unsigned)b & 7));
        }
    }
    /* erstes Fenster, in dem alles frei oder verschiebbar ist */
    for (p = 1; p + n <= top; p++) {
        for (q = p, k = 0; q < p + n; q++) {
            if (!mem_used(q))
                continue;
            if (!(bm[(unsigned)(q >> 3)] & (1 << ((unsigned)q & 7))))
                break;
            k++;
        }
        if (q == p + n && k) {
            best = 1;
            break;
        }
        if (q < p + n)
            p = q;                      /* hinter die feste Seite        */
    }
    mem_free(first, 1);
    if (!best)
        goto out;
    /* freie Seiten des Fensters festhalten, damit kein Umzug dort landet */
    for (q = p; q < p + n; q++)
        if (!mem_used(q)) {
            mem_take(q);
            took |= 1u << (unsigned)(q - p);
        }
    for (t = 0; t < NTASK && r >= 0; t++) {
        if (!task_ok(t))
            continue;
        for (i = 1; i < nldt; i++) {
            struct desc __far *d = LDT(t, i);
            if (!seg_movable(t, i, cur))
                continue;
            if (i == 1 && !task[t].code_unit && LDT(t, 0)->access & 0x80)
                continue;
            b = seg_base(d) >> 12;
            k = seg_pages(t, i);
            if (k >= n || b + k <= p || b >= p + n)
                continue;               /* nicht im Fenster              */
            if (nm == 16 || (newp = mem_alloc(k)) == 0) {
                r = -1;
                break;
            }
            scratch_map(PG_LIN(b), k * MEM_PAGE - 1);
            scratch2_map(PG_LIN(newp), k * MEM_PAGE - 1);
            for (np = 0; np < k; np++)  /* seitenweise (k bis 16)        */
                far_copy(SCRATCH2, (unsigned)np * MEM_PAGE, SCRATCH, (unsigned)np * MEM_PAGE, MEM_PAGE);
            desc_base_set(d, PG_LIN(newp));
            if (i == 1 && task[t].img_unit == b)
                task[t].img_unit = newp;
            oldp[nm] = b;
            oldn[nm++] = k;
        }
    }
    for (q = p; q < p + n; q++)         /* festgehaltene Seiten frei     */
        if (took & (1u << (unsigned)(q - p)))
            mem_free(q, 1);
    for (i = 0; i < nm; i++)
        mem_free(oldp[i], oldn[i]);
    /* Erfolg nur, wenn das Fenster jetzt ganz frei ist - sonst koennte
       mem_get Segmente endlos hin und her schieben */
    for (q = p; q < p + n && !mem_used(q); q++)
        ;
    r = q == p + n;
out:
    win_restore(w);
    return (unsigned)r;
}

/* ---- Schritt 3: Datensegmente auslagern (wie OS/2 1.x MEMMAN=SWAP) -----
   Ein ausgelagertes Datensegment hat im Deskriptor P = 0, Typ und Grenze
   bleiben; base_lo = erster Platz in SWAPPER.DAT (k Plaetze am Stueck),
   base_hi = SW_MARK. Laedt jemand den Selektor (das Programm selbst,
   oder der Kernel beim Ruecksprung ins Programm bzw. beim Zugriff auf
   seine Puffer), kommt #NP (Vektor 11, DS/ES) bzw. #SS (12, Stapel) mit
   dem Selektor als Fehlercode; seg_np holt das Segment zurueck und der
   Befehl laeuft noch einmal. Hinaus geht nur, was ein anderer, gerade
   nicht in einer Operation steckender FX-Task hat und seit der letzten
   Runde nicht benutzt wurde (Accessed-Bit, Uhr wie bei code_drop). */
#define SW_MARK 0xA5

int __far seg_swapped(struct desc __far *d)
{
    return d->access && !(d->access & 0x80) && (d->access & 0x18) == 0x10 &&
           d->base_hi == SW_MARK;
}

static unsigned char so_t;              /* Uhr: Task, Eintrag            */
static unsigned so_i = 1;

unsigned __far seg_out(unsigned n)
{
    unsigned look, i, k, j;
    struct desc __far *d;
    pg_t b;
    int t, sl;

    (void)n;
    for (look = 0; look < 2u * NTASK * nldt; look++) {
        t = so_t;
        i = so_i;
        if (++so_i >= nldt) {
            so_i = 1;
            so_t = (unsigned char)((so_t + 1) % NTASK);
        }
        if (!task_ok(t) || t == current || i >= nldt || kf_in_op(t))
            continue;
        d = LDT(t, i);
        if (!(d->access & 0x80) || (d->access & 0x18) != 0x10 || (d->rsv & D_ALIAS))
            continue;
        if (i == 1 && !task[t].code_unit)
            continue;                   /* Version 1: Code im selben Block */
        if (d->access & 1) {            /* benutzt: zweite Chance        */
            d->access &= (unsigned char)~1;
            continue;
        }
        b = seg_base(d) >> 12;
        k = seg_pages(t, i);
        if ((sl = swap_run_alloc(k)) < 0)
            return 0;                   /* Datei voll                    */
        for (j = 0; j < k; j++)
            if (swap_page_io((unsigned)sl + j, b + j, 1)) {
                swap_run_free((unsigned)sl, k);
                return 0;
            }
        d->access &= (unsigned char)~0x80;
        d->base_lo = (unsigned short)sl;
        d->base_hi = SW_MARK;
        d->rsv &= 0x00FF;
        if (i == 1)
            task[t].img_unit = 0;
        mem_free(b, k);
        kf_seg_acct(i == 1, -(int)k);
        return 1;
    }
    return 0;
}

/* Segment idx von t zurueckholen -> 1 */
static int seg_np(int t, unsigned idx)
{
    struct desc __far *d;
    unsigned k, j, sl;
    pg_t u;
    int r = 0;

    if (idx >= nldt)
        return 0;
    d = LDT(t, idx);
    if (!seg_swapped(d))
        return 0;
    kf_klock_enter();
    if (!seg_swapped(d)) {              /* inzwischen geholt             */
        kf_klock_leave();
        return 1;
    }
    sl = d->base_lo;
    k = idx == 1 ? task[t].img_cnt : PAGES((unsigned long)d->limit + 1);
    while (!(u = mem_alloc(k)) && (seg_compact(k, 0) || code_drop() ||
                                    seg_out(k) || swap_out(k + 16)))
        ;
    if (u) {
        for (j = 0; j < k; j++)
            if (swap_page_io(sl + j, u + j, 0))
                break;
        if (j == k) {
            swap_run_free(sl, k);
            d->base_hi = 0;
            desc_base_set(d, PG_LIN(u));    /* setzt P                   */
            if (idx == 1)
                task[t].img_unit = u;
            kf_seg_acct(idx == 1, (int)k);
            r = 1;
        } else
            mem_free(u, k);
    }
    kf_klock_leave();
    if (!r) {
        kf_vio_kputs(task[t].session, "\n\xFE Ausgelagertes Datensegment nicht "
                                      "zur\x81" "ckzuholen (kein Speicher, Lesefehler)\n");
        kf_abort_current();
    }
    return r;
}

/* Programmende/DosFreeSeg: ausgelagertes Segment -> Plaetze frei, 1 */
int __far seg_drop_sw(int t, unsigned idx)
{
    struct desc __far *d = LDT(t, idx);
    if (!seg_swapped(d))
        return 0;
    swap_run_free(d->base_lo, idx == 1 ? task[t].img_cnt
                                       : PAGES((unsigned long)d->limit + 1));
    d->access = 0;
    d->rsv = 0;
    d->base_hi = 0;
    return 1;
}
