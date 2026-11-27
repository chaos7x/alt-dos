/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.5 - Semaphoren wie OS/2 1.x (KTEXT4, FAPI_SEM 68h,
 * docs/FAPI-IPC.md Stufe 2).
 *
 * Zwei Arten, eine Kennung (32 Bit):
 *  - RAM-Semaphore: ein Doppelwort im Speicher des Programms, Kennung =
 *    Selektor:Offset. Wort 0: 0 = frei, sonst gesetzt. Kein Besitzer.
 *  - Systemsemaphore \SEM\name: Tabelle im Kernelsegment (erst beim ersten
 *    Anlegen geholt), Kennung = 0000h:(Generation << 8 | Platz + 1).
 *    REQUEST merkt den Besitzer (mit Zaehler), endet er, wird sie frei und
 *    der naechste REQUEST meldet 1 ("Vorbesitzer beendet").
 *
 * Laeuft ohne Kernelsperre (fapi_nolock): der Kernel wird nie unterbrochen,
 * die Tabelle aendert sich nur zwischen zwei Schlafpunkten eines Aufrufs -
 * nach jedem proc_block wird alles neu geprueft. Gewartet wird in Stuecken
 * (SLICE), damit Strg+C (Signal) und ein verschobenes Segment (die lineare
 * Adresse einer RAM-Semaphore ist ihr Ereignis) nicht haengen bleiben.
 */
#include "kern.h"
#include "ifs.h"
#include "mem.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4: Hilfen ueber Thunks    */

#define NSEM        32
#define SLICE       20                  /* Takte je Wartestueck           */
#define EV_SYS(i)   (0x53454D00UL + (i))
#define EV_MUX      0x53454DFFUL
#define FOREVER     0xFFFFFFFFUL

extern int current;
unsigned __far kf_proc_run(unsigned long id);
unsigned __far kf_user_ok(unsigned sel, unsigned off, unsigned n, int wr);
unsigned __far kf_sig_waiting(void);

struct ksem {
    unsigned char used, set, excl, died;
    signed char   owner;                /* Task, -1 = keiner              */
    unsigned char gen;
    unsigned      cnt;                  /* REQUEST des Besitzers          */
    unsigned char opens[NTASK];
    char          name[FAPI_PATHLEN];
};

static unsigned sem_sel;                /* Tabelle, 0 = noch keine        */

#define K(i)    ((struct ksem __far *)MK_FP(sem_sel, (i) * sizeof(struct ksem)))

unsigned sdiv(unsigned lo, unsigned hi, unsigned d);    /* hi < d         */
#pragma aux sdiv = "div bx" __parm [__ax] [__dx] [__bx] __value [__ax] __modify [__dx];
unsigned smod(unsigned lo, unsigned hi, unsigned d);
#pragma aux smod = "div bx" __parm [__ax] [__dx] [__bx] __value [__dx] __modify [__ax];

/* ms -> Takte (10 ms), aufgerundet; FOREVER bleibt */
static unsigned long ms_ticks(unsigned long ms)
{
    unsigned hi, r;
    if (ms == FOREVER)
        return FOREVER;
    if (ms > 0xFFFFFFF0UL)
        ms = 0xFFFFFFF0UL;
    ms += 9;
    hi = sdiv((unsigned)(ms >> 16), 0, 10);
    r = smod((unsigned)(ms >> 16), 0, 10);
    return ((unsigned long)hi << 16) | sdiv((unsigned)ms, r, 10);
}

unsigned long __far ipc_ticks(unsigned long ms) { return ms_ticks(ms); }

/* ---- Zugriff ------------------------------------------------------------ */
/* Kennung pruefen: 1 = RAM (*rp gesetzt), 2 = System (*ip), 0 = ungueltig */
static int look(unsigned long h, unsigned __far **rp, int *ip)
{
    unsigned sel = (unsigned)(h >> 16), off = (unsigned)h;
    struct ksem __far *k;
    int i;

    if (sel) {
        if (!(sel & 4) || !kf_user_ok(sel, off, 4, 1))
            return 0;
        *rp = (unsigned __far *)MK_FP(sel, off);
        return 1;
    }
    i = (int)(off & 0xFF) - 1;
    if (!sem_sel || i < 0 || i >= NSEM)
        return 0;
    k = K(i);
    if (!k->used || k->gen != (unsigned char)(off >> 8) || !k->opens[current])
        return 0;
    *ip = i;
    return 2;
}

/* Ereignis einer RAM-Semaphore: lineare Adresse (mit Kernel-Alias, also
   oberstes Bit gesetzt - trifft keine andere Kennung) */
static unsigned long ram_ev(unsigned long h)
{
    struct desc __far *d = LDT(current, (unsigned)(h >> 16) >> 3);
    return (d->base_lo | ((unsigned long)d->base_hi << 16) |
            ((unsigned long)(d->rsv & 0xFF00) << 16)) + (unsigned)h;
}

