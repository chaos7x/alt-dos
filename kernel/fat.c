/*
 * AT-DOS 5.0 - IFS-Treiber FAT (12 und 16 Bit), nur lesend, und IFS-Verteiler
 *
 * Wie bei OS/2 steckt FAT fest im Kernel. Ein Laufwerk ist eine Partition
 * (oder eine Diskette ab Sektor 0) auf einem Blockgeraet. Sektornummern
 * sind 32 Bit breit, Multiplikationen mit der Clustergroesse laufen ueber
 * Schiebeoperationen, damit Watcom keine 32-Bit-Hilfsroutinen braucht.
 */
#include "kern.h"
#include "ifs.h"

struct volume vol[NDRIVE];

static unsigned char __far *dma(void)
{
    return MK_FP(DMABUF, 0);
}

static unsigned rd16(unsigned off)
{
    unsigned char __far *b = dma();
    return b[off] | ((unsigned)b[off + 1] << 8);
}

static unsigned long rd32(unsigned off)
{
    return rd16(off) | ((unsigned long)rd16(off + 2) << 16);
}

static unsigned long shl(unsigned long v, unsigned n)
{
    while (n--)
        v <<= 1;
    return v;
}

static unsigned long shr(unsigned long v, unsigned n)
{
    while (n--)
        v >>= 1;
    return v;
}

/* ---- FAT lesen: zwei aufeinanderfolgende Sektoren zwischengespeichert,
        weil ein 12-Bit-Eintrag ueber die Sektorgrenze reichen kann ------- */
#define FAT_END 0xFFFF
static unsigned char fcache[1024];
static int           fc_drive = -1;
static unsigned long fc_lba;

static int fat_load(int d, unsigned long lba)
{
    struct volume *v = &vol[d];
    if (fc_drive == d && fc_lba == lba)
        return 0;
    fc_drive = -1;
    if (blk_read(v->blk, lba))
        return -1;
    far_copy(KDATA, (unsigned)fcache, DMABUF, 0, 512);
    if (blk_read(v->blk, lba + 1))
        return -1;
    far_copy(KDATA, (unsigned)fcache + 512, DMABUF, 0, 512);
    fc_drive = d;
    fc_lba = lba;
    return 0;
}

static int valid(struct volume *v, unsigned c)
{
    return c >= 2 && c < v->clusters + 2;
}

static unsigned next_cluster(int d, unsigned c)
{
    struct volume *v = &vol[d];
    unsigned off, sec, val;

    if (v->fat16) {
        sec = c >> 8;
        off = (c & 255) * 2;
    } else {
        off = c + (c >> 1);
        sec = off >> 9;
        off &= 511;
    }
    if (fat_load(d, v->fat_lba + sec))
        return FAT_END;
    val = fcache[off] | ((unsigned)fcache[off + 1] << 8);
    if (!v->fat16)
        val = (c & 1) ? val >> 4 : val & 0xFFF;
    return valid(v, val) ? val : FAT_END;
}

static unsigned long clus_lba(struct volume *v, unsigned c)
{
    return v->data_lba + shl((unsigned long)(c - 2), v->spc_shift);
}

/* ---- Einhaengen -------------------------------------------------------- */
static int fat_mount(int d)
{
    struct volume *v = &vol[d];
    unsigned bps, reserved, nfats, spf, spc, n;
    unsigned long total, cl;

    if (blk_read(v->blk, v->start))
        return -1;
    if (rd16(510) != 0xAA55)
        return -1;
    bps      = rd16(11);
    spc      = dma()[13];
    reserved = rd16(14);
    nfats    = dma()[16];
    v->rootents = rd16(17);
    total    = rd16(19);
    spf      = rd16(22);
    if (total == 0)
        total = rd32(32);
    if (bps != 512 || spf == 0 || nfats == 0 || nfats > 2 || spc == 0 || spc > 64)
        return -1;                              /* FAT32 hat spf == 0   */
    for (n = 0; (1U << n) < spc; n++)
        ;
    if ((1U << n) != spc)
        return -1;
    v->spc = (unsigned char)spc;
    v->spc_shift = (unsigned char)n;
    v->sectors  = total;
    v->fat_lba  = v->start + reserved;
    v->root_lba = v->fat_lba + nfats * spf;
    v->data_lba = v->root_lba + (v->rootents + 15) / 16;
    cl = shr(total - (v->data_lba - v->start), n);
    if (cl >= 65525UL)
        return -1;                              /* waere FAT32          */
    v->clusters = (unsigned)cl;
    v->fat16 = cl >= 4085;
    return 0;
}

