/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - Vorlader fuer den Start von Festplatte
 *
 * Henne-Ei-Problem: Die Treiber fuer die Platte stehen in CONFIG.SYS auf
 * eben dieser Platte. Loesung wie bei OS/2: Solange der Kernel noch im Real
 * Mode laeuft, liest er ueber das BIOS (INT 13h) CONFIG.SYS und alle dort
 * mit DEVICE= und IFS= genannten Dateien in den Speicher ab 0600h. Im Protected Mode
 * stellt das Pseudo-Dateisystem "BOOT" (Laufwerk Z:, nur waehrend
 * CONFIG.SYS) diese Dateien bereit; danach wird es wieder entfernt.
 *
 * preload() laeuft im REAL MODE (CS=1000h, DS=SS=kdata_seg): keine Selektoren, keine
 * Bildschirmausgabe ueber vio, nur Speicher per Segment:Offset. Fehler
 * merkt sich pre_err; kmain meldet sie spaeter.
 *
 * FAT16 und (6.0) FAT32; Dateien im Stamm, in \DOS\SYSTEM und \DOS.
 * NPRE begrenzt CONFIG.SYS plus Treiber und IFS-Module auf 16 Dateien.
 */
#include "kern.h"
#include "ifs.h"

/* Ablage der vorgeladenen Dateien: ab 6.0 bei 80000h (bis 60 KB; die
   Speicherverwaltung gibt den Bereich erst nach CONFIG.SYS frei, kmain),
   auf Rechnern mit weniger als 600 KB konventionellem Speicher wie
   bisher 0600h-7BFFh (knapp 30 KB) */
#define PRE_SEG     pre_seg
#define PRE_MAX     pre_max
#define BUF_SEG     0x0800          /* linear 8000h (DMABUF)              */
#define NPRE        16

struct pre_file {
    char           name[12];        /* "AHA154X SYS" (FAT-Verzeichnisform) */
    unsigned       off, size;
};

struct pre_file pre[NPRE];
int             npre;
unsigned        pre_seg = 0x0060, pre_max = 0x7600;
unsigned char   pre_err;            /* PRE_ERR_*                          */
unsigned long   boot_hidden;        /* Startsektor der Startpartition     */
unsigned long   boot_serial;        /* Seriennummer der Startpartition    */

/* FAT16/FAT32 der Startpartition */
static unsigned char  spc, f32;
static unsigned       rootents;
static unsigned long  fat_lba, root_lba, data_lba, rootc;
static unsigned       pre_used;

static unsigned char __far *bpb(void) { return MK_FP(0, 0x7C00); }
static unsigned char __far *buf(void) { return MK_FP(BUF_SEG, 0); }

static unsigned w16(unsigned char __far *p) { return p[0] | (p[1] << 8); }
static unsigned long w32(unsigned char __far *p)
{
    return w16(p) | ((unsigned long)w16(p + 2) << 16);
}

/* Name aus CONFIG.SYS ("C:\\DOS\\aha154x.sys") in Verzeichnisform; nur der
   letzte Teil zaehlt - gesucht wird im Stamm, in \DOS\SYSTEM und \DOS. */
