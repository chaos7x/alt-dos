/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.1 - Loop-Laufwerke: eine Datei als Blockgeraet (KTEXT2)
 *
 * Stufe 1 lesend, Stufe 2 auch schreibend (docs/LOOP-DBLSPACE.md):
 * Abbilder mit 512er-Bloecken auf einem FAT-Traeger sind beschreibbar,
 * ausser mit /RO, schreibgeschuetzter Datei, ISO oder CD als Traeger.
 * Beim Einhaengen werden die
 * Plattenbereiche der Datei einmal ermittelt (ifs_bmap, Extent-Liste);
 * danach liest das Loop-Geraet direkt Sektoren des Traegers - ueber
 * blk_read/blk_write und damit ueber dessen Platten-Cache (eben
 * geschriebene Daten liegen dort oft noch verzoegert; Schreiben des Loop-
 * Geraets wird dort ebenso verzoegert). Keine verschachtelten Dateisystem-
 * aufrufe. Die Datei haelt ein Eintrag der Dateitabelle offen (kernel.c
 * loop_glue): wer sie schreiben, loeschen oder verschieben will, bekommt
 * "in Benutzung"; CHKDSK /F, DEFRAG, CONVERT sperren nur ohne offene
 * Dateien.
 *
 * Je Loop ein Kernelsegment: Kopf (struct lhdr), dahinter die Bereiche
 * zu 8 Byte (Stelle in der Datei, Sektor auf dem Traeger, beides in 512-
 * Byte-Einheiten), hoechstens 8.188. Blockgeraet: blk[].drv = DRV_LOOP,
 * blk[].unit = Platz.
 */
#include "kern.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "k2.h"

#define LH_EXT  32                      /* Bereiche ab hier              */
#define LMAXEXT ((0x10000UL - LH_EXT) / 8)
struct lhdr {
    int            dev, carrier, fk;    /* Blockgeraet, Traeger, Datei   */
    unsigned       n, last;             /* Bereiche, zuletzt getroffen   */
    unsigned char  lsh, csh;            /* 512 << lsh Loop, << csh Traeger */
    unsigned char  kind, ro;           /* LOOPK_*, schreibgeschuetzt     */
    unsigned long  units;               /* nutzbare 512er-Einheiten       */
    unsigned       dbo;                 /* DriveSpace: Stelle von struct dblh */
};

/* DoubleSpace 6.0/6.2 und DriveSpace 6.22 (CVF), nur lesend. Format nach
   der Beschreibung in dmsdos (doc/cvfinfo.doc, GPL, nur gelesen):
   Sektor 0 Superblock ("MSDBL6.0"/"MSDSP6.0"), BITFAT, MDFAT (4 Byte je
   Cluster: u c hhhh llll ? s21), Bootblock (BPB des inneren Laufwerks),
   FAT, Stamm, gepackte Cluster. Das Loop-Geraet zeigt das innere FAT-
   Laufwerk: Sektor n < Datenbeginn liegt ungepackt im CVF, dahinter wird
   der Cluster ueber die MDFAT geholt und entpackt (DS/JM). */
struct dblh {
    unsigned long  bb, fatc, rootc, mdf; /* Bootblock, FAT, Stamm, MDFAT (CVF) */
    unsigned       res;                 /* reservierte Sektoren (innen)   */
    unsigned long  rootin, din;         /* inneres Laufwerk: Stamm, Daten */
    unsigned long  cl;                  /* Cluster im Puffer, 0 = keiner  */
    int            dcl;                 /* MDFAT-Verschiebung (signed)    */
    unsigned char  spc, sh;             /* Sektoren je Cluster, log2      */
};
#define DB_OUT   48                     /* hinter dblh: Cluster entpackt  */
#define DB_IN    (DB_OUT + 8192)        /* gepackte Daten                 */
#define DB_SIZE  (DB_IN + 8192)
typedef char dblh_fits[sizeof(struct dblh) <= DB_OUT ? 1 : -1];
unsigned loop_sel[NLOOP];
unsigned loop_free;                     /* von loop_drop: Segment frei geben */
#define LH(i)    ((struct lhdr __far *)MK_FP(loop_sel[i], 0))
#define EX(i, k) ((unsigned long __far *)MK_FP(loop_sel[i], LH_EXT + (k) * 8))

