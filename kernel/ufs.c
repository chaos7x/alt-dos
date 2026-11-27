/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.6 - Dateisystem in Ring 3 (KTEXT2, aus 6.3): ein Laufwerk,
 * dessen Operationen ein Programm erledigt (Vorbild FUSE unter Linux;
 * Nutzer: SMBCLI --serve fuer NET USE).
 *
 * Das Programm meldet ein Laufwerk an (FAPI_UFS UFS_REG) und holt dann in
 * einer Schleife Auftraege (UFS_GET, wartet) und gibt Antworten (UFS_PUT).
 * Jede IFS-Operation auf dem Laufwerk (ufs_driver) legt einen Auftrag in
 * den Platz des Laufwerks - ein Kernelsegment mit Kopf (struct
 * fapi_ufsreq) und bis UFS_DATA Byte Daten -, weckt den Dienst und
 * schlaeft, bis die Antwort da ist.
 *
 * Anders als 6.3: 5.x hat nur die eine Kernelsperre, und der wartende
 * Task behaelt sie (proc_block wie im Treiber). Der Dienst braucht sie nie
 * (FAPI_UFS und FAPI_NET laufen ohne) - solange ein Auftrag laeuft,
 * warten andere Tasks mit Kernelaufrufen. Freigabemodi beim Server (6.3
 * OPEN_DENY*) gibt es hier nicht: geoeffnet wird immer wie bisher (0x80).
 */
#include "kern.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "k2.h"                         /* KTEXT2, k2.h zuletzt           */

unsigned k2_irq_restore(unsigned f);
#pragma aux k2_irq_restore = "push ax" "popf" __parm [__ax] __value [__ax];
#define irq_restore(f)  k2_irq_restore(f)
#define ifs_share       0x80

#define NUFS        4
#define US_IDLE     0
#define US_POSTED   1                   /* wartet auf UFS_GET             */
#define US_BUSY     2                   /* beim Dienst                    */
#define US_DONE     3
#define EV_CL(i)    (0x55460000UL + (i))
#define EV_SV(i)    (0x55460100UL + (i))
#define DOFF        1024                /* Daten im Segment               */
#define TMO         3000                /* 30 s                           */

unsigned __far kf_net_who(char *prog);  /* kernel.c: laufender Task   */
unsigned __far kf_proc_run(unsigned long id);
void __far kf_kseg_free(unsigned sel);
unsigned __far kf_drv_busy(int d);
int __far kf_user_ok(unsigned sel, unsigned off, unsigned n, int wr);
static char who_buf[14];
#define current         ((int)kf_net_who(who_buf))
#define kf_ufs_block(id, t)     proc_block((id), (t))   /* Sperre bleibt */

static struct uslot {
    signed char   task;                 /* Dienst, -1 = frei              */
    unsigned char drive, blk, state, quit;
    unsigned      seq, sel;
    char          name[80];
} us[NUFS] = { { -1 }, { -1 }, { -1 }, { -1 } };

#define RQ(i)   ((struct fapi_ufsreq __far *)MK_FP(us[i].sel, 0))

static int slot_of(int d)
{
    int i;
    for (i = 0; i < NUFS; i++)
        if (us[i].task >= 0 && us[i].drive == d)
            return i;
    return -1;
}

