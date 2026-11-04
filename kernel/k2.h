/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT-DOS 5.0 - fuer Kernelteile im zweiten Codesegment (KCODE2)
 *
 * Solche Dateien werden mit wcc -nt=KTEXT2 uebersetzt. Ihre Einspruenge
 * sind __far; nahe Aufrufe gehen nur innerhalb von KTEXT2. Kernelhilfen
 * aus dem ersten Codesegment erreichen sie ueber Fern-Thunks (k2thunk.c),
 * Portzugriffe und Interrupt-Schalter sind hier inline. Daten (DS) sind
 * dieselben wie im uebrigen Kernel.
 */
#ifndef K2_H
#define K2_H
#include "kern.h"
#include "mem.h"

/* Ports und Interrupts inline (kein Fernaufruf je Zugriff) */
unsigned char k2_inb(unsigned port);
#pragma aux k2_inb = "in al,dx" __parm [__dx] __value [__al];
void k2_outb(unsigned port, unsigned char v);
#pragma aux k2_outb = "out dx,al" __parm [__dx] [__al];
unsigned k2_irq_off(void);
#pragma aux k2_irq_off = "pushf" "pop ax" "cli" __value [__ax];
void k2_sti(void);
#pragma aux k2_sti = "sti";
void k2_irq_restore(unsigned f);
#pragma aux k2_irq_restore = "push ax" "popf" __parm [__ax];
#define inb(p)          k2_inb(p)
#define outb(p, v)      k2_outb((p), (unsigned char)(v))
#define irq_off()       k2_irq_off()
#define enable_ints()   k2_sti()
#define irq_restore(f)  k2_irq_restore(f)

/* Kernelhilfen ueber Fern-Thunks */
unsigned long __far kf_get_ticks(void);
unsigned __far kf_proc_block(unsigned long id, unsigned timeout);
void __far kf_far_copy(unsigned dsel, unsigned doff, unsigned ssel, unsigned soff, unsigned n);
unsigned __far kf_kseg_dma(unsigned bytes, unsigned long *lin);
#define get_ticks()                 kf_get_ticks()
#define proc_block(id, t)           kf_proc_block((id), (t))
#define far_copy(a, b, c, d, e)     kf_far_copy((a), (b), (c), (d), (e))
#define kseg_dma(b, l)              kf_kseg_dma((b), (l))
void __far kf_scratch_map(unsigned long base, unsigned limit);
void __far kf_scratch2_map(unsigned long base, unsigned limit);
void __far kf_win_save(unsigned short *b);
void __far kf_win_restore(const unsigned short *b);
void __far kf_zero_seg(unsigned sel, unsigned words);
pg_t __far kf_reclaim(unsigned pages);           /* mem_reclaim (Cache)   */
void __far kf_set_desc(int i, unsigned long base, unsigned limit, unsigned char acc);
void __far kf_ldt_set(int t, unsigned sel, unsigned long base, unsigned limit);
void __far kf_ldt_drop(int t, unsigned sel);
void __far kf_redraw(void);
void __far kf_vio_flush(void);
#define scratch_map(b, l)           kf_scratch_map((b), (l))
#define scratch2_map(b, l)          kf_scratch2_map((b), (l))
#define win_save(b)                 kf_win_save(b)
#define win_restore(b)              kf_win_restore(b)
#define zero_seg(s, w)              kf_zero_seg((s), (w))
#define set_desc(i, b, l, a)        kf_set_desc((i), (b), (l), (a))
#define ldt_set(t, s, b, l)         kf_ldt_set((t), (s), (b), (l))
#define ldt_drop(t, s)              kf_ldt_drop((t), (s))
#define redraw()                    kf_redraw()
#define vio_flush()                 kf_vio_flush()

/* fat.c */
int __far kf_blk_read(int dev, unsigned long lba);
int __far kf_blk_read_n(int dev, unsigned long lba, unsigned n);
int __far kf_blk_write(int dev, unsigned long lba);
void __far kf_blk_invalidate(void);
int __far kf_blk_media(int dev);
int __far kf_blk_nomedia(int dev);
unsigned __far kf_kseg_alloc(unsigned long bytes);
#define blk_read(d, l)              kf_blk_read((d), (l))
#define blk_read_n(d, l, n)         kf_blk_read_n((d), (l), (n))
#define blk_write(d, l)             kf_blk_write((d), (l))
#define blk_invalidate()            kf_blk_invalidate()
#define blk_media(d)                kf_blk_media(d)
#define blk_nomedia(d)              kf_blk_nomedia(d)
#define kseg_alloc(b)               kf_kseg_alloc(b)

#endif
