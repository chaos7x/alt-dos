/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.1 - Auslagerungsdatei, Teil 1: Seiten der DOS-Boxen (KTEXT2)
 *
 * Auf kleinen Rechnern (2-4 MB) ist der Speicher der DOS-Boxen der grosse
 * Brocken (~800 KB je Box mit FreeDOS, dazu XMS/EMS/DPMI). Reicht der
 * Speicher nicht mehr (auch nicht, nachdem der Platten-Cache abgegeben
 * hat), lagert swap_out selten benutzte Seiten der Boxen in die Datei
 * SWAPPER.DAT aus (wie OS/2 2.0). Ihr Eintrag in der Seitentabelle wird
 * "nicht vorhanden" mit PTE_SWP und der Nummer des Platzes; greift die Box
 * (V86 oder DPMI) oder der Kernel fuer sie (Redirector, virtuelle SB,
 * XMS-Kopien ueber VWIN/RWIN) darauf zu, holt swap_pf sie zurueck.
 *
 * Plattenzugriffe ohne Dateisystem: die Plattenbereiche der Datei werden
 * beim Start einmal ermittelt (ifs_bmap, wie Loop-Laufwerke); gelesen und
 * geschrieben wird direkt ueber den Treiber in einen eigenen 4-KB-Puffer
 * unter 16 MB (ISA-Busmaster), ohne Platten-Cache. Die Datei bleibt offen
 * (Eintrag des Kernels in der Dateitabelle) - niemand sonst kann sie
 * lesen, schreiben, loeschen.
 *
 * Alles laeuft unter der Kernelsperre (wiedereintrittsfest, zaehlend):
 * Auslagern setzt den Eintrag zuerst auf "nicht vorhanden", dann wird
 * geschrieben - wer die Seite in dem Moment braucht, wartet auf die
 * Sperre und liest danach. Nie ausgelagert: die ersten 64 KB (IVT, BDA,
 * HMA-Umlauf), A0000-BFFFF, C0000-C7FFF, F0000-10FFFF (Stummel, HMA),
 * Seiten ohne PTE_OWN oberhalb 640 KB (EMS-Rahmen, ROM, Bildspeicher),
 * EMS-Seiten selbst, Seitentabellen.
 */
#include "kern.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "k2.h"

#define PTE_P   1UL
#define PTE_W   2UL
#define PTE_U   4UL
#define PTE_A   0x20UL
#define PTE_OWN 0x200UL                 /* wie v86.c                      */

extern pg_t pd_box[NTASK], pt_box[NTASK];   /* v86.c                     */
extern unsigned long v86_pages;
extern int current;                     /* kernel.c                     */
static void k2_cr3(unsigned lo, unsigned hi);
#pragma aux k2_cr3 = "shl edx,16" "mov dx,ax" "mov cr3,edx" \
    __parm [__ax] [__dx] __modify [__dx];
static unsigned long k2_get_cr3(void);
#pragma aux k2_get_cr3 = "mov eax,cr3" "mov edx,eax" "shr edx,16" \
    __value [__dx __ax] __modify [__ax __dx];
static unsigned long k2_cr2(void);
#pragma aux k2_cr2 = "mov eax,cr2" "mov edx,eax" "shr edx,16" \
    __value [__dx __ax] __modify [__ax __dx];
void __far kf_blk_flush(void);
void __far kf_klock_enter(void);            /* kernel.c                   */
void __far kf_klock_leave(void);
int __far kf_swap_rw(int dev, unsigned long lba, unsigned n, unsigned off, int wr);
struct ifs_file __far * __far kf_kopen(int d, const char *canon, int create);

/* Kopf im Kernelsegment sw_sel, dahinter die Bereiche (8 Byte: Stelle in
   der Datei, Sektor auf dem Geraet, in 512er-Einheiten), dahinter die
   Belegung der Plaetze (1 Bit je 4 KB) */
