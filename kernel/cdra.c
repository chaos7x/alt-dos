/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - CD-Laufwerke lesen am Stueck (KTEXT4).
 *
 * Ein CD-Block (2048 Byte) war bisher je Zugriff ein eigener Befehl an
 * das Laufwerk, ohne Platten-Cache (der kennt nur 512er-Sektoren). Beim
 * Windows-SETUP von CD (Lesen in 32-KB-Stuecken, je 16 Befehle) waren das
 * in 86Box ~120 KB/s, unter MS-DOS mit MSCDEX ein Vielfaches. Jetzt liest
 * cd_ra bis zu 16 Bloecke (32 KB) mit einem Befehl in einen eigenen Puffer
 * (unter 16 MB, ohne 64-KB-Grenze - Busmaster/ISA-DMA) und bedient die
 * folgenden Bloecke daraus. Nur fuer Treiber, die beim INIT Bit 80h in
 * count melden (CD-Bloecke am Stueck): IDE.SYS, AIC7XXX.SYS.
 *
 * Gueltig nur kurz (1 s): ein Medienwechsel dauert laenger. Waehrend der
 * Puffer gefuellt wird (Schlafpunkt im Treiber), liest ein anderer Task
 * wie bisher einzeln in seinen DMABUF.
 */
#include "kern.h"
#include "mem.h"
#include "ifs.h"
#include "../include/fapi_abi.h"
#include "task.h"
#include "k2.h"                         /* KTEXT4, k2.h zuletzt           */

int __far kf_dev_read(int dev, unsigned long lba, unsigned n, unsigned long phys, unsigned sel);
unsigned long __far kf_cd_blocks(int d);
void __far kf_klock_enter(void);
void __far kf_klock_leave(void);
extern unsigned char blk_nm[NBLK];

#define RA_N    16                      /* Bloecke je Befehl (32 KB)      */

static unsigned ra_sel, ra_n, ra_busy, ra_fail;
static int ra_dev = -1;
static unsigned long ra_lin, ra_lba, ra_t;

int __far cd_ra(int dev, unsigned long lba)
{
    unsigned n;
    int r;
    if (!ra_sel && !ra_fail && !(ra_sel = kseg_dma(RA_N * 2048U, &ra_lin)))
        ra_fail = 1;
    if (ra_sel && !ra_busy && ra_dev == dev && lba >= ra_lba && lba < ra_lba + ra_n &&
        get_ticks() - ra_t < 100) {
        far_copy(DMABUF, 0, ra_sel, (unsigned)(lba - ra_lba) << 11, 2048);
        return 0;
    }
    if (!ra_sel || ra_busy)
        return kf_dev_read(dev, lba, 1, 0, 0);
    n = RA_N;
    if (drv[blk[dev].drv].maxcnt < n)
        n = drv[blk[dev].drv].maxcnt;
    if (blk[dev].blocks - lba < n)
        n = (unsigned)(blk[dev].blocks - lba);
    if (!n)
        n = 1;
    ra_busy = 1;
    ra_dev = -1;
    r = kf_dev_read(dev, lba, n, ra_lin, ra_sel);
    ra_busy = 0;
    if (r)                              /* Fehler/kein Medium: einzeln    */
        return n > 1 && !blk_nm[dev] ? kf_dev_read(dev, lba, 1, 0, 0) : -1;
    ra_dev = dev;
    ra_lba = lba;
    ra_n = n;
    ra_t = get_ticks();
    far_copy(DMABUF, 0, ra_sel, 0, 2048);
    return 0;
}

/* MSCDEX (redir.c, dem.c): Block lba von CD-Laufwerk d nach sel:off
   (2048 Byte); 0 = ok, 1 kein Medium, -1 Fehler */
int __far kf_cd_read(int d, unsigned long lba, unsigned sel, unsigned off)
{
    int dev, r;
    if (!kf_cd_blocks(d))
        return -1;
    dev = vol[d].blk;
    kf_klock_enter();
    r = blk_read(dev, lba);
    if (!r && sel)                      /* sel 0: im DMABUF lassen        */
        far_copy(sel, off, DMABUF, 0, 2048);
    else if (blk_nm[dev])
        r = 1;
    kf_klock_leave();
    return r;
}

/* codeswap.c: Seiten der Programmabbilder/DosAllocSeg zaehlen (hier nur,
   weil das erste Codesegment voll ist) */
extern unsigned alloc_units;
void __far kf_seg_acct(int img, int pages)
{
    if (img)
        image_units += pages;
    else
        alloc_units += pages;
}