static void wake(unsigned long ev)
{
    kf_proc_run(ev);
    kf_proc_run(EV_MUX);
}

static void sys_free(int i)
{
    struct ksem __far *k = K(i);
    k->owner = -1;
    k->cnt = 0;
    k->set = 0;
    wake(EV_SYS(i));
}

static unsigned do_set(unsigned long h)
{
    unsigned __far *rp;
    struct ksem __far *k;
    int i;

    switch (look(h, &rp, &i)) {
    case 1:
        *rp = 1;
        return 0;
    case 2:
        k = K(i);
        if (k->excl && k->owner >= 0 && k->owner != current)
            return FAPI_ERR_DENIED;
        k->set = 1;
        return 0;
    }
    return FAPI_ERROR;
}

static unsigned do_clear(unsigned long h)
{
    unsigned __far *rp;
    struct ksem __far *k;
    int i;

    switch (look(h, &rp, &i)) {
    case 1:
        *rp = 0;
        wake(ram_ev(h));
        return 0;
    case 2:
        k = K(i);
        if (k->owner >= 0 && k->owner != current && k->excl)
            return FAPI_ERR_DENIED;
        if (k->owner == current && k->cnt > 1) {
            k->cnt--;
            return 0;
        }
        sys_free(i);
        return 0;
    }
    return FAPI_ERROR;
}

/* Warten, bis die Semaphore frei ist; take: danach nehmen (REQUEST).
   -> 0, 1 (genommen, Vorbesitzer beendet), FAPI_ERR_TIMEOUT, _INTR, _ERROR */
static unsigned wait_free(unsigned long h, unsigned long ms, int take)
{
    unsigned long left = ms_ticks(ms), t0, el, ev;
    unsigned __far *rp;
    struct ksem __far *k;
    unsigned f, rc;
    int i, kind;

    for (;;) {
        f = irq_off();
        kind = look(h, &rp, &i);
        if (!kind) {
            irq_restore(f);
            return FAPI_ERROR;
        }
        if (kind == 1) {
            if (!*rp) {
                if (take)
                    *rp = 1;
                irq_restore(f);
                return 0;
            }
            ev = ram_ev(h);
        } else {
            k = K(i);
            if (take && k->owner == current) {
                k->cnt++;
                irq_restore(f);
                return 0;
            }
            if (!k->set) {
                rc = 0;
                if (take) {
                    k->set = 1;
                    k->owner = (signed char)current;
                    k->cnt = 1;
                    rc = k->died;
                    k->died = 0;
                }
                irq_restore(f);
                return rc;
            }
            ev = EV_SYS(i);
        }
        if (!left) {
            irq_restore(f);
            return FAPI_ERR_TIMEOUT;
        }
        if (kf_sig_waiting()) {
            irq_restore(f);
            return FAPI_ERR_INTR;
        }
        t0 = get_ticks();
        proc_block(ev, left > SLICE ? SLICE : (unsigned)left);  /* -> STI */
        if (left != FOREVER) {
            el = get_ticks() - t0;
            left = el >= left ? 0 : left - el;
        }
    }
}

static unsigned mux_wait(struct fapi_muxsem __far *m)
{
    unsigned long left = ms_ticks(m->ms), t0, el;
    unsigned __far *rp;
    unsigned f, n = m->n, j;
    int i, kind;

    if (!n || n > FAPI_MUXSEM_MAX)
        return FAPI_ERROR;
    for (;;) {
        f = irq_off();
        for (j = 0; j < n; j++) {
            kind = look(m->sem[j], &rp, &i);
            if (!kind) {
                irq_restore(f);
                return FAPI_ERROR;
            }
            if (kind == 1 ? !*rp : !K(i)->set) {
                irq_restore(f);
                m->index = j;
                return 0;
            }
        }
        if (!left) {
            irq_restore(f);
            return FAPI_ERR_TIMEOUT;
        }
        if (kf_sig_waiting()) {
            irq_restore(f);
            return FAPI_ERR_INTR;
        }
        t0 = get_ticks();
        proc_block(EV_MUX, left > SLICE ? SLICE : (unsigned)left);
        if (left != FOREVER) {
            el = get_ticks() - t0;
            left = el >= left ? 0 : left - el;
        }
    }
}

/* ---- Systemsemaphoren: Namen -------------------------------------------- */
/* Name aus dem Programm nach nm (gross, / -> \), muss mit pre beginnen
   und dahinter weitergehen (auch fuer shrseg.c) */
int __far ipc_name(unsigned usel, unsigned off, char *nm, const char __far *pre, unsigned pl)
{
    unsigned i;
    char c;

    for (i = 0; i < FAPI_PATHLEN; i++) {
        if (!kf_user_ok(usel, off + i, 1, 0) || off + i < off)
            return -1;
        c = *(char __far *)MK_FP(usel, off + i);
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == '/')
            c = '\\';
        nm[i] = c;
        if (!c)
            break;
        if (i < pl && c != pre[i])
            return -1;
    }
    return i > pl && i < FAPI_PATHLEN ? 0 : -1;
}