/* Einheit u der Datei -> Einheit auf dem Traeger, run = Einheiten bis zum
   Ende des Bereichs; 0 = ok */
static int lmap(int i, unsigned long u, unsigned long *c, unsigned long *run)
{
    struct lhdr __far *h = LH(i);
    unsigned lo, hi, m = h->last;
    unsigned long __far *e = EX(i, m);

    if (u >= h->units || !h->n)
        return -1;
    if (m >= h->n || u < e[0] || (m + 1 < h->n && u >= e[2])) {
        lo = 0;                         /* binaer: e[lo] <= u < e[hi]    */
        hi = h->n;
        while (hi - lo > 1) {
            m = (lo + hi) >> 1;
            if (EX(i, m)[0] <= u)
                lo = m;
            else
                hi = m;
        }
        m = lo;
        h->last = m;
        e = EX(i, m);
    }
    *c = e[1] + (u - e[0]);
    *run = (m + 1 < h->n ? e[2] : h->units) - u;
    return 0;
}

/* 512er-Einheit u der Datei nach DMABUF */
static int raw(int i, unsigned long u)
{
    unsigned long c, run;
    if (lmap(i, u, &c, &run))
        return -1;
    return blk_read_n(LH(i)->carrier, c, run > 4 ? 4 : (unsigned)run);
}

/* ---- Entpacker DS und JM (eigene Umsetzung nach der Beschreibung in
   dmsdos dblspace_dec.c): Bits ab dem niedrigsten, in 16-Bit-Worten ---- */
struct dz {                             /* Bitleser (auf dem Stapel)     */
    const unsigned __far *in, __far *end;
    unsigned w0, w1, p;
};

static unsigned dz_peek(struct dz *z)
{
    return z->p ? (z->w0 >> z->p) | (z->w1 << (16 - z->p)) : z->w0;
}

static void dz_eat(struct dz *z, unsigned n)
{
    z->p += n;
    if (z->p >= 16) {
        z->p -= 16;
        z->w0 = z->w1;
        z->w1 = z->in < z->end ? *z->in++ : 0;
    }
}

static unsigned dz_bits(struct dz *z, unsigned n)   /* n <= 15 */
{
    unsigned u = dz_peek(z) & ((1u << n) - 1);
    dz_eat(z, n);
    return u;
}

static unsigned dz_len(struct dz *z)    /* Laenge wie dmsdos dblb_rdlen  */
{
    unsigned u = dz_peek(z), k;
    for (k = 0; k < 9 && !((u >> k) & 1); k++)
        ;
    if (k == 9)
        return 0;                       /* ungueltig                     */
    if (k == 8) {
        dz_eat(z, 9);
        return dz_bits(z, 8) + 258;
    }
    dz_eat(z, k + 1 + k);
    return ((u >> (k + 1)) & ((1u << k) - 1)) + (1u << k) + 2;
}

static unsigned dz_offs(struct dz *z)   /* JM-Abstand                    */
{
    unsigned u = dz_peek(z);
    if (!(u & 1)) {
        dz_eat(z, 7);
        return (u >> 1) & 63;
    }
    if (!(u & 2)) {
        dz_eat(z, 10);
        return ((u >> 2) & 255) + 64;
    }
    dz_eat(z, 14);
    return ((u >> 2) & 4095) + 320;
}

