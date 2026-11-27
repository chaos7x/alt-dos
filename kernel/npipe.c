/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.5 - Named Pipes wie OS/2 1.x (KTEXT4, FAPI_NPIPE 6Ah,
 * docs/FAPI-IPC.md Stufe 4).
 *
 * Der Diener legt mit MAKE Instanzen von \PIPE\name an (je ein Handle) und
 * wartet mit CONNECT auf einen Kunden; der Kunde oeffnet den Namen mit
 * DosOpen (kernel.c FAPI_OPEN -> np_open). Danach DosRead/DosWrite/
 * DosClose auf beiden Seiten (OF_NPIPE). Je Instanz zwei Ringe zu NPBUF
 * Byte (Kunde -> Diener, Diener -> Kunde). Nachrichtenbetrieb (NP_MSG):
 * jedes Schreiben ist eine Nachricht (Laengenwort im Ring), ein Lesen
 * liefert hoechstens eine.
 *
 * Zustand: NPS_DISC (angelegt bzw. getrennt), NPS_LISTEN (CONNECT laeuft
 * oder lief), NPS_CONN. Schliesst eine Seite, liest die andere den Rest
 * und dann 0 Byte; Schreiben scheitert. Der Diener macht die Instanz mit
 * DISCONNECT wieder frei - ein Handle, das der alte Kunde noch haelt, ist
 * dann tot (Generation im Handle).
 *
 * Ohne Kernelsperre (fapi_nolock; np_open laeuft unter der Sperre von
 * FAPI_OPEN, schlaeft aber nicht): der Kernel wird nie unterbrochen, nach
 * jedem proc_block wird neu geprueft. Warten in Stuecken wie sem.c.
 */
#include "kern.h"
#include "ifs.h"
#include "mem.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4: Hilfen ueber Thunks    */

#define NNP         8
#define NPBUF       2048
#define SLICE       20
#define EV_NP(i)    (0x4E500000UL + (i))
#define EV_NPW      0x4E5000FFUL        /* WAIT/CALL: eine Instanz lauscht */
#define FOREVER     0xFFFFFFFFUL

extern int current;
unsigned __far kf_proc_run(unsigned long id);
unsigned __far kf_user_ok(unsigned sel, unsigned off, unsigned n, int wr);
unsigned __far kf_sig_waiting(void);
unsigned __far kf_np_new(unsigned first, unsigned side);            /* kernel.c */
int __far kf_np_get(unsigned h, unsigned *first, unsigned *side);
int __far ipc_name(unsigned usel, unsigned off, char *nm, const char __far *pre, unsigned pl);
unsigned long __far ipc_ticks(unsigned long ms);                    /* sem.c    */

struct ring { unsigned head, cnt, rem; };   /* rem: Rest der Nachricht   */
struct knp {
    unsigned char used, state, msg, gen;    /* gen: Kunde                 */
    unsigned char srv, cl;                  /* Seite haelt ihr Ende       */
    signed char   cltask;                   /* CALL ohne Handle: Task     */
    unsigned char rsv;
    struct ring   r[2];                     /* 0: zum Diener, 1: zum Kunden */
    char          name[FAPI_PATHLEN];
};

static unsigned np_sel;                 /* Tabelle + Puffer, 0 = keine    */
static char np_name[FAPI_PATHLEN];      /* nur zwischen Schlafpunkten     */
static const char np_pre[] = "\\PIPE\\";

#define P(i)        ((struct knp __far *)MK_FP(np_sel, (i) * sizeof(struct knp)))
#define BOFF(i, d)  (NNP * sizeof(struct knp) + ((i) * 2 + (d)) * NPBUF)

static void wake(int i)
{
    kf_proc_run(EV_NP(i));
}

/* ein Wartestueck; -> 0 weiter pruefen, sonst Fehler */
static unsigned nap(unsigned long ev, unsigned long *left)
{
    unsigned long t0, el;

    if (!*left)
        return FAPI_ERR_TIMEOUT;
    if (kf_sig_waiting())
        return FAPI_ERR_INTR;
    t0 = get_ticks();
    irq_off();
    proc_block(ev, *left > SLICE ? SLICE : (unsigned)*left);    /* -> STI */
    if (*left != FOREVER) {
        el = get_ticks() - t0;
        *left = el >= *left ? 0 : *left - el;
    }
    return 0;
}

/* ---- Ringe -------------------------------------------------------------- */
static void rg_put(int i, int d, unsigned ssel, unsigned soff, unsigned n)
{
    struct ring __far *r = &P(i)->r[d];
    unsigned t = (r->head + r->cnt) % NPBUF, k = NPBUF - t;

    if (k > n)
        k = n;
    far_copy(np_sel, BOFF(i, d) + t, ssel, soff, k);
    if (n > k)
        far_copy(np_sel, BOFF(i, d), ssel, soff + k, n - k);
    r->cnt += n;
}

