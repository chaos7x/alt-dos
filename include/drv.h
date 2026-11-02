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
#define DRV_READ        1           /* count Bloecke lesen (512, CD 2048 Bytes) */
#define DRV_WRITE       2           /* einen Block schreiben (nur 512)  */
#define DRV_FLUSH       3           /* Schreib-Cache der Geraete leeren  */
#define DRV_AUDIO       4           /* Audio: ES:BX = struct aud_req     */

#define DRV_MAXUNITS    8

#define DRV_OK          0
#define DRV_ERR_NODEV   1           /* kein Geraet gefunden             */
#define DRV_ERR_IO      2           /* Lese-/Schreibfehler              */
#define DRV_ERR_CMD     3           /* unbekannter Befehl               */
#define DRV_ERR_WP      4           /* schreibgeschuetzt                */
#define DRV_ERR_NOTREADY 5          /* kein Medium im Laufwerk (CD)     */

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
    unsigned char  count;           /* READ: Bloecke am Stueck (0 = 1);
                                       INIT: Rueckgabe Hoechstzahl je READ
                                       (0 = 1, alte Treiber)              */
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
#define DH_BIOSDISK     4           /* BX = Platte 0/1 (BIOS 80h/81h), CX = Ziel
                                       (16 Byte, DS des Treibers) -> Tabelle
                                       hinter INT 41h/46h vom Start; AX = 0 ok,
                                       1 = BIOS kennt so viele Platten nicht */
#define DH_SETIRQ       5           /* BX = IRQ (3-15, nicht 6), CX = Einsprung
                                       im Treibercode (fern, endet mit RETF;
                                       dcrt0: irq_entry) -> AX = 0 ok,
                                       1 = belegt/ungueltig. Der Kernel ruft ihn
                                       mit DS = Treiberdaten, AX = IRQ, bei
                                       gesperrten Interrupts, und schickt danach
                                       das EOI an den PIC. */
#define DH_UNSETIRQ     6           /* BX = IRQ: wieder maskieren, freigeben */
#define DH_PROCBLOCK    7           /* BX = Ereignis (frei waehlbar, je Treiber
                                       eindeutig), CX = Frist in Takten (0 = keine).
                                       Mit CLI rufen, nachdem die Bedingung
                                       geprueft ist: der Task schlaeft, andere
                                       laufen weiter. Kehrt mit STI zurueck;
                                       AX = 0 geweckt, 1 = Frist abgelaufen */
#define DH_PROCRUN      8           /* BX = Ereignis: alle darauf schlafenden
                                       Tasks wecken (auch aus der Interrupt-
                                       routine) -> AX = Anzahl */
#define DH_MOUREG       9           /* BX = Anzahl Maustasten: Maus anmelden.
                                       Ein Treiber ohne Blockgeraete meldet
                                       beim INIT units = 0 und DRV_OK */
#define DH_MOUEVENT     10          /* aus der Interruptroutine: BX = Tasten
                                       (MOU_* aus fapi_abi.h), CX = dx, DX = dy
                                       (Mickeys, vorzeichenbehaftet, dy > 0
                                       nach unten) */
#define DH_KBDLOCK      11          /* BX Bit 0 = 1: Tastatur-IRQ 1 maskieren
                                       (der Treiber spricht selbst mit dem
                                       Tastaturcontroller, z. B. PS/2-Maus
                                       erkennen), 0: freigeben; Takt laeuft
                                       weiter. Bit 1: AUX-Kanal (PS/2-Maus) ist
                                       an - IRQ 1 laesst Bytes mit Statusbit 5
                                       liegen (beim AT-Controller heisst das Bit
                                       etwas anderes, daher nur auf Ansage) */

#define DH_DMABUF       12          /* BX = Bytes: Puffer fuer ISA-DMA (ohne
                                       64-KB-Grenze, unter 16 MB), CX = 4 Byte
                                       im Treiber-DS fuer die physische
                                       Adresse -> AX = Selektor, 0 = keiner */
#define DH_AUDREG       13          /* Treiber ist das Audiogeraet (FAPI_AUDIO
                                       kommt als DRV_AUDIO an)           */

/* Audio (DRV_AUDIO): der Kernel waehlt, wessen Ton zu hoeren ist, der
   Treiber spielt einen Datenstrom */
struct aud_req {
    unsigned char  cmd;             /* DRV_AUDIO                        */
    unsigned char  fn;              /* AR_*                             */
    unsigned short status;          /* DRV_OK oder DRV_ERR_*            */
    unsigned short a1, a2, a3;
    unsigned short sel, off, len;   /* WRITE/INFO: Puffer des Programms */
    unsigned short result;
};
#define AR_CHECK        0           /* a1 = Rate, a2 = Kanaele, a3 = Bits: geht das? */
#define AR_START        1           /* Format einstellen (Wiedergabe beginnt mit
                                       den ersten Daten)                 */
#define AR_WRITE        2           /* sel:off, len -> result = Bytes; wartet,
                                       bis Platz im Puffer ist           */
#define AR_DRAIN        3           /* warten, bis alles gespielt ist    */
#define AR_STOP         4           /* sofort aufhoeren                  */
#define AR_MIXER        5           /* a1 = AUDMIX_*, a2 = 0-100 / FFFFh -> result */
#define AR_INFO         6           /* sel:off <- Name der Karte (64 Byte) */

#endif