/* gepackte Daten sel:in (lin Byte) nach sel:out (lout Byte); 0 = ok */
static int dz_unpack(unsigned sel, unsigned in, unsigned lin, unsigned out, unsigned lout)
{
    unsigned char __far *o = MK_FP(sel, out);
    unsigned p = 0, u, off, len, jm;
    struct dz zz, *z = &zz;

    z->in = MK_FP(sel, in);
    z->end = z->in + ((lin + 1) >> 1);
    z->p = 0;
    z->w0 = *z->in++;
    z->w1 = *z->in++;
    u = dz_peek(z);                     /* Kennung (Reihenfolge zweier
                                           Aufrufe in einem Ausdruck ist
                                           in C nicht festgelegt)        */
    dz_eat(z, 16);
    if (u == 0x4D4A)
        jm = 1;                         /* "JM": DriveSpace 6.22         */
    else if (u == 0x5344)
        jm = 0;                         /* "DS": DoubleSpace             */
    else
        return -1;
    dz_eat(z, 16);                      /* Version                       */
    while (p < lout) {
        u = dz_peek(z);
        if (jm) {
            if (!(u & 1)) {
                dz_eat(z, 8);
                o[p++] = (unsigned char)((u >> 1) & 127);
                continue;
            }
            if (u & 2) {
                dz_eat(z, 9);
                o[p++] = (unsigned char)(((u >> 2) & 127) | 128);
                continue;
            }
            dz_eat(z, 2);
            off = dz_offs(z);
        } else {
            switch (u & 3) {
            case 1:
            case 2:
                dz_eat(z, 9);
                o[p++] = (unsigned char)(((u >> 2) & 127) | ((u & 3) == 1 ? 128 : 0));
                continue;
            case 0:
                dz_eat(z, 8);
                off = (u >> 2) & 63;
                break;
            default:
                if (u & 4) {
                    dz_eat(z, 15);
                    off = ((u >> 3) & 4095) + 320;
                } else {
                    dz_eat(z, 11);
                    off = ((u >> 3) & 255) + 64;
                }
            }
        }
        if (off == 0x113F)              /* Gleichlauf (alle 512 Byte)    */
            continue;
        len = dz_len(z);
        if (!jm)
            len--;
        if (!off || !len || off > p || len > lout - p)
            return -1;
        for (; len; len--, p++)
            o[p] = o[p - off];
    }
    return 0;
}

/* Cluster cl des inneren Laufwerks entpackt in den Puffer; 0 = ok */
static int dbl_load(int i, struct dblh __far *d, unsigned long cl)
{
    unsigned sel = loop_sel[i], base = LH(i)->dbo, k, l, h;
    unsigned long e, s, idx;
    unsigned char __far *o = MK_FP(sel, base + DB_OUT);

    d->cl = 0;
    idx = cl + (long)d->dcl;
    if (raw(i, d->mdf + (idx >> 7)))
        return -1;
    e = *(unsigned long __far *)MK_FP(DMABUF, ((unsigned)idx & 127) << 2);
    for (k = 0; k < (unsigned)d->spc << 9; k++)
        o[k] = 0;                       /* unbelegt oder kurz: Nullen    */
    if (e & 0x80000000UL) {
        l = (unsigned)(e >> 22) & 15;   /* gepackt: Sektoren - 1         */
        h = (unsigned)(e >> 26) & 15;   /* entpackt: Sektoren - 1        */
        if (h >= d->spc || l > h)
            return -1;
        s = (e & 0x1FFFFFUL) + 1;       /* erster Sektor im CVF          */
        for (k = 0; k <= l; k++) {
            if (raw(i, s + k))
                return -1;
            far_copy(sel, base + DB_IN + (k << 9), DMABUF, 0, 512);
        }
        if (e & 0x40000000UL)           /* ungepackt                     */
            far_copy(sel, base + DB_OUT, sel, base + DB_IN, (l + 1) << 9);
        else if (dz_unpack(sel, base + DB_IN, (l + 1) << 9, base + DB_OUT, (h + 1) << 9))
            return -1;
    }
    d->cl = cl;
    return 0;
}

