/*
 * FAPI - Family API, Aufrufschnittstelle (DOS 5.0, alternative Zeitlinie)
 *
 * Anwendungen laufen in Ring 3 und haben keinen Zugriff auf Hardware,
 * I/O-Ports oder fremden Speicher. Der einzige Weg nach unten ist das
 * FAPI-Call-Gate im GDT:
 *
 *     call far FAPI_GATE:0000
 *
 *     AX = Funktionsnummer, BX/CX = Argumente, Rueckgabe in AX
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

#define FAPI_ERROR          0xFFFF

#define FAPI_VERSION        0x0500

/*
 * Speicher aus Sicht der FAPI: ein einziger Arbeitsspeicher. Die Trennung
 * in "konventionell" und "erweitert" ist ein Real-Mode-Artefakt und wird
 * nicht nach oben gereicht; das Loch 640 KB - 1 MB verwaltet der Kernel.
 */
struct fapi_meminfo {               /* alle Angaben in KB               */
    unsigned short total_kb;        /* nutzbarer Arbeitsspeicher        */
    unsigned short kernel_kb;       /* vom Kernel belegt                */
    unsigned short apps_kb;         /* fuer Anwendungen reserviert      */
    unsigned short rsv;             /* reserviert, 0                    */
};

#endif
