/*
 * DOS 5.0 - gemeinsame Kernel-Definitionen
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
#define NDRV        4               /* Geraetetreiber hoechstens           */
#define DRVC_IDX(i) (9 + 3 * NTASK + 2 * (i))   /* Treibercode, Ring 0    */
#define DRVC_SEL(i) (DRVC_IDX(i) * 8)
#define DRVD_SEL(i) (DRVC_SEL(i) + 8)           /* Treiberdaten           */
#define NGDT        (9 + 3 * NTASK + 2 * NDRV)

#define DMABUF_LIN  0x8000UL        /* unter 1 MB, keine 64-KB-Grenze      */
#define DMABUF_SIZE 512

#define KBASE       0x10000UL       /* Kernel: ein 64-KB-Segment ab hier   */
#define LIN(p)      (KBASE + (unsigned short)(p))

struct desc  { unsigned short limit, base_lo; unsigned char base_hi, access; unsigned short rsv; };

#define ACC_KDATA   0x92

extern struct desc gdt[];

void set_desc(int idx, unsigned long base, unsigned limit, unsigned char acc);
void scratch_map(unsigned long base, unsigned limit);
void scratch2_map(unsigned long base, unsigned limit);

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

extern volatile unsigned char  fdc_irq;
extern volatile unsigned short uptime_s;

/* ---- Hilfen (kernel.c) ------------------------------------------------- */
void far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n);
void delay_ticks(unsigned n);
void panic(const char *msg);
void reboot(void);

/* ---- Blockgeraet: Diskette A: (floppy.c) ------------------------------- */
int  fd_init(void);
int  fd_read(unsigned lba);         /* Sektor in den DMA-Puffer, 0 = ok    */
void fd_motor_off(void);

/* ---- Blockgeraete (kernel.c): Diskette und Einheiten der Treiber ------- */
#define NBLK        8
#define BLK_FLOPPY  0               /* Geraet 0 ist immer Diskette A:      */
struct blkdev {
    signed char    drv;             /* Treiber, -1 = Diskette im Kernel     */
    unsigned char  unit;
    unsigned long  blocks;
};
extern struct blkdev blk[NBLK];
extern int nblk;
int  blk_read(int dev, unsigned long lba);  /* -> DMABUF, 0 = ok         */

/* ---- Sitzungen und Konsole (vio.c) ------------------------------------- */
#define SROWS       23              /* Zeilen 1-23, 0 Titel, 24 Status     */
#define SCOLS       80
#define SBYTES      (SROWS * SCOLS * 2)

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
void vio_status(void);

/* ---- Tastatur (kbd.c) -------------------------------------------------- */
void kbd_init(void);
int  kbd_get(void);                 /* Zeichen | Scancode << 8, -1 = leer  */
int  kbd_peek(void);
void kbd_flush(void);
int  kbd_set_layout(unsigned sel, unsigned off);   /* 0 = ok    */
const char *kbd_layout_id(void);
extern volatile unsigned char kbd_switch_req, kbd_break_req;

#endif
