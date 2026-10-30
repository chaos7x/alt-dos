/*
 * AT-DOS 5.0 - Schnittstelle zwischen Kernel und Geraetetreibern
 *
 * Geraetetreiber sind Dateien (.SYS) im FX-Format mit gesetztem Flag
 * FX_FLAG_DRIVER. Sie laufen in Ring 0 mit eigenem Code- und Datensegment
 * und werden ueber DEVICE= in der CONFIG.SYS geladen.
 *
 * Kernel -> Treiber: Fernaufruf der Strategieroutine, DS = Treiberdaten,
 *                    ES:BX = Anforderung (struct drv_req), SS = Kernelstack.
 * Treiber -> Kernel: Fernaufruf von DevHlp (Adresse in der INIT-Anforderung),
 *                    AX = Funktion, BX/CX/DX = Argumente, Rueckgabe in AX.
 *
 * Das Treiberabbild beginnt mit struct drv_header.
 */
#ifndef DRV_H
#define DRV_H

#define DRV_SIG         "DRV1"

struct drv_header {
    char           sig[4];          /* "DRV1"                           */
    unsigned short strategy;        /* Einsprung (near, Treibersegment) */
};

#define DRV_INIT        0           /* Geraet suchen, Einheiten melden  */
#define DRV_READ        1           /* einen Block lesen (512, CD 2048 Bytes) */
#define DRV_WRITE       2           /* einen Block schreiben (nur 512)  */
#define DRV_FLUSH       3           /* Schreib-Cache der Geraete leeren  */

#define DRV_MAXUNITS    8

#define DRV_OK          0
#define DRV_ERR_NODEV   1           /* kein Geraet gefunden             */
#define DRV_ERR_IO      2           /* Lese-/Schreibfehler              */
#define DRV_ERR_CMD     3           /* unbekannter Befehl               */
#define DRV_ERR_WP      4           /* schreibgeschuetzt                */

struct drv_req {
    unsigned char  cmd;
    unsigned char  unit;
    unsigned short status;          /* DRV_OK oder DRV_ERR_*            */
    unsigned long  lba;             /* READ/WRITE: Sektornummer         */
    unsigned long  buf_phys;        /* physische Adresse des Puffers    */
    unsigned short buf_off, buf_sel;    /* derselbe Puffer, Ring-0-Zeiger */
    unsigned long  base;            /* INIT: lineare Basis des Treibers */
    unsigned short devhlp_off, devhlp_sel;  /* INIT: Einsprung DevHlp   */
    unsigned char  units;           /* INIT: Anzahl Einheiten (Rueckgabe) */
    unsigned char  rsv;
    unsigned long  blocks[DRV_MAXUNITS];    /* INIT: Sektoren je Einheit */
    unsigned short bsize[DRV_MAXUNITS];     /* INIT: Blockgroesse, 0 = 512 (CD: 2048) */
    char           args[81];        /* INIT: Argumente aus CONFIG.SYS   */
};

/* DevHlp-Funktionen */
#define DH_PUTS         0           /* BX = Text (DS des Treibers) ins Startprotokoll */
#define DH_TICKS        1           /* -> AX = Systemtakt (100 Hz), untere 16 Bit */
#define DH_DELAY        2           /* BX = Takte warten                */
#define DH_BLKREAD      3           /* BX = Blockgeraet, CX = LBA unten, DX = LBA oben
                                       -> Block im DMA-Puffer, AX = 0 ok   */

#endif