/* Sektor lba des inneren Laufwerks nach DMABUF */
static int dbl_read(int i, unsigned long lba)
{
    struct dblh __far *d = MK_FP(loop_sel[i], LH(i)->dbo);
    unsigned long rel, cl;

    if (lba < d->res)                   /* Bootblock, reserviert         */
        return raw(i, d->bb + lba);
    if (lba < d->rootin)                /* FAT (nur eine)                */
        return raw(i, d->fatc + (lba - d->res));
    if (lba < d->din)                   /* Stamm                         */
        return raw(i, d->rootc + (lba - d->rootin));
    rel = lba - d->din;
    cl = (rel >> d->sh) + 2;
    if (d->cl != cl && dbl_load(i, d, cl))
        return -1;
    far_copy(DMABUF, 0, loop_sel[i], LH(i)->dbo + DB_OUT + (((unsigned)rel & (d->spc - 1)) << 9), 512);
    return 0;
}

/* Block lba des Loop-Geraets nach DMABUF (Offset 0); 0 = ok */
int __far loop_read(int dev, unsigned long lba)
{
    int i = blk[dev].unit, cd;
    struct lhdr __far *h;
    unsigned long u, c, run;
    unsigned k, n;

    if (i >= NLOOP || !loop_sel[i])
        return -1;
    h = LH(i);
    if (h->kind == LOOPK_DBL)
        return dbl_read(i, lba);
    cd = h->carrier;
    n = 1 << h->lsh;                    /* Einheiten je Loop-Block       */
    u = lba << h->lsh;
    if (lmap(i, u, &c, &run))
        return -1;
    if (h->csh) {                       /* Traeger CD (2048)             */
        if (n == 4)
            return (c & 3) ? -1 : blk_read(cd, c >> 2);
        if (blk_read(cd, c >> 2))
            return -1;
        if (c & 3)
            far_copy(DMABUF, 0, DMABUF, (unsigned)(c & 3) << 9, 512);
        return 0;
    }
    if (blk_read_n(cd, c, run > 4 ? 4 : (unsigned)run))    /* auch voraus */
        return -1;
    if (n == 1)
        return 0;
    /* 2048er-Block (ISO auf einer Platte) aus vier Sektoren, die ueber
       Bereichsgrenzen gehen koennen: von hinten nach vorn, jeder Sektor
       kommt bei 0 an und wird an seine Stelle kopiert */
    for (k = n; --k > 0; ) {
        if (lmap(i, u + k, &c, &run) || blk_read(cd, c))
            return -1;
        far_copy(DMABUF, k << 9, DMABUF, 0, 512);
    }
    return lmap(i, u, &c, &run) || blk_read(cd, c) ? -1 : 0;
}

/* DMABUF (Offset 0) in Block lba des Loop-Geraets; 0 = ok, -2 schreib-
   geschuetzt. Beschreibbar sind nur 512er-Geraete auf 512er-Traegern. */
int __far loop_write(int dev, unsigned long lba)
{
    int i = blk[dev].unit;
    unsigned long c, run;

    if (i >= NLOOP || !loop_sel[i])
        return -1;
    if (LH(i)->ro)
        return -2;
    if (lmap(i, lba, &c, &run))
        return -1;
    return blk_write(LH(i)->carrier, c);
}

/* Datei (offen in files[fk], f) auf Laufwerk d als Loop-Geraet anmelden
   -> Blockgeraet oder FAPI-Fehler (negativ). Eingehaengt wird danach in
   kernel.c (loop_glue). */