/* n Byte ab Stelle skip hinter head nach dsel:doff, ohne zu entnehmen */
static void rg_peek(int i, int d, unsigned skip, unsigned dsel, unsigned doff, unsigned n)
{
    unsigned h = (P(i)->r[d].head + skip) % NPBUF, k = NPBUF - h;

    if (k > n)
        k = n;
    far_copy(dsel, doff, np_sel, BOFF(i, d) + h, k);
    if (n > k)
        far_copy(dsel, doff + k, np_sel, BOFF(i, d), n - k);
}

static void rg_drop(int i, int d, unsigned n)
{
    struct ring __far *r = &P(i)->r[d];
    r->head = (r->head + n) % NPBUF;
    r->cnt -= n;
}

/* ---- Instanzen ---------------------------------------------------------- */
static void np_free(int i)
{
    P(i)->used = 0;
    wake(i);
    kf_proc_run(EV_NPW);
}

static void np_reset(int i)
{
    struct knp __far *p = P(i);
    p->r[0].head = p->r[0].cnt = p->r[0].rem = 0;
    p->r[1].head = p->r[1].cnt = p->r[1].rem = 0;
    p->cl = 0;
    p->cltask = -1;
    p->gen++;
}

/* Kennung aus dem Dateieintrag (Platz | Generation << 8) pruefen */
static int np_valid(unsigned first, unsigned side)
{
    int i = (int)(first & 0xFF);
    if (!np_sel || i >= NNP || !P(i)->used)
        return -1;
    if (side && (P(i)->gen != (unsigned char)(first >> 8) || !P(i)->cl))
        return -1;                      /* vom Diener getrennt            */
    return i;
}

static int same_name(int i, const char *nm)
{
    struct knp __far *p = P(i);
    unsigned j;
    for (j = 0; nm[j] && nm[j] == p->name[j]; j++)
        ;
    return !nm[j] && !p->name[j];
}

/* lauschende Instanz mit dem Namen: -> Platz, -1 = keine lauscht, -2 =
   den Namen gibt es nicht */
static int np_listening(const char *nm)
{
    int i, r = -2;
    for (i = 0; np_sel && i < NNP; i++)
        if (P(i)->used && same_name(i, nm)) {
            if (P(i)->state == NPS_LISTEN)
                return i;
            r = -1;
        }
    return r;
}

static unsigned np_attach(int i)        /* Kunde haengt an -> Kennung     */
{
    struct knp __far *p = P(i);
    p->state = NPS_CONN;
    p->cl = 1;
    wake(i);
    return (unsigned)i | ((unsigned)p->gen << 8);
}

/* FAPI_OPEN auf \PIPE\name (kernel.c, unter der Kernelsperre; schlaeft
   nicht). buf = Name wie vom Programm. -> Handle, FAPI_ERROR,
   FAPI_ERR_INUSE (alle Instanzen besetzt), FFFEh = kein Pipe-Name */
unsigned __far np_open(const char *buf)
{
    unsigned j, h, first;
    char c;
    int i;

    for (j = 0; j < 6; j++) {
        c = buf[j];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == '/')
            c = '\\';
        if (c != np_pre[j])
            return 0xFFFE;
    }
    for (j = 0; j < FAPI_PATHLEN - 1 && buf[j]; j++) {
        c = buf[j];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        np_name[j] = c == '/' ? '\\' : c;
    }
    np_name[j] = 0;
    i = np_listening(np_name);
    if (i < 0)
        return i == -1 ? FAPI_ERR_INUSE : FAPI_ERROR;
    first = (unsigned)i | ((unsigned)P(i)->gen << 8);
    h = kf_np_new(first, 1);
    if (h == 0xFFFF)
        return FAPI_ERROR;
    np_attach(i);
    return h;
}

/* letzter Verweis auf ein Ende weg (kernel.c of_release) bzw. CALL fertig */
void __far np_close(unsigned first, unsigned side)
{
    struct knp __far *p;
    int i = np_valid(first, side);

    if (i < 0)
        return;
    p = P(i);
    if (side) {
        p->cl = 0;
        p->cltask = -1;
        if (!p->srv) {
            np_free(i);
            return;
        }
    } else {
        p->srv = 0;
        if (!p->cl) {
            np_free(i);
            return;
        }
    }
    wake(i);
}

void __far np_task_end(int t)
{
    int i;
    for (i = 0; np_sel && i < NNP; i++)
        if (P(i)->used && P(i)->cl && P(i)->cltask == t)
            np_close((unsigned)i | ((unsigned)P(i)->gen << 8), 1);
}

