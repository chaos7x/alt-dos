/*
 * DOS 5.0 - Installable File System (IFS)
 *
 * Dateisysteme sind austauschbare Treiber hinter einer festen Schnittstelle.
 * Der Kernel kennt kein FAT, sondern nur struct ifs_driver. FAT12 ist der
 * erste Treiber und steckt fest im Kernel, weil von ihm gebootet wird;
 * weitere Dateisysteme sollen spaeter als .IFS-Dateien nachgeladen werden.
 */
#ifndef IFS_H
#define IFS_H

struct ifs_file {
    unsigned       first;               /* treiberspezifisch (FAT: Cluster) */
    unsigned long  size;
    unsigned       cache_clus;          /* zuletzt gelesene Stelle, damit   */
    unsigned long  cache_pos;           /* fortlaufendes Lesen schnell ist  */
};

struct ifs_dirent {
    char           name[13];            /* "NAME.EXT"                       */
    unsigned char  attr;
    unsigned long  size;
    unsigned short date, time;          /* FAT-Format                       */
};

struct ifs_driver {
    const char *name;
    int  (*mount)(void);
    int  (*open)(const char *name, struct ifs_file *f);     /* 0 = ok */
    /* ab Position 'pos' bis zu 'len' Bytes nach sel:off -> Bytes, -1 */
    int  (*read)(struct ifs_file *f, unsigned long pos,
                 unsigned sel, unsigned off, unsigned len);
    /* n-ter Eintrag im Stammverzeichnis, 0 = ok, -1 = Ende */
    int  (*readdir)(unsigned n, struct ifs_dirent *d);
};

extern struct ifs_driver fat12_driver;

/* einziges eingehaengtes Dateisystem (Laufwerk A:) */
int ifs_mount(struct ifs_driver *drv);
int ifs_open(const char *name, struct ifs_file *f);
int ifs_read(struct ifs_file *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len);
int ifs_readdir(unsigned n, struct ifs_dirent *d);
const char *ifs_name(void);

#endif
