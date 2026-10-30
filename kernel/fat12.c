/*
 * DOS 5.0 - IFS-Treiber FAT12 (Disketten), nur lesend
 *
 * Bootsektor (BPB) auswerten, FAT im Kernel zwischenspeichern, Dateien im
 * Stammverzeichnis per 8.3-Namen finden und der Clusterkette folgen.
 */
#include "kern.h"
#include "ifs.h"

#define FAT_MAX_SECT 9                          /* 1,44 MB: 9 Sektoren   */

static unsigned char fat[FAT_MAX_SECT * 512];
static unsigned spc, fat_lba, root_lba, root_secs, data_lba;

static unsigned char __far *dma(void)
{
    return MK_FP(DMABUF, 0);
}

static unsigned rd16(unsigned off)
{
    unsigned char __far *b = dma();
    return b[off] | ((unsigned)b[off + 1] << 8);
}

static int fat_mount(void)
{
    unsigned bps, reserved, nfats, rootents, spf, i;

    if (fd_read(0))
        return -1;
    if (rd16(510) != 0xAA55)
        return -1;
    bps      = rd16(11);
    spc      = dma()[13];
    reserved = rd16(14);
    nfats    = dma()[16];
    rootents = rd16(17);
    spf      = rd16(22);
    if (bps != 512 || spc == 0 || spf == 0 || spf > FAT_MAX_SECT)
        return -1;

    fat_lba   = reserved;
    root_lba  = reserved + nfats * spf;
    root_secs = (rootents * 32 + 511) / 512;
    data_lba  = root_lba + root_secs;

    for (i = 0; i < spf; i++) {
        if (fd_read(fat_lba + i))
            return -1;
        far_copy(KDATA, (unsigned)fat + i * 512, DMABUF, 0, 512);
    }
    return 0;
}

static unsigned next_cluster(unsigned c)
{
    unsigned off = c + c / 2;
    unsigned v = fat[off] | ((unsigned)fat[off + 1] << 8);
    return (c & 1) ? v >> 4 : v & 0xFFF;
}

/* "MEM.EXE" -> "MEM     EXE" */
static void to83(const char *s, char *n)
{
    int i = 0;
    for (i = 0; i < 11; i++)
        n[i] = ' ';
    i = 0;
    while (*s && *s != '.' && i < 8)
        n[i++] = *s++;
    while (*s && *s != '.')
        s++;
    if (*s == '.')
        s++;
    i = 8;
    while (*s && i < 11)
        n[i++] = *s++;
}

static int fat_open(const char *name, struct ifs_file *f)
{
    char n[11];
    unsigned s, e, k;
    unsigned char __far *d;

    to83(name, n);
    for (s = 0; s < root_secs; s++) {
        if (fd_read(root_lba + s))
            return -1;
        d = dma();
        for (e = 0; e < 512; e += 32) {
            if (d[e] == 0)
                return -1;                      /* Ende des Verzeichnisses */
            if (d[e] == 0xE5 || (d[e + 11] & 0x18))
                continue;                       /* geloescht, Label, Ordner */
            for (k = 0; k < 11; k++)
                if (d[e + k] != (unsigned char)n[k])
                    break;
            if (k == 11) {
                f->first = rd16(e + 26);
                f->size  = rd16(e + 28) | ((unsigned long)rd16(e + 30) << 16);
                return 0;
            }
        }
    }
    return -1;
}

static int fat_read(struct ifs_file *f, unsigned long dst, unsigned max)
{
    unsigned c = f->first, s, n, done = 0;
    unsigned long left = f->size;

    if (left > max)
        return -1;
    scratch_map(dst, max ? max - 1 : 0xFFFF);
    while (left > 0) {
        if (c < 2 || c >= 0xFF0)
            return -1;                          /* kaputte Kette          */
        for (s = 0; s < spc && left > 0; s++) {
            if (fd_read(data_lba + (c - 2) * spc + s))
                return -1;
            n = left > 512 ? 512 : (unsigned)left;
            far_copy(SCRATCH, done, DMABUF, 0, n);
            done += n;
            left -= n;
        }
        c = next_cluster(c);
    }
    return 0;
}

struct ifs_driver fat12_driver = { "FAT12", fat_mount, fat_open, fat_read };

/* ---- IFS-Verteiler ----------------------------------------------------- */
static struct ifs_driver *mounted;

int ifs_mount(struct ifs_driver *drv)
{
    if (drv->mount())
        return -1;
    mounted = drv;
    return 0;
}

int ifs_open(const char *name, struct ifs_file *f)
{
    return mounted ? mounted->open(name, f) : -1;
}

int ifs_read(struct ifs_file *f, unsigned long dst, unsigned max)
{
    return mounted ? mounted->read(f, dst, max) : -1;
}

const char *ifs_name(void)
{
    return mounted ? mounted->name : "-";
}
