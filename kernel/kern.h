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

#define DMABUF_LIN  0x8000UL        /* unter 1 MB, keine 64-KB-Grenze      */
#define DMABUF_SIZE 512

struct desc  { unsigned short limit, base_lo; unsigned char base_hi, access; unsigned short rsv; };

#define ACC_KDATA   0x92

extern struct desc gdt[];

/* SCRATCH auf linearen Speicher richten */
void scratch_map(unsigned long base, unsigned limit);

/* ---- Assembler-Routinen (start.asm) ------------------------------------ */
void          outb(unsigned port, unsigned val);
unsigned      inb(unsigned port);
unsigned long get_ticks(void);
void          idle_wait(void);
void          enable_ints(void);
void          zero_seg(unsigned sel, unsigned words);

extern volatile unsigned char fdc_irq;

/* ---- Hilfen ------------------------------------------------------------ */
void far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n);
void delay_ticks(unsigned n);

/* ---- Blockgeraet: Diskette A: (floppy.c) ------------------------------- */
int  fd_init(void);
int  fd_read(unsigned lba);         /* Sektor in den DMA-Puffer, 0 = ok    */
void fd_motor_off(void);

#endif
