/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - gemeinsame Kernel-Definitionen
 */
#ifndef KERN_H
#define KERN_H

#include <i86.h>

#pragma pack(1)

/* ---- GDT-Belegung ------------------------------------------------------ */
#define KCODE       0x08
#define KDATA       0x10
#define VIDEO       0x18
#define GATE_IDX    4               /* -> Selektor 0x20, RPL 3 = 0x23      */
#define TSS_K       0x28
#define SCRATCH     0x30            /* Kernel-Fenster auf beliebigen Speicher */
#define DMABUF      0x38            /* DMA-Puffer fuer Laufwerke           */
#define SCRATCH2    0x40            /* zweites Fenster, z. B. fuer Kopien   */

#define NTASK       8               /* Tasks und Sitzungen hoechstens       */
#define TSS_IDX(i)  (9 + (i))
#define LDT_IDX(i)  (9 + NTASK + (i))
#define SCR_IDX(i)  (9 + 2 * NTASK + (i))   /* Bildschirmpuffer je Sitzung */
#define TSS_SEL(i)  (TSS_IDX(i) * 8)
#define LDT_SEL(i)  (LDT_IDX(i) * 8)
#define SCR_SEL(i)  (SCR_IDX(i) * 8)
#define NDRV        6               /* Geraetetreiber und IFS-Module      */
#define DRVC_IDX(i) (9 + 3 * NTASK + 2 * (i))   /* Treibercode, Ring 0    */
#define DRVC_SEL(i) (DRVC_IDX(i) * 8)
#define DRVD_SEL(i) (DRVC_SEL(i) + 8)           /* Treiberdaten           */
#define NKSEG       12              /* eigene Kernelsegmente (kseg_alloc)  */
#define KSEG_IDX(i) (9 + 3 * NTASK + 2 * NDRV + (i))
#define KCODE2_IDX  (9 + 3 * NTASK + 2 * NDRV + NKSEG)
#define KCODE2      0x1C8           /* = KCODE2_IDX * 8; build.sh traegt ihn ein */
#define NGDT        (KCODE2_IDX + 1)
/* KSEG_IDX(0) ist fest die Speicher-Bitmap (MEMMAP_SEL in mem.h) */

/* Lage von Kernel, DMA-Puffer und Speicher-Bitmap: ab 5.1 kopiert sich
   der Kernel noch im Real Mode nach 1 MB (kmove_prep/kmove), wenn genug
   erweiterter Speicher da ist - 0-640 KB werden frei (Programme, spaeter
   die DOS-Box). Sonst wie 5.0: 10000h, 8000h, 9000h. */
extern unsigned long kcode_lin, kdata_lin, dmabuf_lin, memmap_lin;
extern unsigned char kmoved;        /* Kernel liegt ueber 1 MB           */
#define KMOVE_BASE  0x110000UL      /* hinter der HMA (gehoert der Box)   */
#define DMABUF_LIN  dmabuf_lin      /* keine 64-KB-Grenze                */
#define DMABUF_SIZE 2048            /* ein CD-Block; Platten nutzen 512    */

/* Kernel: Code ab 10000h, Daten ab dem naechsten Paragraphen dahinter */
#define KCODE_LIN   kcode_lin
extern unsigned kcode_size;         /* start.asm: Groesse von CGROUP       */
extern unsigned kcode2_size;        /* start.asm: Groesse von CGROUP2      */
#define KCODE2_LIN  (KCODE_LIN + ((kcode_size + 15) & ~15u))
extern unsigned kdata_seg;          /* start.asm: Real-Mode-Segment DGROUP */
extern unsigned kdata_size;         /* start.asm: Groesse von DGROUP       */
#define KDATA_LIN   kdata_lin
#define LIN(p)      (KDATA_LIN + (unsigned short)(p))

struct desc  { unsigned short limit, base_lo; unsigned char base_hi, access; unsigned short rsv; };

#define ACC_KDATA   0x92

extern struct desc gdt[];

void set_desc(int idx, unsigned long base, unsigned limit, unsigned char acc);
void scratch_map(unsigned long base, unsigned limit);
void scratch2_map(unsigned long base, unsigned limit);
void win_save(unsigned short *b);          /* SCRATCH/SCRATCH2, 8 Worte   */
void win_restore(const unsigned short *b);

/* ---- Assembler-Routinen (start.asm) ------------------------------------ */
void          outb(unsigned port, unsigned val);
unsigned      inb(unsigned port);
unsigned long get_ticks(void);
void          idle_wait(void);
void          enable_ints(void);
void          zero_seg(unsigned sel, unsigned words);
unsigned      irq_off(void);
void          irq_restore(unsigned flags);
void          halt_forever(void);
void          cpu_reset(void);