int __far loop_attach(int fk, struct ifs_file __far *f, int d, int ro)
{
    struct volume *v = &vol[d];
    struct lhdr __far *h;
    int i, dev, n, dbl;
    unsigned char __far *b;
    unsigned long units, e1[2];

    for (i = 0; i < NLOOP && loop_sel[i]; i++)
        ;
    for (dev = 1; dev < nblk && !(blk[dev].bsize == 0 && blk[dev].drv == NDRV); dev++)
        ;
    if (i == NLOOP || dev == NBLK)
        return (int)FAPI_ERR_FULL;
    if (f->attr & ATTR_DIR)
        return (int)FAPI_ERROR;
    if (v->drv == &fat_driver)
        n = ifs_bmap(f, 0, 0, 0);       /* erst zaehlen                  */
    else if (v->drv == &mod_driver && blk[v->blk].bsize == 2048)
        n = 1;                          /* ISO9660.IFS: am Stueck ab fsdata */
    else
        return (int)FAPI_ERR_DENIED;
    if (n < 0)
        return (int)FAPI_ERROR;
    dbl = 0;                            /* DoubleSpace/DriveSpace-CVF?   */
    if (v->drv == &fat_driver && n > 0 && ifs_bmap(f, KDATA, (unsigned)e1, 1) > 0 &&
        blk_read(v->blk, e1[1]) == 0) {
        b = MK_FP(DMABUF, 0);
        if (b[3] == 'M' && b[4] == 'S' && b[5] == 'D' && b[8] == '6' && b[9] == '.' &&
            ((b[6] == 'B' && b[7] == 'L') || (b[6] == 'S' && b[7] == 'P')))
            dbl = 1;
    }
    if ((unsigned)n > (dbl ? LMAXEXT - (DB_SIZE + 64) / 8 : LMAXEXT))
        return (int)FAPI_ERR_FRAG;
    if ((loop_sel[i] = kseg_alloc(LH_EXT + (unsigned long)n * 8 +
                                  (dbl ? DB_SIZE + 16 : 0))) == 0)
        return (int)FAPI_ERR_FULL;
    h = LH(i);
    h->dev = dev;
    h->carrier = v->blk;
    h->fk = fk;
    h->n = n;
    h->last = 0;
    h->lsh = 0;
    h->csh = blk[v->blk].bsize == 2048 ? 2 : 0;
    h->ro = ro || h->csh || (f->attr & ATTR_RO);
    h->units = units = f->size >> 9;
    if (v->drv == &fat_driver)
        ifs_bmap(f, loop_sel[i], LH_EXT, n);
    else {
        EX(i, 0)[0] = 0;
        EX(i, 0)[1] = f->fsdata << 2;
    }
    if (dev == nblk)
        nblk++;
    blk[dev].drv = DRV_LOOP;
    blk[dev].unit = (unsigned char)i;
    blk[dev].bsize = 512;
    blk[dev].blocks = units;
    blk[dev].rem = blk[dev].grp = 0;
    b = MK_FP(DMABUF, 0);
    if (dbl) {                          /* inneres Laufwerk einrichten   */
        struct dblh __far *db;
        unsigned k, spc;
        h->dbo = (LH_EXT + n * 8 + 15) & ~15;
        h->kind = LOOPK_DBL;
        h->ro = 1;                      /* Stufe 1: nur lesend           */
        db = MK_FP(loop_sel[i], h->dbo);
        if (raw(i, 0))
            goto bad;
        db->bb = *(unsigned __far *)(b + 39);
        db->fatc = db->bb + *(unsigned __far *)(b + 14);
        db->rootc = db->bb + *(unsigned __far *)(b + 41);
        db->mdf = *(unsigned __far *)(b + 36) + 1UL;
        db->dcl = *(int __far *)(b + 45);
        db->cl = 0;
        if (raw(i, db->bb) || b[510] != 0x55 || b[511] != 0xAA ||
            *(unsigned __far *)(b + 11) != 512)
            goto bad;
        spc = b[13];
        for (k = 0; k < 5 && (1u << k) != spc; k++)
            ;
        if (k == 5 || !b[16])           /* DriveSpace 3 (64) nicht       */
            goto bad;
        db->spc = (unsigned char)spc;
        db->sh = (unsigned char)k;
        db->res = *(unsigned __far *)(b + 14);
        db->rootin = db->res +
                     (unsigned long)(b[16] * *(unsigned __far *)(b + 22));
        db->din = db->rootin + (*(unsigned __far *)(b + 17) >> 4);
        units = *(unsigned __far *)(b + 19);
        if (!units)
            units = *(unsigned long __far *)(b + 32);
        blk[dev].blocks = units;
        return dev;
bad:
        loop_drop(dev);
        return (int)FAPI_ERR_DENIED;
    }
    h->kind = LOOPK_IMG;
    if (units > 64 && blk_read(dev, 64) == 0 && b[1] == 'C' && b[2] == 'D' &&
        b[3] == '0' && b[4] == '0' && b[5] == '1') {
        h->kind = LOOPK_ISO;            /* Deskriptor bei 8000h          */
        h->ro = 1;
        h->lsh = 2;
        blk[dev].bsize = 2048;
        blk[dev].blocks = units >> 2;
    } else if (units > 1 && blk_read(dev, 0) == 0 && b[0] == 'c' && b[1] == 'o' &&
               b[2] == 'n' && b[3] == 'e' && b[7] == 'x') {
        loop_drop(dev);                 /* dynamische VHD (Kopf vorn)    */
        return (int)FAPI_ERR_DENIED;
    } else if (units > 1 && blk_read(dev, units - 1) == 0 && b[0] == 'c' &&
               b[1] == 'o' && b[2] == 'n' && b[3] == 'e' && b[7] == 'x') {
        if (b[63] != 2) {               /* Typ (big endian): 2 = fest    */
            loop_drop(dev);
            return (int)FAPI_ERR_DENIED;
        }
        h->kind = LOOPK_VHD;
        h->units = --units;             /* Fussteil gehoert nicht dazu   */
        blk[dev].blocks = units;
    }
    if (h->kind != LOOPK_ISO && blk_read(dev, 0) == 0 &&
        !((b[0] == 0xEB || b[0] == 0xE9) && b[11] == 0 && b[12] == 2) &&
        b[510] == 0x55 && b[511] == 0xAA)
        h->kind = LOOPK_DISK;           /* Partitionstabelle, kein BPB   */
    return dev;
}

