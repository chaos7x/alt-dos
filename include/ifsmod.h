/*
 * AT-DOS 5.0 - gemeinsame Definitionen fuer Dateisysteme
 *
 * Wird vom Kernel (kernel/ifs.h) und von nachladbaren Dateisystemen
 * (IFS=, z. B. ISO9660.IFS) benutzt. Ein IFS-Modul ist eine FX-Datei mit
 * Treiber-Flag und Treiberkopf "DRV1" wie ein Geraetetreiber. Initialisiert
 * wird es mit struct drv_req (DRV_INIT, Rueckgabe units = Medienklassen,
 * IFS_CLASS_*), danach ruft der Kernel es mit struct ifs_req auf. Bloecke
 * liest es ueber DevHlp DH_BLKREAD in den DMA-Puffer (buf_sel aus INIT).
 * Nachgeladene Dateisysteme sind vorerst nur lesbar.
 */
#ifndef IFSMOD_H
#define IFSMOD_H

#define IFS_PATHLEN 261             /* kanonischer Pfad intern (260 + 0)  */
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
    unsigned       dir_clus, dir_idx;   /* Verzeichniseintrag (FAT)         */
    unsigned char  flags;               /* IFS_F_*                          */
    unsigned long  fsdata;              /* frei fuer das Dateisystem (ISO: Extent) */
};
#define IFS_F_WRITE  0x01               /* zum Schreiben geoeffnet          */
#define IFS_F_DIRTY  0x02               /* Eintrag beim Schliessen schreiben */

/* Fehler der schreibenden Funktionen (negativ) */
#define IFS_ERR      (-1)               /* nicht gefunden, E/A-Fehler       */
#define IFS_WP       (-2)               /* schreibgeschuetzt                */
#define IFS_FULL     (-3)               /* Datentraeger/Verzeichnis voll    */
#define IFS_EXIST    (-4)               /* Ziel gibt es schon               */
#define IFS_DENIED   (-5)               /* Schreibschutzattribut, nicht leer */

struct ifs_dirent {
    char           name[IFS_NAMELEN];   /* langer Name oder "NAME.EXT"      */
    unsigned char  attr;
    unsigned long  size;
    unsigned short date, time;          /* FAT-Format                       */
    /* bis hier wie struct fapi_dirent (geht so an die Programme) */
    char           sname[13];           /* 8.3-Name, "" = name ist schon 8.3 */
};

/* ---- Aufrufe an ein IFS-Modul ------------------------------------------ */
#define IFS_CLASS_CD    0x01        /* Geraete mit 2048-Byte-Bloecken      */

#define IFSM_MOUNT      0x10        /* Datentraeger pruefen (bei jedem Zugriff) */
#define IFSM_OPEN       0x11        /* path -> f                           */
#define IFSM_READ       0x12        /* f, pos, sel:off, len -> result      */
#define IFSM_READDIR    0x13        /* path, n -> de                       */
#define IFSM_ISDIR      0x14        /* path -> status 0 = Verzeichnis      */
#define IFSM_LABEL      0x15        /* -> de.name = Bezeichnung (11 Zeichen) */
#define IFSM_NAMEPATH   0x16        /* path (gross) -> path in Schreibung des
                                       Datentraegers; n = 1: 8.3-Namen    */

struct ifs_req {
    unsigned char  cmd;             /* IFSM_*                              */
    unsigned char  rsv;
    unsigned short status;          /* 0 = ok                              */
    unsigned short dev;             /* Blockgeraet fuer DH_BLKREAD         */
    unsigned short sel, off, len;   /* READ: Ziel                          */
    unsigned long  pos;             /* READ: Position in der Datei         */
    unsigned short n;               /* READDIR: Nummer des Eintrags        */
    unsigned short result;          /* READ: gelesene Bytes                */
    struct ifs_file   f;
    struct ifs_dirent de;
    char           path[IFS_PATHLEN];   /* kanonisch, "\\DIR\\DATEI"       */
};

#endif