struct swhdr {
    int            dev;                 /* Blockgeraet                    */
    unsigned       n, slots, used;      /* Bereiche, Plaetze, belegt      */
    unsigned       bm, hint;            /* Offset der Bitmap, Suchbeginn  */
    int            ct;                  /* Uhr: Task                      */
    unsigned long  cp;                  /* Uhr: lineare Seite             */
    unsigned long  outs, ins;           /* Zaehler                        */
    unsigned       last;                /* zuletzt getroffener Bereich    */
    unsigned       mm;                  /* Offset der Umzugs-Bitmap       */
};
#define MM_PG   4096U                   /* Umzug nur unter 16 MB          */
#define SW_EXT  32
unsigned sw_sel, sw_buf;                /* Tabelle, Puffer (4 KB)          */
unsigned long sw_bphys;                 /* physische Adresse des Puffers  */
#define SH      ((struct swhdr __far *)MK_FP(sw_sel, 0))
#define EX(k)   ((unsigned long __far *)MK_FP(sw_sel, SW_EXT + (k) * 8))
#define BM(s)   (((unsigned char __far *)MK_FP(sw_sel, SH->bm))[(s) >> 3])

static unsigned long rd32(unsigned sel, unsigned i)
{
    unsigned __far *p = MK_FP(sel, i << 2);
    return p[0] | ((unsigned long)p[1] << 16);
}

static void wr32(unsigned sel, unsigned i, unsigned long v)
{
    unsigned __far *p = MK_FP(sel, i << 2);
    p[0] = (unsigned)v;
    p[1] = (unsigned)(v >> 16);
}

/* Eintrag der Seitentabelle fuer (t, lp): *pt = physische Adresse der
   Tabelle (0 = keine) -> Eintrag */
static unsigned long pte_at(int t, unsigned long lp, unsigned long *pt)
{
    unsigned long pde;
    *pt = 0;
    if (!pd_box[t] || lp >= 0x80000UL)
        return 0;
    scratch2_map(PG_LIN(pd_box[t]), 4095);
    pde = rd32(SCRATCH2, (unsigned)(lp >> 10));
    if (!(pde & PTE_P))
        return 0;
    *pt = pde & ~0xFFFUL;
    scratch2_map(*pt, 4095);
    return rd32(SCRATCH2, (unsigned)(lp & 0x3FF));
}

static void pte_put(unsigned long pt, unsigned long lp, unsigned long e)
{
    scratch2_map(pt, 4095);
    wr32(SCRATCH2, (unsigned)(lp & 0x3FF), e);
}

static void tlb_if(int t)               /* Tabelle der laufenden Box geaendert */
{
    if (t >= 0 && t == current && k2_get_cr3() == PG_LIN(pd_box[t]))
        k2_cr3((unsigned)k2_get_cr3(), (unsigned)(k2_get_cr3() >> 16));
}

/* Platz s (4 KB) lesen/schreiben: ueber die Bereiche, je Stueck ein
   Treiberauftrag in sw_buf; 0 = ok */
static int slot_io(unsigned s, int wr)
{
    struct swhdr __far *h = SH;
    unsigned long u = (unsigned long)s << 3, c, run;
    unsigned done = 0, k, lo, hi, m;

    while (done < 8) {
        m = h->last;
        if (m >= h->n || u < EX(m)[0] || (m + 1 < h->n && u >= EX(m)[2])) {
            lo = 0;
            hi = h->n;
            while (hi - lo > 1) {
                m = (lo + hi) >> 1;
                if (EX(m)[0] <= u)
                    lo = m;
                else
                    hi = m;
            }
            m = lo;
            h->last = m;
        }
        c = EX(m)[1] + (u - EX(m)[0]);
        run = (m + 1 < h->n ? EX(m)[2] : (unsigned long)h->slots << 3) - u;
        k = 8 - done;
        if (run < k)
            k = (unsigned)run;
        if (kf_swap_rw(h->dev, c, k, done << 9, wr))
            return -1;
        done += k;
        u += k;
    }
    return 0;
}