static int get_name(unsigned usel, unsigned off, char *nm)
{
    static const char pre[] = "\\SEM\\";
    return ipc_name(usel, off, nm, pre, 5);
}

static int find(const char *nm)
{
    struct ksem __far *k;
    unsigned j;
    int i;

    for (i = 0; sem_sel && i < NSEM; i++) {
        k = K(i);
        if (!k->used)
            continue;
        for (j = 0; nm[j] && nm[j] == k->name[j]; j++)
            ;
        if (!nm[j] && !k->name[j])
            return i;
    }
    return -1;
}

static char sm_name[FAPI_PATHLEN];      /* nur zwischen Schlafpunkten     */

static unsigned sys_open(unsigned usel, unsigned noff, struct fapi_sem __far *p, int create)
{
    struct ksem __far *k;
    unsigned j;
    int i;

    if (get_name(usel, noff, sm_name))
        return FAPI_ERROR;
    i = find(sm_name);
    if (create) {
        if (i >= 0)
            return FAPI_ERR_EXIST;
        if (!sem_sel) {
            sem_sel = kseg_alloc((unsigned long)NSEM * sizeof(struct ksem));
            if (!sem_sel)
                return FAPI_ERR_FULL;
            zero_seg(sem_sel, NSEM * sizeof(struct ksem) / 2);
        }
        for (i = 0; i < NSEM && K(i)->used; i++)
            ;
        if (i == NSEM)
            return FAPI_ERR_FULL;
        k = K(i);
        k->used = 1;
        k->set = k->died = 0;
        k->excl = (unsigned char)(p->ms & SEM_EXCL);
        k->owner = -1;
        k->cnt = 0;
        if (!++k->gen)
            k->gen = 1;
        for (j = 0; j < NTASK; j++)
            k->opens[j] = 0;
        for (j = 0; (k->name[j] = sm_name[j]) != 0; j++)
            ;
    } else if (i < 0)
        return FAPI_ERROR;
    k = K(i);
    if (k->opens[current] == 255)
        return FAPI_ERR_FULL;
    k->opens[current]++;
    p->sem = ((unsigned)k->gen << 8) | (unsigned)(i + 1);
    return 0;
}

/* letzter Verweis weg -> Platz frei */
static void sys_unref(int i)
{
    struct ksem __far *k = K(i);
    unsigned j;
    for (j = 0; j < NTASK; j++)
        if (k->opens[j])
            return;
    k->used = 0;
    wake(EV_SYS(i));                    /* Wartende: Kennung ungueltig    */
}

static unsigned sys_close(unsigned long h)
{
    unsigned __far *rp;
    struct ksem __far *k;
    int i;

    if (look(h, &rp, &i) != 2)
        return FAPI_ERROR;
    k = K(i);
    if (k->owner == current && k->opens[current] == 1)
        return FAPI_ERR_INUSE;          /* erst freigeben (wie OS/2)      */
    k->opens[current]--;
    sys_unref(i);
    return 0;
}

void __far sem_task_end(int t)
{
    struct ksem __far *k;
    int i;

    if (!sem_sel || t < 0 || t >= NTASK)
        return;
    for (i = 0; i < NSEM; i++) {
        k = K(i);
        if (!k->used)
            continue;
        if (k->owner == t) {
            k->died = 1;
            sys_free(i);
        }
        if (k->opens[t]) {
            k->opens[t] = 0;
            sys_unref(i);
        }
    }
}

/* FAPI_SEM: a1 = SEM_*, a2 = struct fapi_sem bzw. fapi_muxsem, a3 = Name */
unsigned __far sem_fapi(unsigned usel, unsigned a1, unsigned a2, unsigned a3)
{
    struct fapi_sem __far *p = (struct fapi_sem __far *)MK_FP(usel, a2);

    if (a1 == SEM_MUXWAIT) {
        if (!kf_user_ok(usel, a2, sizeof(struct fapi_muxsem), 1))
            return FAPI_ERROR;
        return mux_wait((struct fapi_muxsem __far *)MK_FP(usel, a2));
    }
    if (!kf_user_ok(usel, a2, sizeof(*p), 1))
        return FAPI_ERROR;
    switch (a1) {
    case SEM_CREATE:    return sys_open(usel, a3, p, 1);
    case SEM_OPEN:      return sys_open(usel, a3, p, 0);
    case SEM_CLOSE:     return sys_close(p->sem);
    case SEM_SET:       return do_set(p->sem);
    case SEM_CLEAR:     return do_clear(p->sem);
    case SEM_WAIT:      return wait_free(p->sem, p->ms, 0);
    case SEM_REQUEST:   return wait_free(p->sem, p->ms, 1);
    case SEM_SETWAIT: {
        unsigned r = do_set(p->sem);
        return r ? r : wait_free(p->sem, p->ms, 0);
    }
    }
    return FAPI_ERROR;
}
