/*
 * DOS 5.0 - Installable File System (IFS)
 *
 * Dateisysteme sind austauschbare Treiber hinter einer festen Schnittstelle.
 * Der Kernel kennt kein FAT, sondern nur struct ifs_driver. FAT12 ist der
 * erste Treiber; ein CD-ROM-Dateisystem kaeme spaeter genauso dazu.
 */
#ifndef IFS_H
#define IFS_H

struct ifs_file {
    unsigned       first;               /* treiberspezifisch (FAT: Cluster) */
    unsigned long  size;
};

struct ifs_driver {
    const char *name;
    int  (*mount)(void);
    int  (*open)(const char *name, struct ifs_file *f);     /* 0 = ok */
    /* ganze Datei nach linear 'dst' lesen, hoechstens 'max' Bytes */
    int  (*read)(struct ifs_file *f, unsigned long dst, unsigned max);
};

extern struct ifs_driver fat12_driver;

/* einziges eingehaengtes Dateisystem (Laufwerk A:) */
int ifs_mount(struct ifs_driver *drv);
int ifs_open(const char *name, struct ifs_file *f);
int ifs_read(struct ifs_file *f, unsigned long dst, unsigned max);
const char *ifs_name(void);

#endif