static int slot_alloc(void)
{
    struct swhdr __far *h = SH;
    unsigned s, i;
    for (i = 0; i < h->slots; i++) {
        s = h->hint + i;
        if (s >= h->slots)
            s -= h->slots;
        if (!(BM(s) & (1 << (s & 7)))) {
            BM(s) |= (unsigned char)(1 << (s & 7));
            h->used++;
            h->hint = s + 1;
            return (int)s;
        }
    }
    return -1;
}

void __far swap_slot_free(unsigned long e)     /* aus v86.c beim Freigeben */
{
    unsigned s = (unsigned)(e >> 12);
    if (!sw_sel || s >= SH->slots || !(BM(s) & (1 << (s & 7))))
        return;
    BM(s) &= (unsigned char)~(1 << (s & 7));
    SH->used--;
}

/* darf (lp, e) hinaus? */
static int victim(unsigned long lp, unsigned long e)
{
    if (!(e & PTE_P))
        return 0;
    if (lp < 0x10 || (lp >= 0xA0 && lp < 0xC8) || (lp >= 0xF0 && lp < 0x110))
        return 0;
    return lp < 0xA0 || (e & PTE_OWN);
}

/* eine Seite von Box t hinaus; -> 1 */
static int page_out(int t, unsigned long lp, unsigned long pt, unsigned long e)
{
    int s = slot_alloc();
    pg_t pd = pd_box[t];

    if (s < 0)
        return 0;
    pte_put(pt, lp, ((unsigned long)s << 12) | PTE_SWP | (e & (PTE_W | PTE_U | PTE_OWN)));
    tlb_if(t);
    scratch_map(e & ~0xFFFUL, 4095);
    far_copy(sw_buf, 0, SCRATCH, 0, 4096);
    if (slot_io((unsigned)s, 1)) {      /* Schreibfehler: Seite bleibt   */
        if (pd_box[t] == pd)
            pte_put(pt, lp, e);
        swap_slot_free((unsigned long)s << 12);
        return 0;
    }
    mem_free((pg_t)(e >> 12), 1);
    v86_pages--;
    SH->outs++;
    return 1;
}

/* n Seiten der Boxen auslagern (Uhr-Verfahren ueber das Zugriffsbit);
   -> ausgelagerte Seiten. Nur aus Zusammenhaengen, die schlafen duerfen. */
unsigned __far swap_out(unsigned n)
{
    struct swhdr __far *h;
    unsigned done = 0, look = 0, i;
    unsigned long pt, e, lp;
    unsigned short w[8];
    int t;

    if (!sw_sel || current < 0)
        return 0;
    kf_klock_enter();
    win_save(w);
    h = SH;
    for (i = 0; i < 2 * NTASK && done < n; ) {  /* zwei Runden: A-Bits  */
        t = h->ct;
        if (t >= NTASK || !pd_box[t]) {
            h->ct = (t + 1) % NTASK;
            h->cp = 0;
            i++;
            continue;
        }
        lp = h->cp;
        if (lp >= 0x80000UL || look > 20000) {  /* Box durch             */
            h->ct = (t + 1) % NTASK;
            h->cp = 0;
            i++;
            look = 0;
            continue;
        }
        e = pte_at(t, lp, &pt);
        if (!pt) {                      /* keine Tabelle: 4 MB weiter    */
            h->cp = (lp | 0x3FF) + 1;
            continue;
        }
        h->cp = lp + 1;
        look++;
        if (!victim(lp, e) || h->used >= h->slots)
            continue;
        if (e & PTE_A) {                /* zweite Chance                 */
            pte_put(pt, lp, e & ~PTE_A);
            continue;
        }
        done += page_out(t, lp, pt, e);
        if (h->used >= h->slots)
            break;
    }
    tlb_if(current);
    win_restore(w);
    kf_klock_leave();
    return done;
}

/* ---- Bereich am Stueck freiraeumen (Teil 2, Schritt 2: wie MOVE) -------
   Programmsegmente brauchen bis 16 Seiten am Stueck; ausgelagerte oder
   freigegebene Box-Seiten liegen aber verstreut. swap_make_run sucht n
   Seiten am Stueck, die frei sind oder Box-Seiten, die umziehen duerfen
   (victim), und raeumt sie: Box-Seite in eine freie Seite ausserhalb
   kopieren und den Eintrag umsetzen, gibt es keine, auslagern. */