static int to83(const char *s, char *n)
{
    int i, k;
    const char *q;
    for (i = 0; i < 11; i++)
        n[i] = ' ';
    n[11] = 0;
    for (q = s; *q && *q != ' '; q++)
        if (*q == '\\' || *q == '/' || *q == ':')
            s = q + 1;
    for (k = 0; *s && *s != '.' && *s != ' '; s++) {
        if (*s == '\\' || *s == '/' || *s == ':' || k == 8)
            return -1;                          /* nur Stammverzeichnis */
        n[k++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    }
    if (*s == '.')
        for (s++, k = 8; *s && *s != ' '; s++) {
            if (k == 11 || *s == '\\' || *s == '/')
                return -1;
            n[k++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
        }
    return 0;
}

static unsigned spc_shift(void)
{
    unsigned k = 0, j;
    for (j = spc; j > 1; j >>= 1)
        k++;
    return k;
}

/* Eintrag n in einem Sektor voller Verzeichniseintraege; 1 = gefunden,
   -1 = Ende des Verzeichnisses, 0 = weiter */
static int scan_sec(const char *n, int dir, unsigned long *clus, unsigned long *size)
{
    unsigned e, i;
    unsigned char __far *d;
    for (e = 0; e < 16; e++) {
        d = buf() + (e << 5);
        if (d[0] == 0)
            return -1;
        if ((d[11] & 0x08) || ((d[11] & 0x10) != 0) != dir)
            continue;                           /* Name / falsche Art  */
        for (i = 0; i < 11 && d[i] == (unsigned char)n[i]; i++)
            ;
        if (i == 11) {
            *clus = w16(d + 26) | (f32 ? (unsigned long)w16(d + 20) << 16 : 0);
            *size = w32(d + 28);
            return 1;
        }
    }
    return 0;
}

static int find_chain(unsigned long c, const char *n, int dir, unsigned long *clus,
                      unsigned long *size);
static int find_root(const char *n, int dir, unsigned long *clus, unsigned long *size)
{
    unsigned s;
    int r;
    if (f32)                            /* FAT32: Stamm ist eine Kette */
        return find_chain(rootc, n, dir, clus, size);
    for (s = 0; s < (rootents >> 4); s++) {
        if (rm_read(root_lba + s))
            return -1;
        r = scan_sec(n, dir, clus, size);
        if (r)
            return r > 0 ? 0 : -1;
    }
    return -1;
}

/* gueltiger Cluster (nicht Ende, nicht defekt)? */
static int live(unsigned long c)
{
    return c >= 2 && c < (f32 ? 0x0FFFFFF7UL : 0xFFF7UL);
}

/* naechster Cluster der Kette (FAT-Sektor ueber den BIOS-Puffer);
   0 = Lesefehler */
static unsigned long next_clus(unsigned long c)
{
    if (f32) {
        if (rm_read(fat_lba + (c >> 7)))
            return 0;
        return w32(buf() + (((unsigned)c & 127) << 2)) & 0x0FFFFFFFUL;
    }
    if (rm_read(fat_lba + (c >> 8)))
        return 0;
    return w16(buf() + (((unsigned)c & 0xFF) << 1));
}

/* Verzeichniskette ab Cluster c nach n durchsuchen; 0 = gefunden */
static int find_chain(unsigned long c, const char *n, int dir, unsigned long *clus,
                      unsigned long *size)
{
    unsigned j;
    int r;
    while (live(c)) {
        for (j = 0; j < spc; j++) {
            if (rm_read(data_lba + ((c - 2) << spc_shift()) + j))
                return -1;
            r = scan_sec(n, dir, clus, size);
            if (r)
                return r > 0 ? 0 : -1;
        }
        c = next_clus(c);
    }
    return -1;
}

/* Datei im Stamm, in \DOS\SYSTEM (Treiber, IFS) oder \DOS suchen
   -> erster Cluster, Groesse; 0 = ok */
static int find(const char *n, unsigned long *clus, unsigned long *size)
{
    static const char dosdir[12] = "DOS        ", sysdir[12] = "SYSTEM     ";
    unsigned long dc, sc;
    unsigned long sz;

    if (find_root(n, 0, clus, size) == 0)
        return 0;
    if (find_root(dosdir, 1, &dc, &sz))
        return -1;
    if (find_chain(dc, sysdir, 1, &sc, &sz) == 0 &&
        find_chain(sc, n, 0, clus, size) == 0)
        return 0;
    return find_chain(dc, n, 0, clus, size);
}

/* Datei der FAT-Kette nach in den Vorladebereich lesen; 0 = ok */
static int load(const char *n)
{
    struct pre_file *p;
    unsigned k, i, j, len;
    unsigned long clus;
    unsigned long size, lba;
    unsigned char __far *dst;

    if (npre == NPRE) {
        pre_err = PRE_ERR_FULL;
        return -1;
    }
    if (find(n, &clus, &size))
        return -1;                  /* fehlt: meldet spaeter der Kernel */
    if (size > PRE_MAX - pre_used) {
        pre_err = PRE_ERR_FULL;
        return -1;
    }
    p = &pre[npre];
    for (i = 0; i < 12; i++)
        p->name[i] = n[i];
    p->off = pre_used;
    p->size = (unsigned)size;
    dst = MK_FP(PRE_SEG, pre_used);
    len = (unsigned)size;
    while (len && live(clus)) {
        for (k = 0, j = spc; j > 1; j >>= 1)    /* (clus-2) * spc       */
            k++;
        lba = data_lba + ((unsigned long)(clus - 2) << k);
        for (j = 0; j < spc && len; j++) {
            if (rm_read(lba + j)) {
                pre_err = PRE_ERR_READ;
                return -1;
            }
            for (i = 0; i < 512 && len; i++, len--)
                *dst++ = buf()[i];
        }
        if ((clus = next_clus(clus)) == 0) {
            pre_err = PRE_ERR_READ;
            return -1;
        }
    }
    if (len) {
        pre_err = PRE_ERR_READ;
        return -1;
    }
    pre_used += (p->size + 15) & ~15;
    npre++;
    return 0;
}

void preload(void)
{
    unsigned char __far *b = bpb();
    unsigned char __far *c;
    unsigned i, n;
    unsigned long fatsz;
    char name[12];

    f32 = w16(b + 22) == 0 && b[66] == 0x29;    /* FAT32-BPB            */
    if (w16(b + 11) != 512 || b[13] == 0 || (b[13] & (b[13] - 1)) ||
        (!f32 && (b[38] != 0x29 || w16(b + 22) == 0))) {
        pre_err = PRE_ERR_FS;
        return;
    }
    spc = b[13];
    rootents = w16(b + 17);
    fatsz = f32 ? w32(b + 36) : w16(b + 22);
    boot_hidden = w32(b + 28);
    boot_serial = w32(b + (f32 ? 67 : 39));
    rootc = f32 ? w32(b + 44) : 0;
    fat_lba = boot_hidden + w16(b + 14);
    root_lba = fat_lba;
    for (i = 0; i < b[16]; i++)
        root_lba += fatsz;
    data_lba = root_lba + (f32 ? 0 : rootents >> 4);
    if (rm_geometry()) {
        pre_err = PRE_ERR_READ;
        return;
    }

    if (*(unsigned __far *)MK_FP(0x40, 0x13) >= 600) {
        pre_seg = 0x8000;           /* linear 80000h, bis 60 KB           */
        pre_max = 0xF000;
    }
    if (to83("CONFIG.SYS", name) || load(name))
        return;                     /* ohne CONFIG.SYS keine Treiber    */

    /* DEVICE=- und IFS=-Zeilen: nur den Dateinamen, Argumente bleiben in CONFIG.SYS */
    c = MK_FP(PRE_SEG, pre[0].off);
    n = pre[0].size;
    for (i = 0; i < n; ) {
        unsigned s = i, k;
        while (i < n && c[i] != '\r' && c[i] != '\n')
            i++;
        while (s < i && (c[s] == ' ' || c[s] == '\t'))
            s++;
        if (i - s > 4) {
            static const char dev[] = "DEVICE", ifs[] = "IFS";
            char path[64];
            for (k = 0; k < 6 && (c[s + k] | 0x20) == (dev[k] | 0x20); k++)
                ;
            if (k != 6) {               /* IFS= wird genauso vorgeladen */
                for (k = 0; k < 3 && (c[s + k] | 0x20) == (ifs[k] | 0x20); k++)
                    ;
                k = k == 3 ? 6 : 0;
                if (k)
                    s -= 3;             /* gleicher Sprung wie bei DEVICE */
            }
            if (k == 6) {
                s += 6;
                if (i - s > 4 && (c[s] | 0x20) == 'h' && (c[s + 1] | 0x20) == 'i' &&
                    (c[s + 2] | 0x20) == 'g' && (c[s + 3] | 0x20) == 'h')
                    s += 4;             /* DEVICEHIGH                    */
                if (s < i && c[s] != '=' && c[s] != ' ' && c[s] != '\t')
                    s = i;              /* anderes Wort, z. B. DEVICEX   */
                while (s < i && c[s] != '=')
                    s++;                /* Optionen wie /L:1 vor dem =   */
                if (s < i && c[s] == '=') {
                    s++;
                    while (s < i && c[s] == ' ')
                        s++;
                    while (s < i && (c[s] == '/' || (c[s] | 0x20) == 's') &&
                           (c[s] == '/' || ((c[s + 1] | 0x20) == 'i' && c[s + 4] == '='))) {
                        while (s < i && c[s] != ' ')    /* /L:.. SIZE=.. */
                            s++;
                        while (s < i && c[s] == ' ')
                            s++;
                    }
                    for (k = 0; s < i && c[s] != ' ' && k < sizeof(path) - 1; )
                        path[k++] = c[s++];
                    path[k] = 0;
                    if (to83(path, name) == 0)
                        load(name);
                }
            }
        }
        while (i < n && (c[i] == '\r' || c[i] == '\n'))
            i++;
    }
}

/* ---- Pseudo-Dateisystem BOOT (Protected Mode) --------------------------- */
static int __far pre_mount(int drive) { (void)drive; return 0; }

static int __far pre_open(int drive, const char *path, struct ifs_file __far *f)
{
    char n[12];
    int i, k;
    if (to83(path, n))
        return -1;
    for (i = 0; i < npre; i++) {
        for (k = 0; k < 11 && pre[i].name[k] == n[k]; k++)
            ;
        if (k == 11) {
            f->drive = (unsigned char)drive;
            f->attr = 0;
            f->first = (unsigned)i;
            f->size = pre[i].size;
            f->cache_clus = 0;
            f->cache_pos = 0;
            return 0;
        }
    }
    return -1;
}

static int __far pre_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
                    unsigned off, unsigned len)
{
    struct pre_file *p = &pre[f->first];
    if (pos >= p->size)
        return 0;
    if (len > p->size - (unsigned)pos)
        len = p->size - (unsigned)pos;
    scratch2_map((unsigned long)PRE_SEG << 4, PRE_MAX - 1);
    far_copy(sel, off, SCRATCH2, p->off + (unsigned)pos, len);
    return (int)len;
}

static int __far pre_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d)
{
    (void)drive; (void)dir; (void)n; (void)d;
    return -1;
}

static int __far pre_isdir(int drive, const char *path)
{
    (void)drive;
    return path[0] == '\\' && path[1] == 0;
}

struct ifs_driver pre_driver = {
    "BOOT", pre_mount, pre_open, pre_read, pre_readdir, pre_isdir
};
