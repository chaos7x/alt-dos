/*
 * AT-DOS 5.0 - Installable File System (IFS) und Laufwerke
 *
 * Dateisysteme sind austauschbare Treiber hinter einer festen Schnittstelle
 * (struct ifs_driver). FAT (12 und 16 Bit) steckt fest im Kernel, wie bei
 * OS/2; weitere Dateisysteme sollen als .IFS-Dateien nachgeladen werden.
 *
 * Ein Laufwerk (A:, C:, ...) ist ein eingehaengtes Dateisystem auf einem
 * Blockgeraet ab einer Startsektor-Nummer (Partition). Pfade an die Treiber
 * sind kanonisch: ohne Laufwerk, beginnen mit '\', Grossbuchstaben, ohne
 * "." und "..", z. B. "\DOS\MEM.EXE" oder "\" fuer das Stammverzeichnis.
 */
#ifndef IFS_H
#define IFS_H

#define NDRIVE      26
#define IFS_PATHLEN 128             /* kanonischer Pfad intern            */
#define IFS_NAMELEN 256             /* Name in Verzeichniseintraegen      */

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_DIR    0x10

struct ifs_file {
    unsigned char  drive;
    unsigned char  attr;
    unsigned       first;               /* treiberspezifisch (FAT: Cluster) */
    unsigned long  size;
    unsigned       cache_clus;          /* zuletzt gelesene Stelle, damit   */
    unsigned long  cache_pos;           /* fortlaufendes Lesen schnell ist  */
};

struct ifs_dirent {
    char           name[IFS_NAMELEN];   /* "NAME.EXT"                       */
    unsigned char  attr;
    unsigned long  size;
    unsigned short date, time;          /* FAT-Format                       */
};

struct volume {
    unsigned char  used;
    unsigned char  fat16;               /* sonst FAT12                      */
    int            blk;                 /* Blockgeraet                      */
    unsigned long  start;               /* erster Sektor der Partition      */
    unsigned char  spc, spc_shift;      /* Sektoren je Cluster              */
    unsigned       rootents;
    unsigned long  fat_lba, root_lba, data_lba;
    unsigned       clusters;
    unsigned long  sectors;
    struct ifs_driver *drv;
};

struct ifs_driver {
    const char *name;
    int  (*mount)(int drive);                               /* 0 = ok */
    int  (*open)(int drive, const char *path, struct ifs_file *f);
    /* ab Position 'pos' bis zu 'len' Bytes nach sel:off -> Bytes, -1 */
    int  (*read)(struct ifs_file *f, unsigned long pos,
                 unsigned sel, unsigned off, unsigned len);
    /* n-ter Eintrag im Verzeichnis 'dir', 0 = ok, -1 = Ende */
    int  (*readdir)(int drive, const char *dir, unsigned n, struct ifs_dirent *d);
    /* ist 'path' ein Verzeichnis? 1 = ja */
    int  (*isdir)(int drive, const char *path);
};

extern struct volume vol[NDRIVE];
extern struct ifs_driver fat_driver, pre_driver;
extern int floppy_lazy;

int ifs_mount(int drive, int blk, unsigned long start, struct ifs_driver *drv);
int ifs_open(int drive, const char *path, struct ifs_file *f);
int ifs_read(struct ifs_file *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len);
int ifs_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d);
int ifs_isdir(int drive, const char *path);

#endif
