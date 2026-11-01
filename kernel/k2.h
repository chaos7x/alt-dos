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

/* Ports und Interrupts inline (kein Fernaufruf je Zugriff) */
unsigned char k2_inb(unsigned port);
#pragma aux k2_inb = "in al,dx" __parm [__dx] __value [__al];
void k2_outb(unsigned port, unsigned char v);
#pragma aux k2_outb = "out dx,al" __parm [__dx] [__al];
unsigned k2_irq_off(void);
#pragma aux k2_irq_off = "pushf" "pop ax" "cli" __value [__ax];
void k2_sti(void);
#pragma aux k2_sti = "sti";
#define inb(p)          k2_inb(p)
#define outb(p, v)      k2_outb((p), (unsigned char)(v))
#define irq_off()       k2_irq_off()
#define enable_ints()   k2_sti()

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
unsigned __far kf_mem_alloc(unsigned units);
void __far kf_mem_free(unsigned start, unsigned units);
void __far kf_ldt_set(int t, unsigned sel, unsigned long base, unsigned limit);
void __far kf_ldt_drop(int t, unsigned sel);
void __far kf_redraw(void);
void __far kf_vio_flush(void);
#define scratch_map(b, l)           kf_scratch_map((b), (l))
#define scratch2_map(b, l)          kf_scratch2_map((b), (l))
#define win_save(b)                 kf_win_save(b)
#define win_restore(b)              kf_win_restore(b)
#define zero_seg(s, w)              kf_zero_seg((s), (w))
#define mem_alloc(u)                kf_mem_alloc(u)
#define mem_free(s, u)              kf_mem_free((s), (u))
#define ldt_set(t, s, b, l)         kf_ldt_set((t), (s), (b), (l))
#define ldt_drop(t, s)              kf_ldt_drop((t), (s))
#define redraw()                    kf_redraw()
#define vio_flush()                 kf_vio_flush()

#endif