/* ---- Lesen und Schreiben ------------------------------------------------ */
static unsigned np_write(unsigned first, unsigned side, unsigned usel, unsigned off, unsigned n)
{
    unsigned long left = FOREVER;
    struct knp __far *p;
    unsigned done = 0, k, r;
    int i, d = side ? 0 : 1;

    if (!n)
        return 0;
    if (!kf_user_ok(usel, off, n, 0))
        return FAPI_ERROR;
    for (;;) {
        if ((i = np_valid(first, side)) < 0)
            return FAPI_ERROR;
        p = P(i);
        if (p->state != NPS_CONN || !(side ? p->srv : p->cl))
            return FAPI_ERROR;          /* niemand liest mehr             */
        k = NPBUF - p->r[d].cnt;
        if (p->msg) {
            if (n > NPBUF - 2)
                return FAPI_ERROR;
            if (k >= n + 2) {
                rg_put(i, d, KDATA, (unsigned)&n, 2);
                rg_put(i, d, usel, off, n);
                wake(i);
                return n;
            }
        } else if (k) {
            if (k > n - done)
                k = n - done;
            rg_put(i, d, usel, off + done, k);
            wake(i);
            if ((done += k) == n)
                return n;
            continue;
        }
        if ((r = nap(EV_NP(i), &left)) != 0)
            return done ? done : r;
    }
}

/* peek: nichts entnehmen, nicht warten; *ml = Rest der Nachricht */
static unsigned np_read(unsigned first, unsigned side, unsigned usel, unsigned off,
                        unsigned n, int peek, unsigned *ml)
{
    unsigned long left = FOREVER;
    struct knp __far *p;
    struct ring __far *g;
    unsigned k, r, skip;
    int i, d = side ? 1 : 0;

    if (n && !kf_user_ok(usel, off, n, 1))
        return FAPI_ERROR;
    for (;;) {
        if ((i = np_valid(first, side)) < 0)
            return FAPI_ERROR;
        p = P(i);
        if (p->state != NPS_CONN)
            return FAPI_ERROR;
        g = &p->r[d];
        if (g->cnt) {
            skip = 0;
            k = g->cnt;
            if (p->msg) {
                if (!g->rem) {
                    rg_peek(i, d, 0, KDATA, (unsigned)&k, 2);
                    if (peek)
                        skip = 2;
                    else {
                        rg_drop(i, d, 2);
                        g->rem = k;
                    }
                } else
                    k = g->rem;
            }
            if (ml)
                *ml = k;
            if (k > n)
                k = n;
            rg_peek(i, d, skip, usel, off, k);
            if (!peek) {
                rg_drop(i, d, k);
                if (p->msg)
                    g->rem -= k;
                wake(i);
            }
            return k;
        }
        if (ml)
            *ml = 0;
        if (peek || !n || !(side ? p->srv : p->cl))
            return 0;                   /* Gegenseite zu: Dateiende       */
        if ((r = nap(EV_NP(i), &left)) != 0)
            return r;
    }
}

unsigned __far np_rw(unsigned first, unsigned side, int wr, unsigned usel, unsigned off, unsigned n)
{
    return wr ? np_write(first, side, usel, off, n)
              : np_read(first, side, usel, off, n, 0, 0);
}

/* eine Nachricht hin, eine zurueck */
static unsigned np_trans(unsigned first, unsigned side, unsigned usel, struct fapi_nptrans __far *t)
{
    unsigned r;
    int i = np_valid(first, side);

    if (i < 0 || !P(i)->msg)
        return FAPI_ERROR;
    r = np_write(first, side, usel, t->wbuf, t->wn);
    if (r >= FAPI_ERR_FIRST)
        return r;
    r = np_read(first, side, usel, t->rbuf, t->rn, 0, 0);
    if (r >= FAPI_ERR_FIRST)
        return r;
    t->got = r;
    return 0;
}

/* warten, bis eine Instanz des Namens lauscht -> Platz, sonst Fehler in
   *err. np_name gilt nur zwischen Schlafpunkten: jedes Mal neu holen */
static int np_await(unsigned usel, unsigned noff, unsigned long ms, unsigned *err)
{
    unsigned long left = ipc_ticks(ms);
    int i;

    for (;;) {
        if (ipc_name(usel, noff, np_name, np_pre, 6)) {
            *err = FAPI_ERROR;
            return -1;
        }
        i = np_listening(np_name);
        if (i >= 0)
            return i;
        *err = i == -2 ? FAPI_ERROR : !ms ? FAPI_ERR_INUSE : nap(EV_NPW, &left);
        if (*err)
            return -1;
    }
}

