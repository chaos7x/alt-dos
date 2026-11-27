/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Format von BACKUP/RESTORE wie MS-DOS 3.3-5.0 (Aufbau nachgelesen in
 * MS-DOS 4.0 CMD/BACKUP/BACKUP.H, MIT; Code eigen). Je Datentraeger:
 *
 *   BACKUP.nnn   die gesicherten Dateien hintereinander
 *   CONTROL.nnn  Kopf (139 Byte), dann je Verzeichnis ein Block (70 Byte)
 *                und dahinter je Datei(teil) ein Kopf (34 Byte)
 *   NAMES.nnn    (nur AT-DOS) lange Namen der Eintraege dieses Datentraegers
 *
 * nnn = Nummer des Datentraegers (001 ...). Disketten/Wechselplatten: im
 * Stamm, Bezeichnung "BACKUP  nnn"; Festplatten: in \BACKUP. Dateien, die
 * nicht mehr auf eine Diskette passen, werden geteilt (FH_FSEQ 1, 2, ...,
 * Bit 0 von FH_FLAGS nur am letzten Teil).
 */
#ifndef BKFMT_H
#define BKFMT_H

#define DH_LEN   139
#define DB_LEN    70
#define FH_LEN    34

/* Kopf von CONTROL.nnn */
#define DH_ID      1        /* "BACKUP  " (8)                       */
#define DH_SEQ     9        /* Nummer des Datentraegers              */
#define DH_LAST  138        /* FFh = letzter Datentraeger            */

/* Verzeichnisblock */
#define DB_PATH    1        /* Pfad ohne Laufwerk und fuehrendes \, 63 */
#define DB_NUM    64        /* Zahl der Dateikoepfe dahinter (Wort)    */
#define DB_NEXT   66        /* Stelle des naechsten Blocks, FFFFFFFFh  */
#define DB_PATHMAX 63

/* Dateikopf */
#define FH_NAME    1        /* "NAME.EXT", mit Nullen aufgefuellt (12) */
#define FH_FLAGS  13        /* Bit 0 letzter Teil, Bit 1 erfolgreich   */
#define FH_FLEN   14        /* Groesse der ganzen Datei                */
#define FH_FSEQ   18        /* Teil 1, 2, ... (Wort)                   */
#define FH_BEGIN  20        /* Stelle in BACKUP.nnn                    */
#define FH_PART   24        /* Laenge dieses Teils                     */
#define FH_ATTR   28        /* Attribut (Wort)                         */
#define FH_TIME   30
#define FH_DATE   32

#define FHF_LAST   1
#define FHF_OK     2

#define NAMES_ID  "ATDOS-NAMES 1"

#ifndef BK_READONLY
static void bk_put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void bk_put32(unsigned char *p, unsigned long v)
{
    bk_put16(p, (unsigned)v);
    bk_put16(p + 2, (unsigned)(v >> 16));
}
#endif

static unsigned bk_get16(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static unsigned long bk_get32(const unsigned char *p)
{
    return bk_get16(p) | (unsigned long)bk_get16(p + 2) << 16;
}

/* "nnn" */
static void bk_ext(char *o, unsigned n)
{
    o[0] = (char)('0' + n / 100);
    o[1] = (char)('0' + n / 10 % 10);
    o[2] = (char)('0' + n % 10);
    o[3] = 0;
}

#endif