#define MM(p)   (((unsigned char __far *)MK_FP(sw_sel, SH->mm))[(unsigned)(p) >> 3])

/* alle Box-Eintraege durchgehen: want = 0 Bitmap fuellen, sonst den
   Eintrag mit dieser Seite suchen -> 1 gefunden (*rt, *rlp, *rpt, *re) */
static int box_walk(pg_t want, int *rt, unsigned long *rlp, unsigned long *rpt,
                    unsigned long *re)
{
    int t;
    unsigned pdi, i;
    unsigned long pde, e, lp;
    pg_t p;
    for (t = 0; t < NTASK; t++) {
        if (!pd_box[t])
            continue;
        for (pdi = 0; pdi < 512; pdi++) {
            scratch_map(PG_LIN(pd_box[t]), 4095);
            pde = rd32(SCRATCH, pdi);
            if (!(pde & PTE_P))
                continue;
            scratch2_map(pde & ~0xFFFUL, 4095);
            for (i = 0; i < 1024; i++) {
                e = rd32(SCRATCH2, i);
                lp = ((unsigned long)pdi << 10) | i;
                if (!victim(lp, e))
                    continue;
                p = (pg_t)(e >> 12);
                if (!want) {
                    if (p < MM_PG)
                        MM(p) |= (unsigned char)(1 << ((unsigned)p & 7));
                } else if (p == want) {
                    *rt = t;
                    *rlp = lp;
                    *rpt = pde & ~0xFFFUL;
                    *re = e;
                    return 1;
                }
            }
        }
    }
    return 0;
}

unsigned __far swap_make_run(unsigned n)
{
    unsigned short w[8];
    unsigned i, ok = 0;
    pg_t s, p, q;
    unsigned long lp, pt, e;
    int t;

    if (!sw_sel || current < 0 || n == 0 || n > 16)
        return 0;
    kf_klock_enter();
    win_save(w);
    for (i = 0; i < MM_PG / 8; i++)
        ((unsigned char __far *)MK_FP(sw_sel, SH->mm))[i] = 0;
    box_walk(0, &t, &lp, &pt, &e);
    for (s = 1; s + n <= MM_PG && !ok; s++) {
        for (i = 0; i < n; i++) {
            p = s + i;
            if (mem_used(p) && !(MM(p) & (1 << ((unsigned)p & 7))))
                break;
        }
        if (i < n)
            continue;
        for (i = 0; i < n; i++) {       /* gefunden: Box-Seiten raus     */
            p = s + i;
            if (!mem_used(p) || !box_walk(p, &t, &lp, &pt, &e))
                continue;
            if ((q = mem_alloc_out(s, s + n)) != 0) {
                scratch_map(PG_LIN(p), 4095);
                scratch2_map(PG_LIN(q), 4095);
                far_copy(SCRATCH2, 0, SCRATCH, 0, 4096);
                pte_put(pt, lp, (e & 0xFFFUL) | PG_LIN(q));
                mem_free(p, 1);
                tlb_if(t);
            } else
                page_out(t, lp, pt, e);
        }
        for (ok = 1, i = 0; i < n; i++)
            if (mem_used(s + i))
                ok = 0;
    }
    win_restore(w);
    kf_klock_leave();
    return ok;
}

/* Seite lp der Box t zurueck; -> 1 da (oder war nie draussen), 0 Fehler */
static int page_in(int t, unsigned long lp)
{
    unsigned long pt, e;
    pg_t p;
    unsigned s;

    e = pte_at(t, lp, &pt);
    if (!(e & PTE_SWP))
        return pt != 0 && (e & PTE_P);
    if (!(p = mem_alloc_hi(1)) && (!swap_out(8) || !(p = mem_alloc_hi(1))))
        return 0;
    s = (unsigned)(e >> 12);
    if (slot_io(s, 0)) {
        mem_free(p, 1);
        return 0;
    }
    e = pte_at(t, lp, &pt);             /* inzwischen geaendert?          */
    if (!(e & PTE_SWP) || (unsigned)(e >> 12) != s) {
        mem_free(p, 1);
        return (e & PTE_P) != 0;
    }
    scratch_map(PG_LIN(p), 4095);
    far_copy(SCRATCH, 0, sw_buf, 0, 4096);
    pte_put(pt, lp, PG_LIN(p) | PTE_P | PTE_A | (e & (PTE_W | PTE_U | PTE_OWN)));
    swap_slot_free(e);
    v86_pages++;
    SH->ins++;
    return 1;
}