/* FAPI_NPIPE: a1 = NPIPE_*, a2/a3 siehe fapi_abi.h */
unsigned __far np_fapi(unsigned usel, unsigned a1, unsigned a2, unsigned a3)
{
    unsigned long left = FOREVER;
    struct knp __far *p;
    unsigned first, side, r, j;
    int i;

    switch (a1) {
    case NPIPE_MAKE:                    /* a2 = Name, a3 = NP_*           */
        if (ipc_name(usel, a2, np_name, np_pre, 6))
            return FAPI_ERROR;
        if (!np_sel) {
            np_sel = kseg_alloc(BOFF(NNP, 0));
            if (!np_sel)
                return FAPI_ERR_FULL;
            zero_seg(np_sel, NNP * sizeof(struct knp) / 2);
        }
        for (i = 0; i < NNP && P(i)->used; i++)
            ;
        if (i == NNP)
            return FAPI_ERR_FULL;
        p = P(i);
        r = kf_np_new((unsigned)i, 0);
        if (r == 0xFFFF)
            return FAPI_ERR_FULL;
        p->used = 1;
        p->state = NPS_DISC;
        p->msg = (unsigned char)(a3 & NP_MSG);
        p->srv = 1;
        np_reset(i);
        for (j = 0; (p->name[j] = np_name[j]) != 0; j++)
            ;
        return r;

    case NPIPE_WAIT:                    /* a2 = Name, a3 = Frist (ms)     */
        if (!kf_user_ok(usel, a3, 4, 0))
            return FAPI_ERROR;
        r = 0;
        np_await(usel, a2, *(unsigned long __far *)MK_FP(usel, a3), &r);
        return r;

    case NPIPE_CALL: {                  /* a2 = Name, a3 = fapi_npcall    */
        struct fapi_npcall __far *c = (struct fapi_npcall __far *)MK_FP(usel, a3);
        if (!kf_user_ok(usel, a3, sizeof(*c), 1))
            return FAPI_ERROR;
        r = 0;
        if ((i = np_await(usel, a2, c->ms, &r)) < 0)
            return r;
        if (!P(i)->msg)
            return FAPI_ERROR;
        first = np_attach(i);
        P(i)->cltask = (signed char)current;
        r = np_trans(first, 1, usel, &c->t);
        np_close(first, 1);
        return r;
    }
    }

    /* alles Weitere: a2 = Handle */
    if (!kf_np_get(a2, &first, &side) || (i = np_valid(first, side)) < 0)
        return FAPI_ERROR;
    p = P(i);
    switch (a1) {
    case NPIPE_CONNECT:                 /* Diener: auf einen Kunden warten */
        if (side)
            return FAPI_ERROR;
        for (;;) {
            if ((i = np_valid(first, 0)) < 0)
                return FAPI_ERROR;
            p = P(i);
            if (p->state == NPS_CONN)
                return p->cl || p->r[0].cnt ? 0 : FAPI_ERROR;  /* Kunde schon weg */
            if (p->state == NPS_DISC) {
                p->state = NPS_LISTEN;
                kf_proc_run(EV_NPW);
            }
            if ((r = nap(EV_NP(i), &left)) != 0)
                return r;
        }

    case NPIPE_DISCONNECT:              /* Diener: Kunde ab, Instanz frei */
        if (side)
            return FAPI_ERROR;
        np_reset(i);
        p->state = NPS_DISC;
        wake(i);
        return 0;

    case NPIPE_PEEK: {                  /* a3 = struct fapi_nppeek        */
        struct fapi_nppeek __far *k = (struct fapi_nppeek __far *)MK_FP(usel, a3);
        unsigned ml = 0;
        if (!kf_user_ok(usel, a3, sizeof(*k), 1))
            return FAPI_ERROR;
        k->state = p->state == NPS_CONN && !(side ? p->srv : p->cl) ? NPS_CLOSING : p->state;
        k->got = k->avail = k->msg = 0;
        if (p->state != NPS_CONN)
            return 0;
        r = np_read(first, side, usel, k->buf, k->n, 1, &ml);
        if (r >= FAPI_ERR_FIRST)
            return r;
        k->got = r;
        k->avail = P(i)->r[side ? 1 : 0].cnt;
        k->msg = P(i)->msg ? ml : 0;
        return 0;
    }

    case NPIPE_TRANSACT:                /* a3 = struct fapi_nptrans       */
        if (!kf_user_ok(usel, a3, sizeof(struct fapi_nptrans), 1))
            return FAPI_ERROR;
        return np_trans(first, side, usel, (struct fapi_nptrans __far *)MK_FP(usel, a3));
    }
    return FAPI_ERROR;
}