/* ---- Verzeichnisse ----------------------------------------------------- */
/* i-ter Eintrag des Verzeichnisses ab Cluster dc (0 = Stamm) im DMA-Puffer */
static unsigned char __far *dir_entry(int d, unsigned dc, unsigned i)
{
    struct volume *v = &vol[d];
    static int lc_drive = -1;
    static unsigned lc_dir, lc_n, lc_clus;
    unsigned epc, n, c, k;

    if (dc == 0) {
        if (i >= v->rootents)
            return 0;
        if (blk_read(v->blk, v->root_lba + (i >> 4)))
            return 0;
        return dma() + ((i & 15) << 5);
    }
    epc = (unsigned)v->spc * 16;
    n = i / epc;
    if (lc_drive == d && lc_dir == dc && lc_n <= n) {
        c = lc_clus;                            /* fortlaufend weiter   */
        k = lc_n;
    } else {
        c = dc;
        k = 0;
    }
    for (; k < n; k++) {
        c = next_cluster(d, c);
        if (c == FAT_END)
            return 0;
    }
    lc_drive = d; lc_dir = dc; lc_n = n; lc_clus = c;
    if (blk_read(v->blk, clus_lba(v, c) + ((i % epc) >> 4)))
        return 0;
    return dma() + ((i & 15) << 5);
}

/* 0 = Ende, 1 = Datei/Ordner, 2 = ueberspringen */
static int kind(unsigned char __far *e)
{
    if (e[0] == 0)
        return 0;
    if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
        return 2;                       /* geloescht, langer Name, Label */
    return 1;
}

/* Pfadteil -> 11 Zeichen "NAME    EXT"; -1, wenn kein 8.3-Name */
static int to83(const char *s, unsigned len, char *n)
{
    unsigned i, k = 0, dot = 0;

    for (i = 0; i < 11; i++)
        n[i] = ' ';
    if (len == 1 && s[0] == '.') { n[0] = '.'; return 0; }
    if (len == 2 && s[0] == '.' && s[1] == '.') { n[0] = n[1] = '.'; return 0; }
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '.') {
            if (dot++ || k == 0)
                return -1;
            k = 8;
            continue;
        }
        if (c <= ' ' || c == '"' || c == '*' || c == '?' || c == '/' ||
            c == ':' || c == '<' || c == '>' || c == '|' || c == '+' ||
            c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            return -1;
        if ((!dot && k >= 8) || (dot && k >= 11))
            return -1;                  /* zu lang fuer 8.3 */
        n[k++] = c;
    }
    return k ? 0 : -1;
}

struct found {
    unsigned first;
    unsigned char attr;
    unsigned long size;
};

static int lookup(int d, unsigned dc, const char *n, struct found *f)
{
    unsigned char __far *e;
    unsigned i, k;

    for (i = 0; (e = dir_entry(d, dc, i)) != 0; i++) {
        switch (kind(e)) {
        case 0: return -1;
        case 2: continue;
        }
        for (k = 0; k < 11 && e[k] == (unsigned char)n[k]; k++)
            ;
        if (k == 11) {
            f->first = e[26] | ((unsigned)e[27] << 8);
            f->attr  = e[11];
            f->size  = (e[28] | ((unsigned)e[29] << 8)) |
                       ((unsigned long)(e[30] | ((unsigned)e[31] << 8)) << 16);
            return 0;
        }
    }
    return -1;
}

/* kanonischen Pfad ablaufen; "\" ist das Stammverzeichnis (first = 0) */
static int walk(int d, const char *path, struct found *f)
{
    char n[11];
    const char *p = path, *q;

    f->first = 0;
    f->attr = ATTR_DIR;
    f->size = 0;
    while (*p == '\\')
        p++;
    while (*p) {
        for (q = p; *q && *q != '\\'; q++)
            ;
        if (!(f->attr & ATTR_DIR))
            return -1;                  /* Zwischenstueck ist eine Datei */
        if (to83(p, (unsigned)(q - p), n) || lookup(d, f->first, n, f))
            return -1;
        p = q;
        while (*p == '\\')
            p++;
    }
    return 0;
}

