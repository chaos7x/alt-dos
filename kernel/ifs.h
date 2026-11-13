/* SPDX-License-Identifier: GPL-2.0-or-later */
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
    unsigned char  vfat;                /* lange Namen als VFAT schreiben   */
};
/* Original-Schreibung des letzten Pfadteils beim Anlegen/Umbenennen
   (Pfade kommen kanonisch gross); "" = keine. Setzt der Kernel. */
extern char ifs_orig[];
extern unsigned char lfn_cfg;           /* LONGNAMES= 0 AUTO, 1 VFAT, 2 EA  */

struct ifs_driver {
    const char *name;
    int  (__far *mount)(int drive);                               /* 0 = ok */
    int  (__far *open)(int drive, const char *path, struct ifs_file __far *f);
    /* ab Position 'pos' bis zu 'len' Bytes nach sel:off -> Bytes, -1 */
    int  (__far *read)(struct ifs_file __far *f, unsigned long pos,
                 unsigned sel, unsigned off, unsigned len);
    /* n-ter Eintrag im Verzeichnis 'dir', 0 = ok, -1 = Ende */
    int  (__far *readdir)(int drive, const char *dir, unsigned n, struct ifs_dirent *d);
    /* ist 'path' ein Verzeichnis? 1 = ja */
    int  (__far *isdir)(int drive, const char *path);
    /* ab hier optional (0 = nur lesbar); Rueckgabe 0 oder IFS_* */
    int  (__far *create)(int drive, const char *path, struct ifs_file __far *f);
    int  (__far *write)(struct ifs_file __far *f, unsigned long pos,
                  unsigned sel, unsigned off, unsigned len);  /* -> Bytes */
    int  (__far *close)(struct ifs_file __far *f);
    int  (__far *remove)(int drive, const char *path);
    int  (__far *mkdir)(int drive, const char *path);
    int  (__far *rmdir)(int drive, const char *path);
    int  (__far *move)(int drive, const char *from, const char *to);
    int  (__far *space)(int drive, unsigned long *free_bytes);
    /* mask 1: Attribute (RO/H/S/A), 2: Datum/Uhrzeit (FAT-Format) */
    int  (__far *setinfo)(int drive, const char *path, unsigned mask,
                    unsigned attr, unsigned date, unsigned time);
    /* Bezeichnung (12 Bytes) und Seriennummer lesen; newlbl != 0: setzen
       ("" = loeschen) */
    int  (__far *label)(int drive, char *lbl, unsigned long *serial, const char *newlbl);
    /* kanonischen Pfad (gross) in Namen vom Datentraeger umsetzen:
       shortn = 1 nur 8.3 (Programme ohne FX_FLAG_LONGNAMES), 0 lange Namen
       in Originalschreibung; out IFS_PATHLEN; 0 = ok */
    int  (__far *namepath)(int drive, const char *path, char *out, int shortn);
    /* zum Schreiben geoeffnete Datei auf 'size' kuerzen (nicht laenger);
       0 = ok. Verlaengern macht ifs_setsize mit Nullen ueber write. */
    int  (__far *truncate)(struct ifs_file __far *f, unsigned long size);
};

extern struct volume vol[NDRIVE];
extern struct ifs_driver fat_driver, pre_driver, mod_driver;
extern int floppy_lazy;

int __far ifs_mount(int drive, int blk, unsigned long start, struct ifs_driver *drv);
void __far vol_move(int from, int to);
/* Namensgedaechtnis (fat.c, Unicode Stufe 1): CP437-Name <- UCS-2 */
void __far uni_note_ext(unsigned sel, unsigned name, unsigned ucs, unsigned len);
int __far  vol_present(int d);
void __far vol_reset(int d);
void __far ifs_shutdown(void);
int __far  ifs_setinfo(int d, const char *path, unsigned mask, unsigned attr,
                 unsigned date, unsigned time);
int __far  ifs_label(int d, char *lbl, unsigned long *serial, const char *newlbl);            /* alle FAT16-Platten als sauber markieren */              /* Caches weg, neu einhaengen       */            /* da (A: ggf. einhaengen), Sperre egal */
int __far ifs_open(int drive, const char *path, struct ifs_file __far *f);
int __far ifs_read(struct ifs_file __far *f, unsigned long pos, unsigned sel,
             unsigned off, unsigned len);
int __far ifs_readdir(int drive, const char *dir, unsigned n, struct ifs_dirent *d);
int __far ifs_isdir(int drive, const char *path);
int __far ifs_namepath(int drive, const char *path, char *out, int shortn);
int __far ifs_create(int drive, const char *path, struct ifs_file __far *f);
int __far ifs_write(struct ifs_file __far *f, unsigned long pos, unsigned sel,
              unsigned off, unsigned len);
int __far ifs_close(struct ifs_file __far *f);
int __far ifs_setsize(struct ifs_file __far *f, unsigned long size);   /* DosNewSize */
int __far ifs_remove(int drive, const char *path);
int __far ifs_mkdir(int drive, const char *path);
int __far ifs_rmdir(int drive, const char *path);
int __far ifs_move(int drive, const char *from, const char *to);
int __far ifs_space(int drive, unsigned long *free_bytes);

#endif
