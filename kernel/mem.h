/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEM_H
#define MEM_H

#define MEM_UNIT        256         /* Bytes je Einheit                    */
#define MEMMAP_LIN      memmap_lin  /* Belegungs-Bitmap, 8 KB (kern.h)      */
#define MEMMAP_SEL      0x168       /* = KSEG_IDX(0) * 8, siehe kern.h     */
#define MEM_HIGH_START  0x1000      /* 100000h: 1 MB                       */

void     mem_init(unsigned low, unsigned conv_kb, unsigned ext_kb); /* low: erste freie Einheit hinter dem Kernel */
unsigned mem_alloc(unsigned units);             /* -> erste Einheit, 0 = Fehler */
void     mem_free(unsigned start, unsigned units);
void     mem_reserve(unsigned from, unsigned to);  /* [from, to) belegen */
int      mem_is_free(unsigned from, unsigned to);  /* [from, to) ganz frei */
unsigned mem_free_kb(void);
/* Rueckruf bei Speichermangel (Platten-Cache gibt saubere Stuecke ab):
   -> freigegebene Einheiten, 0 = nichts mehr abzugeben. Darf keine
   Plattenzugriffe machen (mem_alloc kommt auch ohne Kernelsperre). */
extern unsigned (*mem_reclaim)(unsigned units);

#endif