/* Loop-Geraet abmelden (Laufwerke sind schon weg) -> Eintrag der Datei-
   tabelle, den kernel.c schliesst, oder -1. Das Kernelsegment gibt
   kernel.c frei (loop_free). */
int __far loop_drop(int dev)
{
    int i = blk[dev].unit, fk;
    if (blk[dev].drv != DRV_LOOP || i >= NLOOP || !loop_sel[i])
        return -1;
    fk = LH(i)->fk;
    loop_free = loop_sel[i];
    loop_sel[i] = 0;
    blk[dev].drv = NDRV;                /* Luecke, wie DEVLOAD /U        */
    blk[dev].bsize = 0;
    blk[dev].blocks = 0;
    blk[dev].unit = 0;
    return fk;
}

/* Platz i fuer LOOP_INFO -> Eintrag der Dateitabelle, -1 frei, -2 Ende */
int __far loop_info(unsigned i, struct fapi_loopinfo __far *li)
{
    struct lhdr __far *h;
    int x;
    if (i >= NLOOP)
        return -2;
    li->drives = 0;
    if (!loop_sel[i])
        return -1;
    h = LH(i);
    for (x = 0; x < NDRIVE; x++)
        if ((vol[x].used || vol[x].present) && vol[x].blk == h->dev)
            li->drives |= 1UL << x;
    li->blocks = blk[h->dev].blocks;
    li->bsize = blk[h->dev].bsize;
    li->extents = h->n;
    li->kind = h->kind | (h->ro ? LOOPK_RO : 0);
    return h->fk;
}
