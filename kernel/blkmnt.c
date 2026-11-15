/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.1 - Partitionen einhaengen (KTEXT2; aus kernel.c, dort war das
 * erste Codesegment voll): mount_disk liest die Partitionstabelle eines
 * Blockgeraets (primaer, dann logische Laufwerke der erweiterten
 * Partition), mount_one haengt eine FAT-Partition ein, nd_free sucht den
 * naechsten freien Buchstaben. Aufrufer: Start, DEVLOAD, LOOP.
 */
#include "kern.h"
#include "ifs.h"
#include "k2.h"

extern signed char subst_drv[NDRIVE];   /* kernel.c                      */

static unsigned rd16(unsigned sel, unsigned off)
{
    return *(unsigned __far *)MK_FP(sel, off);
}

/* Partitionstabelle lesen und FAT-Partitionen als C:, D:, ... einhaengen */
int next_drive = 2;

#define IS_FATPART(t) ((t) == 0x01 || (t) == 0x04 || (t) == 0x06 || (t) == 0x0E || \
                       (t) == 0x0B || (t) == 0x0C)   /* FAT32 (CHS, LBA) */
#define IS_EXTPART(t) ((t) == 0x05 || (t) == 0x0F)

static unsigned long rd32(unsigned sel, unsigned off)
{
    return rd16(sel, off) | ((unsigned long)rd16(sel, off + 2) << 16);
}

/* naechster freier Buchstabe (zur Laufzeit liegen dazwischen SUBST-
   Laufwerke oder Luecken) -> 0, wenn keiner mehr frei ist */
int __far nd_free(void)
{
    while (next_drive < NDRIVE && (vol[next_drive].used || vol[next_drive].present ||
                                   subst_drv[next_drive] >= 0 || next_drive == PRE_DRIVE))
        next_drive++;
    return next_drive < NDRIVE;
}

void __far mount_one(int dev, unsigned long start, unsigned long size,
                      unsigned char type, unsigned part)
{
    if (!nd_free())
        return;
    vol[next_drive].psize = size;
    ifs_mount(next_drive, dev, start, &fat_driver);  /* auch unformatiert:
                                       Buchstabe fuer FORMAT    */
    vol[next_drive].present = 1;
    vol[next_drive].ptype = type;
    vol[next_drive].part = (unsigned char)part;
    next_drive++;
}

/* Partitionen einer Platte: erst die primaeren (FAT), dann die logischen
   Laufwerke in einer erweiterten Partition (05h/0Fh, Kette von EBRs: im
   EBR Eintrag 1 = logisches Laufwerk relativ zum EBR, Eintrag 2 = naechster
   EBR relativ zum Anfang der erweiterten Partition - wie bei MS-DOS) */
void __far mount_disk(int dev)
{
    unsigned char __far *b;
    unsigned char t[4], lt, nt;
    unsigned long st[4], sz[4], ebr, ls, lz, ns;
    int i, n;

    if (blk[dev].bsize != 512)
        return;                         /* CD-ROM: siehe cd_drives()    */
    if (IS_REM(dev)) {                  /* Wechselplatte: ein Buchstabe, */
        if (nd_free()) {                /* eingehaengt wird bei Bedarf   */
            vol[next_drive].blk = dev;  /* (fat.c revalidate)            */
            vol[next_drive].drv = &fat_driver;
            vol[next_drive].present = 1;
            vol_present(next_drive++);
        }
        return;
    }
    if (blk_read(dev, 0))
        return;
    b = MK_FP(DMABUF, 0);
    if (b[510] != 0x55 || b[511] != 0xAA)
        return;
    for (i = 0; i < 4; i++) {           /* Tabelle merken (ifs_mount    */
        t[i] = b[0x1BE + 16 * i + 4];   /* benutzt DMABUF)              */
        st[i] = rd32(DMABUF, 0x1BE + 16 * i + 8);
        sz[i] = rd32(DMABUF, 0x1BE + 16 * i + 12);
    }
    for (i = 0; i < 4 && next_drive < NDRIVE; i++)
        if (IS_FATPART(t[i]))
            mount_one(dev, st[i], sz[i], t[i], (unsigned)i + 1);
    for (i = 0; i < 4; i++) {
        if (!IS_EXTPART(t[i]))
            continue;
        ebr = st[i];
        for (n = 0; n < 24 && next_drive < NDRIVE; n++) {
            if (blk_read(dev, ebr))
                break;
            b = MK_FP(DMABUF, 0);
            if (b[510] != 0x55 || b[511] != 0xAA)
                break;
            lt = b[0x1BE + 4];
            ls = rd32(DMABUF, 0x1BE + 8);
            lz = rd32(DMABUF, 0x1BE + 12);
            nt = b[0x1CE + 4];
            ns = rd32(DMABUF, 0x1CE + 8);
            if (IS_FATPART(lt) && lz)
                mount_one(dev, ebr + ls, lz, lt, 5 + (unsigned)n);
            if (!IS_EXTPART(nt) || !ns)
                break;
            ebr = st[i] + ns;
        }
    }
}

