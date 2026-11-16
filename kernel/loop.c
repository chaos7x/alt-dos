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
};
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
    int i, dev, n;
    unsigned char __far *b;
    unsigned long units;

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
    if ((unsigned)n > LMAXEXT)
        return (int)FAPI_ERR_FRAG;
    if ((loop_sel[i] = kseg_alloc(LH_EXT + (unsigned long)n * 8)) == 0)
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