/* ---- Start von Festplatte (preload.c, start.asm) ----------------------- */
#define PRE_ERR_FS      1           /* Startpartition nicht FAT16          */
#define PRE_ERR_READ    2           /* BIOS-Lesefehler                     */
#define PRE_ERR_FULL    3           /* Vorladebereich voll                 */
#define PRE_DRIVE       25          /* Z:, nur waehrend CONFIG.SYS         */
extern unsigned char boot_drive;    /* BIOS-Nummer, ab 80h Festplatte      */
extern unsigned char pre_err;
extern unsigned long boot_hidden, boot_serial;
extern int npre;
void preload(void);                 /* nur im Real Mode                    */
int  rm_geometry(void);             /* INT 13h/08h fuer boot_drive, 0 = ok */
int  rm_read(unsigned long lba);    /* ein Sektor nach 0000:8000, 0 = ok   */

extern volatile unsigned char  fdc_irq;
extern volatile unsigned short uptime_s;
extern volatile unsigned short uptime_m;   /* Minuten seit dem Start */
extern volatile unsigned char up60;

/* ---- Hilfen (kernel.c) ------------------------------------------------- */
void far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n);
void delay_ticks(unsigned n);
void panic(const char *msg);
void reboot(void);
/* Kernelsegment ausserhalb von DGROUP (Tabellen, Puffer), genullt;
   -> Selektor oder 0. Fuer alles, was mit der Konfiguration waechst. */
unsigned kseg_alloc(unsigned long bytes);
void reboot_request(void);          /* Strg+Alt+Entf: erst Daten sichern   */
extern volatile unsigned reboot_count;
unsigned long rtc_stamp(void);      /* Datum << 16 | Uhrzeit im FAT-Format */

/* ---- Blockgeraet: Diskette A: (floppy.c) ------------------------------- */
/* floppy.c liegt in KCODE2: alle Einspruenge fern */
int  __far fd_init(unsigned cmos_types);    /* CMOS 10h; 0 = Controller da */
int  __far fd_write(int d, unsigned lba);   /* aus DMABUF in den Spurpuffer */
int  __far fd_read(int d, unsigned lba);    /* Sektor in den DMA-Puffer     */
int  __far fd_sync(void);                   /* Spurpuffer schreiben; -2 = WP */
int  __far fd_check(int d);                 /* 1 = gewechselt/neu erkannt   */
unsigned __far fd_size(int d);              /* Sektoren des Formats         */
int  __far fd_type(int d);                  /* CMOS-Typ, 0 = kein Laufwerk  */
int  __far fd_nomedia(int d);               /* 1 = keine Diskette drin      */
int  __far fd_format(int d, unsigned cyl, unsigned head, unsigned blocks);
void __far fd_tick(void);
void __far fd_motor_off(void);
unsigned kseg_dma(unsigned bytes, unsigned long *lin);  /* ohne 64-KB-Grenze */

/* Schlafen im Kernel wie DevHlp ProcBlock/ProcRun (mit gesperrten
   Interrupts rufen, kehrt mit STI zurueck; 0 geweckt, 1 Frist).
   Ereignisse des Kernels: Treibernummer FFh */
unsigned proc_block(unsigned long id, unsigned timeout);
unsigned proc_run(unsigned long id);
#define FD_EVENT    0xFF0006UL          /* Diskette, IRQ 6               */
#define SLEEP_EVENT 0xFF0000UL          /* niemand weckt: nur Frist      */

/* ---- Blockgeraete (kernel.c): Diskette und Einheiten der Treiber ------- */
#define NBLK        8
#define BLK_FLOPPY  0               /* Geraet 0 ist immer Diskette A:      */
#define IS_FLOPPY(dev) (blk[dev].drv < 0)   /* B: ist Geraet 1, wenn da   */
struct blkdev {
    signed char    drv;             /* Treiber, -1 = Diskette im Kernel     */
    unsigned char  unit;
    unsigned long  blocks;
    unsigned       bsize;           /* 512, CD-ROM 2048                    */
};
extern struct blkdev blk[NBLK];
extern int nblk;
int  blk_read(int dev, unsigned long lba);  /* -> DMABUF, 0 = ok         */
int  blk_read_n(int dev, unsigned long lba, unsigned n);  /* dazu n-1 voraus */
int  blk_write(int dev, unsigned long lba); /* DMABUF ->, -2 = schreibgesch. */
void blk_flush(void);               /* Platten-Caches leeren              */
void blk_invalidate(void);          /* Diskette aus dem Cache (Wechsel)    */
int  blk_media(int dev);            /* Diskette gewechselt? 1 = ja (neu)   */
int  blk_nomedia(int dev);          /* Diskettenlaufwerk leer? (nach blk_media) */
extern unsigned char ifs_notready;  /* im FAPI-Aufruf: Laufwerk leer       */
void blk_cache_init(unsigned kb);   /* CACHE=: Platten-Cache in KB         */
unsigned blk_cache_kb(void);
extern unsigned char bc_sync;       /* 1: blk_write sofort (Sauber-Bit)    */

