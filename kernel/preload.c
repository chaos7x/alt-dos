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
 * preload() laeuft im REAL MODE (CS=DS=SS=1000h): keine Selektoren, keine
 * Bildschirmausgabe ueber vio, nur Speicher per Segment:Offset. Fehler
 * merkt sich pre_err; kmain meldet sie spaeter.
 *
 * Nur FAT16 und nur Dateien im Stammverzeichnis (DEVICE=AHA154X.SYS).
 * NPRE begrenzt CONFIG.SYS plus Treiber und IFS-Module auf 8 Dateien.
 */
#include "kern.h"
#include "ifs.h"

#define PRE_SEG     0x0060          /* linear 0600h                       */
#define PRE_MAX     0x7600          /* bis 7BFFh, darueber der Bootsektor */
#define BUF_SEG     0x0800          /* linear 8000h (DMABUF)              */
#define NPRE        8

struct pre_file {
    char           name[12];        /* "AHA154X SYS" (FAT-Verzeichnisform) */
    unsigned       off, size;
};

struct pre_file pre[NPRE];
int             npre;
unsigned char   pre_err;            /* PRE_ERR_*                          */
unsigned long   boot_hidden;        /* Startsektor der Startpartition     */
unsigned long   boot_serial;        /* Seriennummer der Startpartition    */

/* FAT16 der Startpartition */
static unsigned char  spc;
static unsigned       rootents;
static unsigned long  fat_lba, root_lba, data_lba;
static unsigned       pre_used;

static unsigned char __far *bpb(void) { return MK_FP(0, 0x7C00); }
static unsigned char __far *buf(void) { return MK_FP(BUF_SEG, 0); }

static unsigned w16(unsigned char __far *p) { return p[0] | (p[1] << 8); }
static unsigned long w32(unsigned char __far *p)
{
    return w16(p) | ((unsigned long)w16(p + 2) << 16);
}

/* Name aus CONFIG.SYS ("C:\\DOS\\aha154x.sys") in Verzeichnisform; nur der
   letzte Teil zaehlt - gesucht wird im Stamm und in \DOS. 0 = ok */
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
static int scan_sec(const char *n, int dir, unsigned *clus, unsigned long *size)
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
            *clus = w16(d + 26);
            *size = w32(d + 28);
            return 1;
        }
    }
    return 0;
}

static int find_root(const char *n, int dir, unsigned *clus, unsigned long *size)
{
    unsigned s;
    int r;
    for (s = 0; s < (rootents >> 4); s++) {
        if (rm_read(root_lba + s))
            return -1;
        r = scan_sec(n, dir, clus, size);
        if (r)
            return r > 0 ? 0 : -1;
    }
    return -1;
}

/* Datei im Stamm, sonst in \DOS suchen -> erster Cluster, Groesse; 0 = ok */
static int find(const char *n, unsigned *clus, unsigned long *size)
{
    static const char dosdir[12] = "DOS        ";
    unsigned c, j;
    int r;

    if (find_root(n, 0, clus, size) == 0)
        return 0;
    if (find_root(dosdir, 1, &c, size))
        return -1;
    while (c >= 2 && c < 0xFFF8) {              /* Kette von \DOS      */
        for (j = 0; j < spc; j++) {
            if (rm_read(data_lba + ((unsigned long)(c - 2) << spc_shift()) + j))
                return -1;
            r = scan_sec(n, 0, clus, size);
            if (r)
                return r > 0 ? 0 : -1;
        }
        if (rm_read(fat_lba + (c >> 8)))
            return -1;
        c = w16(buf() + ((c & 0xFF) << 1));
    }
    return -1;
}

/* Datei der FAT-Kette nach in den Vorladebereich lesen; 0 = ok */
static int load(const char *n)
{
    struct pre_file *p;
    unsigned clus, k, i, j, len;
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
    while (len && clus >= 2 && clus < 0xFFF8) {
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
        if (rm_read(fat_lba + (clus >> 8))) {
            pre_err = PRE_ERR_READ;
            return -1;
        }
        clus = w16(buf() + ((clus & 0xFF) << 1));
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
    unsigned fatsz, i, n;
    char name[12];

    if (w16(b + 11) != 512 || b[38] != 0x29 || b[13] == 0 ||
        (b[13] & (b[13] - 1)) || w16(b + 22) == 0) {
        pre_err = PRE_ERR_FS;
        return;
    }
    spc = b[13];
    rootents = w16(b + 17);
    fatsz = w16(b + 22);
    boot_hidden = w32(b + 28);
    boot_serial = w32(b + 39);
    fat_lba = boot_hidden + w16(b + 14);
    root_lba = fat_lba;
    for (i = 0; i < b[16]; i++)
        root_lba += fatsz;
    data_lba = root_lba + (rootents >> 4);
    if (rm_geometry()) {
        pre_err = PRE_ERR_READ;
        return;
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
static int pre_mount(int drive) { (void)drive; return 0; }

static int pre_open(int drive, const char *path, struct ifs_file __far *f)
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

static int pre_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
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

static int pre_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d)
{
    (void)drive; (void)dir; (void)n; (void)d;
    return -1;
}

static int pre_isdir(int drive, const char *path)
{
    (void)drive;
    return path[0] == '\\' && path[1] == 0;
}

struct ifs_driver pre_driver = {
    "BOOT", pre_mount, pre_open, pre_read, pre_readdir, pre_isdir
};
