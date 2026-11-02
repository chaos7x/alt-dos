/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEM_H
#define MEM_H

/* Speicher in 4-KB-Seiten, Seitennummern 32 Bit (AT-DOS 6.0) */
typedef unsigned long pg_t;
#define MEM_PAGE        4096U
#define PAGES(n)        ((unsigned)(((unsigned long)(n) + 4095) >> 12))
#define PG_LIN(p)       ((unsigned long)(p) << 12)
#define MEM_MAX_PG      0x40000UL   /* 1 GB                                */
#define MEMMAP_LIN      0x9000UL    /* Bitmap bis 256 MB (8 KB) hier       */
#define MEMMAP_FIX      8192U
#define MEMMAP_SEL      0x168       /* = KSEG_IDX(0) * 8, siehe kern.h     */

/* Speicherkarte des BIOS (start.asm, INT 15h E820h) */
struct e820 { unsigned long base_lo, base_hi, len_lo, len_hi, type; };
#define E820_MAX        32
extern struct e820    e820_map[E820_MAX];
extern unsigned short e820_n, e801_lo, e801_hi, mem_ext_kb, mem_conv_kb;

/* mem.c liegt im zweiten Codesegment (KTEXT2): Einspruenge fern */
void          __far mem_init(pg_t low, unsigned conv_kb); /* low: erste freie Seite hinter dem Kernel */
pg_t          __far mem_alloc(unsigned pages);      /* -> erste Seite, 0 = kein Speicher */
pg_t          __far mem_alloc_low(unsigned pages);  /* unter 16 MB (ISA-DMA)   */
void          __far mem_free(pg_t start, unsigned pages);
unsigned long __far mem_free_kb(void);
unsigned long __far mem_total_kb(void);
unsigned long __far mem_ram_kb(void);
/* Rueckruf bei Speichermangel (Platten-Cache gibt saubere Stuecke ab):
   -> freigegebene Seiten, 0 = nichts mehr abzugeben. Darf keine
   Plattenzugriffe machen (mem_alloc kommt auch ohne Kernelsperre). */
extern pg_t (*mem_reclaim)(unsigned pages);

#endif