/* ---- Sitzungen und Konsole (vio.c) ------------------------------------- */
#define SROWS       23              /* Zeilen 1-23, 0 Titel, 24 Status     */
#define SCOLS       80
#define SLINES_MAX  50              /* Bildschirmzeilen hoechstens (VGA)   */
#define SBYTES      (SLINES_MAX * SCOLS * 2)    /* Puffer reicht fuer alles */

int  vio_open(const char *title);   /* -> Sitzung oder -1                  */
void vio_close(int s);
void vio_title(int s, const char *title);
void vio_focus(int s);
void vio_next(void);
int  vio_fg(void);
int  vio_used(int s);
void vio_cls(int s);
void vio_tty(int s, unsigned sel, unsigned off, unsigned len);
void vio_str(int s, int row, int col, unsigned sel, unsigned off,
             unsigned limit, unsigned char attr);
void vio_kputs(int s, const char *str);     /* Kerneltext als TTY        */
int  vio_ansi(int s, unsigned on);          /* ANSI an/aus, 2 = abfragen */
void vio_status(void);
void vio_flush(void);               /* Schattenpuffer -> Bildspeicher      */
int  vio_rows(int s);               /* Zeilen der Sitzung (z. B. 23/25)    */
int  vio_setlines(int s, int n);    /* 25/43/50 Bildschirmzeilen, 0 = fragen
                                       -> Zeilenzahl oder -1                 */
void vio_setmode(int s, int full, int owner);   /* owner: Task oder -1 */
void vio_task_end(int s, int t);    /* Vollbild des Tasks zuruecknehmen   */
void vio_setcur(int s, int row, int col);       /* row 0xFF = verbergen */
void vio_mou_reg(unsigned buttons);             /* Maus angemeldet       */
void vio_mou_event(unsigned buttons, int dx, int dy);  /* aus dem Interrupt */
unsigned vio_mou_open(int s, int t);            /* -> Tasten, 0 = keine  */
int  vio_mou_read(int s, unsigned char *e);     /* 1 = Ereignis (3 Byte) */
void vio_mou_show(int s, int on);
int  vio_paste(int take);                       /* Konsolenmaus: einfuegen */
unsigned long vio_buf_lin(int s);               /* VioGetBuf: Pufferbasis */
void vio_showbuf(int s, unsigned a, unsigned b);
int __far vio_setgfx(int s, int t, unsigned mode);   /* 13h / 0 = Text        */
unsigned long __far vio_gfx_base(int s);
unsigned __far vio_gfx_limit(int s);
int __far vio_setplane(int s, unsigned p);
int __far vio_setpage(int s, unsigned p);
void __far vio_idle(void);
int __far vio_gfx_reg(int s, int t, unsigned sel);
int __far vio_is_gfx(int s);
void __far vio_setpal(int s, unsigned first, unsigned n, unsigned sel, unsigned off);
void ldt_set(int t, unsigned sel, unsigned long base, unsigned limit);
void ldt_drop(int t, unsigned sel);
unsigned vio_clip_put(unsigned sel, unsigned off, unsigned len);
unsigned vio_clip_get(unsigned sel, unsigned off, unsigned max);

/* ---- Tastatur (kbd.c) -------------------------------------------------- */
void kbd_init(void);
int  kbd_get(void);                 /* Zeichen | Scancode << 8, -1 = leer  */
int  kbd_peek(void);
void kbd_irq(unsigned sc);          /* Scancode verarbeiten (IRQ 1)       */
unsigned kbd_shift(void);           /* KBDSHIFT_*                          */
void kbd_flush(void);
void vio_rehw(void);
extern unsigned char vio_frozen;     /* 5.1: Bildschirm gehoert der Box */
int  kbd_set_layout(unsigned sel, unsigned off);   /* 0 = ok    */
const char *kbd_layout_id(void);
extern volatile unsigned char kbd_switch_req, kbd_break_req, kbd_tm_req;

#endif