static void fstr(char __far *d, const char *s, unsigned max)
{
    unsigned i;
    for (i = 0; s && s[i] && i < max - 1; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* Auftrag vorbereiten (Kopf leeren, op, Pfad) */
static struct fapi_ufsreq __far *rq_new(int i, unsigned op, const char *path)
{
    struct fapi_ufsreq __far *r = RQ(i);
    unsigned char __far *p = (unsigned char __far *)r;
    unsigned k;
    for (k = 0; k < sizeof(*r); k++)
        p[k] = 0;
    r->op = op;
    fstr(r->path, path, sizeof(r->path));
    return r;
}

/* Auftrag abschicken und warten -> status (IFS_ERR bei Abbruch) */
static int rq_go(int i)
{
    struct uslot *u = &us[i];
    unsigned f;
    if (u->task < 0 || u->quit)
        return IFS_ERR;
    u->seq++;
    RQ(i)->seq = u->seq;
    RQ(i)->status = IFS_ERR;
    u->state = US_POSTED;
    kf_proc_run(EV_SV(i));
    for (;;) {
        f = irq_off();
        if (u->state == US_DONE || u->task < 0) {
            irq_restore(f);
            break;
        }
        if (kf_ufs_block(EV_CL(i), TMO)) {      /* Frist um              */
            u->state = US_IDLE;
            return IFS_ERR;
        }
    }
    if (u->task < 0) {
        u->state = US_IDLE;
        return IFS_ERR;
    }
    u->state = US_IDLE;
    return RQ(i)->status;
}

/* ---- IFS-Treiber -------------------------------------------------------- */
static int __far u_mount(int d)
{
    return slot_of(d) < 0 ? -1 : 0;
}

static int __far u_open(int d, const char *path, struct ifs_file __far *f)
{
    int i = slot_of(d), r;
    if (i < 0)
        return -1;
    rq_new(i, UFSOP_OPEN, path)->mask = ifs_share;
    if ((r = rq_go(i)) != 0)
        return r < 0 ? r : -1;
    f->drive = (unsigned char)d;
    f->first = RQ(i)->h;
    f->size = RQ(i)->size;
    f->attr = (unsigned char)RQ(i)->attr;
    f->cache_clus = f->cache_pos = 0;
    return 0;
}

static int __far u_create(int d, const char *path, struct ifs_file __far *f)
{
    int i = slot_of(d), r;
    if (i < 0)
        return IFS_ERR;
    rq_new(i, UFSOP_CREATE, path)->mask = ifs_share;
    fstr(RQ(i)->path2, ifs_orig, sizeof(RQ(i)->path2));
    if ((r = rq_go(i)) != 0)
        return r;
    f->drive = (unsigned char)d;
    f->first = RQ(i)->h;
    f->size = 0;
    f->attr = 0;
    return 0;
}

static int __far u_read(struct ifs_file __far *f, unsigned long pos,
                        unsigned sel, unsigned off, unsigned len)
{
    int i = slot_of(f->drive), r;
    unsigned done = 0, n, got;
    if (i < 0)
        return -1;
    while (done < len) {
        n = len - done > UFS_DATA ? UFS_DATA : len - done;
        rq_new(i, UFSOP_READ, 0);
        RQ(i)->h = f->first;
        RQ(i)->pos = pos + done;
        RQ(i)->len = n;
        if ((r = rq_go(i)) != 0)
            return done ? (int)done : (r == IFS_LOCKED ? r : -1);
        got = RQ(i)->len > n ? n : RQ(i)->len;
        if (got)
            far_copy(sel, off + done, us[i].sel, DOFF, got);
        done += got;
        if (got < n)
            break;
    }
    return (int)done;
}

static int __far u_write(struct ifs_file __far *f, unsigned long pos,
                         unsigned sel, unsigned off, unsigned len)
{
    int i = slot_of(f->drive), r;
    unsigned done = 0, n;
    if (i < 0)
        return IFS_ERR;
    while (done < len) {
        n = len - done > UFS_DATA ? UFS_DATA : len - done;
        rq_new(i, UFSOP_WRITE, 0);
        RQ(i)->h = f->first;
        RQ(i)->pos = pos + done;
        RQ(i)->len = n;
        far_copy(us[i].sel, DOFF, sel, off + done, n);
        if ((r = rq_go(i)) != 0)
            return done ? (int)done : r;
        done += RQ(i)->len > n ? n : RQ(i)->len;
        if (RQ(i)->len < n)
            break;
    }
    if (pos + done > f->size)
        f->size = pos + done;
    return (int)done;
}

static int __far u_close(struct ifs_file __far *f)
{
    int i = slot_of(f->drive);
    struct fapi_ufsreq __far *r;
    if (i < 0)
        return 0;
    r = rq_new(i, UFSOP_CLOSE, 0);
    r->h = f->first;
    /* 5.x: Datum/Attribut aus der Box gibt es nicht (kein IFS_F_TIME) */
    return rq_go(i) ? IFS_ERR : 0;
}

static int __far u_readdir(int d, const char *dir, unsigned n, struct ifs_dirent *e)
{
    int i = slot_of(d);
    if (i < 0)
        return -1;
    rq_new(i, UFSOP_READDIR, dir);
    RQ(i)->n = n;
    if (rq_go(i) != 0)
        return -1;
    far_copy(KDATA, (unsigned)e, us[i].sel, DOFF, sizeof(*e));
    return 0;
}

static int __far u_isdir(int d, const char *path)
{
    int i = slot_of(d);
    if (i < 0)
        return 0;
    if (!path[0] || (path[0] == '\\' && !path[1]))
        return 1;
    rq_new(i, UFSOP_STAT, path);
    return rq_go(i) == 0 && (RQ(i)->attr & ATTR_DIR);
}

static int simple(int d, unsigned op, const char *path, const char *p2)
{
    int i = slot_of(d);
    if (i < 0)
        return IFS_ERR;
    rq_new(i, op, path);
    if (p2)
        fstr(RQ(i)->path2, p2, sizeof(RQ(i)->path2));
    return rq_go(i);
}

static int __far u_remove(int d, const char *path) { return simple(d, UFSOP_REMOVE, path, 0); }
static int __far u_mkdir(int d, const char *path) { return simple(d, UFSOP_MKDIR, path, ifs_orig); }
static int __far u_rmdir(int d, const char *path) { return simple(d, UFSOP_RMDIR, path, 0); }

static int __far u_move(int d, const char *from, const char *to)
{
    int i = slot_of(d), r;
    unsigned k;
    if (i < 0)
        return IFS_ERR;
    rq_new(i, UFSOP_MOVE, from);
    fstr(RQ(i)->path2, to, sizeof(RQ(i)->path2));
    for (k = 0; RQ(i)->path2[k]; k++)
        ;
    if (ifs_orig[0] && k + 1 < sizeof(RQ(i)->path2))    /* Originalname dahinter */
        fstr(RQ(i)->path2 + k + 1, ifs_orig, sizeof(RQ(i)->path2) - k - 1);
    r = rq_go(i);
    return r;
}

static int __far u_space(int d, unsigned long *r)
{
    int i = slot_of(d);
    if (i < 0)
        return IFS_ERR;
    rq_new(i, UFSOP_SPACE, 0);
    if (rq_go(i))
        return IFS_ERR;
    r[0] = RQ(i)->pos;                  /* Bytes, bei 4 GB gekappt        */
    r[1] = RQ(i)->size ? RQ(i)->size : r[0] >> 10;  /* KB; alter Dienst: aus Bytes */
    return 0;
}

static int __far u_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                           unsigned date, unsigned time)
{
    int i = slot_of(d);
    struct fapi_ufsreq __far *r;
    if (i < 0)
        return IFS_ERR;
    r = rq_new(i, UFSOP_SETINFO, path);
    r->mask = mask;
    r->attr = attr;
    r->date = date;
    r->time = time;
    return rq_go(i);
}

static int __far u_label(int d, char *lbl, unsigned long *serial, const char *nl)
{
    int i = slot_of(d);
    unsigned k;
    if (serial)
        *serial = 0;
    if (i < 0 || nl)
        return IFS_ERR;
    rq_new(i, UFSOP_LABEL, 0);
    if (rq_go(i))
        return IFS_ERR;
    far_copy(KDATA, (unsigned)lbl, us[i].sel, DOFF, 12);
    for (k = 0; k < 12 && lbl[k]; k++)
        ;
    lbl[k < 12 ? k : 11] = 0;
    return 0;
}

static int __far u_namepath(int d, const char *path, char *out, int shortn)
{
    int i = slot_of(d);
    if (i < 0)
        return -1;
    rq_new(i, UFSOP_NAMEPATH, path);
    RQ(i)->shortn = (unsigned)shortn;
    if (rq_go(i))
        return -1;
    far_copy(KDATA, (unsigned)out, us[i].sel, DOFF, IFS_PATHLEN);
    out[IFS_PATHLEN - 1] = 0;
    return 0;
}

static int __far u_trunc(struct ifs_file __far *f, unsigned long size)
{
    int i = slot_of(f->drive);
    if (i < 0)
        return IFS_ERR;
    rq_new(i, UFSOP_TRUNC, 0);
    RQ(i)->h = f->first;
    RQ(i)->size = size;
    if (rq_go(i))
        return IFS_ERR;
    f->size = size;
    return 0;
}


struct ifs_driver ufs_driver = {
    "SMB", u_mount, u_open, u_read, u_readdir, u_isdir, u_create, u_write,
    u_close, u_remove, u_mkdir, u_rmdir, u_move, u_space, u_setinfo, u_label,
    u_namepath, u_trunc
};

/* ---- Netzdrucker (LPTn umgeleitet) -------------------------------------- */
/* LPTn (0-2) an einen Dienst umgeleitet? -> 1 */
int __far ufs_lpt(int n)
{
    return slot_of(UFS_LPT + n) >= 0;
}

/* Platz fuer einen Auftrag: andere Handles auf demselben Drucker warten,
   bis der laufende Auftrag beantwortet ist -> Platz oder -1 */
static int lpt_slot(int n)
{
    int i;
    while ((i = slot_of(UFS_LPT + n)) >= 0 && us[i].state != US_IDLE)
        kf_ufs_block(EV_CL(i), 5);
    return i;
}

/* Daten des Handles key (Dateitabelle + 1) an den Drucker -> Bytes, -1 */
int __far ufs_lpt_write(int n, unsigned key, unsigned sel, unsigned off, unsigned len)
{
    unsigned done = 0, k;
    int i;
    while (done < len) {
        if ((i = lpt_slot(n)) < 0)
            return done ? (int)done : -1;
        k = len - done > UFS_DATA ? UFS_DATA : len - done;
        rq_new(i, UFSOP_WRITE, 0);
        RQ(i)->h = key;
        RQ(i)->len = k;
        far_copy(us[i].sel, DOFF, sel, off + done, k);
        if (rq_go(i) != 0)
            return done ? (int)done : -1;
        done += RQ(i)->len > k ? k : RQ(i)->len;
        if (RQ(i)->len < k)
            break;
    }
    return (int)done;
}

/* Handle key geschlossen: Auftrag beim Server beenden */
void __far ufs_lpt_close(int n, unsigned key)
{
    int i = lpt_slot(n);
    if (i < 0)
        return;
    rq_new(i, UFSOP_CLOSE, 0)->h = key;
    rq_go(i);
}

/* ---- FAPI_UFS (Dienst und NET USE) -------------------------------------- */
static int free_blk(void)
{
    int b;
    for (b = 2; b < nblk && blk[b].bsize; b++)
        ;
    if (b >= NBLK)
        return -1;
    if (b == nblk)
        nblk++;
    blk[b].drv = DRV_UFS;
    blk[b].bsize = 512;
    blk[b].blocks = 0;                  /* blk_read/blk_write: Fehler     */
    blk[b].rem = 0;
    return b;
}

static void slot_free(int i)
{
    struct uslot *u = &us[i];
    unsigned f;
    if (u->task < 0)
        return;
    if (!(u->drive & UFS_LPT)) {
        if (vol[u->drive].drv == &ufs_driver)
            vol_drop(u->drive);
        blk[u->blk].bsize = 0;
        blk[u->blk].drv = NDRV;
    }
    if (u->sel)
        kf_kseg_free(u->sel);
    f = irq_off();
    u->task = -1;
    u->sel = 0;
    irq_restore(f);
    kf_proc_run(EV_CL(i));              /* wartender Auftrag scheitert   */
}

/* task_end (kernel.c): Dienst weg -> Laufwerk weg */
void __far ufs_task_end(int t)
{
    int i;
    for (i = 0; i < NUFS; i++)
        if (us[i].task == t)
            slot_free(i);
}

unsigned __far ufs_fapi(unsigned usel, unsigned a1, unsigned a2, unsigned a3)
{
    static struct fapi_ufsreg rg;
    struct uslot *u;
    int i, d, b;
    unsigned f, n, tmo;
    unsigned long t0 = 0;

    switch (a1) {
    case UFS_REG:
        if (!kf_user_ok(usel, a2, sizeof(rg), 0))
            return FAPI_ERROR;
        far_copy(KDATA, (unsigned)&rg, usel, a2, sizeof(rg));
        d = rg.drive;
        if (d >= UFS_LPT ? d > UFS_LPT + 2 || slot_of(d) >= 0 :
            d < 2 || d >= NDRIVE || vol[d].used || vol[d].present || slot_of(d) >= 0)
            return FAPI_ERR_EXIST;
        for (i = 0; i < NUFS && us[i].task >= 0; i++)
            ;
        if (i == NUFS)
            return FAPI_ERR_FULL;
        u = &us[i];
        if (d >= UFS_LPT) {                 /* Drucker: kein Laufwerk     */
            if (!(u->sel = kseg_alloc(DOFF + UFS_DATA)))
                return FAPI_ERR_FULL;
            u->blk = 0;
            u->drive = (unsigned char)d;
            u->state = US_IDLE;
            u->quit = 0;
            u->seq = 0;
            for (n = 0; n < sizeof(u->name) - 1 && rg.name[n]; n++)
                u->name[n] = rg.name[n];
            u->name[n] = 0;
            u->task = (signed char)current;
            return 0;
        }
        if ((b = free_blk()) < 0)
            return FAPI_ERR_FULL;
        if (!(u->sel = kseg_alloc(DOFF + UFS_DATA))) {
            blk[b].bsize = 0;
            blk[b].drv = NDRV;
            return FAPI_ERR_FULL;
        }
        u->blk = (unsigned char)b;
        u->drive = (unsigned char)d;
        u->state = US_IDLE;
        u->quit = 0;
        u->seq = 0;
        for (n = 0; n < sizeof(u->name) - 1 && rg.name[n]; n++)
            u->name[n] = rg.name[n];
        u->name[n] = 0;
        u->task = (signed char)current;
        vol[d].blk = b;
        vol[d].start = 0;
        vol[d].drv = &ufs_driver;
        vol[d].mod = -1;
        vol[d].locked = 0;
        vol[d].present = 1;
        vol[d].used = 1;
        return 0;

    case UFS_GET:                       /* Dienst holt den naechsten Auftrag */
    case UFS_GETT:                      /* ... hoechstens rq.n Sekunden   */
        for (i = 0; i < NUFS && us[i].task != current; i++)
            ;
        if (i == NUFS || !kf_user_ok(usel, a2, sizeof(struct fapi_ufsreq), 1) ||
            !kf_user_ok(usel, a3, UFS_DATA, 1))
            return FAPI_ERROR;
        u = &us[i];
        tmo = 0;
        if (a1 == UFS_GETT) {
            far_copy(KDATA, (unsigned)&n, usel,
                     a2 + (unsigned)&((struct fapi_ufsreq *)0)->n, 2);
            tmo = (n ? (n > 600 ? 600 : n) : 1) * 100U;
            t0 = get_ticks();
        }
        for (;;) {
            f = irq_off();
            if (u->quit) {
                irq_restore(f);
                rq_new(i, UFSOP_QUIT, 0);
                far_copy(usel, a2, u->sel, 0, sizeof(struct fapi_ufsreq));
                return 0;
            }
            if (u->state == US_POSTED) {
                u->state = US_BUSY;
                irq_restore(f);
                break;
            }
            if (!tmo)
                proc_block(EV_SV(i), 0);    /* kehrt mit STI zurueck      */
            else if (get_ticks() - t0 >= tmo ||
                     proc_block(EV_SV(i), tmo - (unsigned)(get_ticks() - t0))) {
                f = irq_off();
                if (u->state == US_POSTED || u->quit) {
                    irq_restore(f);
                    continue;               /* doch noch etwas gekommen   */
                }
                irq_restore(f);
                rq_new(i, UFSOP_IDLE, 0);
                far_copy(usel, a2, u->sel, 0, sizeof(struct fapi_ufsreq));
                return 0;
            }
        }
        far_copy(usel, a2, u->sel, 0, sizeof(struct fapi_ufsreq));
        if (RQ(i)->op == UFSOP_WRITE)
            far_copy(usel, a3, u->sel, DOFF, RQ(i)->len);
        return 0;

    case UFS_PUT:                       /* Antwort                        */
        for (i = 0; i < NUFS && us[i].task != current; i++)
            ;
        if (i == NUFS || !kf_user_ok(usel, a2, sizeof(struct fapi_ufsreq), 0))
            return FAPI_ERROR;
        u = &us[i];
        if (u->state != US_BUSY)
            return FAPI_ERROR;
        {
            struct fapi_ufsreq __far *r = RQ(i);
            static struct fapi_ufsreq a;
            far_copy(KDATA, (unsigned)&a, usel, a2, sizeof(a));
            if (a.seq != u->seq)
                return FAPI_ERROR;      /* veraltet (Frist war um)        */
            n = a.len > UFS_DATA ? UFS_DATA : a.len;
            if (n && kf_user_ok(usel, a3, n, 0))
                far_copy(u->sel, DOFF, usel, a3, n);
            r->status = a.status;
            r->len = n;
            r->h = a.h;
            r->pos = a.pos;
            r->size = a.size;
            r->attr = a.attr;
        }
        f = irq_off();
        u->state = US_DONE;
        irq_restore(f);
        kf_proc_run(EV_CL(i));
        return 0;

    case UFS_DEL:
        if ((i = slot_of(a2)) < 0)
            return FAPI_ERROR;
        if (a2 < UFS_LPT && kf_drv_busy(a2))
            return FAPI_ERR_INUSE;
        us[i].quit = 1;
        kf_proc_run(EV_SV(i));
        return 0;

    case UFS_INFO:
        if ((i = slot_of(a2)) < 0 || !kf_user_ok(usel, a3, sizeof(rg), 1))
            return FAPI_ERROR;
        rg.drive = a2;
        rg.task = (unsigned)us[i].task;
        for (n = 0; n < sizeof(rg.name); n++)
            rg.name[n] = us[i].name[n];
        far_copy(usel, a3, KDATA, (unsigned)&rg, sizeof(rg));
        return 0;
    }
    return FAPI_ERROR;
}
