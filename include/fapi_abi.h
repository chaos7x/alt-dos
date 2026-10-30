/*
 * FAPI - Family API, Aufrufschnittstelle (AT-DOS 5.0, alternative Zeitlinie)
 *
 * Anwendungen laufen in Ring 3 und haben keinen Zugriff auf Hardware,
 * I/O-Ports oder fremden Speicher. Der einzige Weg nach unten ist das
 * FAPI-Call-Gate im GDT:
 *
 *     call far FAPI_GATE:0000
 *
 *     AX = Funktionsnummer, BX/CX/DX = Argumente, Rueckgabe in AX
 *     Zeiger-Argumente sind near und beziehen sich auf das DS des Aufrufers.
 */
#ifndef FAPI_ABI_H
#define FAPI_ABI_H

#define FAPI_GATE           0x23    /* GDT-Eintrag 4, RPL 3            */

#define FAPI_GETVERSION     0x00    /* -> AX = 0x0500 (Major<<8|Minor)  */
#define FAPI_VIOWRTSTR      0x01    /* BX = String, CX = Zeile<<8|Spalte */
#define FAPI_YIELD          0x02    /* CPU kooperativ abgeben           */
#define FAPI_EXIT           0x03    /* Task beenden                     */
#define FAPI_SLEEP          0x04    /* BX = Millisekunden               */
#define FAPI_MEMINFO        0x05    /* BX = Puffer fuer struct fapi_meminfo */
#define FAPI_ALLOCSEG       0x06    /* BX = Groesse (0 = 64 KB) -> Selektor */
#define FAPI_FREESEG        0x07    /* BX = Selektor                    */
#define FAPI_KBDCHARIN      0x08    /* -> AX = Zeichen | Scancode << 8  */
#define FAPI_KBDPEEK        0x09    /* wie KBDCHARIN, wartet nicht, 0 = nichts */
#define FAPI_VIOWRTTTY      0x0A    /* BX = Text, CX = Laenge           */
#define FAPI_VIOCLS         0x0B    /* Bildschirm der Sitzung loeschen  */
#define FAPI_EXECPGM        0x0C    /* BX = Befehlszeile, CX = EXEC_*   */
#define FAPI_GETARGS        0x0D    /* BX = Puffer (FAPI_ARGLEN Bytes)  */
#define FAPI_OPEN           0x0E    /* BX = Dateiname -> Handle         */
#define FAPI_READ           0x0F    /* BX = Handle, CX = Puffer, DX = Laenge -> Bytes */
#define FAPI_CLOSE          0x10    /* BX = Handle                      */
#define FAPI_FINDENTRY      0x11    /* BX = struct fapi_dirent, CX = Nummer,
                                       DX = Verzeichnis (0 = aktuelles)  */
#define FAPI_KBDSETLAYOUT   0x12    /* BX = .KBL-Daten (278 Bytes), 0 = eingebaut */
#define FAPI_KBDGETLAYOUT   0x13    /* BX = Puffer (4 Bytes) fuer die Kennung */
#define KBL_SIZE_BYTES      278
#define FAPI_CHDIR          0x14    /* BX = Pfad                        */
#define FAPI_QCURDIR        0x15    /* BX = Puffer (130 Bytes) -> "C:\DOS" */
#define FAPI_SELECTDISK     0x16    /* BX = Laufwerk (0 = A:)           */
#define FAPI_QCURDISK       0x17    /* -> AX = Laufwerk (0 = A:)        */
#define FAPI_CREATE         0x18    /* BX = Dateiname -> Handle (neu oder gekuerzt) */
#define FAPI_WRITE          0x19    /* BX = Handle, CX = Puffer, DX = Laenge -> Bytes */
#define FAPI_DELETE         0x1A    /* BX = Dateiname                   */
#define FAPI_MKDIR          0x1B    /* BX = Pfad                        */
#define FAPI_RMDIR          0x1C    /* BX = Pfad (Verzeichnis muss leer sein) */
#define FAPI_MOVE           0x1D    /* BX = alter Pfad, CX = neuer Pfad (gleiches Laufwerk) */
#define FAPI_SHUTDOWN       0x1F    /* BX = 0 anhalten (ausschalten), 1 Neustart; kehrt nicht zurueck */
#define FAPI_FREESPACE      0x1E    /* BX = Laufwerk (0 = A:), CX = Puffer (4 Bytes) -> freie Bytes */

/* Fehler: alle Werte ab FAPI_ERR_FIRST. FAPI_ERROR heisst "nicht gefunden
   oder unzulaessig", die uebrigen sagen genauer, was schiefging. */
#define FAPI_ERROR          0xFFFF
#define FAPI_ERR_WP         0xFFFE  /* Datentraeger schreibgeschuetzt   */
#define FAPI_ERR_FULL       0xFFFD  /* Datentraeger oder Verzeichnis voll */
#define FAPI_ERR_EXIST      0xFFFC  /* Ziel gibt es schon               */
#define FAPI_ERR_DENIED     0xFFFB  /* Attribut verbietet es / nicht leer */
#define FAPI_ERR_INUSE      0xFFFA  /* Datei ist geoeffnet / Verzeichnis in Benutzung */
#define FAPI_ERR_FIRST      0xFFF0

#define FAPI_VERSION        0x0500
#define FAPI_ARGLEN         81
#define FAPI_PATHLEN        261     /* Pfade bis 260 Zeichen + Nullbyte */
#define FAPI_CURDIRLEN      130

/* DosExecPgm */
#define EXEC_SYNC           0       /* in dieser Sitzung, Aufrufer wartet */
#define EXEC_ASYNC          1       /* eigene Sitzung, laeuft nebenher    */
#define EXEC_ERR_NOTFOUND   1
#define EXEC_ERR_NOMEM      2
#define EXEC_ERR_FORMAT     3
#define EXEC_ERR_NOSLOT     4

/*
 * Speicher aus Sicht der FAPI: ein einziger Arbeitsspeicher. Die Trennung
 * in "konventionell" und "erweitert" ist ein Real-Mode-Artefakt und wird
 * nicht nach oben gereicht; das Loch 640 KB - 1 MB verwaltet der Kernel.
 */
struct fapi_meminfo {               /* alle Angaben in KB               */
    unsigned short total_kb;        /* Arbeitsspeicher insgesamt        */
    unsigned short free_kb;         /* frei                             */
    unsigned short kernel_kb;       /* vom Kernel reserviert            */
    unsigned short apps_kb;         /* fuer Programmabbilder reserviert */
    unsigned short alloc_kb;        /* per DosAllocSeg vergeben         */
};

struct fapi_dirent {                /* DosFindEntry                     */
    char           name[256];       /* Dateiname, auch fuer lange Namen */
    unsigned char  attr;            /* FAT-Attribute (10h = Verzeichnis) */
    unsigned long  size;
    unsigned short date, time;      /* FAT-Format                       */
};

#endif