/* Seite (t, lp) sicher da haben (v86_copy aus fremdem Zusammenhang) */
int __far swap_need(int t, unsigned long lp)
{
    unsigned short w[8];
    int r;
    if (!sw_sel)
        return 0;
    kf_klock_enter();
    win_save(w);
    r = page_in(t, lp);
    win_restore(w);
    kf_klock_leave();
    return r;
}

/* Seitenfehler (aus der Box oder aus dem Kernel fuer sie): ausgelagerte
   Seite? -> 1 erledigt (Befehl laeuft noch einmal), 0 nicht zustaendig */
int __far swap_pf(void)
{
    unsigned long a = k2_cr2(), pt, e;
    unsigned short w[8];
    int t = current, r;

    struct desc vw;

    if (!sw_sel || t < 0 || !pd_box[t] || a >= 0x80000000UL)
        return 0;
    win_save(w);
    e = pte_at(t, a >> 12, &pt);
    win_restore(w);
    if (!(e & PTE_SWP))
        return 0;
    vw = gdt[VWIN_IDX];                 /* VWIN teilen sich alle Boxen:  */
    r = swap_need(t, a >> 12);          /* beim Schlafen legt eine andere */
    gdt[VWIN_IDX] = vw;                 /* es vielleicht um               */
    return r;
}

/* Belegung fuer MEM: KB der Datei, KB ausgelagert */
void __far swap_info(unsigned long __far *total_kb, unsigned long __far *used_kb)
{
    *total_kb = sw_sel ? (unsigned long)SH->slots << 2 : 0;
    *used_kb = sw_sel ? (unsigned long)SH->used << 2 : 0;
}

/* ---- Start: SWAPPATH=[X:]\Verzeichnis [KB] -------------------------------
   Ohne Angabe auf Rechnern bis 16 MB: X:\SWAP des Startlaufwerks
   (nur Festplatte), doppelte RAM-Groesse, 2-8 MB. SWAPPATH=OFF: keine.
   Die Datei SWAPPER.DAT wird angelegt bzw. auf die Groesse gebracht. */
static char *mcat(char *m, const char *a)
{
    while (*m)
        m++;
    while (*a)
        *m++ = *a++;
    *m = 0;
    return m;
}
static const char nomsg[] = "\xFE Keine Auslagerungsdatei (Laufwerk, Platz?)\n";
/* sw_path: Puffer des Aufrufers (IFS_PATHLEN) - der Startstapel hat nur
   1 KB, mit dem Pfad darauf lief er beim Anlegen der Datei ueber */