static int fat_open(int d, const char *path, struct ifs_file *fl)
{
    struct found f;
    if (walk(d, path, &f) || (f.attr & ATTR_DIR))
        return -1;
    fl->drive = (unsigned char)d;
    fl->attr = f.attr;
    fl->first = f.first;
    fl->size = f.size;
    fl->cache_clus = 0;
    fl->cache_pos = 0;
    return 0;
}

static int fat_isdir(int d, const char *path)
{
    struct found f;
    return walk(d, path, &f) == 0 && (f.attr & ATTR_DIR);
}

static int fat_readdir(int d, const char *dir, unsigned want, struct ifs_dirent *de)
{
    struct found f;
    unsigned char __far *e;
    unsigned i, k, j, n = 0;

    if (walk(d, dir, &f) || !(f.attr & ATTR_DIR))
        return -1;
    for (i = 0; (e = dir_entry(d, f.first, i)) != 0; i++) {
        switch (kind(e)) {
        case 0: return -1;
        case 2: continue;
        }
        if (n++ != want)
            continue;
        j = 0;
        for (k = 0; k < 8 && e[k] != ' '; k++)
            de->name[j++] = e[k];
        if (e[8] != ' ') {
            de->name[j++] = '.';
            for (k = 8; k < 11 && e[k] != ' '; k++)
                de->name[j++] = e[k];
        }
        de->name[j] = 0;
        de->attr = e[11];
        de->time = e[22] | ((unsigned)e[23] << 8);
        de->date = e[24] | ((unsigned)e[25] << 8);
        de->size = (e[28] | ((unsigned)e[29] << 8)) |
                   ((unsigned long)(e[30] | ((unsigned)e[31] << 8)) << 16);
        return 0;
    }
    return -1;
}

static int fat_read(struct ifs_file *f, unsigned long pos, unsigned sel,
                    unsigned off, unsigned len)
{
    int d = f->drive;
    struct volume *v = &vol[d];
    unsigned csz = (unsigned)v->spc * 512, c, so, b, n, done = 0;
    unsigned long cpos;

    if (pos >= f->size)
        return 0;
    if (f->size - pos < len)
        len = (unsigned)(f->size - pos);

    if (f->cache_clus && pos >= f->cache_pos) {
        c = f->cache_clus;                      /* weiter, wo wir waren */
        cpos = f->cache_pos;
    } else {
        c = f->first;
        cpos = 0;
    }
    while (pos - cpos >= csz) {                 /* zum Cluster vorlaufen */
        if (!valid(v, c))
            return -1;
        c = next_cluster(d, c);
        cpos += csz;
    }
    while (done < len) {
        if (!valid(v, c))
            return -1;                          /* kaputte Kette        */
        f->cache_clus = c;
        f->cache_pos  = cpos;
        so = (unsigned)(pos - cpos);
        if (blk_read(v->blk, clus_lba(v, c) + (so >> 9)))
            return -1;
        b = so & 511;
        n = 512 - b;
        if (n > len - done)
            n = len - done;
        far_copy(sel, off + done, DMABUF, b, n);
        done += n;
        pos  += n;
        if (pos - cpos >= csz) {
            c = next_cluster(d, c);
            cpos += csz;
        }
    }
    return done;
}

struct ifs_driver fat_driver = {
    "FAT", fat_mount, fat_open, fat_read, fat_readdir, fat_isdir
};

/* ---- IFS-Verteiler ----------------------------------------------------- */
int ifs_mount(int drive, int blk, unsigned long start, struct ifs_driver *drv)
{
    struct volume *v = &vol[drive];
    v->blk = blk;
    v->start = start;
    v->drv = drv;
    if (drv->mount(drive))
        return -1;
    v->used = 1;
    return 0;
}

#define VOL_OK(d) ((unsigned)(d) < NDRIVE && vol[d].used)

int ifs_open(int drive, const char *path, struct ifs_file *f)
{
    return VOL_OK(drive) ? vol[drive].drv->open(drive, path, f) : -1;
}

int ifs_read(struct ifs_file *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len)
{
    return VOL_OK(f->drive) ? vol[f->drive].drv->read(f, pos, sel, off, len) : -1;
}

int ifs_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d)
{
    return VOL_OK(drive) ? vol[drive].drv->readdir(drive, dir, n, d) : -1;
}

int ifs_isdir(int drive, const char *path)
{
    return VOL_OK(drive) ? vol[drive].drv->isdir(drive, path) : 0;
}
