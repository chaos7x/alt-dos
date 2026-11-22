/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 6.3 - DEM (DOS-Emulationsmodul): Auftragsblock zwischen dem DOS
 * einer DOS-Box und der Dateiarbeit im Kernel (docs/DOSKERN.md, Stufe 1)
 *
 * Wie bei NTVDM (NTDOS -> BOP -> DEM): das DOS in der Box fuehrt seine
 * Strukturen (SFT, CDS, SDA) selbst, die eigentliche Dateiarbeit erledigt
 * das DEM mit den FAPI-Funktionen im Task der Box. Eingaenge heute: der
 * Redirector (INT 2Fh 11xxh, kernel/redir.c) fuer FreeDOS und die
 * MKDOSIMG-Abbilder; spaeter die Falle des eigenen Box-Kerns (ATDOSCM.SYS).
 *
 * Vertrag fuer 7.0: nur Felder fester Breite, Adressen in der Box linear
 * (Segment * 16 + Offset), keine Segmentregister, kein Wissen ueber SFT/
 * SDA/CDS. Fehler als DOS-Fehlercode (err, 0 = ok).
 */
#ifndef DEM_H
#define DEM_H

typedef unsigned char  dem_u8;
typedef unsigned short dem_u16;
typedef unsigned long  dem_u32;         /* 32 Bit (Watcom 16 Bit)        */

/* Auftraege (fn) */
#define DEM_OPEN        1   /* path, mode, act, attr -> handle, act, attr,
                               time, date, size                          */
#define DEM_CLOSE       2   /* handle, mode, flags (DEM_F_TIME: time/date
                               setzen)                                   */
#define DEM_READ        3   /* handle, pos, size (bisher), buf, count ->
                               count, size                               */
#define DEM_WRITE       4   /* wie READ; count 0 = Groesse auf pos setzen */
#define DEM_SIZE        5   /* handle, size (bisher) -> size (groesste,
                               auch aus anderen Oeffnungen der Box)     */
#define DEM_LOCK        6   /* handle, pos, count = Laenge, flags
                               (DEM_F_UNLOCK)                            */
#define DEM_MKDIR       7   /* path                                      */
#define DEM_RMDIR       8   /* path                                      */
#define DEM_CHDIR       9   /* path: gibt es das Verzeichnis?            */
#define DEM_GETATTR     10  /* path -> attr, time, date, size            */
#define DEM_SETATTR     11  /* path, attr                                */
#define DEM_RENAME      12  /* path -> path2 (auch mit Platzhaltern)     */
#define DEM_DELETE      13  /* path (auch mit Platzhaltern)              */
#define DEM_FINDFIRST   14  /* path (Muster), attr = Suchattribut ->
                               find (Zustand), name, attr, time, date,
                               size                                      */
#define DEM_FINDNEXT    15  /* find -> wie FINDFIRST                     */
#define DEM_SPACE       16  /* drive -> count = Sektoren je Cluster,
                               attr = Bytes je Sektor, size = Cluster,
                               pos = freie Cluster (je hoechstens FFF0h) */
#define DEM_ABSREAD     18  /* drive, pos = Sektor, count = Sektoren, buf;
                               flags 1 = Paketform (INT 25h CX=FFFFh) ->
                               err = INT-25h-Fehler (AH BIOS, AL Treiber) */
#define DEM_DLOCK       19  /* drive, flags 1 = entsperren: Laufwerks-
                               sperre wie MS-DOS 7 (440Dh 4Ah/6Ah)        */
#define DEM_ABSWRITE    20  /* wie ABSREAD, nur mit Sperre (sonst 0300h)
                               - ABSREAD/ABSWRITE flags 2 = 7305h (auch
                               FAT32; INT 25h/26h dort 0207h)             */
#define DEM_LPATH       21  /* path (lang oder kurz, ohne . und ..) ->
                               path = kurze Form, path2 = lange Form, attr,
                               time, date, size; fehlt nur der letzte Teil:
                               E_NOFILE (2), letzter Teil unveraendert
                               angehaengt; fehlt ein Verzeichnis: 03h     */
#define DEM_LFIND       22  /* path = "X:\KURZ\Muster" (Muster lang), attr
                               = erlaubte (unten) | verlangte (oben)
                               Attribute, find[0/1] = Eintrag -> naechster;
                               path2 = langer, name = kurzer Name (FCB-Form),
                               attr, time, date, size; 12h = keine mehr  */
#define DEM_FCLUS       23  /* path (kurz) -> pos = erster Cluster (440Dh
                               0871h)                                    */
#define DEM_DINFO       17  /* drive, buf: 32 Byte Laufwerksinfo (Art,
                               Cluster, Seriennummer, Bezeichnung, FAT)  */

/* act bei DEM_OPEN: Wunsch wie INT 21h 6Ch (Bits 0-3: vorhanden 0 Fehler,
   1 oeffnen, 2 ersetzen; Bits 4-7: fehlt 0 Fehler, 1 anlegen); zurueck
   1 geoeffnet, 2 angelegt, 3 ersetzt */
#define DEM_ACT_OPEN    0x01            /* 3Dh                           */
#define DEM_ACT_CREATE  0x12            /* 3Ch: ersetzen oder anlegen    */
#define DEM_ACT_NEW     0x10            /* 5Bh: nur anlegen              */

#define DEM_F_TIME      1               /* CLOSE: time/date setzen       */
#define DEM_F_UNLOCK    1               /* LOCK: freigeben               */

#define DEM_NOHANDLE    0xFFFFUL
#define DEM_PATHLEN     261             /* Pfade bis 260 Zeichen (LFN)   */
#define DEM_R_LONG      1               /* rsv bei DEM_OPEN/MKDIR/RENAME:
                                           letzter Teil ist ein langer
                                           Name, nicht als 8.3 vergleichen */

struct dem_req {
    dem_u16 fn, err;                    /* Auftrag; DOS-Fehler (0 ok)     */
    dem_u16 mode;                       /* DOS-Modus (Zugriff 0-2,
                                           Freigabe 4-6)                  */
    dem_u16 act;                        /* DEM_OPEN: Wunsch / Ergebnis    */
    dem_u16 attr, time, date;
    dem_u16 flags;
    dem_u16 drive;                      /* 0 = A:                        */
    dem_u16 rsv;                        /* DEM_R_LONG                    */
    dem_u32 handle;                     /* DEM-Handle                    */
    dem_u32 pos, size, count;
    dem_u32 buf;                        /* linear im Speicher der Box     */
    dem_u8  name[11];                   /* gefunden: Name in FCB-Form     */
    dem_u8  find[21];                   /* Suchzustand (DOS legt ihn ab)  */
    char    path[DEM_PATHLEN];          /* "X:\DIR\NAME"                  */
    char    path2[DEM_PATHLEN];
};

#endif