void __far swap_boot(const char __far *spec, int sys, char *msg, char *sw_path)
{
    unsigned long kb = 0, ram = mem_ram_kb(), sz;
    struct ifs_file __far *f;
    unsigned i = 0, j = 0, slots, bmb, kv = 0;
    int d = sys, n;

    msg[0] = 0;
    if (spec) {
        while (spec[i] == ' ')
            i++;
        if ((spec[i] | 32) == 'o' && (spec[i + 1] | 32) == 'f')
            return;                     /* SWAPPATH=OFF                  */
        if (spec[i] && spec[i + 1] == ':') {
            d = (spec[i] | 32) - 'a';
            i += 2;
        }
        if (spec[i] != '\\')
            sw_path[j++] = '\\';
        while (spec[i] && spec[i] != ' ' && j < IFS_PATHLEN - 14) {
            char c = spec[i++];
            sw_path[j++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        while (spec[i] == ' ')
            i++;
        while (spec[i] >= '0' && spec[i] <= '9') {
            if (kv < 6553)
                kv = kv * 10 + (unsigned)(spec[i] - '0');
            i++;
        }
        kb = kv;
    } else {
        if (ram > 16384UL || sys < 2)
            return;
        for (; "\\SWAP"[j]; j++)          /* eigenes Verzeichnis: weder   */
            sw_path[j] = "\\SWAP"[j];     /* Stamm noch \DOS\SYSTEM     */
    }
    if (!kb)                            /* Vorgabe: doppelter RAM, bis 8 MB */
        kb = ram > 4096 ? 8192 : ram << 1;
    if (kb < 2048)
        kb = 2048;
    if (kb > 65532UL)
        kb = 65532UL;                   /* 16.383 Plaetze                */
    if (j > 0 && sw_path[j - 1] == '\\')
        j--;                            /* "\" (Stamm) -> ""            */
    for (i = 0; "\\SWAPPER.DAT"[i]; i++)
        sw_path[j + i] = "\\SWAPPER.DAT"[i];
    sw_path[j + i] = 0;
    if (d < 2 || d >= NDRIVE || !vol_present(d) || !vol[d].used || vol[d].drv != &fat_driver ||
        blk[vol[d].blk].drv < 0 || blk[vol[d].blk].rem || blk[vol[d].blk].bsize != 512) {
        mcat(msg, nomsg);
        return;
    }
    sz = kb << 10;
    if (j) {                            /* Verzeichnis anlegen, wenn noetig */
        sw_path[j] = 0;
        if (!ifs_isdir(d, sw_path))
            ifs_mkdir(d, sw_path);
        sw_path[j] = '\\';
    }
    f = kf_kopen(d, sw_path, 0);
    if (!f && !(f = kf_kopen(d, sw_path, 1))) {
        mcat(msg, nomsg);
        return;
    }
    if (f->size != sz) {
        if (ifs_setsize(f, sz) || f->size != sz) {
            mcat(msg, nomsg);
            return;
        }
        ifs_close(f);                   /* Groesse in den Verzeichnis-   */
        ifs_setinfo(d, sw_path, 1, ATTR_HIDDEN | ATTR_SYSTEM, 0, 0);
        if (ifs_open(d, sw_path, f)) {  /* eintrag (sonst 0 Byte mit     */
            mcat(msg, nomsg);           /* belegten Clustern bis zum     */
            return;                     /* Herunterfahren)               */
        }
        f->flags |= IFS_F_WRITE;
    }
    kf_blk_flush();                     /* nichts Geaendertes im Cache   */
    n = ifs_bmap(f, 0, 0, 0);
    slots = (unsigned)(kb >> 2);
    bmb = (slots + 7) >> 3;
    if (n <= 0 || (unsigned long)SW_EXT + ((unsigned long)n << 3) + bmb + MM_PG / 8 > 0xFFF0UL) {
        mcat(msg, nomsg);
        return;
    }
    if (!(sw_buf = kseg_dma(4096, &sw_bphys)) ||
        !(sw_sel = kseg_alloc(SW_EXT + ((unsigned long)n << 3) + bmb + MM_PG / 8))) {
        sw_sel = 0;
        mcat(msg, nomsg);
        return;
    }
    SH->dev = vol[d].blk;
    SH->n = n;
    SH->slots = slots;
    SH->bm = SW_EXT + n * 8;
    SH->mm = SH->bm + bmb;
    ifs_bmap(f, sw_sel, SW_EXT, n);
    {
        char num[8], *q;
        unsigned v;
        mcat(msg, "\xFE Auslagerungsdatei ");
        num[0] = (char)('A' + d);
        num[1] = 0;
        mcat(msg, num);
        mcat(msg, ":");
        mcat(msg, sw_path);
        mcat(msg, ", ");
        q = num + 7;
        *q = 0;
        v = (unsigned)kb;
        do { *--q = (char)('0' + v % 10); v /= 10; } while (v);
        mcat(msg, q);
        mcat(msg, " KB\n");
    }
}
