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

#include "../include/ifsmod.h"

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
    unsigned char  nfats;
    unsigned       spf;                 /* Sektoren je FAT                  */
    unsigned       hint;                /* hier beginnt die Suche nach Freiem */
    struct ifs_driver *drv;
    int            mod;                 /* nachgeladenes IFS: Treiberplatz */
    unsigned char  part;                /* Partition 1-4, 0 = ganzes Geraet */
    unsigned char  present;             /* Laufwerk da, auch ohne Dateisystem */
    unsigned char  ptype;               /* Partitionstyp (MBR), 0 = keiner  */
    unsigned long  psize;               /* Sektoren der Partition/des Geraets */
    unsigned char  nfree_ok;            /* nfree gezaehlt und gueltig       */
    unsigned char  locked;              /* DosOpenDrive exklusiv: kein IFS  */
    unsigned char  was_dirty;           /* beim Einhaengen nicht sauber abgemeldet */
    unsigned char  in_use;              /* Sauber-Bit auf der Platte geloescht */
    unsigned       nfree;               /* freie Cluster (FAT)              */
};

struct ifs_driver {
    const char *name;
    int  (*mount)(int drive);                               /* 0 = ok */
    int  (*open)(int drive, const char *path, struct ifs_file __far *f);
    /* ab Position 'pos' bis zu 'len' Bytes nach sel:off -> Bytes, -1 */
    int  (*read)(struct ifs_file __far *f, unsigned long pos,
                 unsigned sel, unsigned off, unsigned len);
    /* n-ter Eintrag im Verzeichnis 'dir', 0 = ok, -1 = Ende */
    int  (*readdir)(int drive, const char *dir, unsigned n, struct ifs_dirent *d);
    /* ist 'path' ein Verzeichnis? 1 = ja */
    int  (*isdir)(int drive, const char *path);
    /* ab hier optional (0 = nur lesbar); Rueckgabe 0 oder IFS_* */
    int  (*create)(int drive, const char *path, struct ifs_file __far *f);
    int  (*write)(struct ifs_file __far *f, unsigned long pos,
                  unsigned sel, unsigned off, unsigned len);  /* -> Bytes */
    int  (*close)(struct ifs_file __far *f);
    int  (*remove)(int drive, const char *path);
    int  (*mkdir)(int drive, const char *path);
    int  (*rmdir)(int drive, const char *path);
    int  (*move)(int drive, const char *from, const char *to);
    int  (*space)(int drive, unsigned long *free_bytes);
    /* mask 1: Attribute (RO/H/S/A), 2: Datum/Uhrzeit (FAT-Format) */
    int  (*setinfo)(int drive, const char *path, unsigned mask,
                    unsigned attr, unsigned date, unsigned time);
    /* Bezeichnung (12 Bytes) und Seriennummer lesen; newlbl != 0: setzen
       ("" = loeschen) */
    int  (*label)(int drive, char *lbl, unsigned long *serial, const char *newlbl);
    /* kanonischen Pfad (gross) in Namen vom Datentraeger umsetzen:
       shortn = 1 nur 8.3 (Programme ohne FX_FLAG_LONGNAMES), 0 lange Namen
       in Originalschreibung; out IFS_PATHLEN; 0 = ok */
    int  (*namepath)(int drive, const char *path, char *out, int shortn);
};

extern struct volume vol[NDRIVE];
extern struct ifs_driver fat_driver, pre_driver, mod_driver;
extern int floppy_lazy;

int ifs_mount(int drive, int blk, unsigned long start, struct ifs_driver *drv);
void vol_move(int from, int to);
int  vol_present(int d);
void vol_reset(int d);
void ifs_shutdown(void);
int  ifs_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                 unsigned date, unsigned time);
int  ifs_label(int d, char *lbl, unsigned long *serial, const char *newlbl);            /* alle FAT16-Platten als sauber markieren */              /* Caches weg, neu einhaengen       */            /* da (A: ggf. einhaengen), Sperre egal */
int ifs_open(int drive, const char *path, struct ifs_file __far *f);
int ifs_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len);
int ifs_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d);
int ifs_isdir(int drive, const char *path);
int ifs_namepath(int drive, const char *path, char *out, int shortn);
int ifs_create(int drive, const char *path, struct ifs_file __far *f);
int ifs_write(struct ifs_file __far *f, unsigned long pos, unsigned sel,
              unsigned off, unsigned len);
int ifs_close(struct ifs_file __far *f);
int ifs_remove(int drive, const char *path);
int ifs_mkdir(int drive, const char *path);
int ifs_rmdir(int drive, const char *path);
int ifs_move(int drive, const char *from, const char *to);
int ifs_space(int drive, unsigned long *free_bytes);

#endif
